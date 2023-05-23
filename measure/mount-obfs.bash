#!/bin/bash

set -u
export S3_TEST_BUCKET_NAME='obfs-test'
echo "Starting OBFS with backend $S3_HOSTNAME and bucket $S3_TEST_BUCKET_NAME"

sudo umount -l /mnt/fsbench
sudo rm -rf /mnt/fsbench/*
s3 -u create $S3_TEST_BUCKET_NAME
python3 delete_bucket_files.py $S3_TEST_BUCKET_NAME

cd ../code
make clean
make release

echo "Starting OBFS"
./objectfs -f /mnt/fsbench
