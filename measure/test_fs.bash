#/usr/bin/env bash

set -eu

#url=$S3_HOSTNAME
#key=$S3_ACCESS_KEY
#secret=$S3_SECRET_ACCESS_KEY
#bucket=$1
#python3 ./delete_bucket_files.py $url $bucket $key $secret

echo "==============================="
echo "=== Fileserver workload ======="
echo "==============================="
sudo filebench -f ./fileserver.f

echo "==============================="
echo "=== Webserver workload  ======="
echo "==============================="
sudo filebench -f ./webserver.f

echo "==============================="
echo "=== Random Write        ======="
echo "==============================="
sudo filebench -f ./random_write.f

echo "==============================="
echo "=== Varmail workload    ======="
echo "==============================="
sudo filebench -f ./varmail.f
