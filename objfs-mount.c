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

/* usage: objfs-mount prefix /dir
 */
static struct fuse_opt opts[] = {
    FUSE_OPT_END
};

const char *prefix;

/* the first non-option argument is the prefix
 */
static int myfs_opt_proc(void *data, const char *arg, 
                         int key, struct fuse_args *outargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && prefix == NULL) {
        prefix = arg;
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
    fuse_opt_insert_arg(&args, 1, "-okernel_cache");
    fuse_opt_insert_arg(&args, 1, "-oentry_timeout=1000,attr_timeout=1000");

    /* TODO: run using low-level FUSE interface
     */
    return fuse_main(args.argc, args.argv, &fs_ops, "this is a test");
}

