# Write coalescing

## Monday August 9

New journal entry classes:

CREATE #,S - create inode by number, initialize fields to data in S
DELETE # - delete inode by number.
LINK #1,"name" -> #2 - create a link in directory #1 with name, pointing to #2
UNLINK #,"name" - remove a link.
UPDATE #,S - update inode # with field info from S
SYMLINK #,link - update a symlink
TRUNCATE #,offset
DATA #,offset,data

- there is no rename command - rename(A,B) is output as [link(B),unlink(A)]
- there are no reference counts in the journal entries - refcounts go in memory and in checkpoints

To coalesce writes we take a batch of journal entries and deal with them one type at a time.

1. LINK/UNLINK - before doing anything else, pull out LINK/UNLINK and sort them by (a) parent inode, (b) name, (c) position/timestamp/...
Go through the list and remove any LINK/UNLINK pairs.
Now any LINK records are for directory/name pairs that were created during the batch and remain after the end, and UNLINK is for pairs which existed at the beginning and do not exist at the end.

2. DELETE - for each delete, remove any UPDATE, SYMLINK, DATA, or TRUNCATE entries for that inode.

3. CREATE/DELETE - if there are any CREATE/DELETE pairs, remove them. (we've already removed the corresponding LINK/UNLINK pairs

4. UPDATE - for each inode number, keep only the last UPDATE record

5. TRUNCATE - separate DATA+TRUNCATE into streams by inode number. Move each truncate operation to the front of its per-inode stream, truncating any write operations it skips over - i.e. if you have "write A..B", "write C..D", "truncate X", then change it to "truncate X", "write A..min(B,X)", "write C..min(D,X)"

6. DATA - one inode at a time, put all extents into an extent map to merge them, then pull out the merged extents.

now we can assemble the batch. I'm not sure that order matters, but it should work if we output them in this order:
- CREATE, DELETE - one at a time or mixed
- LINK, UNLINK - one at a time or mixed
- SYMLINK
- TRUNCATE
- DATA
- UPDATE

## extent handling

I've checked in extent.cc, plus some examples of using it. The extent type needs to have:
- fields 'base' and 'limit' - 'limit' is base+len, but makes more sense in some contexts
- 'adjacent(self,other)' - predicate to indicate whether 'other' is to the right 'this' and can be merged with it. In other words, are the logical extents adjacent, and do they point to adjacent physical extents?
- new_base - update the base. (typically adjusts whatever's pointed to)
- new_limit - update the limit. Typically doesn't do much

The thing is that once you've defined this, you can create a temporary extent, toss things in, and pull out merged extents.

Although we want to deal with two different extent types:

1. [offset range] -> [range in memory buffer]
2. [offset range] -> [range in object]

There are likely to be a lot more of the first type. If you create a temporary extent map of [offset range]->[buffer range] mappings, and another map of extents that don't map to anything (i.e. if they are logically adjacent, then they're adjacent), you can use the second one to calculate the in-object extent map.

There's an argument to be made that we don't need to bother, as when the journal gets read back in it will go into an in-memory extent map and everything will get merged. However we're going to need to go through the DATA records in an object when we garbage collect, so it's probably best to avoid fragmenting stuff in the object.

I'm not good at iterators, so to read things out after you've put them in an extent there's a sort-of iterator using first() and next() functions, and terminating with NULL.

## more notes on extent handling

I forgot that objfs already has its own internal map type, and actually it's nice and simple. (I should try to re-work extent.cc to use std::map instead of rbtree.c)

Anyway the sample code (test-extent.cc) creates two maps - one that maps all the little pieces of the buffer, and the other that coalesces ranges in the output. I.e. if offset 0..511 point to the first 512 bytes of the buffer, and 512..1023 point to the last 512 bytes, map1 is going to have 2 entries, each pointing to a 512-byte buffer, which are going to get copied to the first 1024 bytes of the output, and the second map will have a single entry saying offsets 0..1023 are in the first 1024 bytes of the buffer.

I think you can do this easily with the extent\_map type in objfs.cc - instead of buffer extents, just use regular extents with objnum=0 (or whatever) and offsets in the input buffer, and for map2 set objnum=0, offset=<offset parameter to `update`>.

## Friday August 6

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
