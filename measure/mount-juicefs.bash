#!/bin/bash

set -u
export S3_TEST_BUCKET_NAME='juicefs-test'
echo "Starting JuiceFS with backend $S3_HOSTNAME and bucket $S3_TEST_BUCKET_NAME"

s3 -u create $S3_TEST_BUCKET_NAME
python3 delete_bucket_files.py $S3_TEST_BUCKET_NAME

juicefs format --storage minio \
    --bucket http://$S3_HOSTNAME/$S3_TEST_BUCKET_NAME \
    --access-key $S3_ACCESS_KEY_ID \
    --secret-key $S3_SECRET_ACCESS_KEY \
    sqlite3://myjfs.db \
	myjfs

juicefs mount -f sqlite3://myjfs.db /mnt/fsbench
