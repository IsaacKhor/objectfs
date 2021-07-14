/*
 * Peter Desnoyers, April 2021
 *
 * - Geoff's notes on FUSE: https://www.cs.hmc.edu/~geoff/classes/hmc.cs135.201001/homework/fuse/fuse_doc.html
 * - argument parsing: https://www.cs.hmc.edu/~geoff/classes/hmc.cs135.201001/homework/fuse/fuse_doc.html
 */

#define FUSE_USE_VERSION 27
#define _FILE_OFFSET_BITS 64

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <fuse.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>

#include "libs3.h"
#include "s3wrap.h"
#include "objfs.h"

/* usage: objfs-mount prefix /dir
 */
static struct fuse_opt opts[] = {
    {"size=%d",   -1, 0 },      /* object size to write */
    FUSE_OPT_END
};

const char *prefix;
const char *bucket;
int size = 1*1024*1024;

/* the first non-option argument is the prefix
 */
static int myfs_opt_proc(void *data, const char *arg, 
                         int key, struct fuse_args *outargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && prefix == NULL) {
        sscanf(arg, "%m[^/]/%ms", &bucket, &prefix);
        return 0;
    }
    if (key == FUSE_OPT_KEY_OPT && !strncmp(arg, "-size=", 6)) {
        size = atoi(arg+6);
        return 0;
    }
    return 1;
}

extern struct fuse_operations fs_ops;

int main(int argc, char **argv)
{
    /* Argument processing and checking
     */
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, NULL, opts, myfs_opt_proc) == -1)
        exit(1);

    /* various options to (hopefully) get FUSE performance and
     * standard filesystem behavior.
     */
    fuse_opt_insert_arg(&args, 1, "-oallow_other");
    fuse_opt_insert_arg(&args, 1, "-odefault_permissions");
//    fuse_opt_insert_arg(&args, 1, "-okernel_cache");
//    fuse_opt_insert_arg(&args, 1, "-oentry_timeout=1000,attr_timeout=1000");

    struct objfs fs = { .bucket = bucket, .prefix = prefix,
        .host = getenv("S3_HOSTNAME"), .access = getenv("S3_ACCESS_KEY_ID"),
        .secret = getenv("S3_SECRET_ACCESS_KEY"), .use_local = 0,
        .chunk_size = size};

    /* TODO: run using low-level FUSE interface
     */
    return fuse_main(args.argc, args.argv, &fs_ops, &fs);
}

