#/usr/bin/env bash

set -eu

echo "==============================="
echo "=== Fileserver workload ======="
echo "==============================="
python3 ./delete_bucket_files.py
sudo filebench -f ./fileserver.f

echo "==============================="
echo "=== OLTP workload       ======="
echo "==============================="
python3 ./delete_bucket_files.py
sudo filebench -f ./oltp.f

echo "==============================="
echo "=== Random Write        ======="
echo "==============================="
python3 ./delete_bucket_files.py
sudo filebench -f ./random_write.f

echo "==============================="
echo "=== Webserver workload  ======="
echo "==============================="
python3 ./delete_bucket_files.py
sudo filebench -f ./webserver.f

