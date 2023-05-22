#pragma once

#include <stdbool.h>
#include <sys/types.h>

void memcpy_iov(struct iovec *iov, int iov_cnt, ssize_t offset,
		void *buf, ssize_t size, bool to_iov);
void memcpy_from_iov(struct iovec *iov, int iov_cnt, ssize_t offset,
		     void *buf, ssize_t size);
void memcpy_to_iov(struct iovec *iov, int iov_cnt, ssize_t offset,
		   const void *buf, ssize_t size);
void iov_memset(struct iovec *iov, int iov_cnt, char val);
ssize_t iov_sum(struct iovec *iov, int iov_cnt);
