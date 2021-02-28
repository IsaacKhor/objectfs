#include <stdio.h>
#include <stdint.h>

struct log_data {
    uint32_t inum;		// is 32 enough?
    uint32_t obj_offset;	// bytes from start of file data
    int64_t  file_offset;	// in bytes
    int64_t  size;		// file size after this write
    uint32_t len;		// bytes
};

struct log_inode {
    uint32_t        inum;
    uint32_t        mode;
    uint32_t        uid, gid;
    uint32_t        rdev;
    struct timespec mtime;
};

struct log_trunc {
    uint32_t inum;
    int64_t  new_size;		// must be <= existing
};

struct log_delete {
    uint32_t parent;
    uint32_t inum;
    uint8_t  namelen;
    char     name[];
};

struct log_symlink {
    uint32_t inum;
    uint8_t  len;
    char     target[];
};

struct log_rename {
    uint32_t inum;		// of entity to rename
    uint32_t parent1;		// inode number (source)
    uint32_t parent2;		//              (dest)
    uint8_t  name1_len;
    uint8_t  name2_len;
    char     name[];
};

struct log_create {
    uint32_t  parent_inum;
    uint32_t  inum;
    uint8_t   namelen;
    char      name[];
};

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

struct log_record {
    uint16_t type : 4;
    uint16_t len : 12;
    char data[];
};

#define OBJFS_MAGIC 0x5346424f	// "OBFS"

struct obj_header {
    int32_t magic;
    int32_t version;
    int32_t type;		// 1 == data, 2 == metadata
    int32_t hdr_len;
    int32_t this_index;
    char    data[];
};

void read_log_data(idx, rec->data)
{
}
void read_log_inode(rec->data)
{
}
void read_log_trunc(rec->data)
{
}
void read_log_delete(rec->data)
{
}
void read_log_symlink(rec->data)
{
}
void read_log_rename(rec->data)
{
}

int main(int argc, char **argv)
{
    FILE *fp = fopen(argv[1], "r");
    int size = fseek(fp, 0, SEEK_END);
    fseek(fp, 0, SEEK_SET);
    char buf[size];
    fread(buf, size, 1, fp);

    struct obj_header *oh = (void*)buf;
    printf("magic %x\nversion %d\ntype %d\nhdr_len %d\nindex %d\n",
           oh->magic, oh->version, oh->type, oh->hdr_len, oh->this_index);

    int meta_bytes = oh->hdr_len - sizeof(struct obj_header);
    struct log_record *end = (void*)&oh->data[meta_bytes];
    struct log_record *rec = (void*)oh->data;
    
    while (rec < end) {
	switch (rec->type) {
	case LOG_DATA:
	    read_log_data(idx, rec->data);
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
	case LOG_NULL:
            printf("null\n");
	    break;
	default:
	    return -1;
	}
	rec = (void*)&rec->data[rec->len + 2];
    }
    return 0;
}

}
