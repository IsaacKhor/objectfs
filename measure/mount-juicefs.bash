#!/bin/bash

set -u
export S3_TEST_BUCKET_NAME='juicefs-test'
echo "Starting JuiceFS with backend $S3_HOSTNAME and bucket $S3_TEST_BUCKET_NAME"

s3 -u create $S3_TEST_BUCKET_NAME
python3 delete_bucket_files.py $S3_TEST_BUCKET_NAME
rm -f myjfs.db

#juicefs format --storage minio \
#    --bucket http://$S3_HOSTNAME/$S3_TEST_BUCKET_NAME \
#    --access-key $S3_ACCESS_KEY_ID \
#    --secret-key $S3_SECRET_ACCESS_KEY \
#    sqlite3://myjfs.db \
#	myjfs

juicefs format --storage s3 \
    --bucket http://$S3_TEST_BUCKET_NAME.$S3_HOSTNAME \
    --access-key $S3_ACCESS_KEY_ID \
    --secret-key $S3_SECRET_ACCESS_KEY \
    redis://10.1.0.6:6379 \
	myjfs

sudo juicefs mount redis://10.1.0.6:6379 /mnt/fsbench
