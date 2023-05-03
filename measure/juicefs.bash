#!/bin/bash

juicefs format --storage minio \
    --bucket http://10.1.0.8:9000/juicefs-test \
    --access-key minioadmin \
    --secret-key minioadmin \
    sqlite3://myjfs.db \
	myjfs

juicefs mount sqlite3://myjfs.db /mnt/fsbench
