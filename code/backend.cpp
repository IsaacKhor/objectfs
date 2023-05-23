#include "backend.hpp"
#include <fmt/core.h>

S3ObjectStore::S3ObjectStore(std::string _host, std::string _bucket_name,
                             std::string _access_key, std::string _secret_key,
                             bool encrypted)
    : s3wrap(_host.c_str(), _bucket_name.c_str(), _access_key.c_str(),
             _secret_key.c_str(), encrypted)
{
}

S3ObjectStore::~S3ObjectStore() {}

S3Status S3ObjectStore::get(std::string key, size_t offset, void *buf,
                            size_t len)
{
    iovec iov = {.iov_base = buf, .iov_len = len};
    return s3wrap.s3_get(key, offset, len, &iov, 1);
}

S3Status S3ObjectStore::put(std::string key, void *buf, size_t len)
{
    iovec iov = {.iov_base = buf, .iov_len = len};
    return s3wrap.s3_put(key, &iov, 1);
}

S3Status S3ObjectStore::del(std::string key) { return s3wrap.s3_delete(key); }

S3Status S3ObjectStore::list(std::string prefix, std::vector<std::string> &keys)
{
    std::list<std::string> keys_list;
    S3Status status = s3wrap.s3_list(prefix, keys_list);
    if (status != S3StatusOK)
        return status;

    keys = std::vector<std::string>(keys_list.begin(), keys_list.end());
    return S3StatusOK;
}

ObjectBackend::ObjectBackend(S3ObjectStore s3, size_t cache_size,
                                      size_t log_capacity,
                                      size_t log_rollover_threshold)
    : s3(s3), log(new std::byte[log_capacity]), log_capacity(log_capacity),
      log_rollover_threshold(log_rollover_threshold)
{
}

ObjectBackend::~ObjectBackend() { delete[] log; }

std::string ObjectBackend::get_obj_name(objectid_t id)
{
    return fmt::format("objectfs_{:016x}.log", id);
}

void ObjectBackend::get_obj_segment(ObjectSegment seg, byte *buf)
{
    // TODO implement caching

    auto key = get_obj_name(seg.object_id);
    s3.get(key, seg.offset, buf, seg.len);
}

ObjectSegment ObjectBackend::append_fixed(size_t len, void *data)
{
    ObjectSegment ret;
    {
        std::shared_lock<std::shared_mutex> lock(log_mutex);
        ret.object_id = active_object_id;
        ret.offset = log_len.fetch_add(len);
        ret.len = len;
        memcpy(log + ret.offset, data, len);
    }

    maybe_rollover();
    return ret;
}

ObjectSegment ObjectBackend::append_fixed_2(size_t len1, void *buf1,
                                            size_t len2, void *buf2)
{
    ObjectSegment ret;
    {
        std::shared_lock<std::shared_mutex> lock(log_mutex);
        ret.object_id = active_object_id;
        ret.len = len1 + len2;
        ret.offset = log_len.fetch_add(ret.len);
        memcpy(log + ret.offset, buf1, len1);
        memcpy(log + ret.offset + len1, buf2, len2);
    }

    maybe_rollover();
    return ret;
}

void ObjectBackend::maybe_rollover()
{
    if (log_len.load() > log_rollover_threshold) {
        rollover_log();
    }
}

void ObjectBackend::rollover_log()
{
    auto new_log = new std::byte[log_capacity];

    size_t old_len;
    objectid_t old_id;
    byte *old_log;
    {
        std::unique_lock<std::shared_mutex> lock(log_mutex);
        old_len = log_len.exchange(0);
        old_id = active_object_id;
        old_log = log;

        active_object_id += 1;
        log = new_log;
        // We pre-allocate space for the header
        log_len = sizeof(BackendObjectHeader);

        std::shared_ptr<byte[]> old_log_ptr(old_log);
        pending_queue.insert(old_id, std::move(old_log_ptr));
    }

    // Fill in details for the header
    BackendObjectHeader *hdr = reinterpret_cast<BackendObjectHeader *>(old_log);
    hdr->magic = OBJECTFS_MAGIC;
    hdr->object_id = old_id;
    hdr->last_checkpoint_id = 0;
    hdr->len = old_len;

    // push to backend
    auto object_name = get_obj_name(old_id);
    s3.put(object_name, log, log_len);

    // We only erase from the queue *after* the push is complete
    // TODO consider directly putting all the blocks into the cache
    pending_queue.erase(old_id);
}

ObjectSegment ObjectBackend::append_logobj(LogTruncateFile &logobj)
{
    return append_fixed(sizeof(logobj), static_cast<void *>(&logobj));
}

ObjectSegment ObjectBackend::append_logobj(LogChangeFilePerms &logobj)
{
    return append_fixed(sizeof(logobj), static_cast<void *>(&logobj));
}

ObjectSegment ObjectBackend::append_logobj(LogChangeFileOwners &logobj)
{
    return append_fixed(sizeof(logobj), static_cast<void *>(&logobj));
}

ObjectSegment ObjectBackend::append_logobj(LogRemoveDirectory &logobj)
{
    return append_fixed(sizeof(logobj), static_cast<void *>(&logobj));
}

ObjectSegment ObjectBackend::append_logobj(LogRemoveFile &logobj)
{
    return append_fixed(sizeof(logobj), static_cast<void *>(&logobj));
}

ObjectSegment ObjectBackend::append_logobj(LogSetFileData &logobj, size_t len,
                                           void *data)
{
    return append_fixed_2(sizeof(LogSetFileData), &logobj, len, data);
}

ObjectSegment ObjectBackend::append_logobj(LogMakeDirectory &logobj,
                                           std::string name)
{
    return append_fixed_2(sizeof(LogMakeDirectory), &logobj, name.size(),
                          name.data());
}

ObjectSegment ObjectBackend::append_logobj(LogCreateFile &logobj,
                                           std::string name)
{
    return append_fixed_2(sizeof(LogCreateFile), &logobj, name.size(),
                          name.data());
}