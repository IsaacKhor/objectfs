#pragma once

#include <iterator>
#include <libs3.h>
#include <string>
#include <vector>

#include "models.hpp"
#include "s3wrap.hpp"

// 'objectfs' = ['0x6f', '0x62', '0x6a', '0x65', '0x63', '0x74', '0x66', '0x73']
const uint64_t OBJECTFS_MAGIC = 0x73667463656a626f;

class S3ObjectStore
{
  private:
    S3Wrap s3wrap;

  public:
    S3ObjectStore(std::string _host, std::string _bucket_name,
                  std::string _access_key, std::string _secret_key,
                  bool encrypted);
    ~S3ObjectStore();

    S3Status get(std::string key, size_t offset, void *buf, size_t len);
    S3Status put(std::string key, void *buf, size_t len);
    S3Status del(std::string key);
    S3Status list(std::string prefix, std::vector<std::string> &keys);
};

class ObjectBackend
{
  private:
    S3ObjectStore s3;
    LRUCache<std::pair<objectid_t, size_t>, byte *> cache;
    ConcurrentMap<objectid_t, std::shared_ptr<byte[]>> pending_queue;

    // log
    objectid_t active_object_id = 16;
    std::byte *log;
    std::atomic_size_t log_len = sizeof(BackendObjectHeader);
    std::shared_mutex log_mutex;

    size_t log_capacity;
    size_t log_rollover_threshold;

    bool cache_disabled = true;

    std::string get_obj_name(objectid_t id);
    std::optional<objectid_t> parse_obj_name(std::string name);

  public:
    explicit ObjectBackend(S3ObjectStore s3, size_t cache_size,
                           size_t log_capacity, size_t log_rollover_threshold);
    ~ObjectBackend();

    std::pair<std::vector<LogObjectVar>, std::unique_ptr<byte[]>>
    fetch_and_parse_object(std::string);

    /**
     * Force a rollover and push to the backend of the current log. Will
     * block until the HTTP request comes back.
     */
    void rollover_log();
    void maybe_rollover();

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
     * This is a bit of a hack, but for structs with variable sized components
     * we pass them in separately to prevent redundant copying. Should rewrite
     * it to something cleaner later.
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
     * When writing this to the file extent map for file write operations,
     * remember to offset by `offsetof` the correct struct field for the actual
     * data for the correct ObjectSegment where the data lives and to reduce
     * the length by the size of the log object header.
     */
    ObjectSegment append_logobj(LogSetFileData &logobj, size_t data_len,
                                void *buf);

    ObjectSegment append_fixed(size_t len, void *buf);
    ObjectSegment append_fixed_2(size_t len1, void *buf1, size_t len2,
                                 void *buf2);
};
