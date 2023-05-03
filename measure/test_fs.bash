#/usr/bin/env bash

set -eu

#url=$S3_HOSTNAME
#key=$S3_ACCESS_KEY_ID
#secret=$S3_SECRET_ACCESS_KEY
#bucket=$1
#python3 ./delete_bucket_files.py $url $bucket $key $secret

echo "==============================="
echo "=== Fileserver workload ======="
echo "==============================="
sudo filebench -f ./workload-fileserver.f

echo "==============================="
echo "=== Webserver workload  ======="
echo "==============================="
sudo filebench -f ./workload-webserver.f

echo "==============================="
echo "=== Random Write        ======="
echo "==============================="
sudo filebench -f ./workload-random-write.f

echo "==============================="
echo "=== Varmail workload    ======="
echo "==============================="
sudo filebench -f ./workload-varmail.f
