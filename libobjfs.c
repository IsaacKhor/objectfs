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
#include <setjmp.h>
#include <signal.h>
#include <malloc.h>

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

extern struct fuse_operations fs_ops;

struct fuse_context ctx;
struct fuse_context *fuse_get_context(void)
{
    ctx.uid = getuid();
    ctx.gid = getgid();
    return &ctx;
}

jmp_buf bail_buf;
int segv_was_called;

void segv_handler(int sig)
{
    segv_was_called = 1;
    longjmp(bail_buf, 1);
}

void set_handler(void)
{
    signal(SIGSEGV, segv_handler);
}

void unset_handler(void)
{
    return;
}

struct dirent {
    char name[256];
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
    strncpy(d->de[d->i].name, name, 256);
    d->de[d->i].sb = *sb;
    d->i++;
    return 0;
}

int py_getattr(const char *path, struct stat *sb)
{
    set_handler();
    if (setjmp(bail_buf)) {
        unset_handler();
        return 0;
    }    
    void *v = malloc(10);
    free(v);
    //int val = fs_getattr(path, sb);
    int val = fs_ops.getattr(path, sb);
    unset_handler();
    return val;
}

int py_readdir(const char *path, int *n, struct dirent *de,
               struct fuse_file_info *fi)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        struct dir_state ds = {.max = *n, .i = 0, .de = de};
        //val = fs_readdir(path, &ds, filler, 0, fi);
	val = fs_ops.readdir(path, &ds, filler, 0, fi);
        *n = ds.i;
    }
    unset_handler();
    return val;
}

int py_create(const char *path, unsigned int mode, struct fuse_file_info *fi)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_create(path, mode, fi);
	val = fs_ops.create(path, mode, fi);
    }
    unset_handler();
    return val;
}

int py_mkdir(const char *path, unsigned int mode)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_mkdir(path, mode);
	val = fs_ops.mkdir(path, mode);
    }
    unset_handler();
    return val;
}

int py_truncate(const char *path, unsigned int len)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_truncate(path, len);
	val = fs_ops.truncate(path, len);
    }
    unset_handler();
    return val;
}

int py_unlink(const char *path)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_unlink(path);
	val = fs_ops.unlink(path);
    }
    unset_handler();
    return val;
}

int py_rmdir(const char *path)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val =  fs_rmdir(path);
	val =  fs_ops.rmdir(path);
    }
    unset_handler();
    return val;
}

int py_rename(const char *path1, const char *path2)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_rename(path1, path2);
        val = fs_ops.rename(path1, path2);
    }
    unset_handler();
    return val;
}

int py_chmod(const char *path, unsigned int mode)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_chmod(path, mode);
        val = fs_ops.chmod(path, mode);
    }
    unset_handler();
    return val;
}

#if 0
FIX TO USE utimesns
int py_utime(const char *path, unsigned int actime, unsigned int modtime)
{
    int val = 0;
    set_handler();
    struct utimbuf u = {.actime = actime, .modtime = modtime};
    return fs_utime(path, &u);
}
#endif

int py_read(const char *path, char *buf, unsigned int len, unsigned int offset,
             struct fuse_file_info *fi)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_read(path, buf, len, offset, fi);
        val = fs_ops.read(path, buf, len, offset, fi);
    }
    unset_handler();
    return val;
}

int py_write(const char *path, const char *buf, unsigned int  len,
              unsigned int offset, struct fuse_file_info *fi)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_write(path, buf, len, offset, fi);
        val = fs_ops.write(path, buf, len, offset, fi);

    }
    unset_handler();
    return val;
}

int py_statfs(const char *path, struct statvfs *st)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) {
        //val = fs_statfs(path, st);
        val = fs_ops.statfs(path, st);

    }
    unset_handler();
    return val;
}

extern void fs_sync(void);
void py_sync(void)
{
    set_handler();
    if (setjmp(bail_buf) == 0) { 
        //fs_sync();
        fs_ops.fsync(0, 0, 0);
    }
    unset_handler();
}

extern int fs_initialize(const char *prefix); 
extern char *prefix;
int py_init(const char *_prefix)
{
    int val = 0;
    set_handler();
    if (setjmp(bail_buf) == 0) { 
        //val = fs_initialize(prefix);
	prefix = (char *)_prefix;
        fs_ops.init(NULL);

    }
    unset_handler();
    return val;
}

