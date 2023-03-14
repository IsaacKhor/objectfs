# objectfs

## How to build

First, you need to build the version of libs3 included in this repo. To do so:
just `cd libs3` and `make` and `make install`. Then compile the project in
`code`: `cd code; make`

## libs3 differences

The `libs3` in the repo is the latest from upstream, but with a patched openssl
config so it doesn't use deprecated openssl 3.0 functions.

The latest version in ubuntu repos does not incorporate some upstream changes
to the API so we can't really use that easily.

## How to run

In project directory, set s3 credentials in s3.config, then run:

```sh
./objfs-mount -d <s3_bucket>/<s3_prefix> <local_mount_dir>
```

The bucket MUST have an item called `<prefix>.00000000` (8 zeroes). This file is
generated with the `mkfs.py` script. Call it with `python mkfs.py <prefix>`,
then upload the resulting file to the bucket.

## Setup guide with minio

```sh
snap install minio
systemctl start minio
```

Navigate to `hostname:9001`, then:

- Create a bucket `obfs-test`
- Make it public (for testing permission's dont matter)
- Generate a secret / access key pair
- Run `python3 mkfs.py prefix0`
- Upload the resulting file to the bucket
- Put credentials into `s3.config`
- Run the mount executable as usual
- `./objfs-mount -d obfs-test/prefix0 /mnt/obfs-test`

## Benchmarking with filebench

To compile filebench:

```sh
apt install autoconf bison flex
git clone git@github.com:filebench/filebench.git
cd filebench
libtoolize
aclocal
autoheader
automake --add-missing
autoupdate
autoconf
./configure
make
```

Executable at `./filebench`. The modified workloads are in `measure/`, to run
them use `filebench -f measure/workload-webserver.f` or whatever workload you
choose to run.

## things that need to get fixed

### multi-threading

data structures should be locked so that we can handle multiple threads - I
think that will help perforance

### checkpointing

We need to implement checkpointing per the description in the paper and in
union-mount.md

### garbage collection

### write coalescing

see write-coalescing.md

### snapshot

Maybe. I think it's more important to get union mount working.

### union mount

See union-mount.md

## possible things to to

### low-level FUSE interface

I'm not sure whether or not we should port this to use the FUSE low-level
interface. Note that this would mess up using the CS5600 tests.

## testing notes

### CS5600 tests

I should commit those to the repo. They run directly against the high-level FUSE
methods, single-threaded. It's not a stress test, but it tests a lot of cases of
different block sizes for reads and writes.

### POSIX conformance

We might be able to use <https://github.com/pjd/pjdfstest>
it runs OK with ext4.

I haven't looked too thoroughly, but I think it's more of a standards
conformance test than a stress test, although it still could be useful

### xfstests (<https://github.com/kdave/xfstests>)

This is probably much more comprehensive. It's definitely bigger (125K LOC vs
20K)

It would take a bit of shell scripting to pass object prefixes instead of
partition names, but I don't think it would be a huge deal

### stress test

Not sure if this would be helpful, or how much work it would be to use it:
<https://github.com/google/file-system-stress-testing>

### things we'll fail

**hard links** - any tests relying on them are going to fail. I think adding
*them is fairly trivial - we've got inodes, we just have to add a reference
*count to file objects.

**fsync/fsyncdir** - I don't know if we need to do anything more with them

**holes** - I'm not sure if we support holes properly, or if we need to.
