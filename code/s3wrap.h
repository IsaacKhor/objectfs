//
// file:        s3wrap.h
// description: definitions for libs3 wrapper
//

#ifndef __S3WRAP_H__
#define __S3WRAP_H__

#include <libs3.h>

#ifdef __cplusplus

#include <list>
#include <string>

class s3_target
{
    std::string host, bucket, access, secret;
    S3Protocol protocol;

  public:
    s3_target(const char *_host, const char *_bucket, const char *_access,
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

extern "C" void *s3_init(char *bucket, char *host, char *access, char *secret);
extern "C" S3Status s3_read(void *_t, char *key, ssize_t offset, ssize_t len,
                            struct iovec *iov, int iov_cnt);
extern "C" S3Status s3_write(void *_t, char *key, struct iovec *iov,
                             int iov_cnt);
extern "C" S3Status s3_len(void *_t, char *key, ssize_t *p_len);

#else

typedef void *s3_target; // hack

void *s3_init(char *bucket, char *host, char *access, char *secret);
S3Status s3_read(void *_t, char *key, ssize_t offset, ssize_t len,
                 struct iovec *iov, int iov_cnt);
S3Status s3_write(void *_t, char *key, struct iovec *iov, int iov_cnt);
S3Status s3_len(void *_t, char *key, ssize_t *p_len);

#endif

#endif
