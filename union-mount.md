# Union mount design

Assume we have file systems A, with object stream A.1, A.2 ... A.<n\_A> and B = B.1, B.2, ... B.<n\_B> and each has inodes numbered 1, 2, ... (n\_A and n\_B are the max object numbers in A and B)

We want to create a union file system C. Assume C has an object stream C.1, C.2.. and inodes starting at 1.

## Numbering
First we need to make everything fit - we need to deal with the overlapping inode number and object number namespaces between A, B, and C.

### Inode uniqueness:

steal a small number of bits from the top of a 64-bit inode number and use them to indicate the file system.

top bits = 0 - this means it's an inode number in C.
1, 2 - an inode in A or B. Set the top bits to 0 and you have the inode number used in the A and B object streams.

So we'll need a table of inode "namespace identifiers" mapping to object streams.

In the paper it mentions that map checkpoints will include a table mapping inode number to location in the checkpoint. Here we'll have multiple checkpoints - one for each stream. If we don't have an object in memory, we'll have to go through this table to find the object stream, and from there we can figure out where to demand load it from the correct object stream checkpoint.

### Object number uniqueness:

Since we know exactly how many objects there are in A and B, we can just reserve space in the C object stream. We need a table of the form:

0..(n\_A-1) -> stream A, objects 0..(n\_A-1)
n\_A...(n\_A+n\_B-1) -> stream B, objects 0..(n\_B-1)

The lowest-numbered object of the form C.* will thus be C.(n\_A+n\_B).

## Naming

As described in the paper, a directory entry is a mapping from a string to an inode number. For unionFS it's a mapping from a string to an ordered list of inode numbers. 

So if you have `/usr` in both A and B, you could have a directory entry `usr -> A.2,B.3` where A.2 and B.3 are inode numbers in A and B.

In order to actually traverse that directory, you need to read the directories corresponding to A.2 and B.3 and merge them into a single directory. If there are directory entries with the same name, they become multi-inode entries in the new directory. If there are files with the same name, we follow the union mount rule and take the entry from A. 

If there's a file in A and a directory in B, or vice versa, I'm not positive - I think we take the A entry in each case, but would want to check what Linux union mount does.

The in-memory directory entries will map to inode numbers with the high bits set to indicate the A or B inode number spaces.

When we read in file metadata (i.e. extent maps) from checkpoints in A or B we'll need to translate the object numbers into the C namespace - in this case no change to object numbers in A, and add n\_A to object numbers in B.

When we checkpoint metadata to disk, this will write out the directories that have been merged. The result will be that we gradually merge the A and B file systems as we access them, on an as-needed basis.

### more implementation details

For the in-memory directory I think it might be simpler to have two maps - one for multi-entries and one for single entries. First you look in the multi-entry map - if it's a hit, then merge the multiple directories and try again.

If we have an object stream "A.1" "A.2" etc. (or "A\_1", or however we name it) I think it makes sense to have a root object named "A". That can hold:

- location of the most recent metadata checkpoint. we can rely on atomic overwrite of the previous object
- snapshot information - this is where the list of snapshots and sequence numbers goes
- union mount tables described above

That way you can mount the file system by reading "A" (or "C" or whatever) and rolling forward, rather than having to list all the objects in the object stream. (especially useful for RADOS, which I believe can only list objects in hash order)

## Checkpoints

The paper is very vague about when and how we checkpoint. The code has some methods to serialize all the in-memory metadata, but not to write it out.

I'm thinking that the best way is to mark metadata in memory as clean or dirty. Each time we checkpoint, we include a pointer to the previous checkpoint and we write out all the dirty metadata. 

I see the following options for a map from inode number to checkpoint location (for demand-loading, section 2.3 in hotstorage paper)

1. each checkpoint contains a full inode location map. At mount time we load the map into memory

2. each checkpoint contains a partial inode map, e.g. only listing inodes persisted in that checkpoint. At mount time we go through all the live checkpoints and merge them all into memory

3. each checkpoint contains a partial inode map, and we demand load them.

For implementation I think that number 1 makes the most sense - it's going to take a while before we get to the point where we can even test option 2. 

Reviewers are going to want to know how we scale to large numbers of files. Despite modern file systems supporting crazy numbers of files (e.g. ext4 defaults to one inode per 16K, or 1 billion files on a 16TB disk) the largest numbers I could find are rather tame. Looking at CMU-PDL-08-109 and Ian F. Adam's PhD thesis (Ethan Miller student) the largest number I could find was 50 million at I think LANL in what was probably 2012 or so. (thesis date 2013)

Option 2 could be done with 16 bytes of memory per inode, which seems entirely reasonable to me, but might not for reviewers. We could discuss option 3, which looks sort of like an LSM tree. 

(what's the name for an index where you keep the data on disk in sorted order and keep an in-memory list of the first record in each page, so that you can directly fetch the page? That's what we need here, and I can't remember the name)

Since we're talking about containers as a use case, maybe we should measure the number of files in a full install of Ubuntu and RHEL, for some definition of "full install", and mention that that's our target.

### checkpoint garbage collection

If we only checkpoint dirty metadata, the number of live checkpoints grows without bounds. We can have a separate metadata checkpointer that limits the number of active checkpoints by reading in the oldest one and marking everything as dirty.

The problem with this is that the overhead scales with the number of files, not with the amount of I/O. 

Another thing we could do is to GC checkpoints by merging - i.e. merge the last 10 checkpoints into a single larger checkpoint, with its "previous checkpoint" pointer indicating the latest checkpoint before those 10.

I like that idea better, since the amount of work to persist metadata is still proportional to the amount of file system modification. 

We could also do "passive garbage collection" of old checkpoints - any data that gets demand-loaded from sufficiently old checkpoints is marked as dirty and persisted in the next checkpoint. Again, that makes the rate of work proportional to the rate of file system operations, rather than the size of the file system. 

Hmm. Maybe what I like best is to merge checkpoints until they reach a certain size, then only delete them if they drop below a certain threshold of live data, where checkpoint data is only invalidated when objects are actually modified.

That way:

1. the number of checkpoint objects is proportional to the size of the file system (times a small constant - e.g. 2 or less if we GC checkpoints when they're half-empty)

2. the amount of work writing checkpoints is proportional to file system activity. (times 2 for a single level of merging, and times another 2 if we GC at 50% utilization)

That means we need an act
