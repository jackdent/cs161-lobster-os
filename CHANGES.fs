The file system implementation solution set as it currently stands was
written by David A. Holland.

(Various people contributed to the 1.x buffer cache code in a vain
attempt to fix it. It has been nuked from orbit and replaced with new
code for 2.x. I am not even including its old changelog entries.)

------------------------------------------------------------

As of 2016, changes on the fs-related branches go in CHANGES (q.v.).
This file will be merged into there sometime.

20150805 dholland	Version 2.0.1 of the file system solutions released.
20150804 dholland	Always invalidate buffers when freeing blocks.
20150709 dholland	Improve syncer logic and take steps to avoid having
........		it get too far behind.
20150709 dholland	Improve handling of buffer ages in syncer.
20150707 dholland	Fix generation-handling bug in sync_some_buffers().
20150625 dholland	Fix locking comment on sfs_getroot. From Serena Wang.
20150605 dholland	Add a bit of defensive coding in sfs_loadvnode: null
........		out sv from the table search so accidental use before
........		allocating a new vnode doesn't corrupt some other file.
20150527 dholland	sfs_maxblock in sfs_bmap.c is not actually used.
........		From Keno Fischer.
20150422 dholland	Keep the freemap locked during all of truncate.
20150421 dholland	Fix missing buffer_lock release in buffer_drop.
20150127 dholland	Fix error path deadlock in buffer_read_internal().
20150126 dholland	Add buffer_flush().

20150115 dholland	Version 2.0 of the file system solutions released.
20150115 dholland	"nosync" buffers -> "fsmanaged" buffers, for clarity.
20150115 dholland	Avoid buffercache-related hangs under memory pressure.

20140924 dholland	Version 1.99.08 of the file system solutions released.
20140922 dholland	If . or .. is missing, don't let mkdir use these names.
20140919 dholland	Don't corrupt the fs if truncate fails in rmdir.
20140918 dholland	Add support for fs-specific buffer metadata.
20140917 dholland	Add support for "nosync" buffers. See buf.h.
20140917 dholland	Kill off most of the buffer reservation logic.
20140917 dholland	Don't report eviction errors when getting a buffer.
........        	(Try another buffer instead.)
20140829 dholland	Add preliminary design notes for SFS locking.
20140829 dholland	Add preliminary design notes for vfs locking.
20140829 dholland	Change 'bufstats' menu item to 'buf' for consistency.
20140822 dholland	Fix data-corrupting race in buffer_get.
20140808 dholland	Rework buffer cache to solve various problems.
20140806 dholland	Begin cleaning up sfs_itrunc.
20140806 dholland	Improve bmap some.
20140729 dholland	Drop all buffers belonging to a fs being unmounted.
20140729 dholland	sfs_itrunc: Invalidate buffers for dead indir blocks.
20140729 dholland	sfs_itrunc: Track modified indirect blocks properly.
20140729 dholland	Add buffer_is_dirty/buffer_is_valid functions.
20140728 dholland	Fix type of rootdir_data_block in mksfs.
20140510 dholland	In sfs_namefile, clamp large buffer sizes; don't fail.
20140510 dholland	When renaming dirs, only update .. if it's changed.
20140504 dholland	Fix unintialized field in struct buf, from David Ding.
20140501 dholland	Fix another uninitialized variable in sfs_bmap,
........		from David Ding.
20140501 dholland	Fix error path bug in sfs_makeobj, from Ray Li.
20140424 dholland	Fix missing locking in buffer_writeout.
20140404 dholland	Fix uninitialized variable in sfs_bmap.
20140203 dholland	Fix typo in menu printout, from Anne Madoff.

20140123 dholland	Version 1.99.07 of the file system solutions released.
20140123 dholland	Revert the doom counter. System/161 has one now.
20140123 dholland	Added a "bufstats" menu command.
20140123 dholland	In buf.c, busy_buffers -> inuse_buffers for clarity.
20140122 dholland	Make vn_countlock a spinlock.
20140114 dholland	Reorganize SFS sources and revise the solutions.

20130503 dholland	Fix missing error unlock in sfs_balloc, merge mistake.
20130104 nmurphy	Merge doom-counter code into the buffer cache branch.
20100108 dholland	Print buffer cache size during kernel startup.
20090503 dholland	Fix double buffer_release on error branch.
			Found by Gideon Wald.
20090425 dholland	Fix embarrassing bug the buffer cache shipped with.

20090414 dholland	buffercache-1.99.04 released.
20090414 dholland	Make buffer_drop not fail.
20090414 dholland	More assorted fixes to the buffer cache.
20090413 dholland	Add the buffer cache to SFS.
20090413 dholland	Assorted fixes and adjustments to the buffer cache.
20090410 dholland	Implement buffer cache eviction. Add syncer.
20090410 dholland	Changes from a code review of the buffer cache.
20090410 dholland	Finish draft of new buffer cache.
20090409 dholland	Code up new buffer cache.
