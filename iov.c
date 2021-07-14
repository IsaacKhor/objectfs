/*
 * file:        iov.c
 * description: iovect utilities
 */

//#include <stdlib.h>
#include <sys/uio.h>
#include <string.h>
#include <assert.h>
#include "iov.h"

#define min(a,b) ((a)<(b)) ? (a) : (b)

/* set contents of @iov to @val
 */
void iov_memset(struct iovec *iov, int iov_cnt, char val)
{
    for (int i = 0; i < iov_cnt; i++)
	memset(iov[i].iov_base, val, iov[i].iov_len);
}

/* copy @size bytes from @iov starting at @offset
 */
void memcpy_iov(struct iovec *iov, int iov_cnt, ssize_t offset,
		void *buf, ssize_t size, bool to_iov)
{
    void *base = NULL;
    int base_len = 0, i = 0;
    
    for (; offset > 0 && i < iov_cnt; i++) {
	if (offset < iov[i].iov_len) {
	    base = (void*)(offset + (char*)iov[i].iov_base);
	    base_len = min(size, iov[i].iov_len - offset);
	    offset = 0;
	}
	else 
	    offset -= iov[i].iov_len;
    }
    if (base != NULL) {
	to_iov ? memcpy(base, buf, base_len) : memcpy(buf, base, base_len);
	buf = (void*)(base_len + (char*)buf);
	size -= base_len;
    }
    for (; i < iov_cnt && size > 0; i++) {
	size_t bytes = min(iov[i].iov_len, size);
	to_iov ? memcpy(iov[i].iov_base, buf, bytes) :
	    memcpy(buf, iov[i].iov_base, bytes);
	buf = (void*)(bytes + (char*)buf);
	size -= bytes;
    }
    assert(size == 0);
}

/* copy @offset/@size in @iov to @buf
 */
void memcpy_from_iov(struct iovec *iov, int iov_cnt, ssize_t offset,
		     void *buf, ssize_t size)
{
    memcpy_iov(iov, iov_cnt, offset, buf, size, false);
}

/* copy @buf to @offset/@size in @iov
 */
void memcpy_to_iov(struct iovec *iov, int iov_cnt, ssize_t offset,
		   const void *buf, ssize_t size)
{
    memcpy_iov(iov, iov_cnt, offset, (void*)buf, size, true);
}

/* total length of @iov
 */
ssize_t iov_sum(struct iovec *iov, int iov_cnt)
{
    size_t s = 0;
    for (int i = 0; i < iov_cnt; i++)
	s += iov[i].iov_len;
    return s;
}

