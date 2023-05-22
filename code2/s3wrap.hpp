#pragma once
#include <libs3.h>
#include <list>
#include <string>
#include <sys/uio.h>

/**
 * Directly copied from old s3wrap code.
 */
class S3Wrap
{
    std::string host, bucket, access, secret;
    S3Protocol protocol;

  public:
    S3Wrap(const char *_host, const char *_bucket, const char *_access,
           const char *_secret, bool encrypted)
        : host(_host), bucket(_bucket), access(_access), secret(_secret)
    {
        protocol = encrypted ? S3ProtocolHTTPS : S3ProtocolHTTP;
    }

    S3Status s3_get(std::string key, ssize_t offset, ssize_t len,
                    struct iovec *iov, int iov_cnt);
    S3Status s3_delete(std::string key);
    S3Status s3_put(std::string key, struct iovec *iov, int iov_cnt);
    S3Status s3_head(std::string key, ssize_t *p_len);
    S3Status s3_list(std::string prefix, std::list<std::string> &keys);
};
