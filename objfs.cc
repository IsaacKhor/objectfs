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
#include <atomic>
#include <future>
#include <shared_mutex>
#include <condition_variable>

#include <sys/uio.h>
#include <list>
#include <libs3.h>
#include "s3wrap.h"
#include "objfs.h"

#include <mutex>
#include <thread>
#include <chrono>
#include <fstream>
#include <queue>
#include <ctime>

std::shared_mutex inode_mutex;
std::mutex log_mutex;
std::shared_mutex old_log_mutex;
std::shared_mutex ckpting_mutex;  // checkpoint() holds this mutex so that inode map and fs_obj's cannot be modified
std::mutex ckpt_put_mutex; // checkpoint() holds this mutex and wait for write_everything_out to confirm successful put to backend
std::mutex sync_mutex;
std::mutex fifo_mutex;
std::condition_variable cv;
std::condition_variable ckpt_cv;
std::condition_variable sync_cv; // Don't sync before all regular write_everything_out is done
std::condition_variable fifo_cv;
std::mutex cv_m;
std::mutex logger_mutex;
std::shared_mutex data_offsets_mutex;
bool quit_fifo;
bool quit_ckpt;
bool put_before_ckpt; // In checkpoint(), check if write_everything_out completed before serialize_all
std::atomic<int> writing_log_count (0); // Counts how many write_everything_out() for regular logs is executing

std::atomic<int> fifo_size (0);
std::atomic<int> next_s3_index (1);
std::atomic<int> next_inode (3); // the root "" has inum 2
std::atomic<int> ckpt_index (-1); // record the latest checkpoint object index
//std::atomic<int> log_record_index (0); 
std::mutex next_inode_mutex;
std::map<int, void *> meta_log_buffer; // Before a log object is commited to back end, it sits here for read requests
std::map<int, void *> data_log_buffer; // keys are object indices, values are objects
std::map<int, size_t> meta_log_sizes; 
std::map<int, size_t> data_log_sizes;
std::map<int, void *> written_inodes;
std::queue<int> fifo_queue;
std::map<int, std::map<int, time_t>> access_times;  //the first key is objnum, the second key is obj offset, value is last read time
std::set<std::tuple<double, int, int>> util_rates;

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
    //uint32_t        len : 28;	// of serialized metadata
    size_t          len : 28;
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    int64_t         size;
    struct timespec mtime;
    size_t length(void) {return sizeof(fs_obj);}
    size_t serialize(std::stringstream &s);
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
size_t fs_obj::serialize(std::stringstream &s)
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
    std::shared_mutex   mtx;  // Read-Write Lock
public:
    extmap  extents;
    size_t length(void);
    size_t serialize(std::stringstream &s);
    fs_file(void *ptr, size_t len);
    fs_file(){};
    void write_lock(){
        mtx.lock();
    }
    void write_unlock(){
        mtx.unlock();
    }
    void read_lock(){
        mtx.lock_shared();
    }
    void read_unlock(){
        mtx.unlock_shared();
    }
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
        ex = (extent_xp *)((char *)ex + sizeof(extent_xp)); //ex++;
        len -= sizeof(extent_xp);
    }
    assert(len == 0);
}

// length of serialization in bytes
//
size_t fs_file::length(void)
{
    size_t len = sizeof(fs_obj) + extents.size() * sizeof(extent_xp);
    return len;
}

size_t fs_file::serialize(std::stringstream &s)
{
    read_lock();
    fs_obj hdr = *this;
    size_t bytes = hdr.len = length();
    s.write((char*)&hdr, sizeof(fs_obj));

    // TODO - merge adjacent extents (not sure it ever happens...)
    for (auto it = extents.begin(); it != extents.end(); it++) {
        auto [file_offset, ext] = *it;
        extent_xp _e = {.file_offset = file_offset, .objnum = ext.objnum,
                .obj_offset = ext.offset, .len = ext.len};
        s.write((char*)&_e, sizeof(extent_xp));
    }
    read_unlock();

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
    char     name[];
} __attribute__((packed,aligned(1)));

class fs_directory : public fs_obj {
    std::shared_mutex   mtx;  // Read-Write Lock
public:
    std::map<std::string,uint32_t> dirents;
    size_t length(void);
    size_t serialize(std::stringstream &s, std::map<uint32_t,offset_len> &m);
    fs_directory(void *ptr, size_t len);
    fs_directory(){};
    void write_lock(){
        mtx.lock();
    };
    void write_unlock(){
        mtx.unlock();
    };
    void read_lock(){
        mtx.lock_shared();
    };
    void read_unlock(){
        mtx.unlock_shared();
    };
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
        de = (dirent_xp*) (sizeof(*de) + de->namelen + (char *)de);
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

size_t fs_directory::serialize(std::stringstream &s,
			     std::map<uint32_t,offset_len> &map)
{
    fs_obj hdr = *this;
    read_lock();
    size_t bytes = hdr.len = length();
    s.write((char*)&hdr, sizeof(hdr));
    
    for (auto it = dirents.begin(); it != dirents.end(); it++) {
        auto [name, inum] = *it;
        auto[offset,len] = map[inum];
        uint8_t namelen = name.length();
        dirent_xp de = {.inum = inum, .offset = offset,
                .len = len, .namelen = namelen};
        s.write((char*)&de, sizeof(dirent_xp));
        s.write(name.c_str(), namelen);
    }
    read_unlock();
    return bytes;
}

/* extra space in entry is just the target
 */
class fs_link : public fs_obj {
public:
    std::string target;
    size_t length(void);
    size_t serialize(std::stringstream &s);
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
size_t fs_link::serialize(std::stringstream &s)
{
    fs_obj hdr = *this;
    size_t bytes = hdr.len = length();
    s.write((char*)&hdr, sizeof(hdr));
    s.write(target.c_str(), target.length());
    return bytes;
}

class Logger {
    std::string log_path;
    std::fstream file_stream;

public:
    Logger(std::string path);
    ~Logger();
    void log(std::string info);
};


Logger::Logger(std::string path){
    file_stream.open(path);
}

Logger::~Logger(void){
    file_stream.close();
}

void Logger::log(std::string info) {
    const std::unique_lock<std::mutex> lock(logger_mutex);
    file_stream << info;
}

Logger* logger = new Logger("/mnt/ramdisk/log_ckpt.txt");

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
    time_t access_time;  // timestamp for last access
    bool valid;
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
    int32_t index;
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
    int32_t ckpt_index; // the object index of the latest checkpoint
    int32_t data_len; // length of data log
    char    data[];
};


/* until we add metadata objects this is enough global state
 */
std::unordered_map<uint32_t, fs_obj*>    inode_map;


// returns new offset
size_t serialize_tree(std::stringstream &s, size_t offset, uint32_t inum,
		      std::map<uint32_t,offset_len> &map)
{
    fs_obj *obj = inode_map[inum];
    
    if (obj->type != OBJ_DIR) { 
        size_t len;
        if (obj->type == OBJ_FILE) {
            fs_file *file = (fs_file *)obj;
            len = file->serialize(s);
            map[inum] = std::make_pair(offset, len);
        } else if (obj->type == OBJ_SYMLINK) {
            fs_link *link = (fs_link *)obj;
            len = link->serialize(s);
            map[inum] = std::make_pair(offset, len);
        } else {
            len = obj->serialize(s);
            map[inum] = std::make_pair(offset, len);
        }
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

static void set_inode_map(int inum, fs_obj *ptr){
    const std::unique_lock<std::shared_mutex> lock(inode_mutex);
    if (ptr->type == OBJ_DIR){
        fs_directory *d = (fs_directory *)ptr;
        inode_map[inum] = d;
    }
    else if (ptr->type == OBJ_FILE){
        fs_file *f = (fs_file *)ptr;
        inode_map[inum] = f;
    }
    else if (ptr->type == OBJ_SYMLINK){
        fs_link *s = (fs_link *)ptr;
        inode_map[inum] = s;
    } 
    else {
        inode_map[inum] = ptr;
    }
}

static int read_log_inode(log_inode *in)
{
    inode_mutex.lock_shared();
    auto it = inode_map.find(in->inum);
    if (it != inode_map.end()) {
        auto obj = inode_map[in->inum];
        inode_mutex.unlock_shared();
        update_inode(obj, in);
    }
    else {
        if (S_ISDIR(in->mode)) {
            fs_directory *d = new fs_directory;
            d->type = OBJ_DIR;
            d->size = 0;
            inode_mutex.unlock_shared();
            set_inode_map(in->inum, d);
            update_inode(d, in);
        }
        else if (S_ISREG(in->mode)) {
            fs_file *f = new fs_file;
            f->type = OBJ_FILE;
            f->size = 0;
            inode_mutex.unlock_shared();
            update_inode(f, in);
            set_inode_map(in->inum, f);
        }
        else if (S_ISLNK(in->mode)) {
            fs_link *s = new fs_link;
            s->type = OBJ_SYMLINK;
            s->size = 0;
            inode_mutex.unlock_shared();
            update_inode(s, in);
            set_inode_map(in->inum, s);
        }
        else {
            fs_obj *o = new fs_obj;
            o->type = OBJ_OTHER;
            o->size = 0;
            inode_mutex.unlock_shared();
            update_inode(o, in);
            set_inode_map(in->inum, o);
        }
    }
    return 0;
}

void do_trunc(fs_file *f, off_t new_size)
{
    //const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    f->write_lock();
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
    f->write_unlock();
}
    
int read_log_trunc(log_trunc *tr)
{
    inode_mutex.lock_shared();
    auto it = inode_map.find(tr->inum);
    if (it == inode_map.end()){
        inode_mutex.unlock_shared();
	    return -1;
    }

    fs_file *f = (fs_file*)(inode_map[tr->inum]);
    inode_mutex.unlock_shared();
    f->read_lock();
    if (f->size < tr->new_size){
        f->read_unlock();
	    return -1;
    }
    f->read_unlock();
    do_trunc(f, tr->new_size);
    return 0;
}

// assume directory has been emptied or file has been truncated.
//
static int read_log_delete(log_delete *rm)
{
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    const std::unique_lock<std::shared_mutex> lock(inode_mutex);
    if (inode_map.find(rm->parent) == inode_map.end())
	    return -1;
    if (inode_map.find(rm->inum) == inode_map.end())
	    return -1;

    auto name = std::string(rm->name, rm->namelen);
    fs_obj *f = inode_map[rm->inum];
    inode_map.erase(rm->inum);
    fs_directory *parent = (fs_directory*)(inode_map[rm->parent]);
    parent->write_lock();
    parent->dirents.erase(name);
    parent->write_unlock();
    delete f;

    return 0;
}

// assume the inode has already been created
//
static int read_log_symlink(log_symlink *sl)
{
    const std::shared_lock<std::shared_mutex> lock(inode_mutex);
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
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    const std::shared_lock<std::shared_mutex> lock(inode_mutex);
    if (inode_map.find(mv->parent1) == inode_map.end())
	    return -1;
    if (inode_map.find(mv->parent2) == inode_map.end())
	    return -1;
    
    fs_directory *parent1 = (fs_directory*)(inode_map[mv->parent1]);
    fs_directory *parent2 = (fs_directory*)(inode_map[mv->parent2]);

    auto name1 = std::string(&mv->name[0], mv->name1_len);
    auto name2 = std::string(&mv->name[mv->name1_len], mv->name2_len);

    parent1->read_lock();
    if (parent1->dirents.find(name1) == parent1->dirents.end()){
        parent1->read_unlock();
	    return -1;
    }
    if (parent1->dirents[name1] != mv->inum){
        parent1->read_unlock();
	    return -1;
    }
    parent1->read_unlock();
    parent2->read_lock();
    if (parent2->dirents.find(name2) != parent2->dirents.end()){
        parent2->read_unlock();
	    return -1;
    }
    parent2->read_unlock();
	
    parent1->write_lock();
    parent1->dirents.erase(name1);
    parent1->write_unlock();
    parent2->write_lock();
    parent2->dirents[name2] = mv->inum;
    parent2->write_unlock();
    
    return 0;
}

int read_log_data(int idx, log_data *d)
{
    const std::shared_lock<std::shared_mutex> lock(inode_mutex);
    auto it = inode_map.find(d->inum);
    if (it == inode_map.end())
	    return -1;

    fs_file *f = (fs_file*) inode_map[d->inum];
    
    // optimization - check if it extends the previous record?
    extent e = {.objnum = (uint32_t)idx, .offset = d->obj_offset,
		.len = d->len};
    f->write_lock();
    f->extents.update(d->file_offset, e);
    f->size = d->size;
    f->write_unlock();

    return 0;
}


int read_log_create(log_create *c)
{
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    const std::shared_lock<std::shared_mutex> lock(inode_mutex);
    auto it = inode_map.find(c->parent_inum);
    if (it == inode_map.end())
	    return -1;

    fs_directory *d = (fs_directory*) inode_map[c->parent_inum];
    auto name = std::string(&c->name[0], c->namelen);
    d->write_lock();
    d->dirents[name] = c->inum;
    d->write_unlock();

    next_inode_mutex.lock();
    next_inode = std::max(next_inode.load(), (int)(c->inum + 1));
    next_inode_mutex.unlock();
    
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

// more serialization
struct itable_xp {
    uint32_t inum;
    uint32_t objnum;
    uint32_t offset;
    uint32_t len;
};

size_t serialize_itable(std::stringstream &s,
			std::map<uint32_t,offset_len> &map)
{
    const std::shared_lock<std::shared_mutex> lock(inode_mutex);
    size_t bytes = 0;
    for (auto it = inode_map.begin(); it != inode_map.end(); it++) {
        auto [inum, obj] = *it;
        auto [offset, len] = map[inum];
        itable_xp entry = {.inum = inum, .objnum = (uint32_t)next_s3_index,
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
    uint32_t itable_len;
    uint32_t ckpted_s3_index;
    char     data[];
};


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

void write_inode(fs_obj *f);
void gc(struct objfs *, int);
void gc_read(struct objfs *, char *, size_t, int);
void gc_write(struct objfs *, const char *, size_t, int);
void gc_read_write(struct objfs *, int);
int get_total_file_size(int);

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

int serialize_all(int ckpted_s3_index, ckpt_header *h, std::stringstream &objs)
{
    int root_inum = 2;

    h->root_inum = (uint32_t)root_inum;
    h->next_inum = (uint32_t)next_inode;

    std::map<uint32_t,offset_len> imap;
    size_t objs_offset = sizeof(ckpt_header);
    
    h->ckpted_s3_index = ckpted_s3_index;
    size_t itable_offset = serialize_tree(objs, objs_offset, root_inum, imap);

    auto [_off,_len] = imap[root_inum];
    h->root_offset = _off;
    h->root_len = _len;
    h->itable_offset = itable_offset;

    return itable_offset-objs_offset;

    // checkpoint is now in two parts:
    // &h, sizeof(h)
    // objs.str().c_str(), itable_offset-objs_offset
}

void deserialize_tree(void *buf, size_t len){
    void *end = (void*)buf + len;
    // TODO: put in asserts in fs_directory::fs_directory, check that file No and dir No point to valid inodes
    while (buf < end) {
        fs_obj *cur_obj = (fs_obj *)buf;
        if (cur_obj->type == OBJ_DIR) {
	        fs_directory *d = new fs_directory((void*)buf, cur_obj->len);
            buf = (void*)buf + cur_obj->len;
            set_inode_map(cur_obj->inum, d);
        }
        else if (cur_obj->type == OBJ_FILE) {
            fs_file *f = new fs_file((void*)buf, cur_obj->len);
            buf = (void*)buf + cur_obj->len;
            set_inode_map(cur_obj->inum, f);
        }
        else if (cur_obj->type == OBJ_SYMLINK) {
            fs_link *s = new fs_link((void*)buf, cur_obj->len);
            buf = (void*)buf + cur_obj->len;
            set_inode_map(cur_obj->inum, s);
        } else {
            fs_obj *o = new fs_obj((void*)buf, cur_obj->len);
            buf = (void*)buf + cur_obj->len;
            set_inode_map(cur_obj->inum, o);
        }
    }
}

// write_everything_out allocates a new metadata log and a new data log.
// It puts old logs in the buffer so that read threads can get them before they are committed to 
// S3 backend. After s3_put returns success, old logs are removed from buffer
void write_everything_out(struct objfs *fs)
{
    int index = next_s3_index;
    size_t meta_log_size = meta_offset();
    void *meta_log_head_old = meta_log_head;
    meta_log_head = meta_log_tail = malloc(meta_log_len*40);

    size_t data_log_size = data_offset();
    void *data_log_head_old = data_log_head;
    data_log_head = data_log_tail = malloc(data_log_len*40);

    meta_log_buffer[index] = meta_log_head_old;
    data_log_buffer[index] = data_log_head_old;
    meta_log_sizes[index] = meta_log_size;
    data_log_sizes[index] = data_log_size;

    char _key[1024];
    sprintf(_key, "%s.%08x", fs->prefix, next_s3_index.load());
    std::string key(_key);
    
    obj_header h = {
        .magic = OBJFS_MAGIC,
        .version = 1,
        .type = 1,
        .hdr_len = (int)(meta_log_size + sizeof(obj_header)),
        .this_index = next_s3_index,
        .ckpt_index = ckpt_index,
        .data_len = data_log_size,
    };
    next_s3_index++;
    for (auto it = written_inodes.begin(); it != written_inodes.end();
	 it = written_inodes.erase(it));
    
    log_mutex.unlock();

    struct iovec iov[3] = {{.iov_base = (void*)&h, .iov_len = sizeof(h)},
			   {.iov_base = meta_log_head_old, .iov_len = meta_log_size},
			   {.iov_base = data_log_head_old, .iov_len = data_log_size}};
    
    printf("writing %s, meta log size: %d, data log size: %d\n", key.c_str(), (int)meta_log_size, (int)data_log_size);
    printout((void*)&h, sizeof(h));
    printout((void*)meta_log_head_old, meta_log_size); 
    
    std::future<S3Status> status = std::async(std::launch::deferred, &s3_target::s3_put, fs->s3, key, iov, 3);
    
    if (S3StatusOK != status.get()){
        printf("PUT FAILED\n");
        throw "put failed";
    }

    {
        std::unique_lock<std::mutex> ckpt_lk(ckpt_put_mutex);
        put_before_ckpt = true;
    }
    ckpt_cv.notify_all();

    // TODO: spawn separate cleaner thread and keep old logs in buffer, like a cache
    /*old_log_mutex.lock();
    free(meta_log_buffer[index]);
    free(data_log_buffer[index]);
    meta_log_buffer.erase(index);
    data_log_buffer.erase(index);
    meta_log_sizes.erase(index);
    data_log_sizes.erase(index);
    old_log_mutex.unlock();*/
    fifo_queue.push(index);
    fifo_size++;
    fifo_cv.notify_all();

    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
    }
    sync_cv.notify_all();
}

void maybe_write(struct objfs *fs)
{
    log_mutex.lock();
    if ((meta_offset() > meta_log_len) || (data_offset() > data_log_len)) {    
        // TODO: maybe a thread pool?
        {
            std::unique_lock<std::mutex> sync_lk(sync_mutex);
            writing_log_count++;
        }
        std::thread (write_everything_out, fs).detach();
    }
    else {
        log_mutex.unlock();
    }
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
    std::future<S3Status> status = std::async(std::launch::deferred, &s3_target::s3_get, fs->s3, key, offset, len, &iov, 1);
    if (S3StatusOK != status.get()){
        return -1;
    }
    return len;
}



// actual offset of data in file is the offset in the extent entry
// plus the header length. Get header length for object @index
int get_offset(struct objfs *fs, int index, bool ckpt)
{
    data_offsets_mutex.lock_shared();
    if (data_offsets.find(index) != data_offsets.end()) {
        data_offsets_mutex.unlock_shared();
        return data_offsets[index];
    }
    data_offsets_mutex.unlock_shared();

    obj_header h;
    ssize_t len = do_read(fs, index, &h, sizeof(h), 0, ckpt);
    if (len < 0)
        return -1;
    
    data_offsets_mutex.lock();
    data_offsets[index] = h.hdr_len;
    data_offsets_mutex.unlock();
    return h.hdr_len;
}

// read @len bytes of file data from object @index starting at
// data offset @offset (need to adjust for header length)
//
int read_data(struct objfs *fs, void *buf, int index, off_t offset, size_t len)
{
    access_times[index][offset] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    log_mutex.lock();
    if (index == next_s3_index) {
        len = std::min(len, data_offset() - offset);
        memcpy(buf, offset + (char*)data_log_head, len);
        log_mutex.unlock();
        return len;
    }
    log_mutex.unlock();
    old_log_mutex.lock_shared();
    if (meta_log_buffer.find(index) != meta_log_buffer.end()) {
        len = std::min(len, data_log_sizes[index] - offset);
        memcpy(buf, offset + (char*)data_log_buffer[index], len);
        old_log_mutex.unlock_shared();
        return len;
    }
    old_log_mutex.unlock_shared();
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
    uint32_t inum = 2;

    for (auto it = pathvec.begin(); it != pathvec.end(); it++) {
        if (inode_map.find(inum) == inode_map.end())
            return -ENOENT;
        fs_obj *obj = inode_map[inum];
        if (obj->type != OBJ_DIR)
            return -ENOTDIR;
        fs_directory *dir = (fs_directory*) obj;
        dir->read_lock();
        if (dir->dirents.find(*it) == dir->dirents.end()){
            dir->read_unlock();
            return -ENOENT;
        }
        inum = dir->dirents[*it];
        dir->read_unlock();
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
    int parent_inum;
    if (pathvec.size()==0) {
        parent_inum = 2;
    } else {
        parent_inum = vec_2_inum(pathvec);
    }

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
    std::shared_lock ckpting_lock(ckpting_mutex);
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }
    inode_mutex.lock_shared();
    int inum = path_2_inum(path);
    if (inum < 0){
        inode_mutex.unlock_shared();
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    inode_mutex.unlock_shared();
    obj_2_stat(sb, obj);

    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
    }
    return 0;
}

int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
		      off_t offset, struct fuse_file_info *fi)
{
    std::shared_lock ckpting_lock(ckpting_mutex);
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }
    int inum;
    std::shared_lock lock(inode_mutex);
    if (fi->fh != 0) {
        inum = fi->fh;
    } else {
        inum = path_2_inum(path);
    }
    if (inum < 0){
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_DIR){
        return -ENOTDIR;
    }
    
    fs_directory *dir = (fs_directory*)obj;
    dir->read_lock();
    for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
        struct stat sb;
        auto [name, i] = *it;
        fs_obj *o = inode_map[i];
        obj_2_stat(&sb, o);
        filler(ptr, const_cast<char*>(name.c_str()), &sb, 0);
    }
    dir->read_unlock();
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
    }
    return 0;
}


// -------------------------------

int fs_write(const char *path, const char *buf, size_t len,
	     off_t offset, struct fuse_file_info *fi)
{
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }

    int inum;
    inode_mutex.lock_shared(); // TODO: 1469: std::unique_lock lk(inode_mutex);
                               //       1481: lk.unlock();
    /*if (fi->fh != 0) {
        inum = fi->fh;
    } else {*/
        inum = path_2_inum(path);
    //}
    if (inum < 0){
        inode_mutex.unlock_shared();
        return inum;
    }
    // TODOO: path_2_inum shared lock and this shared lock should be one
    fs_obj *obj = inode_map[inum];
    inode_mutex.unlock_shared();
    if (obj->type != OBJ_FILE){
        return -EISDIR;
    }

    fs_file *f = (fs_file*)obj;
    
    f->read_lock();
    off_t new_size = std::max((off_t)(offset+len), (off_t)(f->size));
    f->read_unlock();

    int hdr_bytes = sizeof(log_record) + sizeof(log_data);
    char hdr[hdr_bytes];
    log_record *lr = (log_record*) hdr;
    log_data *ld = (log_data*) lr->data;

    lr->type = LOG_DATA;
    lr->len = sizeof(log_data);

    log_mutex.lock();
    size_t obj_offset = data_offset();

    time_t time_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    *ld = (log_data) { .inum = (uint32_t)inum,
		       .obj_offset = (uint32_t)obj_offset,
		       .file_offset = (int64_t)offset,
		       //.size = (int64_t)new_size, // TODO: 'size' is not needed
		       .len = (uint32_t)len,
               .access_time = time_now,
               .valid = true};

    memcpy(meta_log_tail, hdr, hdr_bytes);
    meta_log_tail = hdr_bytes + (char*)meta_log_tail;
    if (len > 0) {
        memcpy(data_log_tail, buf, len);
        data_log_tail = len + (char*)data_log_tail;
    }

    // TODO: optimization - check if it extends the previous record?
    // TODO: next_s3_index conflicts with ckpt index
    extent e = {.objnum = (uint32_t)next_s3_index,
		.offset = (uint32_t)obj_offset, .len = (uint32_t)len};

    f->write_lock();
    f->size = new_size;
    f->extents.update(offset, e);
    f->write_unlock();

    write_inode(f);
    log_mutex.unlock();
    maybe_write(fs);

    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
    }
    return len;
}

// write_inode stores written inodes in a buffer "written_inodes"
// future writes of the same inode overwrites the previous log_record
// The buffer resets at write_everything_out when the log is committed to S3
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

    // TODO: how do we save time here?
    if (written_inodes.find(in->inum) != written_inodes.end()){
        memcpy(written_inodes[in->inum], rec, len);
    } 
    else {
        memcpy(meta_log_tail, rec, len);
        written_inodes[in->inum] = meta_log_tail;
        meta_log_tail = len + (char*)meta_log_tail;
    }
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
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);

    inode_mutex.lock_shared();
    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum >= 0){
        inode_mutex.unlock_shared();
        return -EEXIST;
    }
    if (parent_inum < 0){
        inode_mutex.unlock_shared();
        return parent_inum;
    }

    fs_directory *parent = (fs_directory*)inode_map[parent_inum];
    inode_mutex.unlock_shared();
    if (parent->type != OBJ_DIR){
        return -ENOTDIR;
    }
    
    next_inode_mutex.lock();
    inum = next_inode++;
    next_inode_mutex.unlock();
    fs_directory *dir = new fs_directory;
    dir->type = OBJ_DIR;
    dir->inum = inum;
    dir->mode = mode | S_IFDIR;
    dir->rdev = dir->size = 0;
    clock_gettime(CLOCK_REALTIME, &dir->mtime);

    struct fuse_context *ctx = fuse_get_context();
    dir->uid = ctx->uid;
    dir->gid = ctx->gid;
    
    set_inode_map(inum, dir);
    parent->write_lock();
    parent->dirents[leaf] = inum;
    parent->write_unlock();
    clock_gettime(CLOCK_REALTIME, &parent->mtime);

    log_mutex.lock();
    write_inode(parent);
    
    write_inode(dir);
    write_dirent(parent_inum, leaf, inum);
    log_mutex.unlock();
    maybe_write(fs);

    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
    }
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
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    inode_mutex.lock_shared();
    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum < 0){
        inode_mutex.unlock_shared();
	    return -ENOENT;
    }
    if (parent_inum < 0){
        inode_mutex.unlock_shared();
	    return parent_inum;
    }
    
    fs_directory *dir = (fs_directory*)inode_map[inum];
    inode_mutex.unlock_shared();
    if (dir->type != OBJ_DIR){
	    return -ENOTDIR;
    }
    dir->read_lock();
    if (!dir->dirents.empty()){
        dir->read_unlock();
	    return -ENOTEMPTY;
    }
    dir->read_unlock();
    
    inode_mutex.lock();
    fs_directory *parent = (fs_directory*)inode_map[parent_inum];
    inode_map.erase(inum);
    inode_mutex.unlock();
    parent->write_lock();
    parent->dirents.erase(leaf);
    parent->write_unlock();
    delete dir;
    
    clock_gettime(CLOCK_REALTIME, &parent->mtime);
    log_mutex.lock();
    write_inode(parent);
    do_log_delete(parent_inum, inum, leaf);
    log_mutex.unlock();
    maybe_write(fs);
    return 0;
}

int create_node(struct objfs *fs, const char *path, mode_t mode, int type, dev_t dev)
{
    inode_mutex.lock_shared();
    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum >= 0){
        inode_mutex.unlock_shared();
	    return -EEXIST;
    }
    if (parent_inum < 0){
        inode_mutex.unlock_shared();
	    return parent_inum;
    }
    
    fs_directory *dir = (fs_directory*)inode_map[parent_inum];
    inode_mutex.unlock_shared();
    if (dir->type != OBJ_DIR)
	    return -ENOTDIR;
    
    next_inode_mutex.lock();
    inum = next_inode++;
    fs_file *f = new fs_file;	// yeah, OBJ_OTHER gets a useless extent map

    f->type = type;
    f->inum = inum;
    next_inode_mutex.unlock();
    f->mode = mode;
    f->rdev = dev;
    f->size = 0;
    clock_gettime(CLOCK_REALTIME, &f->mtime);

    struct fuse_context *ctx = fuse_get_context();
    f->uid = ctx->uid;
    f->gid = ctx->gid;
    
    set_inode_map(inum, f);
    dir->write_lock();
    dir->dirents[leaf] = inum;
    dir->write_unlock();

    log_mutex.lock();
    write_inode(f);
    //log_mutex.unlock();
    write_dirent(parent_inum, leaf, inum);
    
    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    //log_mutex.lock();
    write_inode(dir);
    log_mutex.unlock();

    maybe_write(fs);
    
    return inum;
}

// only called for regular files
int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }
    int inum = create_node(fs, path, mode | S_IFREG, OBJ_FILE, 0);

    if (inum <= 0) {
        return inum;
    }

    fi->fh = inum;

    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
    }
    return 0;
}

// for device files, FIFOs, etc.
int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    int inum = create_node(fs, path, mode, OBJ_OTHER, dev);
    if (inum < 0) {
        return inum;
    }

    return 0;
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
    inode_mutex.lock_shared();
    int inum = path_2_inum(path);
    if (inum < 0){
        inode_mutex.unlock_shared();
	    return inum;
    }

    fs_file *f = (fs_file*)inode_map[inum];
    inode_mutex.unlock_shared();
    
    if (f->type == OBJ_DIR){
	    return -EISDIR;
    }
    if (f->type != OBJ_FILE){
	    return -EINVAL;
    }
    
    do_trunc(f, len);
    log_mutex.lock();
    do_log_trunc(inum, len);

    clock_gettime(CLOCK_REALTIME, &f->mtime);
    write_inode(f);
    log_mutex.unlock();
    maybe_write(fs);
    return 0;
}

/* do I need a parent inum in fs_obj?
 */
int fs_unlink(const char *path)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);

    inode_mutex.lock_shared();
    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum < 0){
        inode_mutex.unlock_shared();
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    if (obj->type == OBJ_DIR){
        inode_mutex.unlock_shared();
        return -EISDIR;
    }
    
    fs_directory *dir = (fs_directory*)inode_map[parent_inum];
    inode_mutex.unlock_shared();

    dir->write_lock();
    dir->dirents.erase(leaf);
    dir->write_unlock();
    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    log_mutex.lock();
    write_inode(dir);
    
    if (obj->type == OBJ_FILE) {
        fs_file *f = (fs_file*)obj;
        do_trunc(f, 0);
        do_log_trunc(inum, 0);
    }
    do_log_delete(parent_inum, inum, leaf);
    log_mutex.unlock();
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
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    
    inode_mutex.lock_shared();
    auto [src_inum, src_parent, src_leaf] = path_2_inum2(src_path);
    if (src_inum < 0){
        inode_mutex.unlock_shared();
        return src_inum;
    }
    
    auto [dst_inum, dst_parent, dst_leaf] = path_2_inum2(dst_path);
    if (dst_inum >= 0){
        inode_mutex.unlock_shared();
        return -EEXIST;
    }
    if (dst_parent < 0){
        inode_mutex.unlock_shared();
        return dst_parent;
    }

    fs_directory *srcdir = (fs_directory*)inode_map[src_parent];
    fs_directory *dstdir = (fs_directory*)inode_map[dst_parent];
    inode_mutex.unlock_shared();

    if (dstdir->type != OBJ_DIR){
        return -ENOTDIR;
    }

    srcdir->write_lock();
    srcdir->dirents.erase(src_leaf);
    srcdir->write_unlock();
    clock_gettime(CLOCK_REALTIME, &srcdir->mtime);
    log_mutex.lock();
    write_inode(srcdir);
    log_mutex.unlock();

    dstdir->write_lock();
    dstdir->dirents[dst_leaf] = src_inum;
    dstdir->write_unlock();
    clock_gettime(CLOCK_REALTIME, &dstdir->mtime);
    log_mutex.lock();
    write_inode(dstdir);
    
    do_log_rename(src_inum, src_parent, dst_parent, src_leaf, dst_leaf);
    log_mutex.unlock();
    maybe_write(fs);
    return 0;
}

int fs_chmod(const char *path, mode_t mode)
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    inode_mutex.lock_shared();
    int inum = path_2_inum(path);
    if (inum < 0){
        inode_mutex.unlock_shared();
	    return inum;
    }

    fs_obj *obj = inode_map[inum];
    inode_mutex.unlock_shared();
    obj->mode = mode | (S_IFMT & obj->mode);
    log_mutex.lock();
    write_inode(obj);
    log_mutex.unlock();
    maybe_write(fs);
    return 0;
}

// see utimensat(2). Oh, and I hate access time...
//
int fs_utimens(const char *path, const struct timespec tv[2])
{
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    inode_mutex.lock_shared();
    int inum = path_2_inum(path);
    if (inum < 0){
        inode_mutex.unlock_shared();
	    return inum;
    }

    fs_obj *obj = inode_map[inum];
    inode_mutex.unlock_shared();
    if (tv == NULL || tv[1].tv_nsec == UTIME_NOW)
	    clock_gettime(CLOCK_REALTIME, &obj->mtime);
    else if (tv[1].tv_nsec != UTIME_OMIT)
	    obj->mtime = tv[1];
    log_mutex.lock();
    write_inode(obj);
    log_mutex.unlock();
    maybe_write(fs);
    
    return 0;
}


int fs_open(const char *path, struct fuse_file_info *fi)
{
    inode_mutex.lock_shared();
    int inum = path_2_inum(path);
    if (inum <= 0){
        inode_mutex.unlock_shared();
        return inum;
    }
    fs_obj *obj = inode_map[inum];
    inode_mutex.unlock_shared();
    if (obj->type != OBJ_FILE){
        return -ENOTDIR;
    }

    fi->fh = inum;

    return 0;
}

int fs_read(const char *path, char *buf, size_t len, off_t offset,
	    struct fuse_file_info *fi)
{
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;

    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }

    int inum;
    inode_mutex.lock_shared();
    /*if (fi->fh != 0) {
        inum = fi->fh;
    } else {*/
        inum = path_2_inum(path);
    //}

    if (inum < 0){
        inode_mutex.unlock_shared();
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    inode_mutex.unlock_shared();
    if (obj->type != OBJ_FILE){
        return -ENOTDIR;
    }
    fs_file *f = (fs_file*)obj;

    size_t bytes = 0;
    
    f->read_lock();
    for (auto it = f->extents.lookup(offset);
	 len > 0 && it != f->extents.end(); it++) {
	    auto [base, e] = *it;
        if (base > offset) {
            // yow, not supposed to have holes
            size_t skip = base-offset; // bytes to skip forward from current 'offset'
            if (skip > len) // extent is past end of requested read
               skip = len;
            bytes += skip;
            offset += skip;
            buf += skip;

            len -= skip;
        }
        else {
            size_t skip = offset - base;
            size_t _len = e.len - skip;  // length of buffer to consume (unmapped=skip,mapped)
            if (_len > len)
                _len = len;
            if (read_data(fs, buf, e.objnum, e.offset+skip, _len) < 0){
                f->read_unlock();
                return -EIO;
            }
            bytes += _len;
            offset += _len;
            buf += _len;

            len -= _len;
        }
    }
    f->read_unlock();
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
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
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);

    inode_mutex.lock_shared();
    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum >= 0){
        inode_mutex.unlock_shared();
	    return -EEXIST;
    }
    if (parent_inum < 0){
        inode_mutex.unlock_shared();
	    return parent_inum;
    }

    fs_directory *dir = (fs_directory*)inode_map[parent_inum];
    inode_mutex.unlock_shared();
    if (dir->type != OBJ_DIR){
	    return -ENOTDIR;
    }
    
    fs_link *l = new fs_link;
    l->type = OBJ_SYMLINK;
    next_inode_mutex.lock();
    l->inum = next_inode++;
    next_inode_mutex.unlock();
    l->mode = S_IFLNK | 0777;

    struct fuse_context *ctx = fuse_get_context();
    l->uid = ctx->uid;
    l->gid = ctx->gid;

    clock_gettime(CLOCK_REALTIME, &l->mtime);

    l->target = leaf;
    set_inode_map(inum, l);
    dir->write_lock();
    dir->dirents[leaf] = l->inum;
    dir->write_unlock();

    log_mutex.lock();
    write_inode(l);
    //log_mutex.unlock();
    write_symlink(inum, leaf);
    write_dirent(parent_inum, leaf, inum);
    
    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    //log_mutex.lock();
    write_inode(dir);
    log_mutex.unlock();
    maybe_write(fs);
    return 0;
}

int fs_readlink(const char *path, char *buf, size_t len)
{
    inode_mutex.lock_shared();
    int inum = path_2_inum(path);
    if (inum < 0){
        inode_mutex.unlock_shared();
	    return inum;
    }
    
    fs_link *l = (fs_link*)inode_map[inum];
    inode_mutex.unlock_shared();
    if (l->type != OBJ_SYMLINK){
	    return -EINVAL;
    }

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
    
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        using namespace std::literals::chrono_literals;
        sync_cv.wait_for(sync_lk, 500ms, []{return writing_log_count.load() == 0;});
    }
    log_mutex.lock();
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }
    write_everything_out(fs);

    return 0;
}

void fifo() {
    int fifo_limit = 1000;
    std::unique_lock lk(fifo_mutex);
    while (true) {
        if (quit_fifo) {
            while (fifo_size > 0) {
                int index = fifo_queue.front();
                fifo_queue.pop();
                fifo_size--;
                old_log_mutex.lock();
                free(meta_log_buffer[index]);
                free(data_log_buffer[index]);
                meta_log_buffer.erase(index);
                data_log_buffer.erase(index);
                meta_log_sizes.erase(index);
                data_log_sizes.erase(index);
                old_log_mutex.unlock();
            }
            quit_fifo = false;
            cv.notify_all();
            printf("quit fifo\n");
            return;
        }
        fifo_cv.wait(lk, [&fifo_limit]{return (fifo_size.load() > fifo_limit)||quit_fifo;});
        while (fifo_size > fifo_limit) {
            int index = fifo_queue.front();
            fifo_queue.pop();
            fifo_size--;
            old_log_mutex.lock();
            free(meta_log_buffer[index]);
            free(data_log_buffer[index]);
            meta_log_buffer.erase(index);
            data_log_buffer.erase(index);
            meta_log_sizes.erase(index);
            data_log_sizes.erase(index);
            old_log_mutex.unlock();
        }
    }
}

void checkpoint(struct objfs *fs)
{
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count++;
    }
    //struct objfs *fs = (struct objfs*) fuse_get_context()->private_data;
    ckpting_mutex.lock();
    if (quit_ckpt) {
        ckpting_mutex.unlock();
        return;
    }
    put_before_ckpt = false;
    log_mutex.lock();
    int ckpted_s3_index;
    if (data_offset() != 0 && meta_offset() != 0) {
        ckpted_s3_index = next_s3_index.load(); // When fs_init, do not load objects with index [0, ckpted_s3_index]
        {
            std::unique_lock<std::mutex> sync_lk(sync_mutex);
            writing_log_count++;
        }
        write_everything_out(fs);
    } else { // If there is nothing in log, then don't write_everything_out
        ckpted_s3_index = next_s3_index.load() - 1;
        put_before_ckpt = true;
        log_mutex.unlock();
    }

    char _key[1024];
    
    int cur_ckpt_index = ckpt_index + 1;
    if (cur_ckpt_index == 0) {
        cur_ckpt_index = 1;
    }
    sprintf(_key, "%s.%08x.ck", fs->prefix, cur_ckpt_index);
    std::string key(_key);

    ckpt_header h;
    std::stringstream objs;
    int objs_size = serialize_all(ckpted_s3_index, &h, objs);
    std::string str = objs.str();

    struct iovec iov[2] = {{.iov_base = (void*)&h, .iov_len = sizeof(h)},
        {.iov_base = (void *)(str.c_str()), .iov_len = (size_t)objs_size}};
    ckpting_mutex.unlock();
    
    {
        std::unique_lock<std::mutex> ckpt_lk(ckpt_put_mutex);
        using namespace std::literals::chrono_literals;
        ckpt_cv.wait_for(ckpt_lk, 500ms, []{return put_before_ckpt;});
    }
    if (quit_ckpt) {
        return;
    }

    printf("writing %s, objs size: %d\n", key.c_str(), objs_size);
    std::future<S3Status> status = std::async(std::launch::deferred, &s3_target::s3_put, fs->s3, key, iov, 2);
    
    if (S3StatusOK != status.get()){
        printf("CHECKPOINT PUT FAILED\n");
        throw "put failed";
    }

    if (ckpt_index == -1) {
        ckpt_index++;
    }
    ckpt_index++;

    gc(fs, ckpted_s3_index);
    {
        std::unique_lock<std::mutex> sync_lk(sync_mutex);
        writing_log_count--;
    }
}

void checkpointer(struct objfs *fs)
{
    using namespace std::literals::chrono_literals;
    std::unique_lock<std::mutex> lk(cv_m);
    while (true) {
        //std::this_thread::sleep_for (std::chrono::seconds(10));
        cv.wait_for(lk, 5000ms, []{return quit_ckpt;});
        checkpoint(fs);
        if (quit_ckpt) {
            quit_ckpt = false;
            cv.notify_all();
            printf("quit checkpointer\n");
            return;
        }
    }
}

int get_total_file_size(int inum){
    fs_obj *obj = inode_map[inum];

    if (obj->type == OBJ_FILE) {
        fs_file *file = (fs_file *)obj;
        int extent_sizes = 0;
        for (auto it = file->extents.begin(); it != file->extents.end(); it++) {
            auto [file_offset, ext] = *it;
            extent_sizes += ext.len;
        }
        return extent_sizes;
    }
    else if (obj->type == OBJ_DIR) {
	    fs_directory *dir = (fs_directory*)obj;
        int subdir_sizes = 0;
        for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
            auto [name,inum2] = *it;
            subdir_sizes += get_total_file_size(inum2);
        }
        return subdir_sizes;
    }
    return 0;
}

void gc_read_write(struct objfs *fs, int inum){
    fs_obj *obj = inode_map[inum];

    if (obj->type == OBJ_FILE) {
        fs_file *file = (fs_file *)obj;
        size_t extent_sizes = 0;
        for (auto it = file->extents.begin(); it != file->extents.end(); it++) {
            auto [file_offset, ext] = *it;
            extent_sizes += ext.len;
        }
        char buf[(int)extent_sizes];
        gc_read(fs, buf, extent_sizes, inum);
        gc_write(fs, buf, extent_sizes, inum);
    }
    else if (obj->type == OBJ_DIR) {
	    fs_directory *dir = (fs_directory*)obj;
        for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
            auto [name,inum2] = *it;
            gc_read_write(fs, inum2);
        }
    }
}

void gc(struct objfs *fs, int ckpted_s3_index){
    int root_inum = 2;

    int total_file_size = get_total_file_size(root_inum);

    int total_data_log_size = 0;
    std::list<std::string> keys;
    if (S3StatusOK != fs->s3->s3_list(fs->prefix, keys))
        throw "bucket list failed";

    int n;
    char postfix[10];
    for (auto it = keys.begin(); it != keys.end(); it++) {
        sscanf(it->c_str(), "%*[^.].%08x%s", &n, postfix); 
        if (strcmp(postfix, ".ck") == 0) {
            postfix[0] = '\0';
            continue;
        }
        obj_header h;
        ssize_t len = do_read(fs, n, &h, sizeof(h), 0, false);

        if (len < 0)
            throw "bad object";

        total_data_log_size += h.data_len;
    }

    float util_rate = total_file_size / total_data_log_size;

    printf("UTIL RATE %f\n", util_rate);
    if (util_rate > 0.5) {
        return;
    }
    gc_read_write(fs, root_inum);

    if (data_offset() != 0 && meta_offset() != 0) {
        ckpted_s3_index = next_s3_index.load(); 
        {
            std::unique_lock<std::mutex> sync_lk(sync_mutex);
            writing_log_count++;
        }
        write_everything_out(fs);
    }

    postfix[0] = '\0';
    // TODO: also remove all other checkpoints before the previous one
    for (auto it = keys.begin(); it != keys.end(); it++) {
        sscanf(it->c_str(), "%*[^.].%08x%s", &n, postfix); 
        if (strcmp(postfix, ".ck") == 0) {
            postfix[0] = '\0';
            continue;
        }
        if (n > ckpted_s3_index) continue;
        std::future<S3Status> status = std::async(std::launch::deferred, &s3_target::s3_delete, fs->s3, it->c_str());
        
        if (S3StatusOK != status.get()){
            printf("DELETE FAILED\n");
            throw "delete failed";
        }
    }
    printf("GC finished\n");
}

void gc_read(struct objfs *fs, char *buf, size_t len, int inum)
{
    fs_file *f = (fs_file *)inode_map[inum];
    off_t offset = 0;

    for (auto it = f->extents.lookup(0);
	 len > 0 && it != f->extents.end(); it++) {
	    auto [base, e] = *it;
        if (base > offset) {
            size_t skip = base-offset; 
            if (skip > len)
               skip = len;
            offset += skip;
            buf += skip;
            len -= skip;
        }
        else {
            size_t skip = offset - base;
            size_t _len = e.len - skip; 
            if (_len > len)
                _len = len;
            if (read_data(fs, buf, e.objnum, e.offset+skip, _len) < 0){
                return;
            }
            offset += _len;
            buf += _len;
            len -= _len;
        }
    }
}

void gc_write(struct objfs *fs, const char *buf, size_t len, int inum)
{
    int hdr_bytes = sizeof(log_record) + sizeof(log_data);
    char hdr[hdr_bytes];
    log_record *lr = (log_record*) hdr;
    log_data *ld = (log_data*) lr->data;

    lr->type = LOG_DATA;
    lr->len = sizeof(log_data);

    size_t obj_offset = data_offset();

    time_t time_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    *ld = (log_data) { .inum = (uint32_t)inum,
		       .obj_offset = (uint32_t)obj_offset,
		       .file_offset = (int64_t)0,
		       .len = (uint32_t)len,
               .access_time = time_now, // TODO: probably not needed
               .valid = true};

    memcpy(meta_log_tail, hdr, hdr_bytes);
    meta_log_tail = hdr_bytes + (char*)meta_log_tail;
    if (len > 0) {
        memcpy(data_log_tail, buf, len);
        data_log_tail = len + (char*)data_log_tail;
    }
    maybe_write(fs);
}

void *fs_init(struct fuse_conn_info *conn)
{
    struct objfs *fs = (objfs *)malloc(sizeof(objfs));
    fs = (objfs *) fuse_get_context()->private_data;
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);

    // initialization - FIXME
    //log_mutex.lock();
    meta_log_len = 2 * 64 * 1024;
    meta_log_head = meta_log_tail = malloc(meta_log_len*4);
    data_log_len = 2 * 8 * 1024 * 1024;
    data_log_head = data_log_tail = malloc(data_log_len*4);  // TODO: what length? Was *2 previously
    //log_mutex.unlock();

    fs->s3 = new s3_target(fs->host, fs->bucket, fs->access, fs->secret, false);

    std::list<std::string> keys;
    if (S3StatusOK != fs->s3->s3_list(fs->prefix, keys))
        throw "bucket list failed";
    
    int last_ckpt_index = -1;
    char postfix[10];
    for (std::list<std::string>::const_iterator it = keys.end() ; it != keys.begin() ; )
    {
        --it;
        
        int last_index;
        sscanf(it->c_str(), "%*[^.].%08x%s", &last_index, postfix);
        if (strcmp(postfix, ".ck") == 0) {
            last_ckpt_index = last_index;
            break;
        }
    }
    
    ckpt_header ckpt_h;
    ckpt_h.ckpted_s3_index = 0;
    ssize_t offset = -1;
    void *buf;
    if (last_ckpt_index >= 0) {
        int len = do_read(fs, last_ckpt_index, &ckpt_h, sizeof(ckpt_h), 0, true);
        if (len < 0)
            throw "can't read ckpt header";

        size_t objfs_itable_size = ckpt_h.itable_offset - sizeof(ckpt_h) + ckpt_h.itable_len;
        buf = malloc(objfs_itable_size);
        len = do_read(fs, last_ckpt_index, buf, objfs_itable_size, sizeof(ckpt_h), true);
        if (len < 0)
            throw "can't read ckpt contents";
        deserialize_tree(buf, ckpt_h.itable_offset - sizeof(ckpt_h));
        free(buf);
        buf = NULL;
    }

    int n;
    next_s3_index = ckpt_h.ckpted_s3_index + 1;
    postfix[0] = '\0';
    for (auto it = keys.begin(); it != keys.end(); it++) {
        sscanf(it->c_str(), "%*[^.].%08x%s", &n, postfix); 
        if (strcmp(postfix, ".ck") == 0) {
            postfix[0] = '\0';
            continue;
        }
        if (n <= (int)ckpt_h.ckpted_s3_index) continue;
        offset = get_offset(fs, n, false);

        if (offset < 0)
            throw "bad object";
        buf = malloc(offset);
        struct iovec iov[] = {{.iov_base = buf, .iov_len = (size_t)offset}};
        std::future<S3Status> status = std::async(std::launch::deferred, &s3_target::s3_get, fs->s3, it->c_str(), 0, offset, iov, 1);
        if (S3StatusOK != status.get())
            throw "can't read header";
        if (read_hdr(n, buf, offset) < 0)
            throw "bad header";
        next_s3_index = std::max(n+1, next_s3_index.load());
    }

    // Put the root dir "" into inode map with inum = 2
    if (inode_map.find(2) == inode_map.end()){
        fs_directory *d = new fs_directory;
        d->type = OBJ_DIR;
        d->rdev = d->size = 0;
        d->inum = 2;
        d->mode = S_IFDIR;
        set_inode_map(2, d);
        clock_gettime(CLOCK_REALTIME, &d->mtime);

        struct fuse_context *ctx = fuse_get_context();
        d->uid = ctx->uid;
        d->gid = ctx->gid;
        write_inode(d);
    }

    ckpt_index = last_ckpt_index;
    next_inode = 3;
    for (auto it = inode_map.begin(); it != inode_map.end(); it++) {
        next_inode = std::max((int)it->first + 1, next_inode.load());
    }

    // TODO: Only libobjfs inits set reserved[0] to 9, this prevents objfs-mount from starting the checkpointing thread and makes testing easier (for now)
    if (conn->reserved[0] == 9) {
        //std::thread(gc, fs).detach();
        std::thread(fifo).detach();
        std::thread (checkpointer, fs).detach();
    }

    return (void*) fs;
}

void fs_teardown(void *foo)
{
    // TODO: access_times (locking, cleaning, initing)
    quit_ckpt = true;
    quit_fifo = true;
    cv.notify_all();
    fifo_cv.notify_all();

    {
        std::unique_lock<std::mutex> lk(cv_m);
        cv.wait(lk, []{return (!quit_ckpt)&&(!quit_fifo);});
        printf("quit ckpter&fifo finished in fs_teardown\n");
    }
    const std::shared_lock<std::shared_mutex> ckpting_lock(ckpting_mutex);
    inode_mutex.lock();
    for (auto it = inode_map.begin(); it != inode_map.end();
	 it = inode_map.erase(it)) ;
    inode_mutex.unlock();
    
    log_mutex.lock();
    next_s3_index = 1;
    for (auto it = written_inodes.begin(); it != written_inodes.end();
	 it = written_inodes.erase(it));
    
    free(meta_log_head);
    free(data_log_head);
    meta_log_head = NULL;
    data_log_head = NULL;
    log_mutex.unlock();
    // TODO: LOCK HERE?
    data_offsets_mutex.lock();
    for (auto it = data_offsets.begin(); it != data_offsets.end();
	 it = data_offsets.erase(it));
    data_offsets_mutex.unlock();
}

#if 0
int fs_mkfs(const char *prefix)
{
    /*if (next_s3_index != 0)
	return -1;
    
    init_stuff(prefix);*/

    auto root = new fs_directory;
    root->inum = 1;
    root->uid = 0;
    root->gid = 0;
    root->mode = S_IFDIR | 0744;
    root->rdev = 0;
    root->size = 0;
    clock_gettime(CLOCK_REALTIME, &root->mtime);

    log_mutex.lock();
    write_inode(root);
    log_mutex.unlock();

    return 0;
}
#endif

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
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .statfs = fs_statfs,
    .fsync = fs_fsync,
    .readdir = fs_readdir,
    .init = fs_init,
    .destroy = fs_teardown,
    .create = fs_create,
    .utimens = fs_utimens,
};

