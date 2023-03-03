//
// define everything the way we compile it
//

#define FUSE_USE_VERSION 27
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fuse.h>
#include <assert.h>


struct record {
    char name[16];
    int  offset;
    int  len;
};

int compare_rec(const void *_a, const void *_b)
{
    const struct record *a = _a, *b = _b;
    return a->offset - b->offset;
}

void print_struct(char *name, int size, struct record *fields, int nfields)
{
    qsort(fields, nfields, sizeof(fields[0]), compare_rec);
    
    printf("class %s(Structure):\n", name);
    
    int pad = 0, offset = 0;
    printf("    _fields_ = [");
    char *comma = "", *_comma = "\n                ";
    char eol[32] = "";
    
    for (; fields->len > 0; fields++) {
	printf("%s%s", eol, comma);
	eol[0] = 0;
	comma = _comma;
	
	int i = fields->offset - offset;
	assert(i >= 0);
	if (i > 0) {
	    printf("(\"_pad%d\", c_char * %d),", pad++, i);
	    printf("\t# %d", offset);
	    printf("%s", comma);
	    offset += i;
	}
	char *type = NULL, buf[32];
	sprintf(eol, "\t# %d", offset);
	if (fields->len == 4)
	    type = "c_int";
	else if (fields->len == 8)
	    type = "c_longlong";
	else if (fields->len > 8) {
	    sprintf(buf, "c_char * %d", fields->len);
	    type = buf;
	}
	
	assert(type != NULL);
	printf("(\"%s\", %s),", fields->name, type);
	offset += fields->len;
    }
    if (offset < size) {
	printf("%s%s(\"_pad%d\", c_char * %d),", eol, comma, pad++, size-offset);
	sprintf(eol, "\t# %d", offset);
    }
    printf("%s -> %d%s]\n\n", eol, size, comma);
}

struct stat s;
struct record stat_fields[] = {
    {"st_dev", offsetof(struct stat, st_dev), sizeof(s.st_dev)},
    {"st_ino", offsetof(struct stat, st_ino), sizeof(s.st_ino)},
    {"st_nlink", offsetof(struct stat, st_nlink), sizeof(s.st_nlink)},
    {"st_mode", offsetof(struct stat, st_mode), sizeof(s.st_mode)},
    {"st_uid", offsetof(struct stat, st_uid), sizeof(s.st_uid)},
    {"st_gid", offsetof(struct stat, st_gid), sizeof(s.st_gid)},
    {"st_rdev", offsetof(struct stat, st_rdev), sizeof(s.st_rdev)},
    {"st_size", offsetof(struct stat, st_size), sizeof(s.st_size)},
    {"st_blksize", offsetof(struct stat, st_blksize), sizeof(s.st_blksize)},
    {"st_blocks", offsetof(struct stat, st_blocks), sizeof(s.st_blocks)},
    {"st_atime", offsetof(struct stat, st_atime), sizeof(s.st_atime)},
    {"st_mtime", offsetof(struct stat, st_mtime), sizeof(s.st_mtime)},
    {"st_ctime", offsetof(struct stat, st_ctime), sizeof(s.st_ctime)},
};

struct dirent {
    char name[64];
    struct stat sb;
};

struct dirent d;
struct record dirent_fields[] = {
    {"name", offsetof(struct dirent, name), sizeof(d.name)},
    {"st_mode", offsetof(struct dirent, sb.st_mode), sizeof(d.sb.st_mode)},
    {"st_uid", offsetof(struct dirent, sb.st_uid), sizeof(d.sb.st_uid)},
    {"st_gid", offsetof(struct dirent, sb.st_gid), sizeof(d.sb.st_gid)},
    {"st_size", offsetof(struct dirent, sb.st_size), sizeof(d.sb.st_size)},
    {"st_mtime", offsetof(struct dirent, sb.st_mtime), sizeof(d.sb.st_mtime)},
    {"st_ctime", offsetof(struct dirent, sb.st_ctime), sizeof(d.sb.st_ctime)},
};
    
struct statvfs v;
struct record statv_fields[] = {
    {"f_bsize", offsetof(struct statvfs, f_bsize), sizeof(v.f_bsize)},
    {"f_blocks", offsetof(struct statvfs, f_blocks), sizeof(v.f_blocks)},
    {"f_bfree", offsetof(struct statvfs, f_bfree), sizeof(v.f_bfree)},
    {"f_bavail", offsetof(struct statvfs, f_bavail), sizeof(v.f_bavail)},
    {"f_namemax", offsetof(struct statvfs, f_namemax), sizeof(v.f_namemax)},
};

struct fuse_file_info fi;
struct record ffi_fields[] = {
    {"flags", offsetof(struct fuse_file_info, flags), sizeof(fi.flags)},
    {"fh", offsetof(struct fuse_file_info, fh), sizeof(fi.fh)},
};

struct fuse_context ctx;
struct record ctx_fields[] = {
    {"fuse", offsetof(struct fuse_context, fuse), sizeof(ctx.fuse)},
    {"uid", offsetof(struct fuse_context, uid), sizeof(ctx.uid)},
    {"gid", offsetof(struct fuse_context, gid), sizeof(ctx.gid)},
    {"pid", offsetof(struct fuse_context, pid), sizeof(ctx.pid)},
    {"umask", offsetof(struct fuse_context, umask), sizeof(ctx.umask)},
};

int main(int argc, char **argv)
{
    print_struct("stat", sizeof(struct stat), stat_fields,
		 sizeof(stat_fields)/sizeof(stat_fields[0]));
    print_struct("dirent", sizeof(struct dirent), dirent_fields,
		 sizeof(dirent_fields)/sizeof(dirent_fields[0]));
    print_struct("statvfs", sizeof(struct statvfs), statv_fields,
		 sizeof(statv_fields)/sizeof(statv_fields[0]));
    print_struct("fuse_file_info", sizeof(struct fuse_file_info), ffi_fields,
		 sizeof(ffi_fields)/sizeof(ffi_fields[0]));
    print_struct("fuse_context", sizeof(struct fuse_context), ctx_fields,
		 sizeof(ctx_fields)/sizeof(ctx_fields[0]));
}
