#!/bin/bash

sudo umount -l /mnt/fsbench

export S3_TEST_BUCKET_NAME='obfs-test'

s3 -u delete $S3_TEST_BUCKET_NAME
s3 -u create $S3_TEST_BUCKET_NAME

cd ../code
make
./objfs-mount -f $S3_TEST_BUCKET_NAME/prefix /mnt/fsbench
