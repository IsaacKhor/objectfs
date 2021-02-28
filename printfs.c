#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

struct log_data {
    uint32_t inum;		// is 32 enough?
    uint32_t obj_offset;	// bytes from start of file data
    int64_t  file_offset;	// in bytes
    int64_t  size;		// file size after this write
    uint32_t len;		// bytes
}__attribute__((packed,aligned(1)));

struct log_inode {
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    struct timespec mtime;
}__attribute__((packed,aligned(1)));

struct log_trunc {
    uint32_t inum;
    int64_t  new_size;		// must be <= existing
}__attribute__((packed,aligned(1)));

struct log_delete {
    uint32_t parent;
    uint32_t inum;
    uint8_t  namelen;
    char     name[];
}__attribute__((packed,aligned(1)));

struct log_symlink {
    uint32_t inum;
    uint8_t  len;
    char     target[];
}__attribute__((packed,aligned(1)));

struct log_rename {
    uint32_t inum;		// of entity to rename
    uint32_t parent1;		// inode number (source)
    uint32_t parent2;		//              (dest)
    uint8_t  name1_len;
    uint8_t  name2_len;
    char     name[];
}__attribute__((packed,aligned(1)));

struct log_create {
    uint32_t  parent_inum;
    uint32_t  inum;
    uint8_t   namelen;
    char      name[];
}__attribute__((packed,aligned(1)));

enum log_rec_type {
    LOG_INODE = 1,
    LOG_TRUNC,
    LOG_DELETE,
    LOG_SYMLNK,
    LOG_RENAME,
    LOG_DATA,
    LOG_CREATE,
    LOG_NULL,			// fill space for alignment
};

char *t2s(int type)
{
    switch (type) {
    case LOG_INODE: return "LOG_INODE";
    case LOG_TRUNC: return "LOG_TRUNC";
    case LOG_DELETE: return "LOG_DELETE";
    case LOG_SYMLNK: return "LOG_SYMLNK";
    case LOG_RENAME: return "LOG_RENAME";
    case LOG_DATA: return "LOG_DATA";
    case LOG_CREATE: return "LOG_CREATE";
    case LOG_NULL: return "LOG_NULL";
    }
    return "*unknown*";
}

struct log_record {
    uint16_t type : 4;
    uint16_t len : 12;
    char data[];
}__attribute__((packed,aligned(1)));

#define OBJFS_MAGIC 0x5346424f	// "OBFS"

struct obj_header {
    int32_t magic;
    int32_t version;
    int32_t type;		// 1 == data, 2 == metadata
    int32_t hdr_len;
    int32_t this_index;
    char    data[];
};

void read_log_data(void *ptr)
{
    struct log_data *l = ptr;
    printf(" inum %d\n obj_offset %d\n file_offset %d\n size %d\n len %d\n",
           l->inum, l->obj_offset, (int)l->file_offset, (int)l->size, l->len);
}
void read_log_inode(void *ptr)
{
    struct log_inode *in = ptr;
    printf(" inum %d\n mode %o\n uid,gid %d %d\n rdev %d\n mtime %d.%09d\n",
           in->inum, in->mode, in->uid, in->gid, in->rdev, (int)in->mtime.tv_sec,
           (int)in->mtime.tv_nsec);
    
}
void read_log_trunc(void *ptr)
{
    struct log_trunc *t = ptr;
    printf(" inum %d\n size %d\n", t->inum, (int)t->new_size);
}
void read_log_delete(void *ptr)
{
    struct log_delete *d = ptr;
    printf(" parent %d\n inum %d\n name %.*s\n",
           d->parent, d->inum, d->namelen, d->name);    
}
void read_log_symlink(void *ptr)
{
    struct log_symlink *s = ptr;
    printf(" inum %d\n target %*s\n",
           s->inum, s->len, s->target);    
}
void read_log_rename(void *ptr)
{
    struct log_rename *r = ptr;
    printf(" inum %d\n srci %d\n dsti %d\n src %.*s\n dst %.*s\n",
           r->inum, r->parent1, r->parent2, r->name1_len, r->name,
           r->name2_len, &r->name[r->name1_len]);
}
void read_log_create(void *ptr)
{
    struct log_create *r = ptr;
    printf(" parent %d\n inum %d\n name %.*s\n",
           r->parent_inum, r->inum, r->namelen, r->name);
}    

void printout(void *hdr, int hdrlen)
{
    return;
    uint8_t *p = (uint8_t*) hdr;
    for (int i = 0; i < hdrlen; i++)
	printf("%02x", p[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    FILE *fp = fopen(argv[1], "r");
    char *buf = malloc(1024*1024);
    int size = fread(buf, 1, 1024*1024, fp);

    struct obj_header *oh = (void*)buf;
    printf("size %d\nmagic %x\nversion %d\ntype %d\nhdr_len %d\nindex %d\n",
           size, oh->magic, oh->version, oh->type, oh->hdr_len, oh->this_index);

    printout(buf, sizeof(*oh));
    int meta_bytes = oh->hdr_len - sizeof(struct obj_header);
    struct log_record *end = (void*)&oh->data[meta_bytes];
    struct log_record *rec = (void*)oh->data;
    
    while (rec < end) {
        uint8_t *p = (uint8_t*) rec;
        printout(p, rec->len+2);
        printf("type: %d (%s) len: %d\n", rec->type, t2s(rec->type), rec->len);
	switch (rec->type) {
	case LOG_DATA:
	    read_log_data(rec->data);
            break;
	case LOG_INODE:
	    read_log_inode(rec->data);
	    break;
	case LOG_TRUNC:
	    read_log_trunc(rec->data);
	    break;
	case LOG_DELETE:
	    read_log_delete(rec->data);
	    break;
	case LOG_SYMLNK:
	    read_log_symlink(rec->data);
	    break;
	case LOG_RENAME:
	    read_log_rename(rec->data);
	    break;
	case LOG_CREATE:
	    read_log_create(rec->data);
	    break;
	case LOG_NULL:
            printf("null\n");
	    break;
	default:
            printf("bad\n");
	}
	rec = (void*)&rec->data[rec->len];
    }
    return 0;
}
