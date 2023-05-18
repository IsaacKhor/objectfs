#include "backend.hpp"

S3ObjectStore::S3ObjectStore(std::string _host, std::string _bucket_name,
                             std::string _access_key, std::string _secret_key,
                             bool encrypted)
    : host(_host), bucket_name(_bucket_name), access_key(_access_key),
      secret_key(_secret_key)
{
    if (encrypted) {
        protocol = S3ProtocolHTTPS;
    } else {
        protocol = S3ProtocolHTTP;
    }
}

S3ObjectStore::~S3ObjectStore() {}
