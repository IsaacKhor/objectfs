#pragma once

#include <expected>
#include <iterator>
#include <libs3.h>
#include <string>
#include <unordered_set>
#include <vector>

#include "models.hpp"

// 'objectfs' = ['0x6f', '0x62', '0x6a', '0x65', '0x63', '0x74', '0x66', '0x73']
const uint64_t OBJECTFS_MAGIC = 0x73667463656a626f;

struct S3ObjInfo {
    std::string key;
    uint64_t size;
};

class S3ObjectStore
{
  private:
    S3BucketContext bucket_ctx;
    S3ResponseHandler resp_handler;
    std::string host, bucket, access, secret;
    S3Protocol protocol;

  public:
    S3ObjectStore(std::string _host, std::string _bucket_name,
                  std::string _access_key, std::string _secret_key,
                  bool encrypted);
    ~S3ObjectStore();

    S3Status get(std::string key, size_t offset, void *buf, size_t len);
    S3Status put(std::string key, void *buf, size_t len);
    S3Status del(std::string key);

    /**
     * List all keys in the bucket. Blocks until we're done and throws if the
     * request failed.
     */
    std::vector<S3ObjInfo> list(std::string prefix);
};

class ObjectBackend
{
  private:
    S3ObjectStore s3;
    LRUCache<std::pair<objectid_t, size_t>, byte *> cache;
    ConcurrentMap<objectid_t, std::shared_ptr<byte[]>> pending_queue;

    // GC
    std::mutex collected_mtx;
    std::unordered_set<objectid_t> collected_objs;

    // log
    objectid_t active_object_id = 16;
    std::byte *log;
    std::atomic_size_t log_len = sizeof(BackendObjectHeader);
    std::shared_mutex log_mutex;

    size_t log_capacity;
    size_t log_rollover_threshold;

    bool cache_disabled = true;

    std::string get_obj_name(objectid_t id);

    ObjectSegment append_fixed(size_t len, void *buf);
    ObjectSegment append_fixed_2(size_t len1, void *buf1, size_t len2,
                                 void *buf2);

  public:
    explicit ObjectBackend(S3ObjectStore s3, size_t cache_size,
                           size_t log_capacity, size_t log_rollover_threshold);
    ~ObjectBackend();

    inline void set_active_id(objectid_t id) { active_object_id = id; }

    std::pair<std::vector<LogObjectVar>, std::unique_ptr<byte[]>>
        fetch_and_parse_object(S3ObjInfo);

    /**
     * Force a rollover and push to the backend of the current log. Will
     * block until the HTTP request comes back.
     */
    void rollover_log();
    void maybe_rollover();

    std::optional<objectid_t> parse_obj_name(std::string name);

    /**
     * Designates an object as having been collected by the garbage collector.
     * This means that we can delete it from the backend the next time we
     * write out a checkpoint, as that is the point at which changes to the
     * inode map are persisted.
     */
    void add_collected_obj(objectid_t id);
    bool is_collectable(objectid_t id);

    /**
     * Get a specific segment of an object. We first check the cache for the
     * segment, and if it's not there, we read it from the backend and cache it.
     *
     * @param seg The segment to read.
     * @param buf The buffer to read into. Must be at least seg.len bytes long.
     *
     * TODO: return some sort of indication about whether we succeded
     */
    bool get_obj_segment(ObjectSegment seg, byte *buf);

    /**
     * Append a log operation to the log. This returns the raw object segment
     * (including the object header) where the log object itself was written
     * to on the log.
     *
     * How this works is that the Log___ object is partially constructed by the
     * caller of the functions, and then the object is fully constructed
     * in-place on the log itself. We do it this way to prevent redundant
     * copying and to make it easier to append variable sized log objects.
     *
     * Not the cleanest implementation, should rewrite it later.
     */
    ObjectSegment append_logobj(LogTruncateFile &logobj);
    ObjectSegment append_logobj(LogChangeFilePerms &logobj);
    ObjectSegment append_logobj(LogChangeFileOwners &logobj);
    ObjectSegment append_logobj(LogMakeDirectory &logobj, std::string name);
    ObjectSegment append_logobj(LogRemoveDirectory &logobj);
    ObjectSegment append_logobj(LogCreateFile &logobj, std::string name);
    ObjectSegment append_logobj(LogRemoveFile &logobj);

    /**
     * Special method to append a LogSetFileData log object in-place to reduce
     * copying. Same as the other append_logobj's in all other aspects
     *
     * Will fill in the data_obj_offset field in-place before appending
     * to the log.
     *
     * When writing this to the file extent map for file write operations,
     * remember to offset by `offsetof` the correct struct field for the actual
     * data for the correct ObjectSegment where the data lives and to reduce
     * the length by the size of the log object header.
     */
    ObjectSegment append_logobj(LogSetFileData &logobj, size_t data_len,
                                void *buf);
};
