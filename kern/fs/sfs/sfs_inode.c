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
 * Inode-level operations and vnode/inode lifecycle logic.
 */
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <vfs.h>
#include <buf.h>
#include <sfs.h>
#include "sfsprivate.h"


/*
 * Constructor for sfs_vnode.
 */
static
struct sfs_vnode *
sfs_vnode_create(uint32_t ino, unsigned type)
{
	struct sfs_vnode *sv;

	sv = kmalloc(sizeof(*sv));
	if (sv == NULL) {
		return NULL;
	}
	sv->sv_lock = lock_create("sfs_vnode");
	if (sv->sv_lock == NULL) {
		kfree(sv);
		return NULL;
	}
	sv->sv_ino = ino;
	sv->sv_type = type;
	sv->sv_dinobuf = NULL;
	sv->sv_dinobufcount = 0;
	return sv;
}

/*
 * Destructor for sfs_vnode.
 */
static
void
sfs_vnode_destroy(struct sfs_vnode *victim)
{
	lock_destroy(victim->sv_lock);
	kfree(victim);
}

/*
 * Load the on-disk inode into sv->sv_dinobuf. This should be done at
 * the beginning of any operation that will need to read or change the
 * inode. When the operation is done, sfs_dinode_unload should be
 * called to release the buffer.
 *
 * XXX: currently it's not done at the beginning most places, and
 * sometimes more than once, so for now it needs to be recursive and
 * we count how many times it's been loaded.
 *
 * Locking: must hold the vnode lock.
 */
int
sfs_dinode_load(struct sfs_vnode *sv)
{
	struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
	int result;

	KASSERT(lock_do_i_hold(sv->sv_lock));

	if (sv->sv_dinobufcount == 0) {
		KASSERT(sv->sv_dinobuf == NULL);
		result = buffer_read(&sfs->sfs_absfs, sv->sv_ino,SFS_BLOCKSIZE,
				     &sv->sv_dinobuf);
		if (result) {
			return result;
		}
	}
	else {
		KASSERT(sv->sv_dinobuf != NULL);
	}
	sv->sv_dinobufcount++;

	return 0;
}

/*
 * Unload the on-disk inode.
 *
 * This should be done in matching pairs with sfs_dinode_load.
 * Ideally this should be exactly once per operation when the
 * operation starts and ends, but we aren't there yet. (XXX)
 *
 * Locking: must hold the vnode lock.
 */
void
sfs_dinode_unload(struct sfs_vnode *sv)
{
	KASSERT(lock_do_i_hold(sv->sv_lock));

	KASSERT(sv->sv_dinobuf != NULL);
	KASSERT(sv->sv_dinobufcount > 0);

	sv->sv_dinobufcount--;
	if (sv->sv_dinobufcount == 0) {
		buffer_release(sv->sv_dinobuf);
		sv->sv_dinobuf = NULL;
	}
}

/*
 * Return a pointer to the on-disk inode. Per the semantics of
 * buffer_map, the pointer remains valid until the buffer is released,
 * that is, when sfs_dinode_unload is called.
 *
 * Locking: must hold the vnode lock.
 */
struct sfs_dinode *
sfs_dinode_map(struct sfs_vnode *sv)
{
	KASSERT(lock_do_i_hold(sv->sv_lock));

	KASSERT(sv->sv_dinobuf != NULL);
	return buffer_map(sv->sv_dinobuf);
}

/*
 * Mark the on-disk inode dirty after scribbling in it with
 * sfs_dinode_map.
 *
 * Locking: must hold the vnode lock.
 */
void
sfs_dinode_mark_dirty(struct sfs_vnode *sv)
{
	KASSERT(lock_do_i_hold(sv->sv_lock));

	KASSERT(sv->sv_dinobuf != NULL);
	buffer_mark_dirty(sv->sv_dinobuf);
}

/*
 * Called when the vnode refcount (in-memory usage count) hits zero.
 *
 * This function should try to avoid returning errors other than EBUSY.
 *
 * Locking: gets/releases vnode lock. Gets/releases sfs_vnlock, and
 *    possibly also sfs_freemaplock, while holding the vnode lock.
 *
 * Requires 1 buffer locally but may also afterward call sfs_itrunc,
 * which takes 4.
 */
int
sfs_reclaim(struct vnode *v)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct sfs_dinode *iptr;
	unsigned ix, i, num;
	bool buffers_needed;
	int result;

	lock_acquire(sv->sv_lock);
	lock_acquire(sfs->sfs_vnlock);

	/*
	 * Make sure someone else hasn't picked up the vnode since the
	 * decision was made to reclaim it. (This must interact
	 * properly with sfs_loadvnode.)
	 */
	spinlock_acquire(&v->vn_countlock);
	if (v->vn_refcount != 1) {

		/* consume the reference VOP_DECREF gave us */
		KASSERT(v->vn_refcount>1);
		v->vn_refcount--;

		spinlock_release(&v->vn_countlock);
		lock_release(sfs->sfs_vnlock);
		lock_release(sv->sv_lock);
		return EBUSY;
	}
	spinlock_release(&v->vn_countlock);

	/*
	 * This grossness arises because reclaim gets called via
	 * VOP_DECREF, which can happen either from outside SFS or
	 * from inside, so we might or might not be inside another
	 * operation.
	 */
	buffers_needed = !curthread->t_did_reserve_buffers;
	if (buffers_needed) {
		reserve_buffers(SFS_BLOCKSIZE);
	}

	/* Get the on-disk inode. */
	result = sfs_dinode_load(sv);
	if (result) {
		/*
		 * This case is likely to lead to problems, but
		 * there's essentially no helping it...
		 */
		lock_release(sfs->sfs_vnlock);
		lock_release(sv->sv_lock);
		if (buffers_needed) {
			unreserve_buffers(SFS_BLOCKSIZE);
		}
		return result;
	}
	iptr = sfs_dinode_map(sv);

	/* If there are no on-disk references to the file either, erase it. */
	if (iptr->sfi_linkcount == 0) {
		result = sfs_itrunc(sv, 0);
		if (result) {
			sfs_dinode_unload(sv);
			lock_release(sfs->sfs_vnlock);
			lock_release(sv->sv_lock);
			if (buffers_needed) {
				unreserve_buffers(SFS_BLOCKSIZE);
			}
			return result;
		}
		sfs_dinode_unload(sv);
		/* Discard the inode */
		buffer_drop(&sfs->sfs_absfs, sv->sv_ino, SFS_BLOCKSIZE);
		sfs_bfree(sfs, sv->sv_ino);
	}
	else {
		sfs_dinode_unload(sv);
	}

	if (buffers_needed) {
		unreserve_buffers(SFS_BLOCKSIZE);
	}

	/* Remove the vnode structure from the table in the struct sfs_fs. */
	num = vnodearray_num(sfs->sfs_vnodes);
	ix = num;
	for (i=0; i<num; i++) {
		struct vnode *v2 = vnodearray_get(sfs->sfs_vnodes, i);
		struct sfs_vnode *sv2 = v2->vn_data;
		if (sv2 == sv) {
			ix = i;
			break;
		}
	}
	if (ix == num) {
		panic("sfs: %s: reclaim vnode %u not in vnode pool\n",
		      sfs->sfs_sb.sb_volname, sv->sv_ino);
	}
	vnodearray_remove(sfs->sfs_vnodes, ix);

	vnode_cleanup(&sv->sv_absvn);

	lock_release(sfs->sfs_vnlock);
	lock_release(sv->sv_lock);

	sfs_vnode_destroy(sv);

	/* Done */
	return 0;
}

/*
 * Function to load a inode into memory as a vnode, or dig up one
 * that's already resident.
 *
 * The vnode is returned unlocked and with its inode not loaded.
 *
 * Locking: gets/releases sfs_vnlock.
 *
 * May require 3 buffers if VOP_DECREF triggers reclaim.
 */
int
sfs_loadvnode(struct sfs_fs *sfs, uint32_t ino, int forcetype,
		 struct sfs_vnode **ret)
{
	struct vnode *v;
	struct sfs_vnode *sv;
	struct buf *dinobuf;
	struct sfs_dinode *dino;
	const struct vnode_ops *ops;
	unsigned i, num;
	int result;

	/* sfs_vnlock protects the vnodes table */
	lock_acquire(sfs->sfs_vnlock);

	/* Look in the vnodes table */
	num = vnodearray_num(sfs->sfs_vnodes);

	/* Linear search. Is this too slow? You decide. */
	for (i=0; i<num; i++) {
		v = vnodearray_get(sfs->sfs_vnodes, i);
		sv = v->vn_data;

		/* Every inode in memory must be in an allocated block */
		if (!sfs_bused(sfs, sv->sv_ino)) {
			panic("sfs: %s: Found inode %u in unallocated block\n",
			      sfs->sfs_sb.sb_volname, sv->sv_ino);
		}

		if (sv->sv_ino==ino) {
			/* Found */

			/* forcetype is only allowed when creating objects */
			KASSERT(forcetype==SFS_TYPE_INVAL);

			VOP_INCREF(&sv->sv_absvn);
			lock_release(sfs->sfs_vnlock);

			*ret = sv;
			return 0;
		}
	}

	/* Didn't have it loaded; load it */

	/* Null out sv to avoid accidental use before it's reassigned below */
	sv = NULL;

	/* Must be in an allocated block */
	if (!sfs_bused(sfs, ino)) {
		panic("sfs: %s: Tried to load inode %u from "
		      "unallocated block\n", sfs->sfs_sb.sb_volname, ino);
	}

	/*
	 * Read the block the inode is in.
	 *
	 * (We can do this before creating and locking the new vnode
	 * because we are holding the vnode table lock. Nobody else can
	 * be in here trying to load the same vnode at the same time.)
	 */
	result = buffer_read(&sfs->sfs_absfs, ino, SFS_BLOCKSIZE, &dinobuf);
	if (result) {
		lock_release(sfs->sfs_vnlock);
		return result;
	}
	dino = buffer_map(dinobuf);

	/*
	 * FORCETYPE is set if we're creating a new file, because the
	 * buffer will have been zeroed out already and thus the type
	 * recorded there will be SFS_TYPE_INVAL.
	 */
	if (forcetype != SFS_TYPE_INVAL) {
		KASSERT(dino->sfi_type == SFS_TYPE_INVAL);
		dino->sfi_type = forcetype;
		buffer_mark_dirty(dinobuf);
	}

	/*
	 * Choose the function table based on the object type,
	 * and cache the type in the vnode.
	 */
	switch (dino->sfi_type) {
	    case SFS_TYPE_FILE:
		ops = &sfs_fileops;
		break;
	    case SFS_TYPE_DIR:
		ops = &sfs_dirops;
		break;
	    default:
		panic("sfs: %s: loadvnode: Invalid inode type "
		      "(inode %u, type %u)\n", sfs->sfs_sb.sb_volname,
		      ino, dino->sfi_type);
	}

	/*
	 * Now, cons up a vnode. Don't give it the inode buffer, as to
	 * be consistent with the case where the vnode is already in
	 * memory (which cannot safely lock the vnode in order to load
	 * the inode) we return the vnode without the inode
	 * loaded. (The buffer will be warm in the buffer cache, so a
	 * subsequent sfs_dinode_load won't incur another I/O.)
	 */
	sv = sfs_vnode_create(ino, dino->sfi_type);
	if (sv==NULL) {
		lock_release(sfs->sfs_vnlock);
		return ENOMEM;
	}

	buffer_release(dinobuf);

	/* Call the common vnode initializer */
	result = vnode_init(&sv->sv_absvn, ops, &sfs->sfs_absfs, sv);
	if (result) {
		sfs_vnode_destroy(sv);
		lock_release(sfs->sfs_vnlock);
		return result;
	}

	/* Add it to our table */
	result = vnodearray_add(sfs->sfs_vnodes, &sv->sv_absvn, NULL);
	if (result) {
		vnode_cleanup(&sv->sv_absvn);
		sfs_vnode_destroy(sv);
		lock_release(sfs->sfs_vnlock);
		return result;
	}
	lock_release(sfs->sfs_vnlock);

	/* Hand it back */
	*ret = sv;
	return 0;
}

/*
 * Create a new filesystem object and hand back its vnode.
 * Always hands back vnode "locked and loaded"
 *
 * As a matter of convenience, returns the vnode with its inode loaded.
 *
 * Locking: Gets/release sfs_freemaplock.
 *    Also gets/releases sfs_vnlock, but does not hold them together.
 *
 * Requires up to 3 buffers as sfs_loadvnode might trigger reclaim and
 * truncate.
 */
int
sfs_makeobj(struct sfs_fs *sfs, int type, struct sfs_vnode **ret)
{
	uint32_t ino;
	struct sfs_dinode *dino;
	int result;

	/*
	 * First, get an inode. (Each inode is a block, and the inode
	 * number is the block number, so just get a block.)
	 */

	result = sfs_balloc(sfs, &ino, NULL);
	if (result) {
		return result;
	}

	/*
	 * Now load a vnode for it.
	 */

	result = sfs_loadvnode(sfs, ino, type, ret);
	if (result) {
		buffer_drop(&sfs->sfs_absfs, ino, SFS_BLOCKSIZE);
		sfs_bfree(sfs, ino);
		return result;
	}

	/* And load the inode. */
	lock_acquire((*ret)->sv_lock);
	result = sfs_dinode_load(*ret);
	if (result) {
		lock_release((*ret)->sv_lock);
		/* this reclaims the inode */
		VOP_DECREF(&(*ret)->sv_absvn);
		return result;
	}

	/* new object; link count should start zero */
	dino = sfs_dinode_map(*ret);
	KASSERT(dino->sfi_linkcount == 0);

	return result;
}

/*
 * Get vnode for the root of the filesystem.
 * The root vnode is always found in block 1 (SFS_ROOTDIR_INO).
 *
 * Locking: none besides what sfs_loadvnode does.
 */
int
sfs_getroot(struct fs *fs, struct vnode **ret)
{
	struct sfs_fs *sfs = fs->fs_data;
	struct sfs_vnode *sv;
	int result;

	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_loadvnode(sfs, SFS_ROOTDIR_INO, SFS_TYPE_INVAL, &sv);
	if (result) {
		kprintf("sfs: %s: getroot: Cannot load root vnode\n",
			sfs->sfs_sb.sb_volname);
		unreserve_buffers(SFS_BLOCKSIZE);
		return result;
	}

	if (sv->sv_type != SFS_TYPE_DIR) {
		kprintf("sfs: %s: getroot: not directory (type %u)\n",
			sfs->sfs_sb.sb_volname, sv->sv_type);
		unreserve_buffers(SFS_BLOCKSIZE);
		return EINVAL;
	}

	unreserve_buffers(SFS_BLOCKSIZE);

	*ret = &sv->sv_absvn;
	return 0;
}
