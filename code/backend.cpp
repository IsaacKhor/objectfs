#include <fmt/core.h>
#include <libs3.h>

#include "backend.hpp"
#include "utils.hpp"

ObjectBackend::ObjectBackend(S3ObjectStore s3, size_t cache_size,
                             size_t log_capacity, size_t log_rollover_threshold)
    : s3(s3), log(new std::byte[log_capacity]), log_capacity(log_capacity),
      log_rollover_threshold(log_rollover_threshold)
{
    // reserve space for the object header
    log_len.store(sizeof(BackendObjectHeader));
}

ObjectBackend::~ObjectBackend() { delete[] log; }

std::string ObjectBackend::get_obj_name(objectid_t id)
{
    return fmt::format("objectfs_{:016x}.log", id);
}

std::optional<objectid_t> ObjectBackend::parse_obj_name(std::string name)
{
    if (!name.starts_with("objectfs_"))
        return std::nullopt;
    auto id_str = name.substr(9, 25);
    auto parsed = std::stoull(id_str, nullptr, 16);
    if (parsed == 0 || parsed == ULLONG_MAX)
        return std::nullopt;
    return parsed;
}

bool ObjectBackend::get_obj_segment(ObjectSegment seg, byte *buf)
{
    // First check the active log
    {
        std::shared_lock lock(log_mutex);
        if (seg.object_id == active_object_id) {
            memcpy(buf, log + seg.offset, seg.len);
            return false;
        }
    }

    // If it's not in the active log, check pending writes
    auto pending = pending_queue.get_copy(seg.object_id);
    if (pending.has_value()) {
        auto sp = pending.value();
        memcpy(buf, sp.get() + seg.offset, seg.len);
        return false;
    }

    // If it's not pending, check our cache
    // TODO implement caching

    // If everything above fails, we have to fetch it from the backend
    // TODO after fetching, put it into the cache

    auto key = get_obj_name(seg.object_id);
    return s3.get(key, seg.offset, buf, seg.len) == S3StatusOK;
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
    // Don't flush if there's nothing in the log
    if (log_len.load() <= sizeof(BackendObjectHeader))
        return;

    auto new_log = new std::byte[log_capacity];

    size_t old_len;
    objectid_t old_id;
    byte *old_log;
    {
        std::unique_lock<std::shared_mutex> lock(log_mutex);
        if (log_len.load() <= sizeof(BackendObjectHeader)) {
            delete[] new_log;
            return;
        }

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
    debug("pushing {} to backend, len {}", object_name, hdr->len);
    s3.put(object_name, old_log, old_len);

    // We only erase from the queue *after* the push is complete
    // TODO consider directly putting all the blocks into the cache
    pending_queue.erase(old_id);
}

std::pair<std::vector<LogObjectVar>, std::unique_ptr<byte[]>>
ObjectBackend::fetch_and_parse_object(S3ObjInfo obj_info)
{
    auto object_id = parse_obj_name(obj_info.key);
    if (object_id == std::nullopt) {
        log_error("invalid object name {}", obj_info.key);
        throw std::runtime_error("invalid object id");
    }

    // Fetch the header to get the size of the log
    auto buf = new std::byte[obj_info.size];
    std::unique_ptr<byte[]> buf_ptr(buf);
    auto ret = s3.get(obj_info.key, 0, buf, obj_info.size);
    if (ret != S3StatusOK)
        throw std::runtime_error("failed to fetch object from backend");

    // Validate that data header is correct
    auto hdr = reinterpret_cast<BackendObjectHeader *>(buf);
    if (hdr->magic != OBJECTFS_MAGIC)
        throw std::runtime_error("invalid magic number in object header");
    if (hdr->object_id != object_id.value())
        throw std::runtime_error("object id mismatch in object header");

    // Fetch the rest of the object
    auto log_length = hdr->len;

    std::vector<LogObjectVar> logobjs;
    byte *log = buf + sizeof(BackendObjectHeader);
    byte *log_end = buf + log_length;

    while (log < log_end) {
        auto log_base = reinterpret_cast<LogObjectBase *>(log);
        switch (log_base->type) {
        case LogObjectType::SetFileData: {
            auto logobj = reinterpret_cast<LogSetFileData *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogSetFileData) + logobj->data_len;
            break;
        }
        case LogObjectType::TruncateFile: {
            auto logobj = reinterpret_cast<LogTruncateFile *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogTruncateFile);
            break;
        }
        case LogObjectType::ChangeFilePermissions: {
            auto logobj = reinterpret_cast<LogChangeFilePerms *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogChangeFilePerms);
            break;
        }
        case LogObjectType::ChangeFileOwners: {
            auto logobj = reinterpret_cast<LogChangeFileOwners *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogChangeFileOwners);
            break;
        }
        case LogObjectType::MakeDirectory: {
            auto logobj = reinterpret_cast<LogMakeDirectory *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogMakeDirectory) + logobj->name_len;
            break;
        }
        case LogObjectType::RemoveDirectory: {
            auto logobj = reinterpret_cast<LogRemoveDirectory *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogRemoveDirectory);
            break;
        }
        case LogObjectType::CreateFile: {
            auto logobj = reinterpret_cast<LogCreateFile *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogCreateFile) + logobj->name_len;
            break;
        }
        case LogObjectType::RemoveFile: {
            auto logobj = reinterpret_cast<LogRemoveFile *>(log);
            logobjs.push_back(logobj);
            log += sizeof(LogRemoveFile);
            break;
        }
        default:
            throw std::runtime_error("invalid log object type");
        }
    }

    return std::make_pair(std::move(logobjs), std::move(buf_ptr));
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
    ObjectSegment ret;
    {
        std::shared_lock<std::shared_mutex> lock(log_mutex);
        ret.object_id = active_object_id;
        ret.len = sizeof(logobj) + len;
        ret.offset = log_len.fetch_add(ret.len);

        // special fill in-place
        logobj.data_obj_id = ret.object_id;
        logobj.data_obj_offset =
            ret.offset + offsetof(LogSetFileData, data);

        memcpy(log + ret.offset, &logobj, sizeof(logobj));
        memcpy(log + ret.offset + sizeof(logobj), data, len);
    }

    maybe_rollover();
    return ret;
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

extern "C" S3Status callback_resp_props(const S3ResponseProperties *p,
                                        void *data);
extern "C" void callback_resp_complete(S3Status status,
                                       const S3ErrorDetails *error, void *data);

S3ObjectStore::S3ObjectStore(std::string _host, std::string _bucket_name,
                             std::string _access_key, std::string _secret_key,
                             bool encrypted)
    : host(_host), bucket(_bucket_name), access(_access_key),
      secret(_secret_key),
      protocol(encrypted ? S3ProtocolHTTPS : S3ProtocolHTTP)
{
    bucket_ctx = {
        .hostName = host.c_str(),
        .bucketName = bucket.c_str(),
        .protocol = protocol,
        .uriStyle = S3UriStylePath,
        .accessKeyId = access.c_str(),
        .secretAccessKey = secret.c_str(),
        .securityToken = NULL,
        .authRegion = NULL,
    };

    resp_handler = {
        .propertiesCallback = callback_resp_props,
        .completeCallback = callback_resp_complete,
    };

    S3_initialize("objectfs", S3_INIT_ALL, host.c_str());
}

S3ObjectStore::~S3ObjectStore()
{
    // S3_deinitialize();
}

struct S3CallbackCtx {
    S3Status request_status;
    void *buf;
    size_t requested_len;
    size_t transferred_len;
    std::optional<std::string> err_msg;
};

extern "C" S3Status callback_resp_props(const S3ResponseProperties *p,
                                        void *data)
{
    // no-op
    return S3StatusOK;
}

extern "C" void callback_resp_complete(S3Status status,
                                       const S3ErrorDetails *error, void *data)
{
    auto ctx = static_cast<S3CallbackCtx *>(data);
    ctx->request_status = status;

    if (error != NULL)
        ctx->err_msg =
            fmt::format("S3 error: resource={}, message={}, details={}",
                        error->resource || "", error->message || "",
                        error->furtherDetails || "");
    // log_error("{}", ctx->err_msg.value());
}

extern "C" S3Status receive_data_cb(int size, const char *buf, void *data)
{
    auto ctx = static_cast<S3CallbackCtx *>(data);

    // sanity bounds-check
    if (ctx->transferred_len + size > ctx->requested_len) {
        log_error("buffer overrun: requested {}, but transferred {}, size {}",
                  ctx->requested_len, ctx->transferred_len, size);
        return S3StatusAbortedByCallback;
    }

    memcpy(ctx->buf + ctx->transferred_len, buf, size);
    ctx->transferred_len += size;
    return S3StatusOK;
}

S3Status S3ObjectStore::get(std::string key, size_t offset, void *buf,
                            size_t len)
{
    S3GetObjectHandler h = {
        .responseHandler = resp_handler,
        .getObjectDataCallback = receive_data_cb,
    };

    S3CallbackCtx ctx = {
        .request_status = S3StatusOK,
        .buf = buf,
        .requested_len = len,
        .transferred_len = 0,
    };

    // trace("fetching: key={}, offset={}, len={}", key, offset, len);
    S3_get_object(&bucket_ctx, key.c_str(), NULL, offset, len, NULL, 0, &h,
                  &ctx);

    return ctx.request_status;
}

extern "C" int send_data_cb(int size, char *buf, void *data)
{
    auto ctx = static_cast<S3CallbackCtx *>(data);
    memcpy(buf, ctx->buf + ctx->transferred_len, size);
    ctx->transferred_len += size;
    return size;
}

S3Status S3ObjectStore::put(std::string key, void *buf, size_t len)
{
    S3PutObjectHandler h = {
        .responseHandler = resp_handler,
        .putObjectDataCallback = send_data_cb,
    };

    S3CallbackCtx ctx = {
        .request_status = S3StatusOK,
        .buf = buf,
        .requested_len = len,
        .transferred_len = 0,
    };

    S3_put_object(&bucket_ctx, key.c_str(), len, NULL, NULL, 0, &h, &ctx);
    return ctx.request_status;
}

S3Status S3ObjectStore::del(std::string key)
{
    S3CallbackCtx ctx = {
        .request_status = S3StatusOK,
        .buf = nullptr,
        .requested_len = 0,
        .transferred_len = 0,
    };

    S3_delete_object(&bucket_ctx, key.c_str(), nullptr, 0, &resp_handler, &ctx);
    return ctx.request_status;
}

struct S3ListCallbackCtx {
    S3Status status;
    bool truncated;
    std::string next_marker;
    std::vector<S3ObjInfo> objs;
};

std::vector<S3ObjInfo> S3ObjectStore::list(std::string prefix)
{
    S3ListBucketHandler h = {
        .responseHandler = {
            .propertiesCallback =
                [](const S3ResponseProperties *p, void *data) {
                    // no-op
                    return S3StatusOK;
                },
            .completeCallback =
                [](S3Status status, const S3ErrorDetails *error, void *data) {
                    auto ctx = static_cast<S3ListCallbackCtx *>(data);
                    ctx->status = status;
                },
        },
        .listBucketCallback =
            [](int isTruncated, const char *nextMarker, int contentsCount,
               const S3ListBucketContent *contents, int commonPrefixesCount,
               const char **commonPrefixes, void *callbackData) {
                auto ctx = static_cast<S3ListCallbackCtx *>(callbackData);

                ctx->truncated = isTruncated != 0;
                if (nextMarker != NULL)
                    ctx->next_marker = std::string(nextMarker);

                for (int i = 0; i < contentsCount; i++) {
                    ctx->objs.push_back(S3ObjInfo{
                        .key = std::string(contents[i].key),
                        .size = contents[i].size,
                    });
                }
                return S3StatusOK;
            },
    };

    S3ListCallbackCtx ctx = {
        .status = S3StatusOK,
        .truncated = false,
        .next_marker = "",
        .objs = {},
    };

    do {
        S3_list_bucket(&bucket_ctx, "", ctx.next_marker.c_str(), nullptr, 0,
                       nullptr, 0, &h, &ctx);
    } while (ctx.truncated && ctx.status == S3StatusOK);

    if (ctx.status != S3StatusOK) {
        log_error("S3 error: {}", ctx.status);
        throw std::runtime_error(fmt::format("S3.list error: {}", ctx.status));
    }

    return ctx.objs;
}
