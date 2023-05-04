#!/bin/bash

juicefs format --storage minio \
    --bucket http://$S3_HOSTNAME/$S3_TEST_BUCKET_NAME \
    --access-key $S3_ACCESS_KEY_ID \
    --secret-key $S3_SECRET_ACCESS_KEY \
    sqlite3://myjfs.db \
	myjfs

juicefs mount -f sqlite3://myjfs.db /mnt/fsbench
