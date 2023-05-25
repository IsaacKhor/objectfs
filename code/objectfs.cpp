#include <algorithm>
#include <future>
#include <sys/stat.h>

#include "models.hpp"
#include "objectfs.hpp"
#include "utils.hpp"

const size_t CACHE_ENTRIES_SIZE = 1000;
const size_t LOG_CAPACITY_BYTES = 20 * 1024 * 1024;
const size_t LOG_ROLLOVER_BYTES = 10 * 1024 * 1024;

const inum_t ROOT_DIR_INUM = 1;

template <class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};

ObjectFS::ObjectFS(S3ObjectStore s3)
    : s3(s3), obj_backend(s3, CACHE_ENTRIES_SIZE, LOG_CAPACITY_BYTES,
                          LOG_ROLLOVER_BYTES)
{
    // Create root directory
    auto root = FSObject::create_directory(ROOT_DIR_INUM, ROOT_DIR_INUM, 0755);
    inodes.insert(ROOT_DIR_INUM, std::move(root));

    std::vector<std::string> objects;
    auto logobjs = s3.list("objectfs", objects);
    if (logobjs != S3StatusOK) {
        throw std::runtime_error("Failed to list objects in objectfs bucket");
        return;
    }

    log_info("Found {} objects in objectfs bucket", objects.size());

    // Make sure they are sorted in the proper order
    std::sort(objects.begin(), objects.end());

    auto replay_entries = 0;
    // TODO replay from latest checkpoint
    for (auto &backend_obj_name : objects) {
        debug("Replaying object {}", backend_obj_name);
        auto [log_entries, backing] =
            obj_backend.fetch_and_parse_object(backend_obj_name);

        for (auto &entry : log_entries) {
            apply_log_entry(entry);
            replay_entries++;
        }
    }

    log_info("Replayed {} entries, filesystem initialised", replay_entries);
}

/**
 * Replay log entries. It's not important for this to be fast or thread-safe,
 * we just replay each entry in order
 */
void ObjectFS::apply_log_entry(LogObjectVar entry)
{
    std::visit(
        overloaded{
            [&](LogSetFileData *lo) {
                // noop for now
                return;
            },
            [&](LogTruncateFile *lo) {
                auto &file = inodes.get_copy(lo->inode_num).value()->get_file();
                file.truncate_to(lo->new_size);
            },
            [&](LogChangeFilePerms *lo) {
                auto fso = inodes.get_copy(lo->inode_num).value();
                fso->update_permissions(lo->new_perms);
            },
            [&](LogChangeFileOwners *lo) {
                auto fso = inodes.get_copy(lo->inode_num).value();
                fso->update_owners(lo->new_uid, lo->new_gid);
            },
            [&](LogMakeDirectory *lo) {
                auto &parent =
                    inodes.get_copy(lo->parent_inum).value()->get_directory();
                auto newdir = FSObject::create_directory(
                    lo->parent_inum, lo->self_inum, lo->permissions);
                auto name = std::string(lo->name, lo->name_len);
                parent.add_child(name, lo->self_inum);
                inodes.insert(lo->self_inum, std::move(newdir));
            },
            [&](LogRemoveDirectory *lo) {
                auto &parent =
                    inodes.get_copy(lo->parent_inum).value()->get_directory();
                parent.remove_child(lo->removed_inum);
                inodes.erase(lo->removed_inum);
            },
            [&](LogCreateFile *lo) {
                auto &parent =
                    inodes.get_copy(lo->parent_inum).value()->get_directory();
                auto newfile = FSObject::create_file(lo->self_inum, lo->mode);
                auto name = std::string(lo->name, lo->name_len);
                parent.add_child(name, lo->self_inum);
                inodes.insert(lo->self_inum, std::move(newfile));
            },
            [&](LogRemoveFile *lo) {
                auto &parent =
                    inodes.get_copy(lo->parent_inum).value()->get_directory();
                parent.remove_child(lo->removed_inum);
                inodes.erase(lo->removed_inum);
            },
        },
        entry);
}

ObjectFS::~ObjectFS()
{
    obj_backend.rollover_log();
    // TODO write out a checkpoint

    log_info("ObjectFS shutting down");
}

inum_t ObjectFS::allocate_inode_num() { return next_inode_num.fetch_add(1); }

std::expected<inum_t, int> ObjectFS::open_file(std::string path)
{
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return std::unexpected(-ENOENT);

    auto fsobj_opt = inodes.get_copy(inode_num.value());
    if (!fsobj_opt.has_value()) {
        log_error("INTERNAL ERR: inode %u not found", inode_num.value());
        return std::unexpected(-ENOENT);
    }

    if (!fsobj_opt.value()->is_file()) {
        return std::unexpected(-EISDIR);
    }

    return inode_num.value();
}

void ObjectFS::release_file(std::string path)
{
    // noop for now
    // later consider retaining some time of reference count for files that
    // can be decremented here and file deleted on last release (if a delete
    // is pending)
    return;
}

inum_t ObjectFS::create_file(std::string path, mode_t mode)
{
    auto filename = path.substr(path.find_last_of('/') + 1);
    auto parent_dirname = path.substr(0, path.find_last_of('/'));

    auto parent_inode_num = path_to_inode_num(parent_dirname);
    if (!parent_inode_num.has_value())
        return -ENOENT;

    auto parent_obj = inodes.get_copy(parent_inode_num.value()).value();
    if (!parent_obj->is_directory())
        return -ENOTDIR;

    inum_t new_inum = allocate_inode_num();
    auto obj = FSObject::create_file(new_inum, mode);
    LogCreateFile logobj = {
        .type = LogObjectType::CreateFile,
        .parent_inum = parent_inode_num.value(),
        .self_inum = new_inum,
        .mode = mode,
        .name_len = filename.size(),
    };

    auto &parent_dir = parent_obj->get_directory();
    {
        std::unique_lock<std::shared_mutex> lock(parent_obj->mtx);
        obj_backend.append_logobj(logobj, filename);
        parent_dir.add_child(filename, new_inum);
        inodes.insert(new_inum, std::move(obj));
    }

    return new_inum;
}

int ObjectFS::delete_file(std::string path)
{
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return -ENOENT;

    auto fsobj = inodes.get_copy(inode_num.value()).value();
    if (!fsobj->is_file())
        return -EISDIR;

    auto filename = path.substr(path.find_last_of('/') + 1);
    auto parent_dirname = path.substr(0, path.find_last_of('/'));

    auto parent_inode_num = path_to_inode_num(parent_dirname);
    auto parent_obj = inodes.get_copy(parent_inode_num.value()).value();
    auto &parent_dir = parent_obj->get_directory();
    LogRemoveFile logobj = {
        .type = LogObjectType::RemoveFile,
        .parent_inum = parent_inode_num.value(),
        .removed_inum = inode_num.value(),
    };

    {
        std::scoped_lock lock(fsobj->mtx, parent_obj->mtx);

        obj_backend.append_logobj(logobj);
        parent_dir.remove_child(filename);
        inodes.erase(inode_num.value());
    }

    return 0;
}

int ObjectFS::read_file(inum_t inum, size_t read_start_offset, size_t len,
                        byte *buf)
{
    auto fsobj_opt = inodes.get_copy(inum);
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return -ENOENT;

    auto fsobj = fsobj_opt.value();
    if (!fsobj->is_file())
        return -EISDIR;

    auto &file = fsobj->get_file();

    // Get all extents that overlap with the file in question. This is the
    // only time we need to lock the file because once we get the extents,
    // we can just read from the backend without locking. NOTE when adding
    // GC, could do it like this: the GC copies the live data somewhere
    // else, then grab a write lock to modify extents. Only after the
    // extents are written do we delete the old data. This way, we don't
    // need to worry about the objects disappearing underneath us
    std::vector<std::pair<int64_t, ObjectSegment>> extents;
    {
        std::shared_lock<std::shared_mutex> lock(fsobj->mtx);
        extents = file.segments_in_range(read_start_offset, len);
    }

    for (auto &[file_extent_offset, segment] : extents) {
        obj_backend.get_obj_segment(segment, buf + file_extent_offset -
                                                 read_start_offset);
    }

    return len;
}

int ObjectFS::write_file(inum_t inum, size_t offset, size_t len, byte *buf)
{
    auto fsobj_opt = inodes.get_copy(inum);
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return -ENOENT;

    auto fsobj = fsobj_opt.value();
    if (!fsobj->is_file())
        return -EISDIR;

    // TODO break up write into smaller chunks that fit in the log

    auto &file = fsobj->get_file();

    // We need the lock for both the append and the extent insertion because
    // we need to preserve modification ordering. Otherwise we might have
    // another write insert a write into the log after us but modify the
    // inode map before us, causing a later write on the log to be applied
    // to the inode map earlier
    {
        std::unique_lock lock(fsobj->mtx);

        LogSetFileData logobj = {
            .type = LogObjectType::SetFileData,
            .inode_num = inum,
            .file_offset = offset,
            .data_len = len,
        };
        auto log_obj_seg = obj_backend.append_logobj(logobj, len, buf);

        ObjectSegment data_seg = {
            .object_id = log_obj_seg.object_id,
            .offset = log_obj_seg.offset + offsetof(LogSetFileData, data),
            .len = len,
        };
        file.insert_segment(offset, data_seg);
    }

    return len;
}

int ObjectFS::truncate_file(std::string path, size_t new_size)
{
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return -ENOENT;

    auto fsobj_opt = inodes.get_copy(inode_num.value());
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return -ENOENT;

    auto fsobj = fsobj_opt.value();
    if (!fsobj->is_file())
        return -EISDIR;

    LogTruncateFile logobj = {
        .type = LogObjectType::TruncateFile,
        .inode_num = inode_num.value(),
        .new_size = new_size,
    };
    auto &file = fsobj->get_file();
    {
        std::unique_lock<std::shared_mutex> lock(fsobj->mtx);

        // expanding is currently unsupported
        if (new_size > file.size())
            return -EINVAL;

        obj_backend.append_logobj(logobj);
        file.truncate_to(new_size);
    }

    return 0;
}

int ObjectFS::sync_file(inum_t inum, bool data_only)
{
    /**
    auto fsobj_opt = inodes.get_copy(inum);
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return -ENOENT;

    auto fsobj = fsobj_opt.value();
    if (!fsobj->is_file())
        return -EISDIR;
    */

    // Just unconditionally rollover the active log
    obj_backend.rollover_log();
    return 0;
}

std::expected<FSObjInfo, int> ObjectFS::get_attributes(std::string path)
{
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return std::unexpected(ENOENT);

    auto fsobj_opt = inodes.get_copy(inode_num.value());
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return std::unexpected(ENOENT);

    auto fsobj = fsobj_opt.value();

    std::shared_lock<std::shared_mutex> lock(fsobj->mtx);
    FSObjInfo info = {
        .name = "",
        .inode_num = inode_num.value(),
        .mode = fsobj->permissions,
        .uid = fsobj->owner_id,
        .gid = fsobj->group_id,
        .size = 0,
    };

    if (fsobj->is_file())
        info.size = fsobj->get_file().size();
    else if (fsobj->is_directory())
        info.size = fsobj->get_directory().num_children() + 2;

    return info;
}

int ObjectFS::change_permissions(std::string path, mode_t new_perms)
{
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return -ENOENT;

    auto fsobj_opt = inodes.get_copy(inode_num.value());
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return -ENOENT;

    LogChangeFilePerms logobj = {
        .type = LogObjectType::ChangeFilePermissions,
        .inode_num = inode_num.value(),
        .new_perms = new_perms,
    };
    auto fsobj = fsobj_opt.value();
    {
        std::unique_lock<std::shared_mutex> lock(fsobj->mtx);
        obj_backend.append_logobj(logobj);
        fsobj->update_permissions(new_perms);
    }

    return 0;
}

int ObjectFS::change_ownership(std::string path, uid_t uid, gid_t gid)
{
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return -ENOENT;

    auto fsobj_opt = inodes.get_copy(inode_num.value());
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return -ENOENT;

    LogChangeFileOwners logobj = {
        .type = LogObjectType::ChangeFileOwners,
        .inode_num = inode_num.value(),
        .new_uid = uid,
        .new_gid = gid,
    };
    auto fsobj = fsobj_opt.value();
    {
        std::unique_lock<std::shared_mutex> lock(fsobj->mtx);
        obj_backend.append_logobj(logobj);
        fsobj->update_owners(uid, gid);
    }

    return 0;
}

std::expected<inum_t, int>
ObjectFS::make_directory(std::string parent, std::string dirname, mode_t mode)
{
    auto parent_inum = path_to_inode_num(parent);
    if (!parent_inum.has_value())
        return -ENOENT;

    auto parent_fsobj_opt = inodes.get_copy(parent_inum.value());
    if (!parent_fsobj_opt.has_value())
        return -ENOENT;

    auto parent_fsobj = parent_fsobj_opt.value();
    if (!parent_fsobj->is_directory())
        return -ENOTDIR;

    auto &parent_dir = parent_fsobj->get_directory();
    {
        std::unique_lock<std::shared_mutex> lock(parent_fsobj->mtx);

        if (parent_dir.get_child(dirname).has_value())
            return -EEXIST;

        auto newdir_inum = allocate_inode_num();
        auto newdir_obj =
            FSObject::create_directory(parent_inum.value(), newdir_inum, mode);

        LogMakeDirectory logobj = {
            .type = LogObjectType::MakeDirectory,
            .parent_inum = parent_inum.value(),
            .self_inum = newdir_inum,
            .permissions = mode,
            .name_len = dirname.size(),
        };
        obj_backend.append_logobj(logobj, dirname);

        parent_dir.add_child(dirname, newdir_inum);
        inodes.insert(newdir_inum, std::move(newdir_obj));
        return newdir_inum;
    }
}

int ObjectFS::remove_directory(std::string path)
{
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return -ENOENT;

    auto fsobj_opt = inodes.get_copy(inode_num.value());
    // fsobj = resolve_symlink_recur(fsobj);
    if (!fsobj_opt.has_value())
        return -ENOENT;

    auto fsobj = fsobj_opt.value();
    if (!fsobj->is_directory())
        return -ENOTDIR;

    auto &dir = fsobj->get_directory();
    auto dirname = path.substr(path.find_last_of('/') + 1);
    {
        std::unique_lock<std::shared_mutex> lock(fsobj->mtx);
        if (dir.num_children() > 0)
            return -ENOTEMPTY;

        auto parent_inum = dir.parent_inum;
        auto parent_obj = inodes.get_copy(parent_inum).value();
        auto &parent_dir = parent_obj->get_directory();

        std::unique_lock<std::shared_mutex> parent_lock(parent_obj->mtx);

        LogRemoveDirectory logobj = {
            .type = LogObjectType::RemoveDirectory,
            .parent_inum = parent_inum,
            .removed_inum = inode_num.value(),
        };
        obj_backend.append_logobj(logobj);
        parent_dir.remove_child(inode_num.value());
        inodes.erase(inode_num.value());
    }

    return 0;
}

std::vector<FSObjInfo> ObjectFS::list_directory(std::string path)
{
    std::vector<FSObjInfo> ret;
    auto inode_num = path_to_inode_num(path);
    if (!inode_num.has_value())
        return ret;

    auto fsobj_opt = inodes.get_copy(inode_num.value());
    if (!fsobj_opt.has_value())
        return ret;

    auto fsobj = fsobj_opt.value();
    if (!fsobj->is_directory())
        return ret;

    auto &dir = fsobj->get_directory();
    std::vector<std::pair<std::string, inum_t>> entries;
    {
        std::shared_lock<std::shared_mutex> lock(fsobj->mtx);
        entries = dir.list_children();
    }

    // Add . and ..
    entries.push_back(std::make_pair(".", inode_num.value()));
    entries.push_back(std::make_pair("..", dir.parent_inum));

    for (auto &[entry_name, entry_inum] : entries) {
        auto child_inode = inodes.get_copy(entry_inum);
        if (!child_inode.has_value())
            continue;

        auto child_fsobj = child_inode.value();
        FSObjInfo info;
        {
            std::shared_lock<std::shared_mutex> lock(child_fsobj->mtx);
            info = {
                .name = entry_name,
                .inode_num = entry_inum,
                .mode = child_fsobj->permissions,
                .uid = child_fsobj->owner_id,
                .gid = child_fsobj->group_id,
                .size = 0,
            };
            if (child_fsobj->is_file())
                info.size = child_fsobj->get_file().size();
        }
        ret.push_back(info);
    }

    return ret;
}

std::optional<inum_t> ObjectFS::path_to_inode_num(std::string path)
{
    if (path.empty())
        return ROOT_DIR_INUM;

    auto pathvec = split_string_on_char(path, '/');
    if (pathvec.size() <= 1)
        return ROOT_DIR_INUM;

    auto cur_inode_num = ROOT_DIR_INUM;
    // Start at +1 because the 0th entry is the empty '/' component
    for (auto it = pathvec.begin() + 1; it != pathvec.end(); it++) {
        auto cur_inode = inodes.get_copy(cur_inode_num);
        // cur_inode = resolve_symlink_recur(cur_inode);
        if (!cur_inode.has_value())
            return std::nullopt;

        if (!cur_inode.value()->is_directory())
            return std::nullopt;

        auto fsobj = cur_inode.value();
        auto &dir = fsobj->get_directory();

        std::optional<inum_t> child;
        {
            std::shared_lock lock(fsobj->mtx);
            child = dir.get_child(*it);
        }
        if (!child.has_value())
            return std::nullopt;

        cur_inode_num = *child;
    }

    return std::optional(cur_inode_num);
}

/**
FSObject *ObjectFS::resolve_symlink_recur(FSObject *obj)
{
    FSObject *cur_obj = obj;
    std::set<FSObject *> visited;
    while (cur_obj != nullptr &&
           std::holds_alternative<FSSymlink>(cur_obj->data)) {
        if (visited.contains(cur_obj)) {
            log_error("symlink loop detected");
            return nullptr;
        }
        visited.insert(cur_obj);

        auto &symlink = std::get<FSSymlink>(obj->data);
        auto inum = path_to_inode_num(symlink.target_path);
        if (!inum.has_value())
            return nullptr;

        cur_obj = inodes.get_opt(inum.value());
    }
    return obj;
}
*/
