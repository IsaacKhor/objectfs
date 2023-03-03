#!/bin/sh


for chunksize in 1024, 8192, 262144, 16384, 2048, 32768, 4096, 65536, 131072
do
    for cachesize in 1000, 2000, 3000, 4000
    do
        rm -rf /local0/mount1/*
        fusermount -u /local0/mount1
        python3 minio_cli.py
        ./objfs-mount -d songs/prefix1 /local0/mount1 -chunksize="$chunksize" -cachesize="$cachesize" &
        filebench -f ../filebench/workloads/fileserver.f > fb_"$chunksize"_"$cachesize".txt &
        sleep 3m

        pkill -f "filebench"
        fusermount -u /local0/mount1
        pkill -f "objfs"
        pkill -f "objfs-mount"
    done
done
