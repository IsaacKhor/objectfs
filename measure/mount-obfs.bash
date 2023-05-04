#!/bin/bash

sudo umount -l /mnt/fsbench

export S3_TEST_BUCKET_NAME='obfs-test'
s3 -u create $S3_TEST_BUCKET_NAME
python3 delete_bucket_files.py $S3_TEST_BUCKET_NAME

cd ../code
make

echo "Starting OBFS"
printf "host=$S3_HOSTNAME
s3_access_key=$S3_ACCESS_KEY_ID
s3_secret_key=$S3_SECRET_ACCESS_KEY
" > s3.config

./objfs-mount -f $S3_TEST_BUCKET_NAME/prefix /mnt/fsbench
