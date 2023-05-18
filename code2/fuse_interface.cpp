#define FUSE_USE_VERSION 27
#include <fuse.h>

#include "models.hpp"
#include "objectfs.hpp"
#include "utils.hpp"

void *fs_init(struct fuse_conn_info *conn)
{
    S3ObjectStore *store =
        static_cast<S3ObjectStore *>(fuse_get_context()->private_data);
    ObjectFS *objfs = new ObjectFS(*store);
    return (void *)objfs;
}

void fs_destroy(void *private_data)
{
    ObjectFS *objfs = static_cast<ObjectFS *>(private_data);
    delete objfs;
}

int fs_open(const char *path, struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto res = ofs->open_file(path);

    if (res.has_value()) {
        fi->fh = res.value();
        return 0;
    } else {
        return res.error();
    }
}

int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto inum = ofs->create_file(path, mode);
    fi->fh = inum;
    return 0;
}

int fs_read(const char *path, char *buf, size_t size, off_t offset,
            struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->read_file(fi->fh, offset, size, (byte *)buf);
}

int fs_write(const char *path, const char *buf, size_t size, off_t offset,
             struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->write_file(fi->fh, offset, size, (byte *)buf);
}

int fs_truncate(const char *path, off_t size)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->truncate_file(path, size);
}

int fs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->sync_file(fi->fh, isdatasync != 0);
}

int fs_chmod(const char *path, mode_t mode)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->change_permissions(path, mode);
}

int fs_chown(const char *path, uid_t uid, gid_t gid)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->change_ownership(path, uid, gid);
}

int fs_getattr(const char *path, struct stat *stbuf)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto res_opt = ofs->get_attributes(path);

    if (!res_opt.has_value())
        return -ENOENT;

    memset(stbuf, 0, sizeof(struct stat));
    auto res = res_opt.value();
    stbuf->st_ino = res.inode_num;
    stbuf->st_mode = res.mode;
    stbuf->st_uid = res.uid;
    stbuf->st_gid = res.gid;
    stbuf->st_size = res.size;
    return 0;
}

int fs_mkdir(const char *path, mode_t mode)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto vec = split_string_on_char(path, '/');
    if (vec.size() == 1)
        return -EEXIST;

    auto parent = slice<0, -1>(vec);
    auto newdir = vec.back();
    auto res = ofs->make_directory(string_join(parent, "/"), newdir, mode);

    if (res.has_value())
        return 0;
    else
        return res.error();
}

int fs_rmdir(const char *path)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->remove_directory(path);
}

int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto res = ofs->list_directory(path);

    for (auto &entry : res) {
        struct stat st = {
            .st_ino = entry.inode_num,
            .st_mode = entry.mode,
            .st_uid = entry.uid,
            .st_gid = entry.gid,
            .st_size = entry.size,
            .st_atim = 0,
            .st_mtim = 0,
            .st_ctim = 0,
        };
        auto has_space = filler(buf, entry.name.c_str(), &st, 0);
        if (has_space == 0)
            break;
    }

    return 0;
}

struct fuse_operations fs_ops = {
    // Filesystem create/destroy
    .init = fs_init,
    .destroy = fs_destroy,

    // File r/w
    .create = fs_create,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .truncate = fs_truncate,
    .fsync = fs_fsync,

    // Metadata
    .chmod = fs_chmod,
    .chown = fs_chown,
    .getattr = fs_getattr,

    // Directory operations
    .mkdir = fs_mkdir,
    .rmdir = fs_rmdir,

    .readdir = fs_readdir,
};
