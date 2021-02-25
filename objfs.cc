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

//#define FUSE_USE_VERSION 27
//#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <sys/stat.h>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <sstream>
#include <errno.h>
//#include <fuse.h>

typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
                                const struct stat *stbuf, off_t off);

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

class fs_obj {
public:
    int8_t          type;
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    int64_t         size;
    struct timespec mtime;
};

class fs_file : public fs_obj {
public:
    extmap  extents;
};
  
class fs_directory : public fs_obj {
public:
    std::map<std::string,uint32_t> dirents;
};

class fs_symlink : public fs_obj {
public:
    std::string target;
};

/****************
 * file header format
 */

/* data update
*/
struct log_data {
    uint32_t inum;		// is 32 enough?
    uint32_t file_offset;	// in bytes
    uint32_t obj_offset;	// bytes from start of file data
    int64_t  size;		// file size after this write
    uint32_t len;		// bytes
};

/* inode update. Note that this is all that's needed for special
 * files. 
 */
struct log_inode {
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    struct timespec mtime;
};

/* truncate a file. maybe require truncate->0 before delete?
 */
struct log_trunc {
    uint32_t inum;
    int64_t  new_size;		// must be <= existing
};

struct log_delete {
    uint32_t parent;
    uint32_t inum;
    uint8_t  namelen;
    char     name[];
};

struct log_symlink {
    uint32_t inum;
    uint8_t  len;
    char     data[];
};

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
};

enum log_rec_type {
    LOG_INODE = 1,
    LOG_TRUNC,
    LOG_DELETE,
    LOG_SYMLNK,
    LOG_RENAME,
    LOG_NULL			// fill space for alignment
};

struct log_record {
    uint16_t type : 4;
    uint16_t len : 12;
    char data[];
};

#define OBJFS_MAGIC 0x4f424653	// "OBFS"

struct obj_header {
    int32_t magic;
    int32_t version;
    int32_t type;		// 1 == data, 2 == metadata
    int32_t hdr_len;
    char    data[];
};

/* until we add metadata objects this is enough global state
 */
std::unordered_map<std::string, fs_obj*> path_map;
std::unordered_map<uint32_t, fs_obj*>    inode_map;


/*
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    struct timespec mtime;
    int64_t         size;
 */
void update_inode(fs_obj *obj, log_inode *in)
{
    obj->inum = in->inum;
    obj->mode = in->mode;
    obj->uid = in->uid;
    obj->gid = in->gid;
    obj->rdev = in->rdev;
    obj->mtime = in->mtime;
}

int do_log_inode(log_inode *in)
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
	    fs_symlink *s = new fs_symlink;
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

int do_log_trunc(log_trunc *tr)
{
    auto it = inode_map.find(tr->inum);
    if (it == inode_map.end())
	return -1;

    fs_file *f = (fs_file*)(inode_map[tr->inum]);
    if (f->size < tr->new_size)
	return -1;
    
    while (true) {
	auto it = f->extents.lookup(tr->new_size);
	if (it == f->extents.end())
	    break;
	auto [offset, e] = *it;
	if (offset < tr->new_size) {
	    e.len = tr->new_size - offset;
	    f->extents.update(offset, e);
	}
	else {
	    f->extents.erase(offset);
	}
    }
    f->size = tr->new_size;
    return 0;
}

// assume directory has been emptied or file has been truncated.
//
int do_log_delete(log_delete *rm)
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
int do_log_symlink(log_symlink *sl)
{
    if (inode_map.find(sl->inum) == inode_map.end())
	return -1;

    fs_symlink *s = (fs_symlink *)(inode_map[sl->inum]);
    s->target = std::string(sl->data, sl->len);
    
    return 0;
}

// all inodes must exist
//
int do_log_rename(log_rename *mv)
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

// returns 0 on success, bytes to read if not enough data,
// -1 if bad format. Must pass at least 32B
//
size_t read_hdr(void *data, size_t len)
{
    obj_header *oh = (obj_header*)data;
    if (oh->hdr_len > len)
	return oh->hdr_len;

    if (oh->magic != OBJFS_MAGIC || oh->version != 1 || oh->type != 1)
	return -1;

    log_record *end = (log_record*)&oh->data[oh->hdr_len];
    log_record *rec = (log_record*)&oh->data[0];

    while (rec < end) {
	switch (rec->type) {
	case LOG_INODE:
	    if (do_log_inode((log_inode*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_TRUNC:
	    if (do_log_trunc((log_trunc*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_DELETE:
	    if (do_log_delete((log_delete*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_SYMLNK:
	    if (do_log_symlink((log_symlink*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_RENAME:
	    if (do_log_rename((log_rename*)&rec->data[0]) < 0)
		return -1;
	    break;
	case LOG_NULL:
	    break;
	default:
	    return -1;
	}
	rec = (log_record*)&rec->data[rec->len + 2];
    }
    return 0;
}


std::vector<std::string> split(const std::string& s, char delimiter)
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
int path_2_inum(const char *path)
{
    auto pathvec = split(path, '/');
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

void obj_2_stat(struct stat *sb, fs_obj *in)
{
    memset(sb, 0, sizeof(*sb));
    sb->st_mode = in->mode;
    sb->st_nlink = 1;
    sb->st_uid = in->uid;
    sb->st_gid = in->gid;
    sb->st_size = in->size;
    sb->st_blocks = (in->size + 4095) / 4096;
    sb->st_atimespec = sb->st_mtimespec =
	sb->st_ctimespec = in->mtime;
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

// need to learn how to use std::smart_ptr

int read_data(void *buf, int index, off_t offset, size_t len)
{
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
    for (auto it = f->extents.lookup(offset); len > 0 && it != f->extents.end(); it++) {
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
