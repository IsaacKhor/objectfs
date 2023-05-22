/*
  Object file system

  Uses data objects and metadata objects. Data objects form a logical
  log, and are a complete record of the file system. Metadata objects
  roll up all changes into a read-optimized form.
 */

#define FUSE_USE_VERSION 27
#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <errno.h>
#include <fmt/core.h>
#include <fuse.h>
#include <libs3.h>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <sys/uio.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "fsmodels.h"
#include "objects.h"
#include "objfs.h"
#include "s3wrap.h"

bool quit_fifo;
bool quit_ckpt;
bool quit_gc;
// In checkpoint(), check if write_everything_out completed before serialize_all
bool put_before_ckpt;

// After gc starts, this sets to true. fs_write and fs_truncate are recorded so
// that gc_write doesn't overwrite
bool started_gc;

// GC all normal s3 objects before and including this index
std::atomic<int> gc_range(0);
std::atomic<int> fifo_size(0);
std::atomic<int> next_s3_index(1);
std::atomic<int> next_inode(3);  // the root "" has inum 2
std::atomic<int> ckpt_index(-1); // record the latest checkpoint object index
int f_bsize = 4096;

// first key is inum, second key is offset, value is len
std::map<int, std::map<int, int> *> writes_after_ckpt;
// first key is inum, second key means fs_truncate off_t len
std::map<int, int> truncs_after_ckpt;
std::map<int, int> file_stale_data;

// Logger *logger;

// Global inode map
std::unordered_map<uint32_t, fs_obj *> inode_map;

// returns new offset
size_t serialize_tree(std::stringstream &s, size_t offset, uint32_t inum,
                      std::map<uint32_t, offset_len> &map)
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
    } else {
        fs_directory *dir = (fs_directory *)obj;
        for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
            auto [name, inum2] = *it;
            offset = serialize_tree(s, offset, inum2, map);
        }
        size_t len = dir->serialize(s, map);
        return offset + len;
    }
}

static void set_inode_map(int inum, fs_obj *ptr)
{
    if (ptr->type == OBJ_DIR) {
        fs_directory *d = (fs_directory *)ptr;
        inode_map[inum] = d;
    } else if (ptr->type == OBJ_FILE) {
        fs_file *f = (fs_file *)ptr;
        inode_map[inum] = f;
    } else if (ptr->type == OBJ_SYMLINK) {
        fs_link *s = (fs_link *)ptr;
        inode_map[inum] = s;
    } else {
        inode_map[inum] = ptr;
    }
}

static int read_log_inode(log_inode *in)
{
    auto it = inode_map.find(in->inum);
    if (it != inode_map.end()) {
        auto obj = inode_map[in->inum];
        update_inode(obj, in);
    } else {
        if (S_ISDIR(in->mode)) {
            fs_directory *d = new fs_directory;
            d->type = OBJ_DIR;
            d->size = 0;
            set_inode_map(in->inum, d);
            update_inode(d, in);
        } else if (S_ISREG(in->mode)) {
            fs_file *f = new fs_file;
            f->type = OBJ_FILE;
            f->size = 0;
            update_inode(f, in);
            set_inode_map(in->inum, f);
        } else if (S_ISLNK(in->mode)) {
            fs_link *s = new fs_link;
            s->type = OBJ_SYMLINK;
            s->size = 0;
            update_inode(s, in);
            set_inode_map(in->inum, s);
        } else {
            fs_obj *o = new fs_obj;
            o->type = OBJ_OTHER;
            o->size = 0;
            update_inode(o, in);
            set_inode_map(in->inum, o);
        }
    }
    return 0;
}

void do_trunc(fs_file *f, off_t new_size)
{
    f->write_lock();
    while (true) {
        auto it = f->extents.lookup(new_size);
        if (it == f->extents.end())
            break;
        auto [offset, e] = *it;
        if (offset < new_size) {
            e.len = new_size - offset;
            f->extents.update(offset, e);
        } else {
            f->extents.erase(offset);
        }
    }
    f->size = new_size;
    f->write_unlock();
}

int read_log_trunc(log_trunc *tr)
{
    auto it = inode_map.find(tr->inum);
    if (it == inode_map.end()) {
        return -1;
    }

    fs_file *f = (fs_file *)(inode_map[tr->inum]);
    f->read_lock();
    if (f->size < tr->new_size) {
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
    if (inode_map.find(rm->parent) == inode_map.end())
        return -1;
    if (inode_map.find(rm->inum) == inode_map.end())
        return -1;

    auto name = std::string(rm->name, rm->namelen);
    fs_obj *f = inode_map[rm->inum];
    inode_map.erase(rm->inum);
    fs_directory *parent = (fs_directory *)(inode_map[rm->parent]);
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

    fs_directory *parent1 = (fs_directory *)(inode_map[mv->parent1]);
    fs_directory *parent2 = (fs_directory *)(inode_map[mv->parent2]);

    auto name1 = std::string(&mv->name[0], mv->name1_len);
    auto name2 = std::string(&mv->name[mv->name1_len], mv->name2_len);

    parent1->read_lock();
    if (parent1->dirents.find(name1) == parent1->dirents.end()) {
        parent1->read_unlock();
        return -1;
    }
    if (parent1->dirents[name1] != mv->inum) {
        parent1->read_unlock();
        return -1;
    }
    parent1->read_unlock();
    parent2->read_lock();
    if (parent2->dirents.find(name2) != parent2->dirents.end()) {
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
    auto it = inode_map.find(d->inum);
    if (it == inode_map.end())
        return -1;

    fs_file *f = (fs_file *)inode_map[d->inum];

    // optimization - check if it extends the previous record?
    extent e = {
        .objnum = (uint32_t)idx, .offset = d->obj_offset, .len = d->len};
    f->write_lock();
    f->extents.update(d->file_offset, e);
    f->size = d->size;
    f->write_unlock();

    return 0;
}

int read_log_create(log_create *c)
{
    auto it = inode_map.find(c->parent_inum);
    if (it == inode_map.end())
        return -1;

    fs_directory *d = (fs_directory *)inode_map[c->parent_inum];
    auto name = std::string(&c->name[0], c->namelen);
    d->write_lock();
    d->dirents[name] = c->inum;
    d->write_unlock();

    next_inode = std::max(next_inode.load(), (int)(c->inum + 1));
    return 0;
}

// returns 0 on success, bytes to read if not enough data,
// -1 if bad format. Must pass at least 32B
//
size_t read_hdr(int idx, void *data, size_t len)
{
    obj_header *oh = (obj_header *)data;
    if ((size_t)(oh->hdr_len) > len)
        return oh->hdr_len;

    if (oh->magic != OBJFS_MAGIC || oh->version != 1 || oh->type != 1)
        return -1;

    size_t meta_bytes = oh->hdr_len - sizeof(obj_header);
    log_record *end = (log_record *)&oh->data[meta_bytes];
    log_record *rec = (log_record *)&oh->data[0];

    while (rec < end) {
        switch (rec->type) {
        case LOG_INODE:
            if (read_log_inode((log_inode *)&rec->data[0]) < 0)
                return -1;
            break;
        case LOG_TRUNC:
            if (read_log_trunc((log_trunc *)&rec->data[0]) < 0)
                return -1;
            break;
        case LOG_DELETE:
            if (read_log_delete((log_delete *)&rec->data[0]) < 0)
                return -1;
            break;
        case LOG_SYMLNK:
            if (read_log_symlink((log_symlink *)&rec->data[0]) < 0)
                return -1;
            break;
        case LOG_RENAME:
            if (read_log_rename((log_rename *)&rec->data[0]) < 0)
                return -1;
            break;
        case LOG_DATA:
            if (read_log_data(idx, (log_data *)&rec->data[0]) < 0)
                return -1;
            break;
        case LOG_CREATE:
            if (read_log_create((log_create *)&rec->data[0]) < 0)
                return -1;
            break;
        case LOG_NULL:
            break;
        default:
            return -1;
        }
        rec = (log_record *)&rec->data[rec->len];
    }
    return 0;
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
struct ckpt_header {
    uint32_t root_inum;
    uint32_t root_offset;
    uint32_t root_len;
    uint32_t next_inum;
    uint32_t itable_offset;
    uint32_t itable_len;
    uint32_t ckpted_s3_index;
    char data[];
};

struct gc_info {
    uint32_t objnum;
    int64_t file_offset;
    uint32_t obj_offset;
    uint32_t len;
};

/*
   when do we decide to write?
   (1) When we get to a fixed object size
   (2) when we get an fsync (maybe)
   (3) on timeout (working on it...)
       (need a reader/writer lock for this one?)
 */

// constants copied from old code
const size_t META_LOG_ROLLOVER_THRESHOLD = 2 * 64 * 1024;
const size_t DATA_LOG_ROLLOVER_THRESHOLD = 2 * 8 * 1024 * 1024;
ConcurrentByteLog meta_log(40 * META_LOG_ROLLOVER_THRESHOLD);
ConcurrentByteLog data_log(40 * DATA_LOG_ROLLOVER_THRESHOLD);

const size_t CACHE_SIZE = 1024 * 1024 * 1024;

void write_inode(fs_obj *f);
void gc_file(struct objfs *);
void gc_obj(struct objfs *, int);
void gc_write(struct objfs *, const char *, size_t, int, int);
void gc_write_extent(void *, int, int, int);
void gc_read_write(struct objfs *, std::vector<gc_info *> *);
void get_total_file_size(int, std::map<int, int> &,
                         std::map<int, std::vector<gc_info *> *> &);

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
    uint8_t *p = (uint8_t *)hdr;
    for (int i = 0; i < hdrlen; i++)
        printf("%02x", p[i]);
    printf("\n");
}

int serialize_all(int ckpted_s3_index, ckpt_header *h, std::stringstream &objs)
{
    int root_inum = 2;

    h->root_inum = (uint32_t)root_inum;
    h->next_inum = (uint32_t)next_inode;

    std::map<uint32_t, offset_len> imap;
    size_t objs_offset = sizeof(ckpt_header);

    h->ckpted_s3_index = ckpted_s3_index;
    size_t itable_offset = serialize_tree(objs, objs_offset, root_inum, imap);

    auto [_off, _len] = imap[root_inum];
    h->root_offset = _off;
    h->root_len = _len;
    h->itable_offset = itable_offset;

    return itable_offset - objs_offset;

    // checkpoint is now in two parts:
    // &h, sizeof(h)
    // objs.str().c_str(), itable_offset-objs_offset
}

void deserialize_tree(void *bufv, size_t len)
{
    char *buf = (char *)bufv;
    char *end = (char *)buf + len;
    // TODO: put in asserts in fs_directory::fs_directory, check that file No
    // and dir No point to valid inodes
    while (buf < end) {
        fs_obj *cur_obj = (fs_obj *)buf;
        if (cur_obj->type == OBJ_DIR) {
            fs_directory *d = new fs_directory((void *)buf, cur_obj->len);
            buf = buf + cur_obj->len;
            set_inode_map(cur_obj->inum, d);
        } else if (cur_obj->type == OBJ_FILE) {
            fs_file *f = new fs_file((void *)buf, cur_obj->len);
            buf = buf + cur_obj->len;
            set_inode_map(cur_obj->inum, f);
        } else if (cur_obj->type == OBJ_SYMLINK) {
            fs_link *s = new fs_link((void *)buf, cur_obj->len);
            buf = buf + cur_obj->len;
            set_inode_map(cur_obj->inum, s);
        } else {
            fs_obj *o = new fs_obj((void *)buf, cur_obj->len);
            buf = buf + cur_obj->len;
            set_inode_map(cur_obj->inum, o);
        }
    }
}

// write_everything_out allocates a new metadata log and a new data log.
// It puts old logs in the buffer so that read threads can get them before they
// are committed to S3 backend. After s3_put returns success, old logs are
// removed from buffer
void write_everything_out(struct objfs *fs)
{

    int index = next_s3_index;
    size_t meta_log_size = meta_log.size();
    void *meta_log_head_old = meta_log.reset();

    size_t data_log_size = data_log.size();
    void *data_log_head_old = data_log.reset();

    std::string key =
        fmt::format("{}.{:08x}", fs->prefix, next_s3_index.load());

    obj_header h = {
        .magic = OBJFS_MAGIC,
        .version = 1,
        .type = 1,
        .hdr_len = (int)(meta_log_size + sizeof(obj_header)),
        .this_index = next_s3_index,
        .ckpt_index = ckpt_index,
        .data_len = static_cast<int32_t>(data_log_size),
    };
    next_s3_index++;

    struct iovec iov[3] = {
        {.iov_base = (void *)&h, .iov_len = sizeof(h)},
        {.iov_base = meta_log_head_old, .iov_len = meta_log_size},
        {.iov_base = data_log_head_old, .iov_len = data_log_size}};

    debug("writing %s, meta log size: %d, data log size: %d", key.c_str(),
          (int)meta_log_size, (int)data_log_size);
    printout((void *)&h, sizeof(h));
    printout((void *)meta_log_head_old, meta_log_size);

    auto status = fs->s3->s3_put(key, iov, 3);
    if (S3StatusOK != status) {
        log_error("s3_put failed: %s\n", S3_get_status_name(status));
        throw "put failed";
    }
}

void maybe_write(struct objfs *fs)
{
    if ((meta_log.size() > META_LOG_ROLLOVER_THRESHOLD) ||
        (data_log.size() > DATA_LOG_ROLLOVER_THRESHOLD)) {
        write_everything_out(fs);
    }
}

void make_record(const void *hdr, size_t hdrlen, const void *data,
                 size_t datalen)
{
    printout((void *)hdr, hdrlen);

    meta_log.append(hdr, hdrlen);
    data_log.append(data, datalen);
}

std::map<int, int> data_offsets;
std::map<int, int> data_lens;

// read at absolute offset @offset in object @index
//
int do_read(struct objfs *fs, int index, void *buf, size_t len, size_t offset,
            bool ckpt)
{
    std::string key =
        fmt::format("{}.{:08x}{}", fs->prefix, index, ckpt ? ".ck" : "");
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    // logger->log("read from s3 backend\n");
    S3Status status = fs->s3->s3_get(key, offset, len, &iov, 1);

    return status == S3StatusOK ? len : -1;
}

// actual offset of data in file is the offset in the extent entry
// plus the header length. Get header length for object @index
int get_offset(struct objfs *fs, int index, bool ckpt)
{
    if (data_offsets.find(index) != data_offsets.end()) {
        return data_offsets[index];
    }

    obj_header h;
    ssize_t len = do_read(fs, index, &h, sizeof(h), 0, ckpt);
    if (len < 0)
        return -1;

    data_offsets[index] = h.hdr_len;
    return h.hdr_len;
}

int get_datalen(struct objfs *fs, int index, bool ckpt)
{
    if (data_lens.find(index) != data_lens.end()) {
        return data_lens[index];
    }

    obj_header h;
    ssize_t len = do_read(fs, index, &h, sizeof(h), 0, ckpt);
    if (len < 0)
        return -1;

    data_lens[index] = h.data_len;
    return h.data_len;
}

/**
 * Reads specific chunk of data from the object store
 *
 * @param fs The objfs instance
 * @param f_ptr The file pointer. Can be NULL. If not null, will be used to lock
 * the appropriate file
 * @param buf The destination buffer
 * @param index The index of the object
 * @param offset The offset in the object in bytes, not including the header
 * @param len The length of the data to read
 *
 * @return The number of bytes read
 */
int read_data(struct objfs *fs, void *f_ptr, void *bufv, int index,
              off_t offset, size_t len)
{
    char *buf = (char *)bufv;
    // Look in the data log buffer first, if it's there we just return it
    // directly
    if (index == next_s3_index) {
        len = std::min(len, data_log.size() - offset);
        data_log.get(buf, offset, len);
        return len;
    }

    int data_len = get_datalen(fs, index, false);
    int hdr_len = get_offset(fs, index, false);

    if (offset + len > data_len) {
        log_error("read_data: offset + len %zu > data_len %i", offset + len,
                  data_len);
        return -1;
    }

    std::set<int> empty_extent_offsets;
    std::map<int, void *> *data_extents;

    int cur_ext_offset = ((int)offset / f_bsize) * f_bsize;
    int left_do_read_offset = cur_ext_offset;

    // TODO Question: is there a lock ordering thing here going on?

    // We may need to upgrade the lock
    bool is_cache_lock_shared = true;

    fs_file *f;
    if (f_ptr != NULL) {
        f = (fs_file *)f_ptr;
        f->rd_lock();
    }

    // Try to see if the object is in the cache before we fetch from backend
    // I don't know the real reason since I didn't write the code, but
    // the reason we need to keep the read lock well beyond when we used the
    // map is because another thread tries to acquire the lock and will
    // modify the *contents* of the entry. Thus the read lock is also a read
    // lock for the contents of the cache entries, not just the cache map itself
    while (cur_ext_offset < offset + len) {
        empty_extent_offsets.insert(cur_ext_offset);
        cur_ext_offset += f_bsize;
    }
    data_extents = new std::map<int, void *>;
    // We change the map here, so we can't use a shared lock
    // Upgrade to exclusive
    is_cache_lock_shared = false;

    int right_do_read_offset = std::min(data_len, cur_ext_offset);

    int cur_ext_read_size;
    if (empty_extent_offsets.size() == 0) {
        int bytes_read = 0;
        cur_ext_offset = ((int)offset / f_bsize) * f_bsize;
        auto it = data_extents->find(cur_ext_offset);
        char *cur_ext = (char *)it->second;
        cur_ext_read_size =
            std::min(cur_ext_offset + f_bsize - (int)offset, (int)len);
        memcpy(buf, cur_ext + offset - cur_ext_offset, cur_ext_read_size);
        buf += cur_ext_read_size;
        len -= cur_ext_read_size;
        bytes_read += cur_ext_read_size;

        while (len >= f_bsize) {
            cur_ext_offset += f_bsize;
            it = data_extents->find(cur_ext_offset);
            cur_ext = (char *)it->second;
            memcpy(buf, cur_ext, f_bsize);
            buf += f_bsize;
            len -= f_bsize;
            bytes_read += f_bsize;
        }
        if (len > 0) {
            cur_ext_offset += f_bsize;
            it = data_extents->find(cur_ext_offset);
            cur_ext = (char *)it->second;
            memcpy(buf, cur_ext, len);
            bytes_read += len;
        }

        if (f_ptr != NULL)
            f->rd_unlock();
        return bytes_read;
    }

    char *data_log = (char *)malloc(
        sizeof(char) * (right_do_read_offset - left_do_read_offset));

    // int do_read_resp = do_read(fs, index, data_log, data_len, hdr_len,
    // false);
    int do_read_resp =
        do_read(fs, index, data_log, right_do_read_offset - left_do_read_offset,
                hdr_len + left_do_read_offset, false);
    if (do_read_resp < 0) {
        log_error("do_read to S3 backend failed");
        if (f_ptr != NULL)
            f->rd_unlock();
        return -1;
    }

    memcpy(buf, data_log + offset - left_do_read_offset, len);

    free(data_log);
    data_log = NULL;
    if (f_ptr != NULL)
        f->rd_unlock();
    return len;
}

static std::vector<std::string> split(const std::string &s, char delimiter)
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
        fs_directory *dir = (fs_directory *)obj;
        dir->read_lock();
        if (dir->dirents.find(*it) == dir->dirents.end()) {
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

std::tuple<int, int, std::string> path_2_inum2(const char *path)
{
    auto pathvec = split(path, '/');
    int inum = vec_2_inum(pathvec);

    auto leaf = pathvec.back();
    pathvec.pop_back();
    int parent_inum;
    if (pathvec.size() == 0) {
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

    int inum = path_2_inum(path);
    if (inum < 0) {
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    obj_2_stat(sb, obj);

    return 0;
}

int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info *fi)
{

    int inum;
    if (fi->fh != 0) {
        inum = fi->fh;
    } else {
        inum = path_2_inum(path);
    }
    if (inum < 0) {
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_DIR) {
        return -ENOTDIR;
    }

    fs_directory *dir = (fs_directory *)obj;
    dir->read_lock();
    for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
        struct stat sb;
        auto [name, i] = *it;
        fs_obj *o = inode_map[i];
        obj_2_stat(&sb, o);
        filler(ptr, const_cast<char *>(name.c_str()), &sb, 0);
    }
    dir->read_unlock();
    return 0;
}

// -------------------------------

int fs_write(const char *path, const char *buf, size_t len, off_t offset,
             struct fuse_file_info *fi)
{

    auto t0 = std::chrono::system_clock::now();
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    auto t1 = std::chrono::system_clock::now();
    std::chrono::duration<double> diff1 = (t1 - t0) * 1000;

    int inum;
    //       1481: lk.unlock();
    /*if (fi->fh != 0) {   //TODO: bring this mechanism back
        inum = fi->fh;
    } else {
        */
    inum = path_2_inum(path);
    //}
    if (inum < 0) {
        return inum;
    }
    t0 = std::chrono::system_clock::now();
    std::chrono::duration<double> diff2 = (t0 - t1) * 1000;
    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_FILE) {
        return -EISDIR;
    }

    fs_file *f = (fs_file *)obj;

    f->read_lock();
    off_t new_size = std::max((off_t)(offset + len), (off_t)(f->size));
    f->read_unlock();
    t1 = std::chrono::system_clock::now();
    std::chrono::duration<double> diff3 = (t1 - t0) * 1000;

    int hdr_bytes = sizeof(log_record) + sizeof(log_data);
    char hdr[hdr_bytes];
    log_record *lr = (log_record *)hdr;
    log_data *ld = (log_data *)lr->data;

    lr->type = LOG_DATA;
    lr->len = sizeof(log_data);

    {
        if (started_gc) {
            f->gc_lock();
            if (writes_after_ckpt.find(inum) == writes_after_ckpt.end()) {
                std::map<int, int> *writes = new std::map<int, int>;
                (*writes)[offset] = (int)len;
                writes_after_ckpt[inum] = writes;
            } else {
                std::map<int, int> *writes = writes_after_ckpt.at(inum);
                (*writes)[offset] = (int)len;
            }
            f->gc_unlock();
        }
    }
    t0 = std::chrono::system_clock::now();
    std::chrono::duration<double> diff4 = (t0 - t1) * 1000;

    size_t obj_offset = data_log.size();

    *ld = (log_data){.inum = (uint32_t)inum,
                     .obj_offset = (uint32_t)obj_offset,
                     .file_offset = (int64_t)offset,
                     .size = (int64_t)new_size,
                     .len = (uint32_t)len};

    meta_log.append(hdr, hdr_bytes);
    data_log.append((void *)buf, len);

    // TODO: optimization - check if it extends the previous record?
    // TODO: next_s3_index conflicts with ckpt index
    extent e = {.objnum = (uint32_t)next_s3_index,
                .offset = (uint32_t)obj_offset,
                .len = (uint32_t)len};

    f->write_lock();
    // int file_len_old = f->length();
    f->size = new_size;
    f->extents.update(offset, e);
    // int overwritten_len = f->length() + len - file_len_old;
    f->write_unlock();

    write_inode(f);
    t1 = std::chrono::system_clock::now();
    std::chrono::duration<double> diff5 = (t1 - t0) * 1000;
    maybe_write(fs);

    t0 = std::chrono::system_clock::now();
    std::chrono::duration<double> diff6 = (t0 - t1) * 1000;

    std::string info = "WRITE1|Time: " + std::to_string(diff1.count()) +
                       ", Path: " + path + "\n";
    // logger->log(info);
    info = "WRITE2|Time: " + std::to_string(diff2.count()) + ", Path: " + path +
           "\n";
    // logger->log(info);
    info = "WRITE3|Time: " + std::to_string(diff3.count()) + ", Path: " + path +
           "\n";
    // logger->log(info);
    info = "WRITE4|Time: " + std::to_string(diff4.count()) + ", Path: " + path +
           "\n";
    // logger->log(info);
    info = "WRITE5|Time: " + std::to_string(diff5.count()) + ", Path: " + path +
           "\n";
    // logger->log(info);
    info = "WRITE6|Time: " + std::to_string(diff6.count()) + ", Path: " + path +
           "\n";
    // logger->log(info);
    return len;
}

// write_inode stores written inodes in a buffer "current_log_inodes"
// future writes of the same inode overwrites the previous log_record
// The buffer resets at write_everything_out when the log is committed to S3
void write_inode(fs_obj *f)
{
    size_t len = sizeof(log_record) + sizeof(log_inode);
    char buf[len];
    log_record *rec = (log_record *)buf;
    log_inode *in = (log_inode *)rec->data;

    rec->type = LOG_INODE;
    rec->len = sizeof(log_inode);

    in->inum = f->inum;
    in->mode = f->mode;
    in->uid = f->uid;
    in->gid = f->gid;
    in->rdev = f->rdev;
    in->mtime = f->mtime;

    // TODO: how do we save time here?
    size_t offset = meta_log.append(rec, len);
}

void write_dirent(uint32_t parent_inum, std::string leaf, uint32_t inum)
{
    size_t len = sizeof(log_record) + sizeof(log_create) + leaf.length();
    char buf[len + sizeof(log_record)];
    log_record *rec = (log_record *)buf;
    log_create *cr8 = (log_create *)rec->data;

    rec->type = LOG_CREATE;
    rec->len = len - sizeof(log_record);
    cr8->parent_inum = parent_inum;
    cr8->inum = inum;
    cr8->namelen = leaf.length();
    memcpy(cr8->name, (void *)leaf.c_str(), leaf.length());

    make_record(rec, len, nullptr, 0);
}

int fs_mkdir(const char *path, mode_t mode)
{

    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum >= 0) {
        return -EEXIST;
    }
    if (parent_inum < 0) {
        return parent_inum;
    }

    fs_directory *parent = (fs_directory *)inode_map[parent_inum];
    if (parent->type != OBJ_DIR) {
        return -ENOTDIR;
    }

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

    set_inode_map(inum, dir);
    parent->write_lock();
    parent->dirents[leaf] = inum;
    parent->write_unlock();
    clock_gettime(CLOCK_REALTIME, &parent->mtime);

    write_inode(parent);

    write_inode(dir);
    write_dirent(parent_inum, leaf, inum);
    maybe_write(fs);

    return 0;
}

void do_log_delete(uint32_t parent_inum, uint32_t inum, std::string name)
{
    size_t len = sizeof(log_record) + sizeof(log_delete) + name.length();
    char buf[len];
    log_record *rec = (log_record *)buf;
    log_delete *del = (log_delete *)rec->data;

    rec->type = LOG_DELETE;
    rec->len = sizeof(*del) + name.length();
    del->parent = parent_inum;
    del->inum = inum;
    del->namelen = name.length();
    memcpy(del->name, (void *)name.c_str(), name.length());

    make_record(rec, len, nullptr, 0);
}

int fs_rmdir(const char *path)
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum < 0) {
        return -ENOENT;
    }
    if (parent_inum < 0) {
        return parent_inum;
    }

    fs_directory *dir = (fs_directory *)inode_map[inum];
    if (dir->type != OBJ_DIR) {
        return -ENOTDIR;
    }
    dir->read_lock();
    if (!dir->dirents.empty()) {
        dir->read_unlock();
        return -ENOTEMPTY;
    }
    dir->read_unlock();

    fs_directory *parent = (fs_directory *)inode_map[parent_inum];
    inode_map.erase(inum);
    parent->write_lock();
    parent->dirents.erase(leaf);
    parent->write_unlock();
    delete dir;

    clock_gettime(CLOCK_REALTIME, &parent->mtime);
    write_inode(parent);
    do_log_delete(parent_inum, inum, leaf);
    maybe_write(fs);
    return 0;
}

int create_node(struct objfs *fs, const char *path, mode_t mode, int type,
                dev_t dev)
{
    auto [inum, parent_inum, leaf] = path_2_inum2(path);

    if (inum >= 0) {
        return -EEXIST;
    }
    if (parent_inum < 0) {
        return parent_inum;
    }

    fs_directory *dir = (fs_directory *)inode_map[parent_inum];
    if (dir->type != OBJ_DIR)
        return -ENOTDIR;

    inum = next_inode++;
    fs_file *f = new fs_file; // yeah, OBJ_OTHER gets a useless extent map

    f->type = type;
    f->inum = inum;
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

    write_inode(f);
    write_dirent(parent_inum, leaf, inum);

    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    write_inode(dir);

    maybe_write(fs);

    return inum;
}

// only called for regular files
int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    int inum = create_node(fs, path, mode | S_IFREG, OBJ_FILE, 0);

    if (inum <= 0) {
        return inum;
    }

    fi->fh = inum;
    return 0;
}

// for device files, FIFOs, etc.
int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

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
    log_record *rec = (log_record *)buf;
    log_trunc *tr = (log_trunc *)rec->data;

    rec->type = LOG_TRUNC;
    rec->len = sizeof(log_trunc);
    tr->inum = inum;
    tr->new_size = offset;

    make_record(rec, len, nullptr, 0);
}

int fs_truncate(const char *path, off_t len)
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    int inum = path_2_inum(path);
    if (inum < 0) {
        return inum;
    }

    fs_file *f = (fs_file *)inode_map[inum];

    if (f->type == OBJ_DIR) {
        return -EISDIR;
    }
    if (f->type != OBJ_FILE) {
        return -EINVAL;
    }

    {
        if (started_gc) {
            f->gc_lock();
            if (truncs_after_ckpt.find(inum) == truncs_after_ckpt.end()) {
                truncs_after_ckpt[inum] = (int)len;
            } else {
                if (len < truncs_after_ckpt[inum]) {
                    truncs_after_ckpt[inum] = (int)len;
                }
            }
            f->gc_unlock();
        }
    }

    do_trunc(f, len);
    do_log_trunc(inum, len);

    clock_gettime(CLOCK_REALTIME, &f->mtime);
    write_inode(f);
    maybe_write(fs);

    return 0;
}

/* do I need a parent inum in fs_obj?
 */
int fs_unlink(const char *path)
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum < 0) {
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    if (obj->type == OBJ_DIR) {
        return -EISDIR;
    }

    fs_directory *dir = (fs_directory *)inode_map[parent_inum];

    inode_map.erase(inum);

    dir->write_lock();
    dir->dirents.erase(leaf);
    dir->write_unlock();
    clock_gettime(CLOCK_REALTIME, &dir->mtime);

    write_inode(dir);

    if (obj->type == OBJ_FILE) {
        fs_file *f = (fs_file *)obj;

        {
            if (started_gc) {
                f->gc_lock();
                truncs_after_ckpt[inum] = 0;
                f->gc_unlock();
            }
        }
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
    int len = sizeof(log_record) + sizeof(log_rename) + src_leaf.length() +
              dst_leaf.length();
    char buf[len];
    log_record *rec = (log_record *)buf;
    log_rename *mv = (log_rename *)rec->data;

    rec->type = LOG_RENAME;
    rec->len = len - sizeof(log_record);

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
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    auto [src_inum, src_parent, src_leaf] = path_2_inum2(src_path);
    if (src_inum < 0) {
        return src_inum;
    }

    auto [dst_inum, dst_parent, dst_leaf] = path_2_inum2(dst_path);
    if (dst_inum >= 0) {
        return -EEXIST;
    }
    if (dst_parent < 0) {
        return dst_parent;
    }

    fs_directory *srcdir = (fs_directory *)inode_map[src_parent];
    fs_directory *dstdir = (fs_directory *)inode_map[dst_parent];

    if (dstdir->type != OBJ_DIR) {
        return -ENOTDIR;
    }

    srcdir->write_lock();
    srcdir->dirents.erase(src_leaf);
    srcdir->write_unlock();
    clock_gettime(CLOCK_REALTIME, &srcdir->mtime);
    write_inode(srcdir);

    dstdir->write_lock();
    dstdir->dirents[dst_leaf] = src_inum;
    dstdir->write_unlock();
    clock_gettime(CLOCK_REALTIME, &dstdir->mtime);
    write_inode(dstdir);

    do_log_rename(src_inum, src_parent, dst_parent, src_leaf, dst_leaf);
    maybe_write(fs);
    return 0;
}

int fs_chmod(const char *path, mode_t mode)
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    int inum = path_2_inum(path);
    if (inum < 0) {
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    obj->mode = mode | (S_IFMT & obj->mode);
    write_inode(obj);
    maybe_write(fs);
    return 0;
}

// see utimensat(2). Oh, and I hate access time...
//
int fs_utimens(const char *path, const struct timespec tv[2])
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    int inum = path_2_inum(path);
    if (inum < 0) {
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    if (tv == NULL || tv[1].tv_nsec == UTIME_NOW)
        clock_gettime(CLOCK_REALTIME, &obj->mtime);
    else if (tv[1].tv_nsec != UTIME_OMIT)
        obj->mtime = tv[1];
    write_inode(obj);
    maybe_write(fs);

    return 0;
}

int fs_open(const char *path, struct fuse_file_info *fi)
{
    int inum = path_2_inum(path);
    if (inum <= 0) {
        return inum;
    }
    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_FILE) {
        return -ENOTDIR;
    }

    fi->fh = inum;

    return 0;
}

int fs_read(const char *path, char *buf, size_t len, off_t offset,
            struct fuse_file_info *fi)
{
    // logger->log("===fs_read start" + std::string(path) + "\n");
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;
    int inum = path_2_inum(path);

    if (inum < 0) {
        log_error("read: path_2_inum failed %i", inum);
        return inum;
    }

    fs_obj *obj = inode_map[inum];
    if (obj->type != OBJ_FILE) {
        return -ENOTDIR;
    }
    fs_file *f = (fs_file *)obj;

    size_t bytes = 0;

    f->read_lock();
    for (auto it = f->extents.lookup(offset); len > 0 && it != f->extents.end();
         it++) {
        auto [base, e] = *it;
        if (base > offset) {
            // yow, not supposed to have holes
            size_t skip =
                base - offset; // bytes to skip forward from current 'offset'
            if (skip > len)    // extent is past end of requested read
                skip = len;
            bytes += skip;
            offset += skip;
            buf += skip;
            len -= skip;
        } else {
            size_t skip = offset - base;
            size_t _len =
                e.len -
                skip; // length of buffer to consume (unmapped=skip,mapped)
            if (_len > len)
                _len = len;
            if (read_data(fs, f, buf, e.objnum, e.offset + skip, _len) < 0) {
                f->read_unlock();
                log_error("read_data failed");
                return -EIO;
            }
            bytes += _len;
            offset += _len;
            buf += _len;

            len -= _len;
        }
    }
    f->read_unlock();
    return bytes;
}

void write_symlink(int inum, std::string target)
{
    size_t len = sizeof(log_record) + sizeof(log_symlink) + target.length();
    char buf[sizeof(log_record) + len];
    log_record *rec = (log_record *)buf;
    log_symlink *l = (log_symlink *)rec->data;

    rec->type = LOG_SYMLNK;
    rec->len = len;
    l->inum = inum;
    l->len = target.length();
    memcpy(l->target, target.c_str(), l->len);

    make_record(rec, len, nullptr, 0);
}

int fs_symlink(const char *path, const char *contents)
{
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;

    auto [inum, parent_inum, leaf] = path_2_inum2(path);
    if (inum >= 0) {
        return -EEXIST;
    }
    if (parent_inum < 0) {
        return parent_inum;
    }

    fs_directory *dir = (fs_directory *)inode_map[parent_inum];
    if (dir->type != OBJ_DIR) {
        return -ENOTDIR;
    }

    fs_link *l = new fs_link;
    l->type = OBJ_SYMLINK;
    l->inum = next_inode++;
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

    write_inode(l);
    write_symlink(inum, leaf);
    write_dirent(parent_inum, leaf, inum);

    clock_gettime(CLOCK_REALTIME, &dir->mtime);
    write_inode(dir);
    maybe_write(fs);
    return 0;
}

int fs_readlink(const char *path, char *buf, size_t len)
{
    int inum = path_2_inum(path);
    if (inum < 0) {
        return inum;
    }

    fs_link *l = (fs_link *)inode_map[inum];
    if (l->type != OBJ_SYMLINK) {
        return -EINVAL;
    }

    size_t val = std::min(len, l->target.length());
    memcpy(buf, l->target.c_str(), val);
    return val;
}

#include <sys/statvfs.h>

/* once we're tracking objects I can iterate over them
 */
int fs_statfs(const char *path, struct statvfs *st)
{
    st->f_bsize = f_bsize;
    st->f_blocks = 0;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_namemax = 255;
    return 0;
}

void checkpoint(struct objfs *fs)
{

    put_before_ckpt = false;
    int ckpted_s3_index;
    if (data_log.size() != 0 && meta_log.size() != 0) {
        ckpted_s3_index =
            next_s3_index.load(); // When fs_init, do not load objects with
                                  // index [0, ckpted_s3_index]

        write_everything_out(fs);
    } else { // If there is nothing in log, then don't write_everything_out
        ckpted_s3_index = next_s3_index.load() - 1;
        put_before_ckpt = true;
    }

    if (ckpt_index == -1) {
        ckpt_index++;
    }
    ckpt_index++;
    std::string key =
        fmt::format("{}.{:08x}.ck", fs->prefix, ckpt_index.load());

    ckpt_header h;
    std::stringstream objs;
    int objs_size = serialize_all(ckpted_s3_index, &h, objs);
    std::string str = objs.str();

    struct iovec iov[2] = {
        {.iov_base = (void *)&h, .iov_len = sizeof(h)},
        {.iov_base = (void *)(str.c_str()), .iov_len = (size_t)objs_size}};

    started_gc = true;

    printf("writing %s, objs size: %d\n", key.c_str(), objs_size);
    auto status = fs->s3->s3_put(key, iov, 2);
    if (status != S3StatusOK) {
        log_error("put failed with status %s\n", S3_get_status_name(status));
        throw "put failed";
    }

    writes_after_ckpt.clear();
    truncs_after_ckpt.clear();
    gc_range = ckpted_s3_index;
}

void checkpoint_timer(struct objfs *fs)
{
    using namespace std::literals::chrono_literals;
    while (true) {
        checkpoint(fs);
        if (quit_ckpt) {
            quit_ckpt = false;
            printf("quit checkpoint_timer\n");
            return;
        }
    }
}

void gc_timer(struct objfs *fs)
{
    using namespace std::literals::chrono_literals;
    int prev_gc_range = 0;
    while (true) {
        if (quit_gc) {
            quit_gc = false;
            printf("quit gc_timer\n");
            return;
        }
        // gc_file(fs);
        gc_obj(fs, gc_range);
        prev_gc_range = gc_range;
    }
}

void gc_write(struct objfs *fs, void *bufv, size_t len, int inum,
              int file_offset)
{
    char *buf = (char *)bufv;
    fs_obj *obj = inode_map[inum];
    if (obj == NULL || obj->type != OBJ_FILE) {
        return;
    }

    fs_file *f = (fs_file *)obj;
    // TODO: improve started_gc_mutex
    // started_gc_mutex.lock();
    f->gc_lock();
    if (truncs_after_ckpt.find(inum) != truncs_after_ckpt.end()) {
        int t_len = truncs_after_ckpt.at(inum);
        if (t_len <= file_offset) {
            // started_gc_mutex.unlock();
            f->gc_unlock();
            return;
        }
        if (t_len <= file_offset + (int)len) {
            len = t_len - file_offset;
        }
    }
    std::map<int, int> *writes;
    if (writes_after_ckpt.find(inum) != writes_after_ckpt.end()) {
        writes = writes_after_ckpt[inum];

        int w_offset, w_len;
        for (auto it = writes->begin(); it != writes->end(); it++) {
            w_offset = it->first;
            w_len = it->second;

            if (w_offset + w_len <= file_offset)
                continue;
            if (w_offset >= file_offset + (int)len)
                break;
            int skip = w_offset + w_len - file_offset;
            if (w_offset <= file_offset) {
                buf = buf + skip;
                file_offset += skip;
                len -= skip;
                continue;
            }

            gc_write_extent(buf, inum, file_offset, w_offset - file_offset);
            buf = buf + skip;
            file_offset += skip;
            len -= skip;
        }
    }
    if (len > 0) {
        gc_write_extent(buf, inum, file_offset, len);
    }
    // started_gc_mutex.unlock();
    f->gc_unlock();
    maybe_write(fs);
}

void gc_write_extent(void *buf, int inum, int file_offset, int len)
{
    int hdr_bytes = sizeof(log_record) + sizeof(log_data);
    char hdr[hdr_bytes];
    log_record *lr = (log_record *)hdr;
    log_data *ld = (log_data *)lr->data;

    lr->type = LOG_DATA;
    lr->len = sizeof(log_data);

    fs_file *f = (fs_file *)inode_map[inum];
    f->read_lock();
    off_t new_size = std::max((off_t)(file_offset + len), (off_t)(f->size));
    f->read_unlock();

    size_t obj_offset = data_log.size();
    *ld = (log_data){.inum = (uint32_t)inum,
                     .obj_offset = (uint32_t)obj_offset,
                     .file_offset = (int64_t)file_offset,
                     .size = (int64_t)new_size,
                     .len = (uint32_t)len};

    meta_log.append(hdr, hdr_bytes);
    data_log.append(buf, len);

    extent e = {.objnum = (uint32_t)next_s3_index,
                .offset = (uint32_t)obj_offset,
                .len = (uint32_t)len};

    f->write_lock();
    f->size = new_size;
    f->extents.update(file_offset, e);
    f->write_unlock();
}

void gc_read_write(struct objfs *fs, int filenum, void *infos_ptr,
                   std::set<int> *objnum_to_delete)
{
    std::vector<gc_info *> *infos = (std::vector<gc_info *> *)infos_ptr;
    if (infos == NULL || infos->size() == 0)
        return;
    for (auto it = infos->begin(); it != infos->end(); it++) {
        gc_info *info = *it;
        if (objnum_to_delete->find(info->objnum) == objnum_to_delete->end())
            continue;
        char buf[info->len];
        read_data(fs, NULL, (void *)buf, info->objnum, info->obj_offset,
                  info->len);
        // do_read(fs, info->objnum, (void *)buf, (size_t)info->len,
        // (size_t)info->obj_offset, false);
        gc_write(fs, (void *)buf, info->len, filenum, info->file_offset);
    }
}

void get_total_file_size(int inum, std::map<int, int> &total_file_sizes,
                         std::map<int, std::vector<gc_info *> *> &gc_infos)
{
    fs_obj *obj = inode_map[inum];

    if (obj->type == OBJ_FILE) {
        fs_file *file = (fs_file *)obj;
        for (auto it = file->extents.begin(); it != file->extents.end(); it++) {
            auto [file_offset, ext] = *it;
            if (total_file_sizes.find(ext.objnum) == total_file_sizes.end()) {
                total_file_sizes[ext.objnum] = ext.len;
            } else {
                total_file_sizes[ext.objnum] =
                    total_file_sizes[ext.objnum] + ext.len;
            }
            if (gc_infos.find(inum) == gc_infos.end()) {
                std::vector<gc_info *> *infos = new std::vector<gc_info *>;
                gc_infos[inum] = infos;
            }
            gc_info *info = (gc_info *)malloc(sizeof(gc_info));
            info->objnum = ext.objnum;
            info->file_offset = file_offset;
            info->obj_offset = ext.offset;
            info->len = ext.len;
            // gc_infos[ext.objnum]->push_back(info);
            gc_infos[inum]->push_back(info);
        }
    } else if (obj->type == OBJ_DIR) {
        fs_directory *dir = (fs_directory *)obj;
        for (auto it = dir->dirents.begin(); it != dir->dirents.end(); it++) {
            auto [name, inum2] = *it;
            get_total_file_size(inum2, total_file_sizes, gc_infos);
        }
    }
}

void gc_obj(struct objfs *fs, int ckpted_s3_index)
{

    int root_inum = 2;

    std::map<int, int> total_file_sizes; // keys are obj No.s, vals are total
                                         // valid data sizes per obj
    std::map<int, std::vector<gc_info *> *> gc_infos;
    get_total_file_size(root_inum, total_file_sizes, gc_infos);

    std::list<std::string> keys;
    if (S3StatusOK != fs->s3->s3_list(fs->prefix, keys))
        throw "bucket list failed";

    if (quit_gc) {
        return;
    }

    int n;
    char postfix[10];
    std::set<int> objnum_to_delete;

    for (auto it = keys.begin(); it != keys.end(); it++) {
        sscanf(it->c_str(), "%*[^.].%08x%s", &n, postfix);
        if (strcmp(postfix, ".ck") == 0) {
            // TODO: remove ckpt objs
            postfix[0] = '\0';
            continue;
        }
        if (n == 0)
            continue;
        if (n > ckpted_s3_index)
            break;

        float util_rate =
            (float)total_file_sizes[n] / (float)get_datalen(fs, n, false);
        printf("OBJ NUM %d UTIL RATE %f\n", n, util_rate);
        if (util_rate > 0.8) {
            continue;
        }
        objnum_to_delete.insert(n);
        printf("OBJECT %s to be deleted\n", it->c_str());
    }

    started_gc = false;
    if (objnum_to_delete.size() > 0) {
        for (auto it = gc_infos.begin(); it != gc_infos.end(); it++) {
            int filenum = it->first;
            std::vector<gc_info *> *infos = it->second;
            gc_read_write(fs, filenum, (void *)infos, &objnum_to_delete);
            // printf("FILE %d to be GCed\n", filenum);
        }
    }

    gc_infos.clear();

    if (data_log.size() != 0 && meta_log.size() != 0) {

        printf("Additional GC write everything out\n");
        write_everything_out(fs);
    }

    postfix[0] = '\0';
    // TODO: maybe remove checkpoints
    // Remove old objects where we already read and wrote valid data again in
    // new objects
    for (std::set<int>::iterator it = objnum_to_delete.begin();
         it != objnum_to_delete.end(); ++it) {
        n = *it;

        std::string key = fmt::format("{}.{:08x}", fs->prefix, n);
        auto status = fs->s3->s3_delete(key);

        if (S3StatusOK != status) {
            log_error("delete failed");
            throw "delete failed";
        }
    }
    printf("GC obj finished\n");
}

void *fs_init(struct fuse_conn_info *conn)
{
    struct objfs *fs = (objfs *)malloc(sizeof(objfs));
    fs = (objfs *)fuse_get_context()->private_data;

    f_bsize = fs->chunk_size; // 4096;
    // fifo_limit = fs->cache_size;
    // logger = new Logger("/mnt/ramdisk/log_" + std::to_string(f_bsize) + "_" +
    //                   std::to_string(CACHE_SIZE) + ".txt");

    fs->s3 = new S3Wrap(fs->host, fs->bucket, fs->access, fs->secret, false);

    std::list<std::string> keys;
    if (S3StatusOK != fs->s3->s3_list(fs->prefix, keys))
        throw "bucket list failed";

    int last_ckpt_index = -1;
    char postfix[10];
    for (std::list<std::string>::const_iterator it = keys.end();
         it != keys.begin();) {
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
        int len =
            do_read(fs, last_ckpt_index, &ckpt_h, sizeof(ckpt_h), 0, true);
        if (len < 0)
            throw "can't read ckpt header";

        size_t objfs_itable_size =
            ckpt_h.itable_offset - sizeof(ckpt_h) + ckpt_h.itable_len;
        buf = malloc(objfs_itable_size);
        len = do_read(fs, last_ckpt_index, buf, objfs_itable_size,
                      sizeof(ckpt_h), true);
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
        if (n <= (int)ckpt_h.ckpted_s3_index)
            continue;
        offset = get_offset(fs, n, false);

        if (offset < 0)
            throw "bad object";
        buf = malloc(offset);
        struct iovec iov[] = {{.iov_base = buf, .iov_len = (size_t)offset}};
        // logger->log("read header from s3\n");
        auto status = fs->s3->s3_get(it->c_str(), 0, offset, iov, 1);
        if (S3StatusOK != status)
            throw "can't read header";
        if (read_hdr(n, buf, offset) < 0)
            throw "bad header";
        next_s3_index = std::max(n + 1, next_s3_index.load());
    }

    // Put the root dir "" into inode map with inum = 2
    if (inode_map.find(2) == inode_map.end()) {
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

    started_gc = false;
    // TODO: Only libobjfs inits set reserved[0] to 9, this prevents objfs-mount
    // from starting the checkpointing thread and makes testing easier (for now)
    // if (conn->reserved[0] == 9) {
    // std::thread(fifo).detach();
    // std::thread(checkpoint_timer, fs).detach();
    // std::thread(gc_timer, fs).detach();
    //}

    return (void *)fs;
}

void fs_teardown(void *foo)
{
    quit_ckpt = true;
    quit_gc = true;
    quit_fifo = true;
    // {
    //     std::unique_lock<std::mutex> lk(cv_m);
    //     cv.wait(lk, [] { return (!quit_fifo) && (!quit_ckpt) && (!quit_gc);
    //     }); printf("quit ckpter & fifo & gc finished in fs_teardown\n");
    // }

    for (auto it = inode_map.begin(); it != inode_map.end();
         it = inode_map.erase(it))
        ;

    next_s3_index = 1;
    for (auto it = data_offsets.begin(); it != data_offsets.end();
         it = data_offsets.erase(it))
        ;
    for (auto it = data_lens.begin(); it != data_lens.end();
         it = data_lens.erase(it))
        ;
}

int fs_fsync(const char *path, int, struct fuse_file_info *fi)
{
    return 0;
    struct objfs *fs = (struct objfs *)fuse_get_context()->private_data;
    write_everything_out(fs);
    return 0;
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

    write_inode(root);

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
