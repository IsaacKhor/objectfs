#define FUSE_USE_VERSION 27
#define _FILE_OFFSET_BITS 64

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fuse.h>
#include <string.h>

extern int fs_getattr(const char *path, struct stat *sb);
extern int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi);
extern int fs_write(const char *path, const char *buf, size_t len,
                    off_t offset, struct fuse_file_info *fi);
extern int fs_mkdir(const char *path, mode_t mode);
extern int fs_rmdir(const char *path);
extern int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
extern int fs_mknod(const char *path, mode_t mode, dev_t dev);
extern int fs_unlink(const char *path);
extern int fs_rename(const char *src_path, const char *dst_path);
extern int fs_chmod(const char *path, mode_t mode);
extern int fs_utimens(const char *path, const struct timespec tv[2]);
extern int fs_read(const char *path, char *buf, size_t len, off_t offset,
                   struct fuse_file_info *fi);
extern int fs_symlink(const char *path, const char *contents);
extern int fs_readlink(const char *path, char *buf, size_t len);
extern int fs_statfs(const char *path, struct statvfs *st);
extern int fs_fsync(const char * path, int, struct fuse_file_info *fi);
extern int fs_truncate(const char *path, off_t len);

struct dirent {
    char name[64];
    struct stat sb;
};
struct dir_state {
    int max;
    int i;
    struct dirent *de;
};

static int filler(void *buf, const char *name, const struct stat *sb, off_t off)
{
    struct dir_state *d = buf;
    if (d->i >= d->max)
        return -ENOMEM;
    strncpy(d->de[d->i].name, name, 64);
    d->de[d->i].sb = *sb;
    d->i++;
    return 0;
}

int py_readdir(const char *path, int n, struct dirent *de,
               struct fuse_file_info *fi)
{
    struct dir_state ds = {.max = n, .i = 0, .de = de};
    int val = fs_readdir(path, &ds, filler, 0, fi);
    if (val < 0)
        return val;
    return ds.i;
}

int py_create(const char *path, unsigned int mode, struct fuse_file_info *fi)
{
    return fs_create(path, mode, fi);
}

int py_mkdir(const char *path, unsigned int mode)
{
    return fs_mkdir(path, mode);
}

int py_truncate(const char *path, unsigned int len)
{
    return fs_truncate(path, len);
}

int py_unlink(const char *path)
{
    return fs_unlink(path);
}

int py_rmdir(const char *path)
{
    return fs_rmdir(path);
}

int py_rename(const char *path1, const char *path2)
{
    return fs_rename(path1, path2);
}

int py_chmod(const char *path, unsigned int mode)
{
    return fs_chmod(path, mode);
}

#if 0
FIX TO USE utimesns
int py_utime(const char *path, unsigned int actime, unsigned int modtime)
{
    struct utimbuf u = {.actime = actime, .modtime = modtime};
    return fs_utime(path, &u);
}
#endif

int py_read(const char *path, char *buf, unsigned int len, unsigned int offset,
             struct fuse_file_info *fi)
{
    return fs_read(path, buf, len, offset, fi);
}

int py_write(const char *path, const char *buf, unsigned int  len,
              unsigned int offset, struct fuse_file_info *fi)
{
    return fs_write(path, buf, len, offset, fi);
}

int py_statfs(const char *path, struct statvfs *st)
{
    return fs_statfs(path, st);
}

