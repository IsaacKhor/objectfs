#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

#include "backend.hpp"
#include "containers.hpp"
#include "models.hpp"

struct FSObjInfo {
    std::string name;
    inum_t inode_num;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    size_t size;
};

class ObjectFS
{
  private:
    /**
     * In-memory inodes map. This is the source of truth for the FS. We map
     * inode_num's to shared pointers because we might have somebody reading
     * a file and then it getting deleted after that read, so we don't delete
     * the actual FSObject until the fs operation is done
     */
    ConcurrentMap<inum_t, std::shared_ptr<FSObject>> inodes;
    S3ObjectStore s3;
    ObjectBackend obj_backend;

    std::atomic<inum_t> next_inode_num = 0;

    /**
     * Resolve symlinks recursively, and if the target does not exist,
     * return NULL. Will detect cycles in the symlink chain and return null.
     */
    // FSObject *resolve_symlink_recur(FSObject *obj);
    inum_t allocate_inode_num();

  public:
    ObjectFS(S3ObjectStore s3);
    ~ObjectFS();

    std::optional<inum_t> path_to_inode_num(std::string path);

    inum_t create_file(std::string path, mode_t mode);
    int delete_file(std::string path);

    std::expected<inum_t, int> open_file(std::string path);
    void release_file(std::string path);

    int read_file(inum_t inum, size_t offset, size_t len, byte *buf);
    int write_file(inum_t inum, size_t offset, size_t len, byte *buf);
    int truncate_file(std::string path, size_t new_size);
    int sync_file(inum_t inum, bool data_only);

    std::optional<FSObjInfo> get_attributes(std::string path);
    int change_permissions(std::string path, mode_t mode);
    int change_ownership(std::string path, uid_t uid, gid_t gid);

    std::expected<inum_t, int> make_directory(std::string parent,
                                              std::string name, mode_t mode);
    int remove_directory(std::string path);

    std::vector<FSObjInfo> list_directory(std::string path);
};
