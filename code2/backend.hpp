#pragma once

#include <libs3.h>
#include <string>
#include <vector>

#include "models.hpp"

class S3ObjectStore
{
  private:
    std::string host;
    std::string bucket_name;
    std::string access_key;
    std::string secret_key;

    S3Protocol protocol;

  public:
    S3ObjectStore(std::string _host, std::string _bucket_name,
                  std::string _access_key, std::string _secret_key,
                  bool encrypted);

    S3Status get(std::string key, ssize_t offset, ssize_t len,
                 struct iovec *iov, int iov_cnt);
    S3Status put(std::string key, struct iovec *iov, int iov_cnt);
    S3Status del(std::string key);
    S3Status head(std::string key, ssize_t *p_len);
    S3Status list(std::string prefix, std::vector<std::string> &keys);
};

class ObjectBackend
{
  private:
    S3ObjectStore s3;
    LRUCache<objectid_t, byte *> cache;
    objectid_t active_object_id;

    // log
    std::byte *log;
    size_t log_len;
    size_t log_capacity;
    size_t log_rollover_threshold;

  public:
    explicit ObjectBackend(S3ObjectStore s3, size_t cache_size,
                           size_t log_capacity, size_t log_rollover_threshold);

    /**
     * Force a rollover and push to the backend of the current log. Will block
     * until the HTTP request comes back.
     */
    void rollover_log();

    /**
     * Get a specific segment of an object. We first check the cache for the
     * segment, and if it's not there, we read it from the backend and cache it.
     *
     * @param seg The segment to read.
     * @param buf The buffer to read into. Must be at least seg.len bytes long.
     *
     * TODO: return some sort of indication about whether we succeded
     */
    void get_obj_segment(ObjectSegment seg, byte *buf);

    /**
     * Append a log operation to the log. This returns the raw object segment
     * (not including the object header) where the log object itself was written
     * to on the log.
     */
    ObjectSegment &append_logobj(LogTruncateFile &logobj);
    ObjectSegment &append_logobj(LogChangeFilePerms &logobj);
    ObjectSegment &append_logobj(LogChangeFileOwners &logobj);
    ObjectSegment &append_logobj(LogMakeDirectory &logobj);
    ObjectSegment &append_logobj(LogRemoveDirectory &logobj);
    ObjectSegment &append_logobj(LogCreateFile &logobj);
    ObjectSegment &append_logobj(LogRemoveFile &logobj);

    /**
     * Special method to append a LogSetFileData log object in-place to reduce
     * copying. Same as append_logobj in all other aspects
     *
     * When writing this to the file extent map for file write operations,
     * remember to offset by `offsetof` the correct struct field for the actual
     * data for the correct ObjectSegment where the data lives and to reduce
     * the length by the size of the log object header.
     */
    ObjectSegment append_data(inum_t inum, size_t file_offset, size_t data_len,
                              byte *buf);
};
