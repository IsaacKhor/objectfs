# Write coalescing

The journal types are:

- INODE
- TRUNCATE
- DELETE
- SYMLINK
- RENAME
- DATA
- CREATE

If DELETE and TRUNCATE didn't exist, it would be simple:

- sort DATA records by inode, for each referenced inode create the minimal set of DATA entries
- sort INODE records by inode, take the most recent one for each inode
- sort RENAME records by source inode, take the most recent one
- sort SYMLINK entries by inode, take the most recent one

CREATE and INODE records need to be properly ordered:

First note that CREATE only provides a (name->inode#) tuple within a parent directory (identified by an inode number); the INODE record identifies the object as a file, directory, or special file.

That means for inode N you need a CREATE and INODE record before:
- CREATE records for entries in that directory, if N is a directory
- DATA records, if N is a file
- SYMLINK records if N is a symlink

If you write to an object, in addition to DATA records it's going to generate an INODE entry for updated timestamp, size, etc. (hmm, we could roll those into the DATA record, I suppose)

The complicated parts:

If you DELETE something, then any other operations on that object can be removed from the batch, as well.

If you TRUNCATE a file to length L, any DATA records before that operation are truncated to L. (e.g. deleted, if L=0)

So assuming we have a set of journal entries E\_1, ... E\_i, I think we can do the following steps:

1. renames. R\_i = sequence of renames for inode i = (e | e.inum = i and e.op = RENAME)
collapse each sequence, so rename A -> B -> C becomes rename A -> C
put all collapsed sequences at the beginning of the set

2. deletes. D = set of deleted inodes, {e.inum | e.op = DELETE}
remove all operations that refer to an inode in D. Note that this can include renames that we already processed.
Put D at the beginning of the set. (or after the renames, doesn't matter)

3. inode updates. scan the set for CREATEs, keep the first INODE after each CREATE; all the remaining INODE records get merged and put at the end of the batch. The initial CREATE/INODE records go at the very beginning of the batch, preserving this order.

3. writes and truncates. Divide these operations in two dimensions: (a) by inode number, and (b) in sequences where each sequence ends in a TRUNCATE or at the end of the batch.

Merge the WRITEs in each sequence, then trim it appropriately if it ends in a TRUNCATE.

(Note that you can create a temporary extent map for merging.)

Now the WRITEs get output inode by inode, and sequence by sequence for each inode. They go after everything except the INODE updates.

This could all be done by putting pointers to the records in a C++ list or vector, and with each step we start by going through the list (vector?), pulling out one or two types of record, and putting them in a temporary list.

-----

Thoughts about handling hard links properly:

Maybe we should have LINK/UNLINK to create names, and CREATE/DELETE to create/delete objects by inode number, not name; CREATE could basically be a duplicate of the INODE record. Reference counts would exist in the in-memory objects and their serialized form in checkpoints, but journal records would only record the actions we take, not the state (i.e. reference count) that caused those actions.
