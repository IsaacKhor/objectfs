#pragma once

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <sys/stat.h>
#include <variant>

#include "containers.hpp"

using std::byte;
using inum_t = uint32_t;
using objectid_t = uint32_t;

/**
 * Addresses an object in the backend. The offset does NOT include the object
 * header, so it must be translated into an actual backend offset before being
 * used to read an object.
 */
struct ObjectSegment {
    uint64_t object_id;
    size_t offset;
    size_t len;
};

class FSFile
{
  private:
    std::map<int64_t, ObjectSegment> extents_map;

    explicit FSFile(FSFile &other) = delete;
    FSFile &operator=(FSFile &other) = delete;

  public:
    explicit FSFile() = default;
    explicit FSFile(FSFile &&other) = default;
    FSFile &operator=(FSFile &&other) = default;

    /**
     * List of all segments in the specified range, with the first and last
     * segment truncated to match the boundaries of the range.
     */
    std::vector<std::pair<int64_t, ObjectSegment>>
    segments_in_range(int64_t offset, size_t len);

    /**
     * Insert a new extent into the map. If the new extent overlaps with
     * existing extents, the existing extents are chopped off to make room
     * for the new one.
     */
    void insert_segment(int64_t offset, ObjectSegment e);
    ssize_t truncate_to(size_t new_size);
    size_t size();
};

class FSDirectory
{
  private:
    explicit FSDirectory(FSDirectory &other) = delete;
    FSDirectory &operator=(FSDirectory &other) = delete;

  public:
    inline explicit FSDirectory(inum_t parent) : parent_inum(parent){};
    explicit FSDirectory(FSDirectory &&other) = default;
    FSDirectory &operator=(FSDirectory &&other) = default;

    inum_t parent_inum;
    std::map<std::string, inum_t> children;

    void add_child(std::string name, inum_t child_inum);
    void remove_child(std::string name);

    /**
     * This assumes that we don't have hardlinks
     */
    void remove_child(inum_t child_inum);

    std::vector<std::pair<std::string, inum_t>> list_children();
    std::optional<inum_t> get_child(std::string name);

    inline size_t num_children() { return children.size(); }
};

class FSSymlink
{
  private:
    explicit FSSymlink(FSSymlink &other) = delete;
    FSSymlink &operator=(FSSymlink &other) = delete;

  public:
    explicit FSSymlink(std::string target) : target_path(target){};
    explicit FSSymlink(FSSymlink &&other) = default;
    FSSymlink &operator=(FSSymlink &&other) = default;

    std::string target_path;
};

using FSObjectData = std::variant<FSFile, FSDirectory, FSSymlink>;

class FSObject
{
  private:
    explicit FSObject(inum_t inode_num, mode_t permissions);

  public:
    static std::unique_ptr<FSObject> create_file(inum_t inode_num,
                                                 mode_t permissions);
    static std::unique_ptr<FSObject>
    create_directory(inum_t parent_inum, inum_t self_inum, mode_t permissions);

    /**
     * There's no real way to make the entire class always thread safe, so
     * we delegate it to the caller and make fields public. Before modifying
     * or reading any of the fields, the caller MUST acquire a read or write
     * lock. The reason is that we might need to do something like atomically
     * move a file from one directory to another, which requires grabbing
     * write locks to both directories
     *
     * This also matters for ordering. When changing the inode, we also append
     * the modification to the log, so to ensure that the ordering of the change
     * on the log and on this inode is consistent, the caller must acquire a
     * write lock before doing both with said lock
     */
    std::shared_mutex mtx;
    FSObjectData data;

    inum_t inode_num;
    mode_t permissions;
    uid_t owner_id;
    gid_t group_id;

    inline void update_permissions(mode_t new_perms)
    {
        permissions = new_perms | (permissions & S_IFMT);
    }
    inline void update_owners(uid_t uid, gid_t gid)
    {
        owner_id = uid;
        group_id = gid;
    }

    inline bool is_file() { std::holds_alternative<FSFile>(data); }
    inline bool is_directory() { std::holds_alternative<FSDirectory>(data); }
    inline bool is_symlink() { std::holds_alternative<FSSymlink>(data); }

    inline FSFile &get_file() { return std::get<FSFile>(data); }
    inline FSDirectory &get_directory() { return std::get<FSDirectory>(data); }
    inline FSSymlink &get_symlink() { return std::get<FSSymlink>(data); }
};

/**
 * The raw log entries for the filesystem
 */

using std::byte;

struct BackendObjectHeader {
    int32_t magic;
};

enum class LogObjectType : uint8_t {
    SetFileData = 1,
    TruncateFile,
    ChangeFilePermissions,
    MakeDirectory,
    RemoveDirectory,
    CreateFile,
    RemoveFile,
};

struct LogObjectBase {
    LogObjectType type;
};

struct LogSetFileData {
    LogObjectType type;
    inum_t inode_num;
    size_t file_offset;
    size_t data_len;
    byte data[];
};

struct LogTruncateFile {
    LogObjectType type;
    inum_t inode_num;
    size_t new_size;
};

struct LogChangeFilePerms {
    LogObjectType type;
    inum_t inode_num;
    mode_t new_perms;
};

struct LogChangeFileOwners {
    LogObjectType type;
    inum_t inode_num;
    uid_t new_uid;
    gid_t new_gid;
};

struct LogMakeDirectory {
    LogObjectType type;
    inum_t parent_inum;
    inum_t self_inum;
    mode_t permissions;
    std::string name;
};

struct LogRemoveDirectory {
    LogObjectType type;
    /** index by inum because we assume no hardlinks. */
    inum_t removed_inum;
};

struct LogCreateFile {
    LogObjectType type;
    inum_t parent_inum;
    inum_t self_inum;
    mode_t mode;
    std::string name;
};

struct LogRemoveFile {
    LogObjectType type;
    inum_t parent_dir_inum;
    inum_t removed_inum;
};
