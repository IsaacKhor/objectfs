//
// file:        s3wrap.cc
// description: wrap libs3
//

#include <assert.h>
#include <cstddef>
#include <libs3.h>
#include <list>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include "s3wrap.hpp"

/**
 * copied from the old iov.c
 */

#define min(a, b) ((a) < (b)) ? (a) : (b)

/* set contents of @iov to @val
 */
void iov_memset(struct iovec *iov, int iov_cnt, char val)
{
    for (int i = 0; i < iov_cnt; i++)
        memset(iov[i].iov_base, val, iov[i].iov_len);
}

/* copy @size bytes from @iov starting at @offset
 */
void memcpy_iov(struct iovec *iov, int iov_cnt, ssize_t offset, void *buf,
                ssize_t size, bool to_iov)
{
    void *base = NULL;
    int base_len = 0, i = 0;

    for (; offset > 0 && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            base = (void *)(offset + (char *)iov[i].iov_base);
            base_len = min(size, iov[i].iov_len - offset);
            offset = 0;
        } else
            offset -= iov[i].iov_len;
    }
    if (base != NULL) {
        to_iov ? memcpy(base, buf, base_len) : memcpy(buf, base, base_len);
        buf = (void *)(base_len + (char *)buf);
        size -= base_len;
    }
    for (; i < iov_cnt && size > 0; i++) {
        size_t bytes = min(iov[i].iov_len, size);
        to_iov ? memcpy(iov[i].iov_base, buf, bytes)
               : memcpy(buf, iov[i].iov_base, bytes);
        buf = (void *)(bytes + (char *)buf);
        size -= bytes;
    }
    assert(size == 0);
}

/* copy @offset/@size in @iov to @buf
 */
void memcpy_from_iov(struct iovec *iov, int iov_cnt, ssize_t offset, void *buf,
                     ssize_t size)
{
    memcpy_iov(iov, iov_cnt, offset, buf, size, false);
}

/* copy @buf to @offset/@size in @iov
 */
void memcpy_to_iov(struct iovec *iov, int iov_cnt, ssize_t offset,
                   const void *buf, ssize_t size)
{
    memcpy_iov(iov, iov_cnt, offset, (void *)buf, size, true);
}

/* total length of @iov
 */
ssize_t iov_sum(struct iovec *iov, int iov_cnt)
{
    size_t s = 0;
    for (int i = 0; i < iov_cnt; i++)
        s += iov[i].iov_len;
    return s;
}

class s3_context
{
  public:
    S3Status status;
    off_t content_length;
    int retries;
    int t_sleep;

    struct iovec *iov;
    int iov_cnt;
    size_t bytes_wanted; // used for both read
    size_t bytes_xfered; //   and write

    std::list<std::string> *keys;
    bool truncated;
    char next_marker[256];

    std::string msg;

    s3_context()
        : retries(5), t_sleep(1), iov_cnt(0), bytes_xfered(0),
          status(S3StatusOK), truncated(false)
    {
        next_marker[0] = 0;
    }
    bool should_retry(void)
    {
        if (retries--) {
            sleep(t_sleep++);
            return true;
        }
        return false;
    }
};

class s3_error : std::exception
{
    std::string msg;

  public:
    s3_error(std::string _msg) : msg{_msg} {}
    const char *what() { return msg.c_str(); }
};

extern "C" S3Status response_properties(const S3ResponseProperties *p,
                                        void *data);
S3Status response_properties(const S3ResponseProperties *p, void *data)
{
    s3_context *ctx = (s3_context *)data;
    ctx->content_length = p->contentLength;
    return S3StatusOK;
}

extern "C" void response_complete(S3Status status, const S3ErrorDetails *error,
                                  void *data);
void response_complete(S3Status status, const S3ErrorDetails *error, void *data)
{
    s3_context *ctx = (s3_context *)data;
    ctx->status = status;
    if (error != NULL) {
#if 0
	std::ostringstream out;  
	out << "Message: " << error->message << "\n"
	    << "Resource: " << error->resource << "\n"
	    << "Further Details: " << error->furtherDetails << "\n";
	ctx->msg = std::string(out.str());
#endif
    }
}

extern "C" S3Status recv_data_callback(int size, const char *buf, void *data);
S3Status recv_data_callback(int size, const char *buf, void *data)
{
    s3_context *ctx = (s3_context *)data;

    // don't overrun the buffer - should never happen
    if (size + ctx->bytes_xfered > ctx->bytes_wanted)
        return S3StatusAbortedByCallback;
    memcpy_to_iov(ctx->iov, ctx->iov_cnt, ctx->bytes_xfered, buf, size);
    ctx->bytes_xfered += size;
    return S3StatusOK;
}

// offset, len are in BYTES
//
S3Status S3Wrap::s3_get(std::string key, ssize_t offset, ssize_t len,
                        struct iovec *iov, int iov_cnt)
{
    S3GetObjectHandler h;
    h.responseHandler.propertiesCallback = response_properties;
    h.responseHandler.completeCallback = response_complete;
    h.getObjectDataCallback = recv_data_callback;

    s3_context ctx;
    ctx.iov = iov;
    ctx.iov_cnt = iov_cnt;
    ctx.bytes_wanted = len;

    S3BucketContext bkt_ctx = {host.c_str(),
                               bucket.c_str(),
                               protocol,
                               S3UriStylePath,
                               access.c_str(),
                               secret.c_str(),
                               0,  /* security token */
                               0}; /* authRegion */
    do {
        S3_get_object(&bkt_ctx, key.c_str(), NULL, /* no conditions */
                      offset, len, 0,              /* requestContext */
                      0,                           /* timeoutMs */
                      &h, (void *)&ctx);
    } while (S3_status_is_retryable(ctx.status) && ctx.should_retry());

    // TODO throw exception if status != S3StatusOK
    return ctx.status;
}

S3Status S3Wrap::s3_delete(std::string key)
{
    S3ResponseHandler h;
    h.propertiesCallback = response_properties;
    h.completeCallback = response_complete;

    s3_context ctx;

    S3BucketContext bkt_ctx = {host.c_str(),
                               bucket.c_str(),
                               protocol,
                               S3UriStylePath,
                               access.c_str(),
                               secret.c_str(),
                               0,  /* security token */
                               0}; /* authRegion */
    do {
        S3_delete_object(&bkt_ctx, key.c_str(), 0, /* requestContext */
                         0,                        /* timeoutMs */
                         &h, (void *)&ctx);
    } while (S3_status_is_retryable(ctx.status) && ctx.should_retry());

    // TODO throw exception if status != S3StatusOK
    return ctx.status;
}

int put_data_callback(int size, char *buf, void *data)
{
    s3_context *ctx = (s3_context *)data;
    // don't overrun the buffer - should never happen
    assert(size + ctx->bytes_xfered <= ctx->bytes_wanted);
    memcpy_from_iov(ctx->iov, ctx->iov_cnt, ctx->bytes_xfered, (void *)buf,
                    size);
    ctx->bytes_xfered += size;
    return size;
}

S3Status S3Wrap::s3_put(std::string key, struct iovec *iov, int iov_cnt)
{
    S3PutObjectHandler h;
    h.responseHandler.propertiesCallback = response_properties;
    h.responseHandler.completeCallback = response_complete;
    h.putObjectDataCallback = put_data_callback;

    s3_context ctx;
    ctx.iov = iov;
    ctx.iov_cnt = iov_cnt;
    size_t len = ctx.bytes_wanted = iov_sum(iov, iov_cnt);

    S3BucketContext bkt_ctx = {host.c_str(),
                               bucket.c_str(),
                               protocol,
                               S3UriStylePath,
                               access.c_str(),
                               secret.c_str(),
                               0,  /* security token */
                               0}; /* authRegion */

    S3PutProperties put_prop = {NULL, // binary/octet-stream
                                NULL, // MD5
                                NULL, // cache control
                                NULL, // content disposition
                                NULL, // content encoding
                                -1,   // expires (never)
                                S3CannedAclPrivate,
                                0,    // metaproperties count
                                NULL, // metaproperty list
                                0};   // use server encryption

    do {
        S3_put_object(&bkt_ctx, key.c_str(), len, &put_prop,
                      0, /* requestContext */
                      0, /* timeoutMs */
                      &h, (void *)&ctx);
    } while (S3_status_is_retryable(ctx.status) && ctx.should_retry());

    return ctx.status;
}

S3Status S3Wrap::s3_head(std::string key, ssize_t *p_len)
{
    S3ResponseHandler h;
    h.propertiesCallback = response_properties;
    h.completeCallback = response_complete;

    s3_context ctx;
    S3BucketContext bkt_ctx = {host.c_str(),
                               bucket.c_str(),
                               protocol,
                               S3UriStylePath,
                               access.c_str(),
                               secret.c_str(),
                               0,  /* security token */
                               0}; /* authRegion */

    do {
        S3_head_object(&bkt_ctx, key.c_str(), 0, /* requestContext */
                       0,                        /* timeoutMs */
                       &h, (void *)&ctx);
    } while (S3_status_is_retryable(ctx.status) && ctx.should_retry());

    // TODO throw exception if status != S3StatusOK
    *p_len = ctx.content_length;
    return ctx.status;
}

// TODO: need to handle exceptions properly

extern "C" S3Status list_callback(int, const char *, int,
                                  const S3ListBucketContent *, int,
                                  const char **, void *);

S3Status list_callback(int isTruncated, const char *nextMarker,
                       int contentsCount, const S3ListBucketContent *contents,
                       int commonPrefixesCount, const char **commonPrefixes,
                       void *callbackData)
{
    s3_context *ctx = (s3_context *)callbackData;
    ctx->truncated = isTruncated != 0;
    if (nextMarker)
        snprintf(ctx->next_marker, sizeof(ctx->next_marker), "%s", nextMarker);
    else
        ctx->next_marker[0] = 0;

    for (int i = 0; i < contentsCount; i++) {
        std::string tmp(contents[i].key);
        ctx->keys->push_back(tmp);
    }
    return S3StatusOK;
}

S3Status S3Wrap::s3_list(std::string prefix, std::list<std::string> &keys)
{
    S3ListBucketHandler h;
    h.responseHandler.propertiesCallback = response_properties;
    h.responseHandler.completeCallback = response_complete;
    h.listBucketCallback = list_callback;

    s3_context ctx;
    ctx.keys = &keys;

    S3BucketContext bkt_ctx = {host.c_str(),
                               bucket.c_str(),
                               protocol,
                               S3UriStylePath,
                               access.c_str(),
                               secret.c_str(),
                               0,  /* security token */
                               0}; /* authRegion */

    do {
        do {
            S3_list_bucket(&bkt_ctx, prefix.c_str(), ctx.next_marker,
                           0, // delimiter
                           0, // maxkeys
                           0, // request context
                           0, // timeout ms
                           &h, (void *)&ctx);
        } while (S3_status_is_retryable(ctx.status) && ctx.should_retry());
    } while (ctx.truncated && ctx.status == S3StatusOK);

    // TODO throw exception if status != S3StatusOK
    return ctx.status;
}
