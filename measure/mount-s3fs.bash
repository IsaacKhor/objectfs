#!/bin/bash

export S3_TEST_BUCKET_NAME='s3fs-test'
echo "$S3_ACCESS_KEY:$S3_SECRET_ACCESS_KEY" > ./passwd-s3fs

s3 -u delete $S3_TEST_BUCKET_NAME
s3 -u create $S3_TEST_BUCKET_NAME

sudo s3fs $S3_TEST_BUCKET_NAME /mnt/fsbench \
	-o passwd_file=./passwd-s3fs \
	-o url=http://$S3_HOSTNAME/ \
	-o use_path_request_style
