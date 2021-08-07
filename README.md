# objectfs

## things that need to get fixed

### multi-threading

data structures should be locked so that we can handle multiple threads - I think that will help perforance

### checkpointing

We need to implement checkpointing per the description in the paper and in union-mount.md

### garbage collection

### write coalescing

see write-coalescing.md

### snapshot

Maybe. I think it's more important to get union mount working.

### union mount

See union-mount.md

## possible things to to

### low-level FUSE interface

I'm not sure whether or not we should port this to use the FUSE low-level interface. Note that this would mess up using the CS5600 tests.


## testing notes:

### CS5600 tests

I should commit those to the repo. They run directly against the high-level FUSE methods, single-threaded. It's not a stress test, but it tests a lot of cases of different block sizes for reads and writes.

### POSIX conformance

we might be able to use https://github.com/pjd/pjdfstest
it runs OK with ext4.

I haven't looked too thoroughly, but I think it's more of a standards conformance test than a stress test, although it still could be useful

### xfstests (https://github.com/kdave/xfstests)

This is probably much more comprehensive. It's definitely bigger (125K LOC vs 20K)

It would take a bit of shell scripting to pass object prefixes instead of partition names, but I don't think it would be a huge deal

### stress test

Not sure if this would be helpful, or how much work it would be to use it:
https://github.com/google/file-system-stress-testing

### things we'll fail

**hard links** - any tests relying on them are going to fail. I think adding them is fairly trivial - we've got inodes, we just have to add a reference count to file objects. 

**fsync/fsyncdir** - I don't know if we need to do anything more with them

**holes** - I'm not sure if we support holes properly, or if we need to.

