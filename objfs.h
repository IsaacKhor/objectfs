//
//

#ifndef __OBJFS_H__
#define __OBJFS_H__

struct objfs {
    const char *bucket;
    const char *prefix;
    const char *host;
    const char *access;
    const char *secret;
    int         use_local;      /* prefix is a file path */
    s3_target  *s3;
    size_t      chunk_size;
};

#ifdef __cplusplus
extern "C" int fs_getattr(const char *path, struct stat *sb);
extern "C" int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi);
extern "C" int fs_write(const char *path, const char *buf, size_t len,
                    off_t offset, struct fuse_file_info *fi);
extern "C" int fs_mkdir(const char *path, mode_t mode);
extern "C" int fs_rmdir(const char *path);
extern "C" int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
extern "C" int fs_mknod(const char *path, mode_t mode, dev_t dev);
extern "C" int fs_unlink(const char *path);
extern "C" int fs_rename(const char *src_path, const char *dst_path);
extern "C" int fs_chmod(const char *path, mode_t mode);
extern "C" int fs_utimens(const char *path, const struct timespec tv[2]);
extern "C" int fs_read(const char *path, char *buf, size_t len, off_t offset,
                   struct fuse_file_info *fi);
extern "C" int fs_symlink(const char *path, const char *contents);
extern "C" int fs_readlink(const char *path, char *buf, size_t len);
extern "C" int fs_statfs(const char *path, struct statvfs *st);
extern "C" int fs_fsync(const char * path, int, struct fuse_file_info *fi);
extern "C" int fs_truncate(const char *path, off_t len);
extern "C" int fs_initialize(const char*);
extern "C" int fs_mkfs(const char*);
extern "C" void fs_sync(void);
extern "C" void fs_teardown(void);
#endif

#endif
