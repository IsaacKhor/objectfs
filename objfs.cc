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
#include <errno.h>
#include <fuse.h>
#include <queue>
#include <memory>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <time.h>
#include <string.h>
#include <sstream>
#include <cassert>

#include <sys/uio.h>
#include <list>
#include <libs3.h>
#include "s3wrap.h"
#include "objfs.h"

//typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
//                                const struct stat *stbuf, off_t off);

/**********************************
 * Yet another extent map...
 */
struct extent {
    uint32_t objnum;
    uint32_t offset;		// within object (bytes)
    uint32_t len;		// (bytes)
};
    
typedef std::map<int64_t,extent> internal_map;

class extmap {
    internal_map the_map;

public:
    internal_map::iterator begin() { return the_map.begin(); }
    internal_map::iterator end() { return the_map.end(); }
    int size() { return the_map.size(); }
    
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
    size_t length(void) {return sizeof(fs_obj);}
    size_t serialize(std::ostream &s);
    fs_obj(void *ptr, size_t len);
    fs_obj(){}
};

fs_obj::fs_obj(void *ptr, size_t len)
{
    assert(len == sizeof(*this));
    *this = *(fs_obj*)ptr;
}

/* note that all the serialization routines are risky, because we're
 * using the object in-memory layout itself, so any change to the code
 * might change the on-disk layout.
 */
size_t fs_obj::serialize(std::ostream &s)
{
    fs_obj hdr = *this;
    size_t bytes = hdr.len = sizeof(hdr);
    s.write((char*)&hdr, sizeof(hdr));
    return bytes;
}

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

/* internal map uses the map key for the file offset
 * export version (_xp) has explicit file_offset
 */
struct extent_xp {
    int64_t  file_offset;
    uint32_t objnum;
    uint32_t obj_offset;
    uint32_t len;
};
    
class fs_file : public fs_obj {
public:
    extmap  extents;
    size_t length(void);
    size_t serialize(std::ostream &s);
    fs_file(void *ptr, size_t len);
    fs_file(){}
};

// de-serialize from serialized form
//
fs_file::fs_file(void *ptr, size_t len)
{
    assert(len >= sizeof(fs_obj));
    *(fs_obj*)this = *(fs_obj*)ptr;
    len -= sizeof(fs_obj);
    extent_xp *ex = (extent_xp*)(sizeof(fs_obj) + (char*)ptr);

    while (len > 0) {
	extent e = {.objnum = ex->objnum,
		    .offset = ex->obj_offset, .len = ex->len};
	extents.update(ex->file_offset, e);
	ex++;
	len -= sizeof(*ex);
    }
    assert(len == 0);
}

// length of serialization in bytes
//
size_t fs_file::length(void)
{
    return sizeof(*this) + extents.size() * sizeof(extent_xp);
}

size_t fs_file::serialize(std::ostream &s)
{
    fs_obj hdr = *this;
    size_t bytes = hdr.len = length();
    s.write((char*)&hdr, sizeof(hdr));

    // TODO - merge adjacent extents (not sure it ever happens...)
    for (auto it = extents.begin(); it != extents.end(); it++) {
	auto [file_offset, ext] = *it;
	extent_xp _e = {.file_offset = file_offset, .objnum = ext.objnum,
			.obj_offset = ext.offset, .len = ext.len};
	s.write((char*)&_e, sizeof(_e));
    }
    return bytes;
}

typedef std::pair<uint32_t,uint32_t> offset_len;
    
/* directory entry serializes as:
 *  - uint32 inode #
 *  - uint32 byte offset [in metadata checkpoint]
 *  - uint32 byte length
 *  - uint8  namelen
 *  - char   name[]
 *
 * we rely on a depth-first traversal of the tree so that we know the
 * location (in the checkpoint) of the object pointed to by each
 * directory entry before we serialize that entry.
 */
struct dirent_xp {
    uint32_t inum;
    uint32_t offset;
    uint32_t len;
    uint8_t  namelen;
    char     name[0];
} __attribute__((packed,aligned(1)));

class fs_directory : public fs_obj {
public:
    std::map<std::string,uint32_t> dirents;
    size_t length(void);
    size_t serialize(std::ostream &s, std::map<uint32_t,offset_len> &m);
    fs_directory(void *ptr, size_t len);
    fs_directory(){};
};

// de-serialize a directory from a checkpoint
//
fs_directory::fs_directory(void *ptr, size_t len)
{
    assert(len >= sizeof(fs_obj));
    *(fs_obj*)this = *(fs_obj*)ptr;
    len -= sizeof(fs_obj);
    dirent_xp *de = (dirent_xp*)(sizeof(fs_obj) + (char*)ptr);

    while (len > 0) {
	std::string name(de->name, de->namelen);
	dirents[name] = de->inum;
	// TODO - do something with offset/len
	len -= (sizeof(*de) + de->namelen);
    }
    assert(len == 0);
}

size_t fs_directory::length(void)
{
    size_t bytes = sizeof(fs_obj);
    for (auto it = dirents.begin(); it != dirents.end(); it++) {
	auto [name,inum] = *it;
	bytes += (sizeof(dirent_xp) + name.length());
    }
    return bytes;
}

size_t fs_directory::serialize(std::ostream &s,
			     std::map<uint32_t,offset_len> &map)
{
    fs_obj hdr = *this;
    size_t bytes = hdr.len = length();
    s.write((char*)&hdr, sizeof(hdr));
    
    for (auto it = dirents.begin(); it != dirents.end(); it++) {
	auto [name, inum] = *it;
	auto[offset,len] = map[inum];
	uint8_t namelen = name.length();
	dirent_xp de = {.inum = inum, .offset = offset,
			.len = len, .namelen = namelen};
	s.write((char*)&de, sizeof(de));
	s.write(name.c_str(), namelen);
    }
    return bytes;
}

/* extra space in entry is just the target
 */
class fs_link : public fs_obj {
public:
    std::string target;
    size_t length(void);
    size_t serialize(std::ostream &s);
    fs_link(void *ptr, size_t len);
    fs_link(){}
};

// deserialize a symbolic link.
//
fs_link::fs_link(void *ptr, size_t len)
{
    assert(len >= sizeof(fs_obj));
    *(fs_obj*)this = *(fs_obj*)ptr;
    len -= sizeof(fs_obj);
    std::string _target((char*)ptr, len);
    target = _target;
}

// serialized length in bytes
//
size_t fs_link::length(void)
{
    return sizeof(fs_obj) + target.length();
}

// serialize to an ostream
//
size_t fs_link::serialize(std::ostream &s)
{
    fs_obj hdr = *this;
    size_t bytes = hdr.len = length();
    s.write((char*)&hdr, sizeof(hdr));
    s.write(target.c_str(), target.length());
    return bytes;
}


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


// returns new offset
size_t serialize_tree(std::ostream &s, size_t offset, uint32_t inum,
		      std::map<uint32_t,offset_len> &map)
{
    fs_obj *obj = inode_map[inum];
    
    if (obj->type != OBJ_DIR) {
	size_t len = obj->serialize(s);
	map[inum] = std::make_pair(offset, len);
	return offset + len;
    }
    else {
	fs_directory *dir = (fs_directory*)obj;
	for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
	    auto [name,inum2] = *it;
	    offset = serialize_tree(s, offset, inum2, map);
	}
	size_t len = dir->serialize(s, map);
	return offset + len;
    }
}

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
    extent e = {.objnum = (uint32_t)idx, .offset = d->obj_offset,
		.len = d->len};
    f->extents.update(d->file_offset, e);
    f->size = d->size;

    return 0;
}

int next_inode = 2;

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

// more serialization
struct itable_xp {
    uint32_t inum;
    uint32_t objnum;
    uint32_t offset;
    uint32_t len;
};

size_t serialize_itable(std::ostream &s,
			std::map<uint32_t,offset_len> &map)
{
    size_t bytes = 0;
    for (auto it = inode_map.begin(); it != inode_map.end(); it++) {
	auto [inum, obj] = *it;
	auto [offset, len] = map[inum];
	itable_xp entry = {.inum = inum, .objnum = (uint32_t)this_index,
			   .offset = offset, .len = len};
	s.write((char*)&entry, sizeof(entry));
	bytes += sizeof(entry);
    }
    return bytes;
}

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

void serialize_all(void)
{
    int root_inum = 1;
    ckpt_header h = {.root_inum = (uint32_t)root_inum,
		     .next_inum = (uint32_t)next_inode};
    std::stringstream objs;
    std::stringstream itable;
    std::map<uint32_t,offset_len> imap;
    size_t objs_offset = sizeof(h);
    
    size_t itable_offset = serialize_tree(objs, objs_offset, root_inum, imap);

    auto [_off,_len] = imap[root_inum];
    h.root_offset = _off;
    h.root_len = _len;
    h.itable_offset = itable_offset;

    size_t itable_len = serialize_itable(itable, imap);

    // checkpoint is now in three parts:
    // &h, sizeof(h)
    // objs.str().c_str(), itable_offset-objs_offset
    // itable.str().c_str(), itable_len
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

void write_everything_out(struct objfs *fs)
{
    for (auto it = dirty_inodes.begin(); it != dirty_inodes.end();
	 it = dirty_inodes.erase(it)) {
	write_inode(*it);
    }

    char _key[1024];
    sprintf(_key, "%s.%08x", fs->prefix, this_index);
    std::string key(_key);
    
    obj_header h = {
	.magic = OBJFS_MAGIC,
	.version = 1,
	.type = 1,
	.hdr_len = (int)(meta_offset() + sizeof(obj_header)),
	.this_index = this_index,
    };
    this_index++;

    struct iovec iov[3] = {{.iov_base = (void*)&h, .iov_len = sizeof(h)},
			   {.iov_base = meta_log_head, .iov_len = meta_offset()},
			   {.iov_base = data_log_head, .iov_len = data_offset()}};
    
    printf("writing %s:\n", key.c_str());
    printout((void*)&h, sizeof(h));
    printout((void*)meta_log_head, meta_offset());

    if (S3StatusOK != fs->s3->s3_put(key, iov, 3))
	throw "put failed";
    
    meta_log_tail = meta_log_head;
    data_log_tail = data_log_head;
}

void fs_sync(void)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
    write_everything_out(fs);
}

void maybe_write(struct objfs *fs)
{
    if ((meta_offset() > meta_log_len) ||
	(data_offset() > data_log_len))
	write_everything_out(fs);
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

// read at absolute offset @offset in object @index
//
int do_read(struct objfs *fs, int index, void *buf, size_t len, size_t offset, bool ckpt)
{
    char key[256];
    sprintf(key, "%s.%08x%s", fs->prefix, index, ckpt ? ".ck" : "");
    struct iovec iov = {.iov_base = buf, .iov_len = (size_t)len};
    if (S3StatusOK != fs->s3->s3_get(key, offset, len, &iov, 1))
	return -1;
    return len;
}

fs_obj *load_obj(struct objfs *fs, int index, uint32_t offset, size_t len)
{
    char buf[len];
    size_t val = do_read(fs, index, (void*)buf, len, offset, true);
    if (val != len)
	return nullptr;
    fs_obj *o = (fs_obj*)buf;
    if (o->type == OBJ_DIR)
	return new fs_directory((void*)buf, len);
    if (o->type == OBJ_FILE)
	return new fs_file((void*)buf, len);
    if (o->type == OBJ_SYMLINK)
	return new fs_link((void*)buf, len);
    return new fs_obj((void*)buf, len);
}


// actual offset of data in file is the offset in the extent entry
// plus the header length. Get header length for object @index
int get_offset(struct objfs *fs, int index, bool ckpt)
{
    if (data_offsets.find(index) != data_offsets.end())
	return data_offsets[index];

    obj_header h;
    ssize_t len = do_read(fs, index, &h, sizeof(h), 0, ckpt);
    if (len < 0)
	return -1;

    data_offsets[index] = h.hdr_len;
    return h.hdr_len;
}

// read @len bytes of file data from object @index starting at
// data offset @offset (need to adjust for header length)
//
int read_data(struct objfs *fs, void *buf, int index, off_t offset, size_t len)
{
    if (index == this_index) {
	len = std::min(len, data_offset() - offset);
	memcpy(buf, offset + (char*)data_log_head, len);
	return len;
    }
    size_t n = get_offset(fs, index, false);
    if (n < 0)
	return n;
    return do_read(fs, index, buf, len, offset + n, false);
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

// TODO - how to read objects on demand? Two possibilities:
// - in-memory directories keep offset/len information in leaves
// - load separate in-memory table of inode->offset/len
// first approach may not work so well when we write the next
// checkpoint 

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

// TODO - cache path to inum translations?

static int path_2_inum(const char *path)
{
    auto pathvec = split(path, '/');
    int inum = vec_2_inum(pathvec);
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
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
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
    extent e = {.objnum = (uint32_t)this_index,
		.offset = (uint32_t)obj_offset, .len = (uint32_t)len};
    f->extents.update(offset, e);
    dirty_inodes.insert(f);
    maybe_write(fs);
    
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
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
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
    clock_gettime(CLOCK_REALTIME, &dir->mtime);

    struct fuse_context *ctx = fuse_get_context();
    dir->uid = ctx->uid;
    dir->gid = ctx->gid;
    
    inode_map[inum] = dir;
    parent->dirents[leaf] = inum;
    clock_gettime(CLOCK_REALTIME, &parent->mtime);
    dirty_inodes.insert(parent);
    
    write_inode(dir);	// can't rely on dirty_inodes
    write_dirent(parent_inum, leaf, inum);
    maybe_write(fs);

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
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
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
    
    clock_gettime(CLOCK_REALTIME, &parent->mtime);
    dirty_inodes.insert(parent);
    do_log_delete(parent_inum, inum, leaf);
    maybe_write(fs);
    
    return 0;
}

int create_node(struct objfs *fs, const char *path, mode_t mode, int type, dev_t dev)
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
    clock_gettime(CLOCK_REALTIME, &f->mtime);

    struct fuse_context *ctx = fuse_get_context();
    f->uid = ctx->uid;
    f->gid = ctx->gid;
    
    inode_map[inum] = f;
    dir->dirents[leaf] = inum;

    write_inode(f);	// can't rely on dirty_inodes
    write_dirent(parent_inum, leaf, inum);
    
    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    dirty_inodes.insert(dir);
    maybe_write(fs);
    
    return 0;
}

// only called for regular files
int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    return create_node(fs, path, mode | S_IFREG, OBJ_FILE, 0);
}

// for device files, FIFOs, etc.
int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    return create_node(fs, path, mode, OBJ_OTHER, dev);
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
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

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

    clock_gettime(CLOCK_REALTIME, &f->mtime);
    dirty_inodes.insert(f);
    maybe_write(fs);
    
    return 0;
}

/* do I need a parent inum in fs_obj?
 */
int fs_unlink(const char *path)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum < 0)
	return inum;
    fs_obj *obj = inode_map[inum];
    if (obj->type == OBJ_DIR)
	return -EISDIR;
    
    fs_directory *dir = (fs_directory*)inode_map[parent_inum];

    dir->dirents.erase(leaf);
    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    dirty_inodes.insert(dir);

    if (obj->type == OBJ_FILE) {
	fs_file *f = (fs_file*)obj;
	do_trunc(f, 0);
	do_log_trunc(inum, 0);
    }
    do_log_delete(parent_inum, inum, leaf);
    maybe_write(fs);
    
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
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

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
    clock_gettime(CLOCK_REALTIME, &srcdir->mtime);
    dirty_inodes.insert(srcdir);

    dstdir->dirents[dst_leaf] = src_inum;
    clock_gettime(CLOCK_REALTIME, &dstdir->mtime);
    dirty_inodes.insert(dstdir);
    
    do_log_rename(src_inum, src_parent, dst_parent, src_leaf, dst_leaf);
    maybe_write(fs);
    
    return 0;
}

int fs_chmod(const char *path, mode_t mode)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    obj->mode = mode | (S_IFMT & obj->mode);
    dirty_inodes.insert(obj);
    maybe_write(fs);
    
    return 0;
}

// see utimensat(2). Oh, and I hate access time...
//
int fs_utimens(const char *path, const struct timespec tv[2])
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    int inum = path_2_inum(path);
    if (inum < 0)
	return inum;

    fs_obj *obj = inode_map[inum];
    if (tv == NULL || tv[1].tv_nsec == UTIME_NOW)
	clock_gettime(CLOCK_REALTIME, &obj->mtime);
    else if (tv[1].tv_nsec != UTIME_OMIT)
	obj->mtime = tv[1];
    dirty_inodes.insert(obj);
    maybe_write(fs);
    
    return 0;
}


int fs_read(const char *path, char *buf, size_t len, off_t offset,
	    struct fuse_file_info *fi)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

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
	    if (read_data(fs, buf, e.objnum, e.offset+skip, _len) < 0)
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
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

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

    clock_gettime(CLOCK_REALTIME, &l->mtime);

    l->target = leaf;
    inode_map[inum] = l;
    dir->dirents[leaf] = l->inum;

    write_inode(l);
    write_symlink(inum, leaf);
    write_dirent(parent_inum, leaf, inum);
    
    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    dirty_inodes.insert(dir);
    maybe_write(fs);
    
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
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
    write_everything_out(fs);
    return 0;
}

void *fs_init(struct fuse_conn_info *conn)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    // initialization - FIXME
    meta_log_len = 64 * 1024;
    meta_log_head = meta_log_tail = malloc(meta_log_len*2);
    data_log_len = 8 * 1024 * 1024;
    data_log_head = data_log_tail = malloc(data_log_len);

    fs->s3 = new s3_target(fs->host, fs->bucket, fs->access, fs->secret, false);

    std::list<std::string> keys;
    if (S3StatusOK != fs->s3->s3_list(fs->prefix, keys))
	throw "bucket list failed";

    for (auto it = keys.begin(); it != keys.end(); it++) {
	int n;
	printf("key: %s\n", it->c_str());
	sscanf(it->c_str(), "%*[^.].%d", &n);
	ssize_t offset = get_offset(fs, n, false);

	if (offset < 0)
	    throw "bad object";
	void *buf = malloc(offset);
	struct iovec iov[] = {{.iov_base = buf, .iov_len = (size_t)offset}};
	if (S3StatusOK != fs->s3->s3_get(it->c_str(), 0, offset, iov, 1))
	    throw "can't read header";
	if (read_hdr(n, buf, offset) < 0)
	    throw "bad header";
	this_index = n+1;
    }

    return (void*) fs;
}

void fs_teardown(void)
{
    for (auto it = inode_map.begin(); it != inode_map.end();
	 it = inode_map.erase(it)) ;
    this_index = 0;

    for (auto it = dirty_inodes.begin(); it != dirty_inodes.end();
	 it = dirty_inodes.erase(it));

    free(meta_log_head);
    free(data_log_head);

    for (auto it = data_offsets.begin(); it != data_offsets.end();
	 it = data_offsets.erase(it));

    next_inode = 2;
}

#if 0
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
    clock_gettime(CLOCK_REALTIME, &root->mtime);

    write_inode(root);
    write_everything_out(fs);

    return 0;
}
#endif

/*
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
};*/

struct fuse_operations fs_ops = {
    .getattr = fs_getattr,
    .readlink = fs_readlink,
    .mkdir = fs_mkdir,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .symlink = fs_symlink,
    .rename = fs_rename,
    .chmod = fs_chmod,
    .truncate = fs_truncate,
    .read = fs_read,
    .write = fs_write,
    .statfs = fs_statfs,
    .fsync = fs_fsync,
    .readdir = fs_readdir,
    .init = fs_init,
    .create = fs_create,
    .utimens = fs_utimens,
};

