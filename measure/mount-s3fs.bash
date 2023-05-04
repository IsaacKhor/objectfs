#!/bin/bash

set -u
export S3_TEST_BUCKET_NAME='s3fs-test'
echo "Starting S3FS with backend $S3_HOSTNAME and bucket $S3_TEST_BUCKET_NAME"

s3 -u create $S3_TEST_BUCKET_NAME
python3 delete_bucket_files.py $S3_TEST_BUCKET_NAME

echo "$S3_ACCESS_KEY_ID:$S3_SECRET_ACCESS_KEY" > ./passwd-s3fs
sudo s3fs $S3_TEST_BUCKET_NAME /mnt/fsbench \
	-o passwd_file=./passwd-s3fs \
	-o url=http://$S3_HOSTNAME/ \
	-o use_path_request_style
