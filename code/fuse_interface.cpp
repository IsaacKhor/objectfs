#define FUSE_USE_VERSION 27
#define _FILE_OFFSET_BITS 64
#include <fuse.h>

#include "models.hpp"
#include "objectfs.hpp"
#include "utils.hpp"

void *fs_init(struct fuse_conn_info *conn)
{
    S3ObjectStore *store =
        static_cast<S3ObjectStore *>(fuse_get_context()->private_data);
    ObjectFS *objfs = new ObjectFS(*store);

    log_info("Log level: {}", LOGLV);
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

int fs_release(const char *path, struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    ofs->release_file(path);
    return 0;
}

int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto inum = ofs->create_file(path, mode);
    fi->fh = inum;
    return 0;
}

int fs_unlink(const char *path)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    return ofs->delete_file(path);
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
        return -res_opt.error();

    memset(stbuf, 0, sizeof(struct stat));
    auto res = res_opt.value();
    stbuf->st_ino = res.inode_num;
    stbuf->st_mode = res.mode;
    stbuf->st_nlink = 1;
    stbuf->st_uid = res.uid;
    stbuf->st_gid = res.gid;
    stbuf->st_size = res.size;
    stbuf->st_blocks = 1;
    stbuf->st_atime = 0;
    stbuf->st_mtime = 0;
    stbuf->st_ctime = 0;

    return 0;
}

int fs_mkdir(const char *path, mode_t mode)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto vec = split_string_on_char(path, '/');
    if (vec.size() == 1)
        return -EEXIST;

    std::vector<std::string> parent(vec.begin(), vec.end() - 1);
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

int fs_statfs(const char *path, struct statvfs *st)
{
    st->f_bsize = 4096;
    st->f_blocks = 0;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_namemax = 255;
    return 0;
}

int fs_opendir(const char *path, struct fuse_file_info *fi)
{
    // noop
    return 0;
}

int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
               [[maybe_unused]] off_t offset,
               [[maybe_unused]] struct fuse_file_info *fi)
{
    auto ofs = static_cast<ObjectFS *>(fuse_get_context()->private_data);
    auto res = ofs->list_directory(path);

    for (auto &entry : res) {
        struct stat st = {
            .st_ino = entry.inode_num,
            .st_mode = entry.mode,
            .st_uid = entry.uid,
            .st_gid = entry.gid,
            .st_size = (long)entry.size,
            .st_atim = {0, 0},
            .st_mtim = {0, 0},
            .st_ctim = {0, 0},
        };
        auto has_space = filler(buf, entry.name.c_str(), &st, 0);
        if (has_space != 0)
            break;
    }

    return 0;
}

int fs_utimens([[maybe_unused]] const char *path,
               [[maybe_unused]] const struct timespec tv[2])
{
    // noop
    return 0;
}

struct fuse_operations fs_ops = {
    .getattr = fs_getattr,
    .readlink = nullptr,
    .getdir = nullptr,
    .mknod = nullptr,
    .mkdir = fs_mkdir,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .symlink = nullptr,
    .rename = nullptr,
    .link = nullptr,
    .chmod = fs_chmod,
    .chown = fs_chown,
    .truncate = fs_truncate,
    .utime = nullptr,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .statfs = fs_statfs,
    .flush = nullptr,
    .release = fs_release,
    .fsync = fs_fsync,
    .setxattr = nullptr,
    .getxattr = nullptr,
    .listxattr = nullptr,
    .removexattr = nullptr,
    .opendir = fs_opendir,
    .readdir = fs_readdir,
    .releasedir = nullptr,
    .fsyncdir = nullptr,
    .init = fs_init,
    .destroy = fs_destroy,
    .access = nullptr,
    .create = fs_create,
    .ftruncate = nullptr,
    .fgetattr = nullptr,
    .lock = nullptr,
    .utimens = fs_utimens,
    .bmap = nullptr,
    .flag_nullpath_ok = 0,
    .flag_nopath = 0,
    .flag_utime_omit_ok = 0,
    .flag_reserved = 0,
    .ioctl = nullptr,
    .poll = nullptr,
    .write_buf = nullptr,
    .read_buf = nullptr,
    .flock = nullptr,
    .fallocate = nullptr,
    // .copy_file_range = nullptr,
    // .lseek = nullptr,
};

int main(int argc, char **argv)
{
    fuse_args args = FUSE_ARGS_INIT(argc, argv);
    fuse_opt_parse(&args, nullptr, nullptr, nullptr);
    fuse_opt_add_arg(&args, "-oallow_other");
    fuse_opt_add_arg(&args, "-odefault_permissions");
    fuse_opt_add_arg(&args, "-oauto_unmount");
    // fuse_opt_add_arg(&args, "-okernel_cache");
    fuse_opt_add_arg(&args, "-ouse_ino");
    fuse_opt_add_arg(&args, "-obig_writes");
    fuse_opt_add_arg(&args, "-omax_write=1048576");
    fuse_opt_add_arg(&args, "-omax_read=1048576");

    auto s3_host = std::getenv("S3_HOSTNAME");
    auto s3_access = std::getenv("S3_ACCESS_KEY_ID");
    auto s3_secret = std::getenv("S3_SECRET_ACCESS_KEY");
    auto s3_bucket_name = std::getenv("S3_TEST_BUCKET_NAME");

    if (s3_host == nullptr || s3_access == nullptr || s3_secret == nullptr ||
        s3_bucket_name == nullptr) {
        log_error("Missing environment variables");
        return 1;
    }

    log_info("Mounting {}/{} ({}:{})", s3_host, s3_bucket_name, s3_access,
             s3_secret);

    S3ObjectStore store(s3_host, s3_bucket_name, s3_access, s3_secret, false);
    return fuse_main(args.argc, args.argv, &fs_ops, &store);
}