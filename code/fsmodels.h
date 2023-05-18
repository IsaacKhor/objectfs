#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>

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

enum obj_type { OBJ_FILE = 1, OBJ_DIR = 2, OBJ_SYMLINK = 3, OBJ_OTHER = 4 };

struct extent {
    uint32_t objnum;
    uint32_t offset; // within object (bytes)
    uint32_t len;    // (bytes)
};

typedef std::map<int64_t, extent> internal_map;
class extmap
{
    internal_map the_map;

  public:
    internal_map::iterator begin() { return the_map.begin(); }
    internal_map::iterator end() { return the_map.end(); }
    int size() { return the_map.size(); }

    // returns one of:
    // - extent containing @offset
    // - lowest extent with base > @offset
    // - end()
    internal_map::iterator lookup(int64_t offset)
    {
        auto it = the_map.lower_bound(offset);
        if (it == the_map.end())
            return it;
        auto &[base, e] = *it;
        if (base > offset && it != the_map.begin()) {
            it--;
            auto &[base0, e0] = *it;
            if (offset < base0 + e0.len)
                return it;
            it++;
        }
        return it;
    }

    void update(int64_t offset, extent e)
    {
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
            if (key >= offset && key + val.len <= offset + e.len) {
                it++;
                the_map.erase(key);
            } else
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
                auto new_len = val.len - (new_key - key);
                val.len = offset - key;
                the_map[key] = val;
                val.offset += (new_key - key);
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

    void erase(int64_t offset) { the_map.erase(offset); }
};

/* serializes in its in-memory layout.
 * Except maybe packed or something.
 * oh, actually use 1st 4 bytes for type/length
 */
class fs_obj
{
  public:
    uint32_t type : 4;
    // uint32_t        len : 28;	// of serialized metadata
    size_t len : 28;
    uint32_t inum;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t rdev;
    int64_t size;
    struct timespec mtime;
    size_t length(void) { return sizeof(fs_obj); }
    size_t serialize(std::stringstream &s);
    fs_obj(void *ptr, size_t len);
    fs_obj() {}
};

fs_obj::fs_obj(void *ptr, size_t len)
{
    assert(len == sizeof(*this));
    *this = *(fs_obj *)ptr;
}

/* note that all the serialization routines are risky, because we're
 * using the object in-memory layout itself, so any change to the code
 * might change the on-disk layout.
 */
size_t fs_obj::serialize(std::stringstream &s)
{
    fs_obj hdr = *this;
    size_t bytes = hdr.len = sizeof(hdr);
    s.write((char *)&hdr, sizeof(hdr));
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
    int64_t file_offset;
    uint32_t objnum;
    uint32_t obj_offset;
    uint32_t len;
};

class fs_file : public fs_obj
{
    std::shared_mutex mtx;    // Read-Write Lock
    std::shared_mutex rd_mtx; // Lock for read_data()
    std::mutex gc_mtx;

  public:
    extmap extents;
    size_t length(void);
    size_t serialize(std::stringstream &s);
    fs_file(void *ptr, size_t len);
    fs_file(){};
    void write_lock() { mtx.lock(); }
    void write_unlock() { mtx.unlock(); }
    void read_lock() { mtx.lock_shared(); }
    void read_unlock() { mtx.unlock_shared(); }
    void rd_lock() { rd_mtx.lock(); }
    void rd_unlock() { rd_mtx.unlock(); }
    void gc_lock() { gc_mtx.lock(); }
    void gc_unlock() { gc_mtx.unlock(); }
};

// de-serialize from serialized form
//
fs_file::fs_file(void *ptr, size_t len)
{
    assert(len >= sizeof(fs_obj));
    *(fs_obj *)this = *(fs_obj *)ptr;
    len -= sizeof(fs_obj);
    extent_xp *ex = (extent_xp *)(sizeof(fs_obj) + (char *)ptr);

    while (len > 0) {
        extent e = {
            .objnum = ex->objnum, .offset = ex->obj_offset, .len = ex->len};
        extents.update(ex->file_offset, e);
        ex = (extent_xp *)((char *)ex + sizeof(extent_xp)); // ex++;
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
    s.write((char *)&hdr, sizeof(fs_obj));

    // TODO - merge adjacent extents (not sure it ever happens...)
    for (auto it = extents.begin(); it != extents.end(); it++) {
        auto [file_offset, ext] = *it;
        extent_xp _e = {.file_offset = file_offset,
                        .objnum = ext.objnum,
                        .obj_offset = ext.offset,
                        .len = ext.len};
        s.write((char *)&_e, sizeof(extent_xp));
    }
    read_unlock();

    return bytes;
}

typedef std::pair<uint32_t, uint32_t> offset_len;

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
    uint8_t namelen;
    char name[];
} __attribute__((packed, aligned(1)));

class fs_directory : public fs_obj
{
    std::shared_mutex mtx; // Read-Write Lock
  public:
    std::map<std::string, uint32_t> dirents;
    size_t length(void);
    size_t serialize(std::stringstream &s, std::map<uint32_t, offset_len> &m);
    fs_directory(void *ptr, size_t len);
    fs_directory(){};
    void write_lock() { mtx.lock(); };
    void write_unlock() { mtx.unlock(); };
    void read_lock() { mtx.lock_shared(); };
    void read_unlock() { mtx.unlock_shared(); };
};

// de-serialize a directory from a checkpoint
//
fs_directory::fs_directory(void *ptr, size_t len)
{
    assert(len >= sizeof(fs_obj));
    *(fs_obj *)this = *(fs_obj *)ptr;
    len -= sizeof(fs_obj);
    dirent_xp *de = (dirent_xp *)(sizeof(fs_obj) + (char *)ptr);

    while (len > 0) {
        std::string name(de->name, de->namelen);
        dirents[name] = de->inum;
        // TODO - do something with offset/len
        len -= (sizeof(*de) + de->namelen);
        de = (dirent_xp *)(sizeof(*de) + de->namelen + (char *)de);
    }
    assert(len == 0);
}

size_t fs_directory::length(void)
{
    size_t bytes = sizeof(fs_obj);
    for (auto it = dirents.begin(); it != dirents.end(); it++) {
        auto [name, inum] = *it;
        bytes += (sizeof(dirent_xp) + name.length());
    }
    return bytes;
}

size_t fs_directory::serialize(std::stringstream &s,
                               std::map<uint32_t, offset_len> &map)
{
    fs_obj hdr = *this;
    read_lock();
    size_t bytes = hdr.len = length();
    s.write((char *)&hdr, sizeof(hdr));

    for (auto it = dirents.begin(); it != dirents.end(); it++) {
        auto [name, inum] = *it;
        auto [offset, len] = map[inum];
        uint8_t namelen = name.length();
        dirent_xp de = {
            .inum = inum, .offset = offset, .len = len, .namelen = namelen};
        s.write((char *)&de, sizeof(dirent_xp));
        s.write(name.c_str(), namelen);
    }
    read_unlock();
    return bytes;
}

/* extra space in entry is just the target
 */
class fs_link : public fs_obj
{
  public:
    std::string target;
    size_t length(void);
    size_t serialize(std::stringstream &s);
    fs_link(void *ptr, size_t len);
    fs_link() {}
};

// deserialize a symbolic link.
//
fs_link::fs_link(void *ptr, size_t len)
{
    assert(len >= sizeof(fs_obj));
    *(fs_obj *)this = *(fs_obj *)ptr;
    len -= sizeof(fs_obj);
    std::string _target((char *)ptr, len);
    target = _target;
}

// serialized length in bytes
//
size_t fs_link::length(void) { return sizeof(fs_obj) + target.length(); }

// serialize to an ostream
//
size_t fs_link::serialize(std::stringstream &s)
{
    fs_obj hdr = *this;
    size_t bytes = hdr.len = length();
    s.write((char *)&hdr, sizeof(hdr));
    s.write(target.c_str(), target.length());
    return bytes;
}

/****************
 * file header format
 */

/* data update
 */
struct log_data {
    uint32_t inum;       // is 32 enough?
    uint32_t obj_offset; // bytes from start of file data
    int64_t file_offset; // in bytes
    int64_t size;        // file size after this write
    uint32_t len;        // bytes
} __attribute__((packed, aligned(1)));

/* inode update. Note that this is all that's needed for special
 * files.
 */
struct log_inode {
    uint32_t inum;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t rdev;
    struct timespec mtime;
} __attribute__((packed, aligned(1)));

/* truncate a file. maybe require truncate->0 before delete?
 */
struct log_trunc {
    uint32_t inum;
    int64_t new_size; // must be <= existing
} __attribute__((packed, aligned(1)));

// need a log_create

struct log_delete {
    uint32_t parent;
    uint32_t inum;
    uint8_t namelen;
    char name[];
} __attribute__((packed, aligned(1)));

struct log_symlink {
    uint32_t inum;
    uint8_t len;
    char target[];
} __attribute__((packed, aligned(1)));

/* cross-directory rename is handled by specifying both source and
 * destination parent directory.
 */
struct log_rename {
    uint32_t inum;    // of entity to rename
    uint32_t parent1; // inode number (source)
    uint32_t parent2; //              (dest)
    uint8_t name1_len;
    uint8_t name2_len;
    char name[];
} __attribute__((packed, aligned(1)));

/* create a new name
 */
struct log_create {
    uint32_t parent_inum;
    uint32_t inum;
    uint8_t namelen;
    char name[];
} __attribute__((packed, aligned(1)));

enum log_rec_type {
    LOG_INODE = 1,
    LOG_TRUNC,
    LOG_DELETE,
    LOG_SYMLNK,
    LOG_RENAME,
    LOG_DATA,
    LOG_CREATE,
    LOG_NULL, // fill space for alignment
};

struct log_record {
    uint16_t type : 4;
    uint16_t len : 12;
    int32_t index;
    char data[];
} __attribute__((packed, aligned(1)));

// #define OBJFS_MAGIC 0x4f424653	// "OBFS"
#define OBJFS_MAGIC 0x5346424f // "OBFS"

struct obj_header {
    int32_t magic;
    int32_t version;
    int32_t type; // 1 == data, 2 == metadata
    int32_t hdr_len;
    int32_t this_index;
    int32_t ckpt_index; // the object index of the latest checkpoint
    int32_t data_len;   // length of data log
    char data[];
};

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