/* lots cribbed from rbd.c
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <scsi/scsi.h>

#include "darray.h"
#include "tcmu-runner.h"
#include "tcmur_cmd_handler.h"
#include "libtcmu.h"
#include "tcmur_device.h"

typedef int S3Status;
enum {S3StatusOK = 0};

#include "s3wrap.h"

#define NR_THREADS 30

struct tcmu_s3_state {
    void *s3;
    char *prefix;
    int   sectors;
    pthread_t th[NR_THREADS];
};

extern const char *S3_get_status_name(S3Status status);
static void *worker(void *tmp);

static int tcmu_s3_open(struct tcmu_device *dev, bool reopen)
{
    struct tcmu_s3_state *state = calloc(1, sizeof(*state));
    tcmur_dev_set_private(dev, state);
    
    char *cfg = tcmu_dev_get_cfgstring(dev);
    cfg = strchr(cfg, '/');
    if (!cfg)
        return -EINVAL;
    cfg++;

    FILE *fp=fopen("/tmp/s3_open", "w+");
    fprintf(fp, "\n--------\ncfg=%s\n", cfg);
    fflush(fp);
    
    char *bucket = strtok(cfg, "/");
    state->prefix = strtok(NULL, ";");
    char *host = NULL, *access = NULL, *secret = NULL;

    for (char *tmp = strtok(NULL, ";"); tmp != NULL; tmp = strtok(NULL, ";")) {
        if (!strncmp(tmp, "host=", 5)) {
            host = tmp+5;
	    char *p = strchr(host,'@'); // translate to :
	    if (p) *p = ':';
	}
        else if (!strncmp(tmp, "secret=", 7))
            secret = tmp+7;
        else if (!strncmp(tmp, "access=", 7))
            access = tmp+7;
        else {
            fprintf(fp, "bad cfg: %s\n", tmp);
            fclose(fp);
            tcmu_dev_err(dev, "bad cfg: %s\n", tmp);
            return -EINVAL;
        }
    }
    fprintf(fp, "bucket '%s' host '%s' access '%s' secret '%s'\n", bucket, host, access, secret);
    if (host && access && secret) {
        state->s3 = s3_init(bucket, host, access, secret);
    }
    else {
        tcmu_dev_err(dev, "missing: %s %s %s\n",
                       host ? "" : "host", access ? "" : "access", secret ? "" : "secret");
        fprintf(fp, "missing: %s %s %s\n",
                host ? "" : "host", access ? "" : "access", secret ? "" : "secret");
        fclose(fp);
        return -EINVAL;
    }
        
    ssize_t len;
    S3Status status = s3_len(state->s3, state->prefix, &len);
    if (status != S3StatusOK) {
        tcmu_dev_err(dev, "%s/%s: %s\n", bucket, state->prefix, S3_get_status_name(status));
        fprintf(fp, "%s/%s: %s\n", bucket, state->prefix, S3_get_status_name(status));
        fclose(fp);
        return -EINVAL;
    }


    if (len/512 != tcmu_dev_get_num_lbas(dev)) {
        tcmu_dev_err(dev, "bad #LBAs: %lu (cfg) %ld (s3)\n", tcmu_dev_get_num_lbas(dev), len/512);
        fprintf(fp, "bad #LBAs: %lu (cfg) %ld (s3)\n", tcmu_dev_get_num_lbas(dev), len/512);
        fclose(fp);
        return -EINVAL;
    }

    for (int i = 0; i < NR_THREADS; i++)
        pthread_create(&state->th[i], NULL, worker, NULL);
    
    tcmu_dev_set_write_cache_enabled(dev, 1);
    return 0;
}

static void tcmu_s3_close(struct tcmu_device *dev)
{
    struct tcmu_s3_state *state = tcmur_dev_get_private(dev);
    for (int i = 0; i < NR_THREADS; i++) 
        pthread_cancel(state->th[i]);
//    for (int i = 0; i < NR_THREADS; i++) 
//        pthread_join(state->th[i], NULL);
    printf("close complete\n");
    free(state);
}

struct queued_read {
    struct queued_read *next;
    struct tcmu_device *dev;
    struct tcmur_cmd *tcmur_cmd;
    struct iovec *iov;
    size_t iov_cnt;
    size_t length;
    off_t offset;
};

static pthread_mutex_t m;
static pthread_cond_t  C;
static struct queued_read *q_head, *q_tail;

static void *worker(void *tmp)
{
    while (1) {
        pthread_mutex_lock(&m);
        while (q_head == NULL)
            pthread_cond_wait(&C, &m);
        struct queued_read *r = q_head;
        q_head = r->next;
        pthread_mutex_unlock(&m);
        
        struct tcmu_s3_state *state = tcmur_dev_get_private(r->dev);
        S3Status status = s3_read(state->s3, state->prefix, r->offset, r->length, r->iov, r->iov_cnt);
        tcmur_cmd_complete(r->dev, r->tcmur_cmd, TCMU_STS_OK);
    }
    return NULL;
}

static int tcmu_s3_read(struct tcmu_device *dev, struct tcmur_cmd *tcmur_cmd,
                        struct iovec *iov, size_t iov_cnt, size_t length,
                        off_t offset)
{
    struct queued_read *q = calloc(1, sizeof(*q));
    *q = (struct queued_read){.next = NULL, .dev = dev, .tcmur_cmd = tcmur_cmd, .iov = iov,
                              .iov_cnt = iov_cnt, .length = length, .offset = offset};
    pthread_mutex_lock(&m);
    if (q_head == NULL)
        q_head = q_tail = q;
    else {
        q_tail->next = q;
        q_tail = q;
    }
    pthread_cond_signal(&C);
    pthread_mutex_unlock(&m);

    return TCMU_STS_OK;
}

#if 0
static int tcmu_s3_read(struct tcmu_device *dev, struct tcmur_cmd *tcmur_cmd,
                        struct iovec *iov, size_t iov_cnt, size_t length,
                        off_t offset)
{
    struct tcmu_s3_state *state = tcmur_dev_get_private(dev);
    S3Status status = s3_read(state->s3, state->prefix, offset, length, iov, iov_cnt);
    tcmur_cmd_complete(dev, tcmur_cmd, TCMU_STS_OK);
    return TCMU_STS_OK;
}
#endif
static int tcmu_s3_write(struct tcmu_device *dev, struct tcmur_cmd *tcmur_cmd,
                         struct iovec *iov, size_t iov_cnt, size_t length,
                         off_t offset)
{
    return TCMU_STS_WR_ERR;
}

static int tcmu_s3_flush(struct tcmu_device *dev, struct tcmur_cmd *tcmur_cmd)
{
    return TCMU_STS_OK;
}

static int tcmu_s3_init(void)
{
    return 0;
}

static void tcmu_s3_destroy(void)
{
}

static const char tcmu_s3_cfg_desc[] =
    "S3 config string is of the form:\n"
    "bucket/prefix;host=HOST;access=KEY;secret=SECRET\n";

struct tcmur_handler tcmu_s3_handler = {
        .name          = "S3 BlockDev handler",
        .subtype       = "s3",
        .cfg_desc      = tcmu_s3_cfg_desc,
        .open          = tcmu_s3_open,
        .close         = tcmu_s3_close,
        .read          = tcmu_s3_read,
        .write         = tcmu_s3_write,
	.flush         = tcmu_s3_flush,
        .init          = tcmu_s3_init,
        .destroy       = tcmu_s3_destroy,
};

int handler_init(void)
{
    FILE *fp = fopen("/tmp/it-ran", "w");
    fprintf(fp, "it ran\n");
    fclose(fp);
    return tcmur_register_handler(&tcmu_s3_handler);
}
