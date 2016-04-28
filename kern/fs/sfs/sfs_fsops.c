/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009, 2014
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * SFS filesystem
 *
 * Filesystem-level interface routines.
 */

#define TXID_TINLINE

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <array.h>
#include <bitmap.h>
#include <synch.h>
#include <uio.h>
#include <vfs.h>
#include <buf.h>
#include <device.h>
#include <sfs.h>
#include "sfsprivate.h"
#include "sfs_transaction.h"
#include "sfs_graveyard.h"

/* Shortcuts for the size macros in kern/sfs.h */
#define SFS_FS_NBLOCKS(sfs)        ((sfs)->sfs_sb.sb_nblocks)
#define SFS_FS_FREEMAPBITS(sfs)    SFS_FREEMAPBITS(SFS_FS_NBLOCKS(sfs))
#define SFS_FS_FREEMAPBLOCKS(sfs)  SFS_FREEMAPBLOCKS(SFS_FS_NBLOCKS(sfs))


#ifndef TXID_TINLINE
#define TXID_TINLINE INLINE
#endif

DECLARRAY(txid_t, TXID_TINLINE);
DEFARRAY(txid_t, TXID_TINLINE);

/*
 * Routine for doing I/O (reads or writes) on the free block bitmap.
 * We always do the whole bitmap at once; writing individual sectors
 * might or might not be a worthwhile optimization. Similarly, storing
 * the freemap in the buffer cache might or might not be a worthwhile
 * optimization. (But that would require a total rewrite of the way
 * it's handled, so not now.)
 *
 * The free block bitmap consists of SFS_FREEMAPBLOCKS 512-byte
 * sectors of bits, one bit for each sector on the filesystem. The
 * number of blocks in the bitmap is thus rounded up to the nearest
 * multiple of 512*8 = 4096. (This rounded number is SFS_FREEMAPBITS.)
 * This means that the bitmap will (in general) contain space for some
 * number of invalid sectors that are actually beyond the end of the
 * disk device. This is ok. These sectors are supposed to be marked
 * "in use" by mksfs and never get marked "free".
 *
 * The sectors used by the superblock and the bitmap itself are
 * likewise marked in use by mksfs.
 */
static
int
sfs_freemapio(struct sfs_fs *sfs, enum uio_rw rw)
{
	uint32_t j, freemapblocks;
	char *freemapdata;
	int result;

	KASSERT(lock_do_i_hold(sfs->sfs_freemaplock));

	/* Number of blocks in the free block bitmap. */
	freemapblocks = SFS_FS_FREEMAPBLOCKS(sfs);

	/* Pointer to our freemap data in memory. */
	freemapdata = bitmap_getdata(sfs->sfs_freemap);

	/* For each block in the free block bitmap... */
	for (j=0; j<freemapblocks; j++) {

		/* Get a pointer to its data */
		void *ptr = freemapdata + j*SFS_BLOCKSIZE;

		/* and read or write it. The freemap starts at sector 2. */
		if (rw == UIO_READ) {
			result = sfs_readblock(&sfs->sfs_absfs,
					       SFS_FREEMAP_START + j,
					       ptr, SFS_BLOCKSIZE);
		}
		else {
			result = sfs_writeblock(&sfs->sfs_absfs,
						SFS_FREEMAP_START + j, NULL,
						ptr, SFS_BLOCKSIZE);
		}

		/* If we failed, stop. */
		if (result) {
			return result;
		}
	}
	return 0;
}

#if 0	/* This is subsumed by sync_fs_buffers, plus would now be recursive */
/*
 * Sync routine for the vnode table.
 */
static
int
sfs_sync_vnodes(struct sfs_fs *sfs)
{
	unsigned i, num;

	/* Go over the array of loaded vnodes, syncing as we go. */
	num = vnodearray_num(sfs->sfs_vnodes);
	for (i=0; i<num; i++) {
		struct vnode *v = vnodearray_get(sfs->sfs_vnodes, i);
		VOP_FSYNC(v);
	}
	return 0;
}

#endif

/*
 * Sync routine for the freemap.
 */
static
int
sfs_sync_freemap(struct sfs_fs *sfs)
{
	int result;

	lock_acquire(sfs->sfs_freemaplock);

	if (sfs->sfs_freemapdirty) {
		result = sfs_freemapio(sfs, UIO_WRITE);
		if (result) {
			lock_release(sfs->sfs_freemaplock);
			return result;
		}
		sfs->sfs_freemapdirty = false;
	}

	lock_release(sfs->sfs_freemaplock);
	return 0;
}

/*
 * Sync routine for the superblock.
 *
 * For the time being at least the superblock shares the freemap lock.
 */
static
int
sfs_sync_superblock(struct sfs_fs *sfs)
{
	int result;

	lock_acquire(sfs->sfs_freemaplock);

	if (sfs->sfs_superdirty) {
		result = sfs_writeblock(&sfs->sfs_absfs, SFS_SUPER_BLOCK,
					NULL,
					&sfs->sfs_sb, sizeof(sfs->sfs_sb));
		if (result) {
			lock_release(sfs->sfs_freemaplock);
			return result;
		}
		sfs->sfs_superdirty = false;
	}
	lock_release(sfs->sfs_freemaplock);
	return 0;
}

/*
 * Sync routine. This is what gets invoked if you do FS_SYNC on the
 * sfs filesystem structure.
 */
static
int
sfs_sync(struct fs *fs)
{
	struct sfs_fs *sfs;
	int result;


	/*
	 * Get the sfs_fs from the generic abstract fs.
	 *
	 * Note that the abstract struct fs, which is all the VFS
	 * layer knows about, is actually a member of struct sfs_fs.
	 * The pointer in the struct fs points back to the top of the
	 * struct sfs_fs - essentially the same object. This can be a
	 * little confusing at first.
	 *
	 * The following diagram may help:
	 *
	 *     struct sfs_fs        <-------------\
         *           :                            |
         *           :   sfs_absfs (struct fs)    |   <------\
         *           :      :                     |          |
         *           :      :  various members    |          |
         *           :      :                     |          |
         *           :      :  fs_data  ----------/          |
         *           :      :                             ...|...
         *           :                                   .  VFS  .
         *           :                                   . layer .
         *           :   other members                    .......
         *           :
         *           :
	 *
	 * This construct is repeated with vnodes and devices and other
	 * similar things all over the place in OS/161, so taking the
	 * time to straighten it out in your mind is worthwhile.
	 */

	sfs = fs->fs_data;

	// TODO: is it as simple as moving up jphys flusing to here?
	result = sfs_jphys_flushall(sfs);
	if (result) {
		return result;
	}

	/* Sync the buffer cache */
	result = sync_fs_buffers(fs);
	if (result) {
		return result;
	}

	/* If the free block map needs to be written, write it. */
	result = sfs_sync_freemap(sfs);
	if (result) {
		return result;
	}

	/* If the superblock needs to be written, write it. */
	result = sfs_sync_superblock(sfs);
	if (result) {
		return result;
	}

	return 0;
}

/*
 * Code called when buffers are attached to and detached from the fs.
 * This can allocate and destroy fs-specific buffer data. We don't
 * currently have any, but later changes might want to.
 */
static
int
sfs_attachbuf(struct fs *fs, daddr_t diskblock, struct buf *buf)
{
	struct sfs_fs *sfs = fs->fs_data;
	void *olddata;

	(void)sfs;
	(void)diskblock;

	/* Install null as the fs-specific buffer data. */
	olddata = buffer_set_fsdata(buf, NULL);

	/* There should have been no fs-specific buffer data beforehand. */
	KASSERT(olddata == NULL);

	return 0;
}

static
void
sfs_detachbuf(struct fs *fs, daddr_t diskblock, struct buf *buf)
{
	struct sfs_fs *sfs = fs->fs_data;
	void *bufdata;

	(void)sfs;
	(void)diskblock;

	/* Clear the fs-specific metadata by installing null. */
	bufdata = buffer_set_fsdata(buf, NULL);

	/* The fs-specific buffer data we installed before was just NULL. */
	KASSERT(bufdata == NULL);
}

/*
 * Routine to retrieve the volume name. Filesystems can be referred
 * to by their volume name followed by a colon as well as the name
 * of the device they're mounted on.
 */
static
const char *
sfs_getvolname(struct fs *fs)
{
	struct sfs_fs *sfs = fs->fs_data;

	/*
	 * VFS only uses the volume name transiently, and its
	 * synchronization guarantees that we will not disappear while
	 * it's using the name. Furthermore, we don't permit the volume
	 * name to change on the fly (this is also a restriction in VFS)
	 * so there's no need to synchronize.
	 */

	return sfs->sfs_sb.sb_volname;
}

/*
 * Destructor for struct sfs_fs.
 */
static
void
sfs_fs_destroy(struct sfs_fs *sfs)
{
	sfs_jphys_destroy(sfs->sfs_jphys);
	lock_destroy(sfs->sfs_renamelock);
	lock_destroy(sfs->sfs_freemaplock);
	lock_destroy(sfs->sfs_vnlock);
	if (sfs->sfs_freemap != NULL) {
		bitmap_destroy(sfs->sfs_freemap);
	}
	vnodearray_destroy(sfs->sfs_vnodes);
	KASSERT(sfs->sfs_device == NULL);
	kfree(sfs);
}

/*
 * Unmount code.
 *
 * VFS calls FS_SYNC on the filesystem prior to unmounting it.
 */
static
int
sfs_unmount(struct fs *fs)
{
	struct sfs_fs *sfs = fs->fs_data;

	lock_acquire(sfs->sfs_vnlock);
	lock_acquire(sfs->sfs_freemaplock);

	/* Do we have any files open? If so, can't unmount. */
	if (vnodearray_num(sfs->sfs_vnodes) > 0) {
		lock_release(sfs->sfs_freemaplock);
		lock_release(sfs->sfs_vnlock);
		return EBUSY;
	}

	sfs_jphys_stopwriting(sfs);

	unreserve_fsmanaged_buffers(2, SFS_BLOCKSIZE);

	/* We should have just had sfs_sync called. */
	KASSERT(sfs->sfs_superdirty == false);
	KASSERT(sfs->sfs_freemapdirty == false);

	/* All buffers should be clean; invalidate them. */
	drop_fs_buffers(fs);

	/* The vfs layer takes care of the device for us */
	sfs->sfs_device = NULL;

	/* Release the locks. VFS guarantees we can do this safely. */
	lock_release(sfs->sfs_vnlock);
	lock_release(sfs->sfs_freemaplock);

	/* Destroy the fs object; once we start nuking stuff we can't fail. */
	sfs_fs_destroy(sfs);

	/* nothing else to do */
	return 0;
}

/*
 * File system operations table.
 */
static const struct fs_ops sfs_fsops = {
	.fsop_sync = sfs_sync,
	.fsop_getvolname = sfs_getvolname,
	.fsop_getroot = sfs_getroot,
	.fsop_unmount = sfs_unmount,
	.fsop_readblock = sfs_readblock,
	.fsop_writeblock = sfs_writeblock,
	.fsop_attachbuf = sfs_attachbuf,
	.fsop_detachbuf = sfs_detachbuf,
};

/*
 * Basic constructor for struct sfs_fs. This initializes all fields
 * but skips stuff that requires reading the volume, like allocating
 * the freemap.
 */
static
struct sfs_fs *
sfs_fs_create(void)
{
	struct sfs_fs *sfs;

	/*
	 * Make sure our on-disk structures aren't messed up
	 */
	COMPILE_ASSERT(sizeof(struct sfs_superblock)==SFS_BLOCKSIZE);
	COMPILE_ASSERT(sizeof(struct sfs_dinode)==SFS_BLOCKSIZE);
	COMPILE_ASSERT(SFS_BLOCKSIZE % sizeof(struct sfs_direntry) == 0);

	/* Allocate object */
	sfs = kmalloc(sizeof(struct sfs_fs));
	if (sfs==NULL) {
		goto fail;
	}

	/*
	 * Fill in fields
	 */

	/* abstract vfs-level fs */
	sfs->sfs_absfs.fs_data = sfs;
	sfs->sfs_absfs.fs_ops = &sfs_fsops;

	/* superblock */
	/* (ignore sfs_super, we'll read in over it shortly) */
	sfs->sfs_superdirty = false;

	/* device we mount on */
	sfs->sfs_device = NULL;

	/* vnode table */
	sfs->sfs_vnodes = vnodearray_create();
	if (sfs->sfs_vnodes == NULL) {
		goto cleanup_object;
	}

	/* freemap */
	sfs->sfs_freemap = NULL;
	sfs->sfs_freemapdirty = false;

	/* locks */
	sfs->sfs_vnlock = lock_create("sfs_vnlock");
	if (sfs->sfs_vnlock == NULL) {
		goto cleanup_vnodes;
	}
	sfs->sfs_freemaplock = lock_create("sfs_freemaplock");
	if (sfs->sfs_freemaplock == NULL) {
		goto cleanup_vnlock;
	}
	sfs->sfs_renamelock = lock_create("sfs_renamelock");
	if (sfs->sfs_renamelock == NULL) {
		goto cleanup_freemaplock;
	}

	sfs->sfs_transaction_set = sfs_transaction_set_create();
	if (sfs->sfs_transaction_set == NULL) {
		goto cleanup_renamelock;
	}

	/* journal */
	sfs->sfs_jphys = sfs_jphys_create();
	if (sfs->sfs_jphys == NULL) {
		goto cleanup_transaction_set;
	}

	return sfs;

cleanup_transaction_set:
	sfs_transaction_set_destroy(sfs->sfs_transaction_set);
cleanup_renamelock:
	lock_destroy(sfs->sfs_renamelock);
cleanup_freemaplock:
	lock_destroy(sfs->sfs_freemaplock);
cleanup_vnlock:
	lock_destroy(sfs->sfs_vnlock);
cleanup_vnodes:
	vnodearray_destroy(sfs->sfs_vnodes);
cleanup_object:
	kfree(sfs);
fail:
	return NULL;
}

static
void
sfs_undo_unsuccessful_transactions(struct sfs_fs *sfs, struct txid_tarray *commited_txs)
{
	int err;
	struct sfs_jiter *ji;
	enum sfs_record_type record_type;
	void *record_ptr;
	size_t record_len;
	struct sfs_record record;

	err = sfs_jiter_revcreate(sfs, &ji);
	if (err) {
		panic("Error while reading journal\n");
	}

	while (!sfs_jiter_done(ji)) {
		record_type = sfs_jiter_type(ji);
		record_ptr = sfs_jiter_rec(ji, &record_len);
		memcpy(&record, record_ptr, record_len);

		if (!txid_tarray_contains(commited_txs, (void*)record.r_txid)) {
			sfs_record_undo(sfs, record, record_type);
		}

		err = sfs_jiter_prev(sfs, ji);
		if (err) {
			panic("Error while reading journal\n");
		}
	}

	sfs_jiter_destroy(ji);
}

static
void
sfs_redo_records(struct sfs_fs *sfs)
{
	int err;
	struct sfs_jiter *ji;
	enum sfs_record_type record_type;
	void *record_ptr;
	size_t record_len;
	struct sfs_record record;

	err = sfs_jiter_fwdcreate(sfs, &ji);
	if (err) {
		panic("Error while reading journal\n");
	}

	while (!sfs_jiter_done(ji)) {
		record_type = sfs_jiter_type(ji);
		record_ptr = sfs_jiter_rec(ji, &record_len);
		memcpy(&record, record_ptr, record_len);

		sfs_record_redo(sfs, record, record_type);

		err = sfs_jiter_next(sfs, ji);
		if (err) {
			panic("Error while reading journal\n");
		}
	}

	sfs_jiter_destroy(ji);
}

// Returns an array of transaction id's that have commit records
static
struct txid_tarray *
sfs_check_records(struct sfs_fs *sfs)
{
	int err;
	struct sfs_jiter *ji;
	enum sfs_record_type record_type;
	struct txid_tarray *commited_txs;

	void *record_ptr;
	size_t record_len;
	struct sfs_record record;

	commited_txs = txid_tarray_create();
	if (commited_txs == NULL) {
		panic("Error while reading journal\n");
	}

	err = sfs_jiter_fwdcreate(sfs, &ji);
	if (err) {
		panic("Error while reading journal\n");
	}

	while (!sfs_jiter_done(ji)) {
		record_type = sfs_jiter_type(ji);

		if (record_type == R_TX_COMMIT) {
			record_ptr = sfs_jiter_rec(ji, &record_len);
			memcpy(&record, record_ptr, record_len);

			err = txid_tarray_add(commited_txs, (void*)record.r_txid, NULL);
			if (err) {
				panic("Error while reading journal\n");
			}
		}

		err = sfs_jiter_next(sfs, ji);
		if (err) {
			panic("Error while reading journal\n");
		}
	}

	sfs_jiter_destroy(ji);
	return commited_txs;
}

static
void
sfs_recover(struct fs *fs)
{
	int err;
	struct sfs_fs *sfs = fs->fs_data;
	struct txid_tarray *commited_txs;

	// Pass 1: (forward) note which transactions committed successfully
	commited_txs = sfs_check_records(sfs);

	// TODO: handle metadata->userdata changes

	// Pass 2: (forward) redo every record
	sfs_redo_records(sfs);

	// Pass 3: (reverse) undo transactions without a commit record
	sfs_undo_unsuccessful_transactions(sfs, commited_txs);

	txid_tarray_destroy(commited_txs);

	err = sync_fs_buffers(fs);
	if (err) {
		panic("Error while flushing during recovery\n");
	}

	err = sfs_sync_freemap(sfs);
	if (err) {
		panic("Error while flushing during recovery\n");
	}

	err = sfs_sync_superblock(sfs);
	if (err) {
		panic("Error while flushing during recovery\n");
	}
}

/*
 * Mount routine.
 *
 * The way mount works is that you call vfs_mount and pass it a
 * filesystem-specific mount routine. Said routine takes a device and
 * hands back a pointer to an abstract filesystem. You can also pass
 * a void pointer through.
 *
 * This organization makes cleanup on error easier. Hint: it may also
 * be easier to synchronize correctly; it is important not to get two
 * filesystems with the same name mounted at once, or two filesystems
 * mounted on the same device at once.
 */
static
int
sfs_domount(void *options, struct device *dev, struct fs **ret)
{
	int result;
	struct sfs_fs *sfs;
	sfs_lsn_t next_lsn;

	/* We don't pass any options through mount */
	(void)options;

	/*
	 * We can't mount on devices with the wrong sector size.
	 *
	 * (Note: for all intents and purposes here, "sector" and
	 * "block" are interchangeable terms. Technically a filesystem
	 * block may be composed of several hardware sectors, but we
	 * don't do that in sfs.)
	 */
	if (dev->d_blocksize != SFS_BLOCKSIZE) {
		kprintf("sfs: Cannot mount on device with blocksize %zu\n",
			dev->d_blocksize);
		return ENXIO;
	}

	sfs = sfs_fs_create();
	if (sfs == NULL) {
		return ENOMEM;
	}

	/* Set the device so we can use sfs_readblock() */
	sfs->sfs_device = dev;

	/* Acquire the locks so various stuff works right */
	lock_acquire(sfs->sfs_vnlock);
	lock_acquire(sfs->sfs_freemaplock);

	/* Load superblock */
	result = sfs_readblock(&sfs->sfs_absfs, SFS_SUPER_BLOCK,
			       &sfs->sfs_sb, sizeof(sfs->sfs_sb));
	if (result) {
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_freemaplock);
		sfs->sfs_device = NULL;
		sfs_fs_destroy(sfs);
		return result;
	}

	/* Make some simple sanity checks */

	if (sfs->sfs_sb.sb_magic != SFS_MAGIC) {
		kprintf("sfs: Wrong magic number in superblock "
			"(0x%x, should be 0x%x)\n",
			sfs->sfs_sb.sb_magic,
			SFS_MAGIC);
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_freemaplock);
		sfs->sfs_device = NULL;
		sfs_fs_destroy(sfs);
		return EINVAL;
	}

	if (sfs->sfs_sb.sb_journalblocks >= sfs->sfs_sb.sb_nblocks) {
		kprintf("sfs: warning - journal takes up whole volume\n");
	}

	if (sfs->sfs_sb.sb_nblocks > dev->d_blocks) {
		kprintf("sfs: warning - fs has %u blocks, device has %u\n",
			sfs->sfs_sb.sb_nblocks, dev->d_blocks);
	}

	/* Ensure null termination of the volume name */
	sfs->sfs_sb.sb_volname[sizeof(sfs->sfs_sb.sb_volname)-1] = 0;

	/* Load free block bitmap */
	sfs->sfs_freemap = bitmap_create(SFS_FS_FREEMAPBITS(sfs));
	if (sfs->sfs_freemap == NULL) {
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_freemaplock);
		sfs->sfs_device = NULL;
		sfs_fs_destroy(sfs);
		return ENOMEM;
	}
	result = sfs_freemapio(sfs, UIO_READ);
	if (result) {
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_freemaplock);
		sfs->sfs_device = NULL;
		sfs_fs_destroy(sfs);
		return result;
	}

	/* Hand back the abstract fs */
	*ret = &sfs->sfs_absfs;

	lock_release(sfs->sfs_vnlock);
	lock_release(sfs->sfs_freemaplock);

	reserve_fsmanaged_buffers(2, SFS_BLOCKSIZE);

	/*
	 * Load up the journal container. (basically, recover it)
	 */

	SAY("*** Loading up the jphys container ***\n");
	result = sfs_jphys_loadup(sfs);
	if (result) {
		unreserve_fsmanaged_buffers(2, SFS_BLOCKSIZE);
		drop_fs_buffers(&sfs->sfs_absfs);
		sfs->sfs_device = NULL;
		sfs_fs_destroy(sfs);
		return result;
    }

	/*
	 * High-level recovery.
	 */

	/* Enable container-level scanning */
	sfs_jphys_startreading(sfs);

	reserve_buffers(SFS_BLOCKSIZE);

	sfs_recover(&sfs->sfs_absfs);

	unreserve_buffers(SFS_BLOCKSIZE);

	/* Done with container-level scanning */
	sfs_jphys_stopreading(sfs);

	/* Spin up the journal. */
	SAY("*** Starting up ***\n");
	result = sfs_jphys_startwriting(sfs);
	if (result) {
		unreserve_fsmanaged_buffers(2, SFS_BLOCKSIZE);
		drop_fs_buffers(&sfs->sfs_absfs);
		sfs->sfs_device = NULL;
		sfs_fs_destroy(sfs);
		return result;
	}

	reserve_buffers(SFS_BLOCKSIZE);

	/* Empty the journal */
	next_lsn = sfs_jphys_peeknextlsn(sfs);
	sfs_jphys_trim(sfs, next_lsn);
	sfs_jphys_flushall(sfs);

	/* Empty the graveyard, and empty the journal again */
	graveyard_flush(sfs);
	next_lsn = sfs_jphys_peeknextlsn(sfs);
	sfs_jphys_trim(sfs, next_lsn);
	sfs_jphys_flushall(sfs);

	unreserve_buffers(SFS_BLOCKSIZE);

	return 0;
}

/*
 * Actual function called from high-level code to mount an sfs.
 */
int
sfs_mount(const char *device)
{
	return vfs_mount(device, NULL, sfs_domount);
}
