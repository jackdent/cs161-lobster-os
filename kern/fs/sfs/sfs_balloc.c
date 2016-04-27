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
 * Block allocation.
 */
#include <types.h>
#include <lib.h>
#include <bitmap.h>
#include <synch.h>
#include <buf.h>
#include <sfs.h>
#include "sfsprivate.h"

/*
 * Zero out a disk block.
 *
 * Uses one buffer; returns it if bufret is not NULL.
 */
static
int
sfs_clearblock(struct sfs_fs *sfs, daddr_t block, struct buf **bufret)
{
	struct buf *buf;
	void *ptr;
	int result;

	result = buffer_get(&sfs->sfs_absfs, block, SFS_BLOCKSIZE, &buf);
	if (result) {
		return result;
	}

	ptr = buffer_map(buf);
	bzero(ptr, SFS_BLOCKSIZE);
	buffer_mark_valid(buf);
	buffer_mark_dirty(buf);

	if (bufret != NULL) {
		*bufret = buf;
	}
	else {
		buffer_release(buf);
	}

	return 0;
}

/*
 * Allocate a block.
 *
 * Returns the block number, plus a buffer for it if BUFRET isn't
 * null. The buffer, if any, is marked valid and dirty, and zeroed
 * out.
 *
 * Uses 1 buffer.
 */
int
sfs_balloc(struct sfs_fs *sfs, daddr_t *diskblock, struct buf **bufret)
{
	int result;

	lock_acquire(sfs->sfs_freemaplock);

	result = bitmap_alloc(sfs->sfs_freemap, diskblock);
	if (result) {
		lock_release(sfs->sfs_freemaplock);
		return result;
	}
	sfs->sfs_freemapdirty = true;

	lock_release(sfs->sfs_freemaplock);

	if (*diskblock >= sfs->sfs_sb.sb_nblocks) {
		panic("sfs: %s: balloc: invalid block %u\n",
		      sfs->sfs_sb.sb_volname, *diskblock);
	}

	/* Clear block before returning it */
	result = sfs_clearblock(sfs, *diskblock, bufret);
	if (result) {
		lock_acquire(sfs->sfs_freemaplock);
		bitmap_unmark(sfs->sfs_freemap, *diskblock);
		lock_release(sfs->sfs_freemaplock);
	}
	return result;
}

/*
 * Free a block, for when we already have the freemap locked.
 */
void
sfs_bfree_prelocked(struct sfs_fs *sfs, daddr_t diskblock)
{
	KASSERT(lock_do_i_hold(sfs->sfs_freemaplock));

	bitmap_unmark(sfs->sfs_freemap, diskblock);
	sfs->sfs_freemapdirty = true;
}

/*
 * Free a block.
 *
 * Note: the caller should in general invalidate any buffers it has
 * for the block, either with buffer_release_and_invalidate() or
 * buffer_drop(), before coming here. (We could call buffer_drop()
 * here; but in some cases that would generate redundant work.)
 */
void
sfs_bfree(struct sfs_fs *sfs, daddr_t diskblock)
{
	lock_acquire(sfs->sfs_freemaplock);
	sfs_bfree_prelocked(sfs, diskblock);
	lock_release(sfs->sfs_freemaplock);
}

/*
 * Check if a block is in use.
 */
int
sfs_bused(struct sfs_fs *sfs, daddr_t diskblock)
{
	int result;

	if (diskblock >= sfs->sfs_sb.sb_nblocks) {
		panic("sfs: %s: sfs_bused called on out of range block %u\n",
		      sfs->sfs_sb.sb_volname, diskblock);
	}

	lock_acquire(sfs->sfs_freemaplock);
	result = bitmap_isset(sfs->sfs_freemap, diskblock);
	lock_release(sfs->sfs_freemaplock);

	return result;
}

/*
 * Explicitly lock and unlock the freemap.
 */
void
sfs_lock_freemap(struct sfs_fs *sfs)
{
	lock_acquire(sfs->sfs_freemaplock);
}

void
sfs_unlock_freemap(struct sfs_fs *sfs)
{
	lock_release(sfs->sfs_freemaplock);
}
