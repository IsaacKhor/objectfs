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

    // Parse host, access key and secret key from config file
    FILE *fp = fopen("s3.config", "r");
    char line[256];
    
    fgets(line, sizeof(line), fp);
    char host[251];
    strncpy(host, &line[5], strlen(line));
    host[strlen(host)-1] = '\0';
    printf("host: %s", host); 


    fgets(line, sizeof(line), fp);
    char access[251];
    strncpy(access, &line[14], strlen(line));
    access[strlen(access)-1] = '\0';
    printf("access: %s", access); 

    fgets(line, sizeof(line), fp);
    char secret[251];
    strncpy(secret, &line[14], strlen(line));
    secret[strlen(secret)-1] = '\0';
    printf("secret: %s", secret); 


    fclose(fp);


    struct objfs fs = { .bucket = bucket, .prefix = prefix,
        .host = host, .access = access, .secret = secret,
        .use_local = 0, .chunk_size = size};

    /* TODO: run using low-level FUSE interface
     */
    return fuse_main(args.argc, args.argv, &fs_ops, &fs);
}

