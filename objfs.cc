/*
  Object file system
  
  Uses data objects and metadata objects. Data objects form a logical
  log, and are a complete record of the file system. Metadata objects
  roll up all changes into a read-optimized form.

 */

/* data objects:
   - header (length, version, yada yada)
   - metadata (log records)
   - file data

   data is always in the current object, and is identified by offsets
   from the beginning of the file data section. (simplifies assembling
   the object before writing it out)

   all offsets are in units of bytes, even if we do R/M/W of 4KB pages
   of file data. This limits us to 4GB objects, which should be OK.
   when it's all done we'll check the space requirements for going to
   64 (or maybe 48)
*/

#define FUSE_USE_VERSION 27
#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <sys/stat.h>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <errno.h>
#include <fuse.h>
#include <queue>
#include <memory>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <time.h>
#include <string.h>

extern "C" int fs_getattr(const char *path, struct stat *sb);
extern "C" int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi);
extern "C" int fs_write(const char *path, const char *buf, size_t len,
                    off_t offset, struct fuse_file_info *fi);
extern "C" int fs_mkdir(const char *path, mode_t mode);
extern "C" int fs_rmdir(const char *path);
extern "C" int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
extern "C" int fs_mknod(const char *path, mode_t mode, dev_t dev);
extern "C" int fs_unlink(const char *path);
extern "C" int fs_rename(const char *src_path, const char *dst_path);
extern "C" int fs_chmod(const char *path, mode_t mode);
extern "C" int fs_utimens(const char *path, const struct timespec tv[2]);
extern "C" int fs_read(const char *path, char *buf, size_t len, off_t offset,
                   struct fuse_file_info *fi);
extern "C" int fs_symlink(const char *path, const char *contents);
extern "C" int fs_readlink(const char *path, char *buf, size_t len);
extern "C" int fs_statfs(const char *path, struct statvfs *st);
extern "C" int fs_fsync(const char * path, int, struct fuse_file_info *fi);
extern "C" int fs_truncate(const char *path, off_t len);
extern "C" int fs_initialize(const char*);
extern "C" int fs_mkfs(const char*);
extern "C" void fs_sync(void);
extern "C" void fs_teardown(void);

void xclock_gettime(int x, struct timespec *time)
{
    time->tv_sec = 0;
    time->tv_nsec = 0;
}

//typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
//                                const struct stat *stbuf, off_t off);

/**********************************
 * Yet another extent map...
 */
struct extent {
    int64_t  objnum;
    uint32_t offset;
    uint32_t len;
};
    
typedef std::map<int64_t,extent> internal_map;

class extmap {
    internal_map the_map;

public:
    internal_map::iterator begin() { return the_map.begin(); }
    internal_map::iterator end() { return the_map.end(); }

    // returns one of:
    // - extent containing @offset
    // - lowest extent with base > @offset
    // - end()
    internal_map::iterator lookup(int64_t offset) {
	auto it = the_map.lower_bound(offset);
	if (it == the_map.end())
	    return it;
	auto& [base, e] = *it;
	if (base > offset && it != the_map.begin()) {
	    it--;
	    auto& [base0, e0] = *it;
	    if (offset < base0 + e0.len)
		return it;
	    it++;
	}
	return it;
    }

    void update(int64_t offset, extent e) {
	// two special cases
	// (1) map is empty - just add and we're done
	//
	if (the_map.empty()) {
	    the_map[offset] = e;
	    return;
	}

	// extending the last extent
	//
	auto [key, val] = *(--the_map.end());
	if (offset == key + val.len && e.offset == val.offset + val.len) {
	    val.len += e.len;
	    the_map[key] = val;
	    return;
	}
	
	auto it = the_map.lower_bound(offset);

	// we're at the the end of the list
	if (it == end()) {
	    the_map[offset] = e;
	    return;
	}

	// erase any extents fully overlapped
	//       -----  --- 
	//   +++++++++++++++++
	// = +++++++++++++++++
	//
	while (it != the_map.end()) {
	    auto [key, val] = *it;
	    if (key >= offset && key+val.len <= offset + e.len) {
		it++;
		the_map.erase(key);
	    }
	    else
		break;
	}

	if (it != the_map.end()) {
	    // update right-hand overlap
	    //        ---------
	    //   ++++++++++
	    // = ++++++++++----
	    //
	    auto [key, val] = *it;
	
	    if (key < offset + e.len) {
		auto new_key = offset + e.len;
		val.len -= (new_key - key);
		val.offset += (new_key - key);
		the_map.erase(key);
		the_map[new_key] = val;
	    }
	}

	it = the_map.lower_bound(offset);	
	if (it != the_map.begin()) {
	    it--;
	    auto [key, val] = *it;

	    // we bisect an extent
	    //   ------------------
	    //           +++++
	    // = --------+++++-----
	    if (key < offset && key + val.len > offset + e.len) {
		auto new_key = offset + e.len;
		auto new_len = val.len - (new_key-key);
		val.len = offset - key;
		the_map[key] = val;
		val.offset += (new_key-key);
		val.len = new_len;
		the_map[new_key] = val;
	    }

	    // left-hand overlap
	    //   ---------
	    //       ++++++++++
	    // = ----++++++++++
	    //
	    else if (key < offset && key + val.len > offset) {
		val.len = offset - key;
		the_map[key] = val;
	    }
	}

	the_map[offset] = e;
    }

    void erase(int64_t offset) {
	the_map.erase(offset);
    }
};

enum obj_type {
    OBJ_FILE = 1,
    OBJ_DIR = 2,
    OBJ_SYMLINK = 3,
    OBJ_OTHER = 4
};

/* maybe have a factory that creates the appropriate object type
 * given a pointer to its encoding.
 * need a standard method to serialize an object.
 */

/* serializes in its in-memory layout. 
 * Except maybe packed or something.
 * oh, actually use 1st 4 bytes for type/length
 */
class fs_obj {
public:
    uint32_t        type : 4;
    uint32_t        len : 28;	// of serialized metadata
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    int64_t         size;
    struct timespec mtime;
    size_t serialize(void *);
};

/* serializes to inode + extent array
 * No extent count needed - can just use the size field
 *
 * actually the extent might be packed somewhat - 
 * we can get it down to 12 bytes
 *  - objnum      : 40
 *  - offset      : 30
 *  - len         : 20
 *  - flags/cruft : 6
 */
class fs_file : public fs_obj {
public:
    extmap  extents;
    size_t serialize(void*);
};

/* directory entry serializes as:
 *  - uint32 inode #
 *  - uint32 byte offset
 *  - uint32 byte length
 *  - uint8  namelen
 *  - char   name[]
 */
class fs_directory : public fs_obj {
public:
    std::map<std::string,uint32_t> dirents;
    size_t serialize(void*);
};

/* extra space in entry is just the target
 */
class fs_link : public fs_obj {
public:
    std::string target;
    size_t serialize(void*);
};

/****************
 * file header format
 */

/* data update
*/
struct log_data {
    uint32_t inum;		// is 32 enough?
    uint32_t obj_offset;	// bytes from start of file data
    int64_t  file_offset;	// in bytes
    int64_t  size;		// file size after this write
    uint32_t len;		// bytes
} __attribute__((packed,aligned(1)));

/* inode update. Note that this is all that's needed for special
 * files. 
 */
struct log_inode {
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    struct timespec mtime;
} __attribute__((packed,aligned(1)));

/* truncate a file. maybe require truncate->0 before delete?
 */
struct log_trunc {
    uint32_t inum;
    int64_t  new_size;		// must be <= existing
} __attribute__((packed,aligned(1)));

// need a log_create

struct log_delete {
    uint32_t parent;
    uint32_t inum;
    uint8_t  namelen;
    char     name[];
} __attribute__((packed,aligned(1)));

struct log_symlink {
    uint32_t inum;
    uint8_t  len;
    char     target[];
} __attribute__((packed,aligned(1)));

/* cross-directory rename is handled by specifying both source and
 * destination parent directory.
 */
struct log_rename {
    uint32_t inum;		// of entity to rename
    uint32_t parent1;		// inode number (source)
    uint32_t parent2;		//              (dest)
    uint8_t  name1_len;
    uint8_t  name2_len;
    char     name[];
} __attribute__((packed,aligned(1)));

/* create a new name
 */
struct log_create {
    uint32_t  parent_inum;
    uint32_t  inum;
    uint8_t   namelen;
    char      name[];
} __attribute__((packed,aligned(1)));


enum log_rec_type {
    LOG_INODE = 1,
    LOG_TRUNC,
    LOG_DELETE,
    LOG_SYMLNK,
    LOG_RENAME,
    LOG_DATA,
    LOG_CREATE,
    LOG_NULL,			// fill space for alignment
};

struct log_record {
    uint16_t type : 4;
    uint16_t len : 12;
    char data[];
} __attribute__((packed,aligned(1)));

//#define OBJFS_MAGIC 0x4f424653	// "OBFS"
#define OBJFS_MAGIC 0x5346424f	// "OBFS"

struct obj_header {
    int32_t magic;
    int32_t version;
    int32_t type;		// 1 == data, 2 == metadata
    int32_t hdr_len;
    int32_t this_index;
    char    data[];
};

/* until we add metadata objects this is enough global state
 */
std::unordered_map<uint32_t, fs_obj*>    inode_map;


/*
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    struct timespec mtime;
    int64_t         size;
 */
static void update_inode(fs_obj *obj, log_inode *in)
{
    obj->inum = in->inum;
    obj->mode = in->mode;
    obj->uid = in->uid;
    obj->gid = in->gid;
    obj->rdev = in->rdev;
    obj->mtime = in->mtime;
}

static int read_log_inode(log_inode *in)
{
    auto it = inode_map.find(in->inum);
    if (it != inode_map.end()) {
	auto obj = inode_map[in->inum];
	update_inode(obj, in);
    }
    else {
	if (S_ISDIR(in->mode)) {
	    fs_directory *d = new fs_directory;
	    d->type = OBJ_DIR;
	    d->size = 0;
	    inode_map[in->inum] = d;
	    update_inode(d, in);
	}
	else if (S_ISREG(in->mode)) {
	    fs_file *f = new fs_file;
	    f->type = OBJ_FILE;
	    f->size = 0;
	    update_inode(f, in);
	    inode_map[in->inum] = f;
	}
	else if (S_ISLNK(in->mode)) {
	    fs_link *s = new fs_link;
	    s->type = OBJ_SYMLINK;
	    update_inode(s, in);
	    s->size = 0;
	    inode_map[in->inum] = s;
	}
	else {
	    fs_obj *o = new fs_obj;
	    o->type = OBJ_OTHER;
	    update_inode(o, in);
	    o->size = 0;
	    inode_map[in->inum] = o;
	}
    }
    return 0;
}

void do_trunc(fs_file *f, off_t new_size)
{
    while (true) {
	auto it = f->extents.lookup(new_size);
	if (it == f->extents.end())
	    break;
	auto [offset, e] = *it;
	if (offset < new_size) {
	    e.len = new_size - offset;
	    f->extents.update(offset, e);
	}
	else {
	    f->extents.erase(offset);
	}
    }
    f->size = new_size;
}
    
int read_log_trunc(log_trunc *tr)
{
    auto it = inode_map.find(tr->inum);
    if (it == inode_map.end())
	return -1;

    fs_file *f = (fs_file*)(inode_map[tr->inum]);
    if (f->size < tr->new_size)
	return -1;

    do_trunc(f, tr->new_size);
    return 0;
}

// assume directory has been emptied or file has been truncated.
//
static int read_log_delete(log_delete *rm)
{
    if (inode_map.find(rm->parent) == inode_map.end())
	return -1;
    if (inode_map.find(rm->inum) == inode_map.end())
	return -1;

    fs_directory *parent = (fs_directory*)(inode_map[rm->parent]);
    auto name = std::string(rm->name, rm->namelen);
    fs_obj *f = inode_map[rm->inum];
    inode_map.erase(rm->inum);
    parent->dirents.erase(name);
    delete f;

    return 0;
}

// assume the inode has already been created
//
static int read_log_symlink(log_symlink *sl)
{
    if (inode_map.find(sl->inum) == inode_map.end())
	return -1;

    fs_link *s = (fs_link *)(inode_map[sl->inum]);
    s->target = std::string(sl->target, sl->len);
    
    return 0;
}

// all inodes must exist
//
static int read_log_rename(log_rename *mv)
{
    if (inode_map.find(mv->parent1) == inode_map.end())
	return -1;
    if (inode_map.find(mv->parent2) == inode_map.end())
	return -1;
    
    fs_directory *parent1 = (fs_directory*)(inode_map[mv->parent1]);
    fs_directory *parent2 = (fs_directory*)(inode_map[mv->parent2]);

    auto name1 = std::string(&mv->name[0], mv->name1_len);
    auto name2 = std::string(&mv->name[mv->name1_len], mv->name2_len);

    if (parent1->dirents.find(name1) == parent1->dirents.end())
	return -1;
    if (parent1->dirents[name1] != mv->inum)
	return -1;
    if (parent2->dirents.find(name2) != parent1->dirents.end())
	return -1;
	    
    parent1->dirents.erase(name1);
    parent2->dirents[name2] = mv->inum;
    
    return 0;
}

int read_log_data(int idx, log_data *d)
{
    auto it = inode_map.find(d->inum);
    if (it == inode_map.end())
	return -1;

    fs_file *f = (fs_file*) inode_map[d->inum];
    
    // optimization - check if it extends the previous record?
    extent e = {.objnum = idx, .offset = d->obj_offset,
		.len = d->len};
    f->extents.update(d->file_offset, e);
    f->size = d->size;

    return 0;
}

int next_inode = 2;

/* checkpoint has fields:
 * .. same obj header w/ type=2 ..
 * root inode #, offset, len
 * next inode #
 * inode table offset : u32
 *  [stuff]
 * inode table []:
 *    - u32 inum 
 *    - u32 offset
 *    - u32 len
 * this allows us to generate the inode table as we serialize all the
 * objects. 
 */

/* follows the obj_header 
 */
struct ckpt_header  {
    uint32_t root_inum;
    uint32_t root_offset;
    uint32_t root_len;
    uint32_t next_inum;
    uint32_t itable_offset;
    /* itable_len is implicit */
    char     data[];
};

struct itable_entry {		// need to add obj#
    uint32_t inum;
    uint32_t offset;
    uint32_t len;
};

int read_log_create(log_create *c)
{
    auto it = inode_map.find(c->parent_inum);
    if (it == inode_map.end())
	return -1;

    fs_directory *d = (fs_directory*) inode_map[c->parent_inum];
    auto name = std::string(&c->name[0], c->namelen);
    d->dirents[name] = c->inum;

    next_inode = std::max(next_inode, (int)(c->inum + 1));
    
    return 0;
}

// returns 0 on success, bytes to read if not enough data,
// -1 if bad format. Must pass at least 32B
//
size_t read_hdr(int idx, void *data, size_t len)
{
    obj_header *oh = (obj_header*)data;
    if ((size_t)(oh->hdr_len) > len)
	return oh->hdr_len;

    if (oh->magic != OBJFS_MAGIC || oh->version != 1 || oh->type != 1)
	return -1;

    size_t meta_bytes = oh->hdr_len - sizeof(obj_header);
    log_record *end = (log_record*)&oh->data[meta_bytes];
    log_record *rec = (log_record*)&oh->data[0];

    while (rec < end) {
	switch (rec->type) {
	case LOG_INODE:
	    if (read_log_inode((log_inode*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_TRUNC:
	    if (read_log_trunc((log_trunc*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_DELETE:
	    if (read_log_delete((log_delete*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_SYMLNK:
	    if (read_log_symlink((log_symlink*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_RENAME:
	    if (read_log_rename((log_rename*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_DATA:
	    if (read_log_data(idx, (log_data*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_CREATE:
	    if (read_log_create((log_create*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_NULL:
	    break;
	default:
	    return -1;
	}
	rec = (log_record*)&rec->data[rec->len];
    }
    return 0;
}


int this_index = 0;
char *file_prefix;

// https://docs.microsoft.com/en-us/cpp/cpp/how-to-create-and-use-shared-ptr-instances?view=msvc-160
// not hugely useful here, but good exercise for later

class openfile {
public:
    int index;
    int fd;
};

// simple cache using FIFO eviction, much easier than implementing LRU
//
std::map<int,std::shared_ptr<openfile>> fd_cache;
std::queue<std::shared_ptr<openfile>> fd_fifo;
#define MAX_OPEN_FDS 50

int get_fd(int index)
{
    // hit?
    if (fd_cache.find(index) != fd_cache.end()) {
	auto of = fd_cache[index];
	return of->fd;
    }

    // evict?
    if (fd_fifo.size() >= MAX_OPEN_FDS) {
	auto of = fd_fifo.front();
	close(of->fd);
	fd_cache.erase(of->index);
	fd_fifo.pop();
    }

    // open a new one
    char buf[256];
    sprintf(buf, "%s.%08x", file_prefix, index);
    int fd = open(buf, O_RDONLY);
    if (fd < 0)
	return -1;
    
    auto of = std::make_shared<openfile>();
    of->index = index;
    of->fd = fd;
    fd_cache[index] = of;
    fd_fifo.push(of);

    return fd;
}

/* 
   when do we decide to write? 
   (1) When we get to a fixed object size
   (2) when we get an fsync (maybe)
   (3) on timeout (working on it...)
       (need a reader/writer lock for this one?)
 */

void  *meta_log_head;
void  *meta_log_tail;
size_t meta_log_len;

void  *data_log_head;
void  *data_log_tail;
size_t data_log_len;

size_t data_offset(void)
{
    return (char*)data_log_tail - (char*)data_log_head;
}

size_t meta_offset(void)
{
    return (char*)meta_log_tail - (char*)meta_log_head;
}
    
//std::set<std::shared_ptr<fs_obj>> dirty_inodes;
std::set<fs_obj*> dirty_inodes;

void init_stuff(const char *prefix)
{
    file_prefix = strdup(prefix);
    meta_log_len = 64 * 1024;
    meta_log_head = meta_log_tail = malloc(meta_log_len*2);

    data_log_len = 8 * 1024 * 1024;
    data_log_head = data_log_tail = malloc(data_log_len);
}

void write_inode(fs_obj *f);

int verbose;

extern "C" int test_function(int);
int test_function(int v)
{
    verbose = v;
    return 0;
}

void printout(void *hdr, int hdrlen)
{
    if (!verbose)
	return;
    uint8_t *p = (uint8_t*) hdr;
    for (int i = 0; i < hdrlen; i++)
	printf("%02x", p[i]);
    printf("\n");
}

void write_everything_out(void)
{
    for (auto it = dirty_inodes.begin(); it != dirty_inodes.end();
	 it = dirty_inodes.erase(it)) {
	write_inode(*it);
    }

    char path[strlen(file_prefix) + 32];
    sprintf(path, "%s.%08x", file_prefix, this_index);

    obj_header h = {
	.magic = OBJFS_MAGIC,
	.version = 1,
	.type = 1,
	.hdr_len = (int)(meta_offset() + sizeof(obj_header)),
	.this_index = this_index,
    };
    this_index++;
    
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0777);
    if (fd < 0)
	perror(path), exit(1);
	/* ERROR */;

    printf("writing %s:\n", path);
    printout((void*)&h, sizeof(h));
    printout((void*)meta_log_head, meta_offset());

    write(fd, &h, sizeof(h));
    write(fd, meta_log_head, meta_offset());
    write(fd, data_log_head, data_offset());
    close(fd);
    
    meta_log_tail = meta_log_head;
    data_log_tail = data_log_head;
}

void fs_sync(void)
{
    write_everything_out();
}

void maybe_write(void)
{
    if ((meta_offset() > meta_log_len) ||
	(data_offset() > data_log_len))
	write_everything_out();
}

void make_record(const void *hdr, size_t hdrlen,
		 const void *data, size_t datalen)
{
    printout((void*)hdr, hdrlen);
    
    memcpy(meta_log_tail, hdr, hdrlen);
    meta_log_tail = hdrlen + (char*)meta_log_tail;
    if (datalen > 0) {
	memcpy(data_log_tail, data, datalen);
	data_log_tail = datalen + (char*)data_log_tail;
    }
}

std::map<int,int> data_offsets;

int get_offset(int index)
{
    if (data_offsets.find(index) != data_offsets.end())
	return data_offsets[index];

    int fd = get_fd(index);	// I should think about exceptions...
    if (fd < 0)
	return -1;
    
    obj_header h;
    size_t len = pread(fd, &h, sizeof(h), 0);
    if (len < 0)
	return -1;

    data_offsets[index] = h.hdr_len;
    return h.hdr_len;
}

int read_data(void *buf, int index, off_t offset, size_t len)
{
    if (index == this_index) {
	len = std::min(len, data_offset() - offset);
	memcpy(buf, offset + (char*)data_log_head, len);
	return len;
    }
    
    int fd = get_fd(index);
    if (fd < 0)
	return -1;

    size_t n = get_offset(index);
    if (n < 0)
	return n;
    
    return pread(fd, buf, len, offset + n);
}

static std::vector<std::string> split(const std::string& s, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
	if (token != "")
	    tokens.push_back(token);
    }
    return tokens;
}

// returns inode number or -ERROR
// kind of conflicts with uint32_t for inode #...
//
static int vec_2_inum(std::vector<std::string> pathvec)
{
    uint32_t inum = 1;

    for (auto it = pathvec.begin(); it != pathvec.end(); it++) {
	if (inode_map.find(inum) == inode_map.end())
	    return -ENOENT;
	fs_obj *obj = inode_map[inum];
	if (obj->type != OBJ_DIR)
	    return -ENOTDIR;
	fs_directory *dir = (fs_directory*) obj;
	if (dir->dirents.find(*it) == dir->dirents.end())
	    return -ENOENT;
	inum = dir->dirents[*it];
    }
    
    return inum;
}

std::unordered_map<std::string, int> path_map;
std::queue<std::string> path_fifo;
#define MAX_CACHED_PATHS 100

static int path_2_inum(const char *path)
{
#if 0
    NEED TO REMOVE ENTRIES ON RMDIR/UNLINK
    if (path_map.find(path) != path_map.end())
	return path_map[path];
#endif     
    auto pathvec = split(path, '/');
    int inum = vec_2_inum(pathvec);

    path_map[path] = inum;

#if 0
    if (inum >= 0) {
	if (path_fifo.size() >= MAX_CACHED_PATHS) {
	    auto old = path_fifo.front();
	    path_map.erase(old);
	    path_fifo.pop();
	}
#endif
    
    return inum;
}

std::tuple<int,int,std::string> path_2_inum2(const char* path)
{
    auto pathvec = split(path, '/');
    int inum = vec_2_inum(pathvec);

    auto leaf = pathvec.back();
    pathvec.pop_back();
    int parent_inum = vec_2_inum(pathvec);

    return make_tuple(inum, parent_inum, leaf);
}

static void obj_2_stat(struct stat *sb, fs_obj *in)
{
    memset(sb, 0, sizeof(*sb));
    sb->st_ino = in->inum;
    sb->st_mode = in->mode;
    sb->st_nlink = 1;
    sb->st_uid = in->uid;
    sb->st_gid = in->gid;
    sb->st_size = in->size;
    sb->st_blocks = (in->size + 4095) / 4096;
    sb->st_atim = sb->st_mtim = sb->st_ctim = in->mtime;
}

int fs_getattr(const char *path, struct stat *sb)
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    obj_2_stat(sb, obj);

    return 0;
}

int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
		      off_t offset, struct fuse_file_info *fi)
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_DIR)
	return -ENOTDIR;
    
    fs_directory *dir = (fs_directory*)obj;
    for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
	struct stat sb;
	auto [name, i] = *it;
	fs_obj *o = inode_map[i];
	obj_2_stat(&sb, o);
	filler(ptr, const_cast<char*>(name.c_str()), &sb, 0);
    }

    return 0;
}


// -------------------------------

int fs_write(const char *path, const char *buf, size_t len,
	     off_t offset, struct fuse_file_info *fi)
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_FILE)
	return -EISDIR;

    fs_file *f = (fs_file*)obj;
    off_t new_size = std::max((off_t)(offset+len), (off_t)(f->size));
    size_t obj_offset = data_offset();

    int hdr_bytes = sizeof(log_record) + sizeof(log_data);
    char hdr[hdr_bytes];
    log_record *lr = (log_record*) hdr;
    log_data *ld = (log_data*) lr->data;

    lr->type = LOG_DATA;
    lr->len = sizeof(log_data);

    *ld = (log_data) { .inum = (uint32_t)inum,
		       .obj_offset = (uint32_t)obj_offset,
		       .file_offset = (int64_t)offset,
		       .size = (int64_t)new_size,
		       .len = (uint32_t)len };

    make_record((void*)hdr, hdr_bytes, buf, len);

    // optimization - check if it extends the previous record?
    extent e = {.objnum = this_index,
		.offset = (uint32_t)obj_offset, .len = (uint32_t)len};
    f->extents.update(offset, e);
    dirty_inodes.insert(f);
    maybe_write();
    
    return len;
}

void write_inode(fs_obj *f)
{
    size_t len = sizeof(log_record) + sizeof(log_inode);
    char buf[len];
    log_record *rec = (log_record*)buf;
    log_inode *in = (log_inode*)rec->data;

    rec->type = LOG_INODE;
    rec->len = sizeof(log_inode);

    in->inum = f->inum;
    in->mode = f->mode;
    in->uid = f->uid;
    in->gid = f->gid;
    in->rdev = f->rdev;
    in->mtime = f->mtime;

    make_record(rec, len, NULL, 0);
}

void write_dirent(uint32_t parent_inum, std::string leaf, uint32_t inum)
{
    size_t len = sizeof(log_record) + sizeof(log_create) + leaf.length();
    char buf[len+sizeof(log_record)];
    log_record *rec = (log_record*) buf;
    log_create *cr8 = (log_create*) rec->data;

    rec->type = LOG_CREATE;
    rec->len = len - sizeof(log_record);
    cr8->parent_inum = parent_inum;
    cr8->inum = inum;
    cr8->namelen = leaf.length();
    memcpy(cr8->name, (void*)leaf.c_str(), leaf.length());

    make_record(rec, len, nullptr, 0);
}

int fs_mkdir(const char *path, mode_t mode)
{
    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum >= 0)
	return -EEXIST;
    if (parent_inum < 0)
	return parent_inum;

    fs_directory *parent = (fs_directory*)inode_map[parent_inum];
    if (parent->type != OBJ_DIR)
	return -ENOTDIR;
    
    inum = next_inode++;
    fs_directory *dir = new fs_directory;
    dir->type = OBJ_DIR;
    dir->inum = inum;
    dir->mode = mode | S_IFDIR;
    dir->rdev = dir->size = 0;
    xclock_gettime(CLOCK_REALTIME, &dir->mtime);

    struct fuse_context *ctx = fuse_get_context();
    dir->uid = ctx->uid;
    dir->gid = ctx->gid;
    
    inode_map[inum] = dir;
    parent->dirents[leaf] = inum;
    xclock_gettime(CLOCK_REALTIME, &parent->mtime);
    dirty_inodes.insert(parent);
    
    write_inode(dir);	// can't rely on dirty_inodes
    write_dirent(parent_inum, leaf, inum);
    maybe_write();

    return 0;
}

void do_log_delete(uint32_t parent_inum, uint32_t inum, std::string name)
{
    size_t len = sizeof(log_record) + sizeof(log_delete) + name.length();
    char buf[len];
    log_record *rec = (log_record*) buf;
    log_delete *del = (log_delete*) rec->data;

    rec->type = LOG_DELETE;
    rec->len = sizeof(*del) + name.length();
    del->parent = parent_inum;
    del->inum = inum;
    del->namelen = name.length();
    memcpy(del->name, (void*)name.c_str(), name.length());

    make_record(rec, len, nullptr, 0);
}

int fs_rmdir(const char *path)
{
    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum < 0)
	return -ENOENT;
    if (parent_inum < 0)
	return parent_inum;
    
    fs_directory *dir = (fs_directory*)inode_map[inum];
    if (dir->type != OBJ_DIR)
	return -ENOTDIR;
    if (!dir->dirents.empty())
	return -ENOTEMPTY;
    
    fs_directory *parent = (fs_directory*)inode_map[parent_inum];
    inode_map.erase(inum);
    parent->dirents.erase(leaf);
    delete dir;
    
    xclock_gettime(CLOCK_REALTIME, &parent->mtime);
    dirty_inodes.insert(parent);
    do_log_delete(parent_inum, inum, leaf);
    maybe_write();
    
    return 0;
}

int create_node(const char *path, mode_t mode, int type, dev_t dev)
{
    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum >= 0)
	return -EEXIST;
    if (parent_inum < 0)
	return parent_inum;
    
    fs_directory *dir = (fs_directory*)inode_map[parent_inum];
    if (dir->type != OBJ_DIR)
	return -ENOTDIR;
    
    inum = next_inode++;
    fs_file *f = new fs_file;	// yeah, OBJ_OTHER gets a useless extent map

    f->type = type;
    f->inum = inum;
    f->mode = mode;
    f->rdev = dev;
    f->size = 0;
    xclock_gettime(CLOCK_REALTIME, &f->mtime);

    struct fuse_context *ctx = fuse_get_context();
    f->uid = ctx->uid;
    f->gid = ctx->gid;
    
    inode_map[inum] = f;
    dir->dirents[leaf] = inum;

    write_inode(f);	// can't rely on dirty_inodes
    write_dirent(parent_inum, leaf, inum);
    
    xclock_gettime(CLOCK_REALTIME, &dir->mtime);
    dirty_inodes.insert(dir);
    maybe_write();
    
    return 0;
}

// only called for regular files
int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    return create_node(path, mode | S_IFREG, OBJ_FILE, 0);
}

// for device files, FIFOs, etc.
int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    return create_node(path, mode, OBJ_OTHER, dev);
}

void do_log_trunc(uint32_t inum, off_t offset)
{
    size_t len = sizeof(log_record) + sizeof(log_trunc);
    char buf[len];
    log_record *rec = (log_record*) buf;
    log_trunc *tr = (log_trunc*) rec->data;

    rec->type = LOG_TRUNC;
    rec->len = sizeof(log_trunc);
    tr->inum = inum;
    tr->new_size = offset;

    make_record(rec, len, nullptr, 0);
}

int fs_truncate(const char *path, off_t len)
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_file *f = (fs_file*)inode_map[inum];
    if (f->type == OBJ_DIR)
	return -EISDIR;
    if (f->type != OBJ_FILE)
	return -EINVAL;
    
    do_trunc(f, len);
    do_log_trunc(inum, len);

    xclock_gettime(CLOCK_REALTIME, &f->mtime);
    dirty_inodes.insert(f);
    maybe_write();
    
    return 0;
}

/* do I need a parent inum in fs_obj?
 */
int fs_unlink(const char *path)
{
    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum < 0)
	return inum;
    fs_obj *obj = inode_map[inum];
    if (obj->type == OBJ_DIR)
	return -EISDIR;
    
    fs_directory *dir = (fs_directory*)inode_map[parent_inum];

    dir->dirents.erase(leaf);
    xclock_gettime(CLOCK_REALTIME, &dir->mtime);
    dirty_inodes.insert(dir);

    if (obj->type == OBJ_FILE) {
	fs_file *f = (fs_file*)obj;
	do_trunc(f, 0);
	do_log_trunc(inum, 0);
    }
    do_log_delete(parent_inum, inum, leaf);
    maybe_write();
    
    return 0;
}

void do_log_rename(int src_inum, int src_parent, int dst_parent,
		      std::string src_leaf, std::string dst_leaf)
{
    int len = sizeof(log_record) + sizeof(log_rename) +
	src_leaf.length() + dst_leaf.length();
    char buf[len];
    log_record *rec = (log_record*)buf;
    log_rename *mv = (log_rename*)rec->data;

    rec->type = LOG_RENAME;
    rec->len = len-sizeof(log_record);

    mv->inum = src_inum;
    mv->parent1 = src_parent;
    mv->parent2 = dst_parent;
    mv->name1_len = src_leaf.length();
    memcpy(mv->name, src_leaf.c_str(), src_leaf.length());
    mv->name2_len = dst_leaf.length();
    memcpy(&mv->name[mv->name1_len], dst_leaf.c_str(), dst_leaf.length());
    
    make_record(rec, len, nullptr, 0);
}

int fs_rename(const char *src_path, const char *dst_path)
{
    auto [src_inum, src_parent, src_leaf] = path_2_inum2(src_path);
    if (src_inum < 0)
	return src_inum;
    
    auto [dst_inum, dst_parent, dst_leaf] = path_2_inum2(dst_path);
    if (dst_inum >= 0)
	return -EEXIST;
    if (dst_parent < 0)
	return dst_parent;

    fs_directory *srcdir = (fs_directory*)inode_map[src_parent];
    fs_directory *dstdir = (fs_directory*)inode_map[src_parent];

    if (dstdir->type != OBJ_DIR)
	return -ENOTDIR;

    srcdir->dirents.erase(src_leaf);
    xclock_gettime(CLOCK_REALTIME, &srcdir->mtime);
    dirty_inodes.insert(srcdir);

    dstdir->dirents[dst_leaf] = src_inum;
    xclock_gettime(CLOCK_REALTIME, &dstdir->mtime);
    dirty_inodes.insert(dstdir);
    
    do_log_rename(src_inum, src_parent, dst_parent, src_leaf, dst_leaf);
    maybe_write();
    
    return 0;
}

int fs_chmod(const char *path, mode_t mode)
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    obj->mode = mode | (S_IFMT & obj->mode);
    dirty_inodes.insert(obj);
    maybe_write();
    
    return 0;
}

// see utimensat(2). Oh, and I hate access time...
//
int fs_utimens(const char *path, const struct timespec tv[2])
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    if (tv == NULL || tv[1].tv_nsec == UTIME_NOW)
	xclock_gettime(CLOCK_REALTIME, &obj->mtime);
    else if (tv[1].tv_nsec != UTIME_OMIT)
	obj->mtime = tv[1];
    dirty_inodes.insert(obj);
    maybe_write();
    
    return 0;
}


int fs_read(const char *path, char *buf, size_t len, off_t offset,
	    struct fuse_file_info *fi)
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_FILE)
	return -ENOTDIR;
    fs_file *f = (fs_file*)obj;

    size_t bytes = 0;
    for (auto it = f->extents.lookup(offset);
	 len > 0 && it != f->extents.end(); it++) {
	auto [base, e] = *it;
	if (base > offset) {
	    // yow, not supposed to have holes
	    size_t skip = base-offset;
	    if (skip > len)
		skip = len;
	    bytes += skip;
	    offset += skip;
	    buf += skip;
	}
	else {
	    size_t skip = offset - base;
	    size_t _len = e.len - skip;
	    if (_len > len)
		_len = len;
	    if (read_data(buf, e.objnum, e.offset+skip, _len) < 0)
		return -EIO;
	    bytes += _len;
	    offset += _len;
	    buf += _len;
	}
    }
    return bytes;
}

void write_symlink(int inum, std::string target)
{
    size_t len = sizeof(log_record) + sizeof(log_symlink) + target.length();
    char buf[sizeof(log_record) + len];
    log_record *rec = (log_record*) buf;
    log_symlink *l = (log_symlink*) rec->data;

    rec->type = LOG_SYMLNK;
    rec->len = len;
    l->inum = inum;
    l->len = target.length();
    memcpy(l->target, target.c_str(), l->len);

    make_record(rec, len, nullptr, 0);
}

int fs_symlink(const char *path, const char *contents)
{
    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum >= 0)
	return -EEXIST;
    if (parent_inum < 0)
	return parent_inum;

    fs_directory *dir = (fs_directory*)inode_map[parent_inum];
    if (dir->type != OBJ_DIR)
	return -ENOTDIR;
    
    fs_link *l = new fs_link;
    l->type = OBJ_SYMLINK;
    l->inum = next_inode++;
    l->mode = S_IFLNK | 0777;

    struct fuse_context *ctx = fuse_get_context();
    l->uid = ctx->uid;
    l->gid = ctx->gid;

    xclock_gettime(CLOCK_REALTIME, &l->mtime);

    l->target = leaf;
    inode_map[inum] = l;
    dir->dirents[leaf] = l->inum;

    write_inode(l);
    write_symlink(inum, leaf);
    write_dirent(parent_inum, leaf, inum);
    
    xclock_gettime(CLOCK_REALTIME, &dir->mtime);
    dirty_inodes.insert(dir);
    maybe_write();
    
    return 0;
}

int fs_readlink(const char *path, char *buf, size_t len)
{
    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_link *l = (fs_link*)inode_map[inum];
    if (l->type != OBJ_SYMLINK)
	return -EINVAL;

    size_t val = std::min(len, l->target.length());
    memcpy(buf, l->target.c_str(), val);
    return val;
}

/*
do_log_inode(
do_log_trunc(
do_log_delete(
do_log_symlink(
do_log_rename(
*/

#include <sys/statvfs.h>

/* once we're tracking objects I can iterate over them
 */
int fs_statfs(const char *path, struct statvfs *st)
{
    st->f_bsize = 4096;
    st->f_blocks = 0;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_namemax = 255;

    return 0;
}

int fs_fsync(const char * path, int, struct fuse_file_info *fi)
{
    return 0;
}

int fs_initialize(const char *prefix)
{
    init_stuff(prefix);
    
    for (int i = 0; ; i++) {
	ssize_t offset = get_offset(i);
	if (offset < 0)
	    return 0;
	void *buf = malloc(offset);
	if (pread(get_fd(i), buf, offset, 0) != (int)offset)
	    return -1;
	if (read_hdr(i, buf, offset) < 0)
	    return -1;
	free(buf);
	this_index = i+1;
    }

    return 0;
}

void *fs_init(struct fuse_conn_info *conn)
{
    return NULL;
}


void fs_teardown(void)
{
    for (auto it = inode_map.begin(); it != inode_map.end();
	 it = inode_map.erase(it)) ;
    this_index = 0;

    for (auto it = fd_cache.begin(); it != fd_cache.end(); it = fd_cache.erase(it)) {
	auto [key, val] = *it;
	close(val->fd);
    }

    while (!fd_fifo.empty())
	fd_fifo.pop();

    for (auto it = dirty_inodes.begin(); it != dirty_inodes.end();
	 it = dirty_inodes.erase(it));

    free(meta_log_head);
    free(data_log_head);

    for (auto it = data_offsets.begin(); it != data_offsets.end();
	 it = data_offsets.erase(it));

    for (auto it = path_map.begin(); it != path_map.end();
	 it = path_map.erase(it));
    while (!path_fifo.empty())
	path_fifo.pop();

    next_inode = 2;
}

int fs_mkfs(const char *prefix)
{
    if (this_index != 0)
	return -1;
    
    init_stuff(prefix);

    auto root = new fs_directory;
    root->inum = 1;
    root->uid = 0;
    root->gid = 0;
    root->mode = S_IFDIR | 0744;
    root->rdev = 0;
    root->size = 0;
    xclock_gettime(CLOCK_REALTIME, &root->mtime);

    write_inode(root);
    write_everything_out();

    return 0;
}

struct fuse_operations fs_ops = {
    .getattr = fs_getattr,
    .readlink = fs_readlink,
    .mkdir = fs_mkdir,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .rename = fs_rename,
    .chmod = fs_chmod,
    .truncate = fs_truncate,
    .statfs = fs_statfs,
    .fsync = fs_fsync,
    .init = fs_init,
    .create = fs_create,
    .utimens = fs_utimens,
};
