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
 * File-level (vnode) interface routines.
 */
#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <limits.h>
#include <stat.h>
#include <lib.h>
#include <uio.h>
#include <synch.h>
#include <vfs.h>
#include <buf.h>
#include <sfs.h>
#include "sfsprivate.h"

/*
 * Locking protocol for sfs:
 *    The following locks exist:
 *       vnode locks (sv_lock)
 *       vnode table lock (sfs_vnlock)
 *       freemap lock (sfs_freemaplock)
 *       rename lock (sfs_renamelock)
 *       buffer lock
 *
 *    Ordering constraints:
 *       rename lock       before  vnode locks
 *       vnode locks       before  vnode table lock
 *       vnode locks       before  buffer locks
 *       vnode table lock  before  freemap lock
 *       buffer lock       before  freemap lock
 *
 *    I believe the vnode table lock and the buffer locks are
 *    independent.
 *
 *    Ordering among vnode locks:
 *       directory lock    before  lock of a file within the directory
 *
 *    Ordering among directory locks:
 *       Parent first, then child.
 */

/* Slot in a directory that ".." is expected to appear in */
#define DOTDOTSLOT  1

////////////////////////////////////////////////////////////
// Vnode operations.

/*
 * This is called on *each* open().
 *
 * Locking: not needed
 */
static
int
sfs_eachopen(struct vnode *v, int openflags)
{
	/*
	 * At this level we do not need to handle O_CREAT, O_EXCL,
	 * O_TRUNC, or O_APPEND.
	 *
	 * Any of O_RDONLY, O_WRONLY, and O_RDWR are valid, so we don't need
	 * to check that either.
	 */

	(void)v;
	(void)openflags;

	return 0;
}

/*
 * This is called on *each* open() of a directory.
 * Directories may only be open for read.
 *
 * Locking: not needed
 */
static
int
sfs_eachopendir(struct vnode *v, int openflags)
{
	switch (openflags & O_ACCMODE) {
	    case O_RDONLY:
		break;
	    case O_WRONLY:
	    case O_RDWR:
	    default:
		return EISDIR;
	}
	if (openflags & O_APPEND) {
		return EISDIR;
	}

	(void)v;
	return 0;
}

/*
 * Called for read(). sfs_io() does the work.
 *
 * Locking: gets/releases vnode lock.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_read(struct vnode *v, struct uio *uio)
{
	struct sfs_vnode *sv = v->vn_data;
	int result;

	KASSERT(uio->uio_rw==UIO_READ);

	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_io(sv, uio);

	unreserve_buffers(SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);

	return result;
}

/*
 * Called for write(). sfs_io() does the work.
 *
 * Locking: gets/releases vnode lock.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_write(struct vnode *v, struct uio *uio)
{
	struct sfs_vnode *sv = v->vn_data;
	int result;

	KASSERT(uio->uio_rw==UIO_WRITE);

	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_io(sv, uio);

	unreserve_buffers(SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);

	return result;
}

/*
 * Called for getdirentry()
 *
 * Locking: gets/releases vnode lock.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_getdirentry(struct vnode *v, struct uio *uio)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_direntry tsd;
	off_t pos;
	int nentries;
	int result;

	KASSERT(uio->uio_offset >= 0);
	KASSERT(uio->uio_rw==UIO_READ);
	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_dinode_load(sv);
	if (result) {
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	result = sfs_dir_nentries(sv, &nentries);
	if (result) {
		sfs_dinode_unload(sv);
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	/* Use uio_offset as the slot index. */
	pos = uio->uio_offset;

	while (1) {
		if (pos >= nentries) {
			/* EOF */
			result = 0;
			break;
		}

		result = sfs_readdir(sv, pos, &tsd);
		if (result) {
			break;
		}

		pos++;

		if (tsd.sfd_ino == SFS_NOINO) {
			/* Blank entry */
			continue;
		}

		/* Ensure null termination, just in case */
		tsd.sfd_name[sizeof(tsd.sfd_name)-1] = 0;

		result = uiomove(tsd.sfd_name, strlen(tsd.sfd_name), uio);
		break;
	}

	sfs_dinode_unload(sv);

	unreserve_buffers(SFS_BLOCKSIZE);

	lock_release(sv->sv_lock);

	/* Update the offset the way we want it */
	uio->uio_offset = pos;

	return result;
}

/*
 * Called for ioctl()
 * Locking: not needed.
 */
static
int
sfs_ioctl(struct vnode *v, int op, userptr_t data)
{
	/*
	 * No ioctls.
	 */

	(void)v;
	(void)op;
	(void)data;

	return EINVAL;
}

/*
 * Called for stat/fstat/lstat.
 *
 * Locking: gets/releases vnode lock.
 *
 * Requires 1 buffer.
 */
static
int
sfs_stat(struct vnode *v, struct stat *statbuf)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_dinode *inodeptr;
	int result;

	/* Fill in the stat structure */
	bzero(statbuf, sizeof(struct stat));

	result = VOP_GETTYPE(v, &statbuf->st_mode);
	if (result) {
		return result;
	}

	lock_acquire(sv->sv_lock);

	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_dinode_load(sv);
	if (result) {
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	inodeptr = sfs_dinode_map(sv);

	statbuf->st_size = inodeptr->sfi_size;
	statbuf->st_nlink = inodeptr->sfi_linkcount;

	/* We don't support this yet */
	statbuf->st_blocks = 0;

	/* Fill in other fields as desired/possible... */

	sfs_dinode_unload(sv);
	unreserve_buffers(SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);
	return 0;
}

/*
 * Return the type of the file (types as per kern/stat.h)
 * Locking: not needed (the type of the vnode is fixed once it's created)
 */
static
int
sfs_gettype(struct vnode *v, uint32_t *ret)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = v->vn_fs->fs_data;

	switch (sv->sv_type) {
	case SFS_TYPE_FILE:
		*ret = S_IFREG;
		return 0;
	case SFS_TYPE_DIR:
		*ret = S_IFDIR;
		return 0;
	}
	panic("sfs: %s: gettype: Invalid inode type (inode %u, type %u)\n",
	      sfs->sfs_sb.sb_volname, sv->sv_ino, sv->sv_type);
	return EINVAL;
}

/*
 * Check if seeking is allowed. The answer is "yes".
 *
 * Locking: not needed
 */
static
bool
sfs_isseekable(struct vnode *v)
{
	(void)v;
	return true;
}

/*
 * Called for fsync().
 *
 * Since for now the buffer cache can't sync just one file, sync the
 * whole fs. This is lame.
 *
 * Locking: gets/releases vnode lock. (XXX: really?)
 */
static
int
sfs_fsync(struct vnode *v)
{
	struct sfs_vnode *sv = v->vn_data;

	return FSOP_SYNC(sv->sv_absvn.vn_fs);
}

/*
 * Called for mmap().
 */
static
int
sfs_mmap(struct vnode *v   /* add stuff as needed */)
{
	(void)v;
	return ENOSYS;
}

/*
 * Truncate a file.
 *
 * Locking: gets/releases vnode lock.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_truncate(struct vnode *v, off_t len)
{
	struct sfs_vnode *sv = v->vn_data;
	int result;

	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_itrunc(sv, len);

	unreserve_buffers(SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);
	return result;
}

/*
 * Helper function for sfs_namefile.
 *
 * Locking: must hold vnode lock on parent.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_getonename(struct sfs_vnode *parent, uint32_t targetino,
	       char *buf, size_t *bufpos)
{
	size_t bp = *bufpos;

	struct sfs_direntry sd;
	size_t namelen;
	int result;

	KASSERT(lock_do_i_hold(parent->sv_lock));
	KASSERT(targetino != SFS_NOINO);

	result = sfs_dir_findino(parent, targetino, &sd, NULL);
	if (result) {
		return result;
	}

	/* include a trailing slash in the length */
	namelen = strlen(sd.sfd_name)+1;
	if (namelen > bp) {
		/*
		 * Doesn't fit. ERANGE is the error from the BSD man page,
		 * even though ENAMETOOLONG would make more sense...
		 */
		return ERANGE;
	}
	buf[bp-1] = '/';
	memmove(buf+bp-namelen, sd.sfd_name, namelen-1);
	*bufpos = bp-namelen;
	return 0;
}

/*
 * Get the full pathname for a file. This only needs to work on directories.
 *
 * Locking: Gets/releases vnode locks, but only one at a time.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_namefile(struct vnode *vv, struct uio *uio)
{
	struct sfs_vnode *sv = vv->vn_data;
	struct sfs_vnode *parent = NULL;
	int result;
	char *buf;
	size_t bufpos, bufmax, len;

	KASSERT(uio->uio_rw == UIO_READ);

	bufmax = uio->uio_resid+1;
	if (bufmax > PATH_MAX) {
		bufmax = PATH_MAX;
	}

	buf = kmalloc(bufmax);
	if (buf == NULL) {
		return ENOMEM;
	}

	reserve_buffers(SFS_BLOCKSIZE);

	bufpos = bufmax;

	VOP_INCREF(&sv->sv_absvn);

	while (1) {
		lock_acquire(sv->sv_lock);
		/* not allowed to lock child since we're going up the tree */
		result = sfs_lookonce(sv, "..", &parent, NULL);
		lock_release(sv->sv_lock);

		if (result) {
			VOP_DECREF(&sv->sv_absvn);
			kfree(buf);
			unreserve_buffers(SFS_BLOCKSIZE);
			return result;
		}

		if (parent == sv) {
			/* .. was equal to . - must be root, so we're done */
			VOP_DECREF(&parent->sv_absvn);
			VOP_DECREF(&sv->sv_absvn);
			break;
		}

		lock_acquire(parent->sv_lock);
		result = sfs_getonename(parent, sv->sv_ino, buf, &bufpos);
		lock_release(parent->sv_lock);

		if (result) {
			VOP_DECREF(&parent->sv_absvn);
			VOP_DECREF(&sv->sv_absvn);
			kfree(buf);
			unreserve_buffers(SFS_BLOCKSIZE);
			return result;
		}

		VOP_DECREF(&sv->sv_absvn);
		sv = parent;
		parent = NULL;
	}

	/* Done looking, now send back the string */

	if (bufmax == bufpos) {
		/* root directory; do nothing (send back empty string) */
		result = 0;
	}
	else {
		len = bufmax - bufpos;
		len--;  /* skip the trailing slash */
		KASSERT(len <= uio->uio_resid);
		result = uiomove(buf+bufpos, len, uio);
	}

	kfree(buf);
	unreserve_buffers(SFS_BLOCKSIZE);
	return result;
}

/*
 * Create a file. If EXCL is set, insist that the filename not already
 * exist; otherwise, if it already exists, just open it.
 *
 * Locking: Gets/releases the vnode lock for v. Does not lock the new vnode,
 * as nobody else can get to it except by searching the directory it's in,
 * which is locked.
 *
 * Requires up to 4 buffers as VOP_DECREF invocations may take 3.
 */
static
int
sfs_creat(struct vnode *v, const char *name, bool excl, mode_t mode,
	  struct vnode **ret)
{
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_vnode *newguy;
	struct sfs_dinode *sv_dino;
	struct sfs_dinode *new_dino;
	uint32_t ino;
	int result;

	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_dinode_load(sv);
	if (result) {
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}
	sv_dino = sfs_dinode_map(sv);

	if (sv_dino->sfi_linkcount == 0) {
		sfs_dinode_unload(sv);
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return ENOENT;
	}

	sfs_dinode_unload(sv);

	/* Look up the name */
	result = sfs_dir_findname(sv, name, &ino, NULL, NULL);
	if (result!=0 && result!=ENOENT) {
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	/* If it exists and we didn't want it to, fail */
	if (result==0 && excl) {
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return EEXIST;
	}

	if (result==0) {
		/* We got something; load its vnode and return */
		result = sfs_loadvnode(sfs, ino, SFS_TYPE_INVAL, &newguy);
		if (result) {
			unreserve_buffers(SFS_BLOCKSIZE);
			lock_release(sv->sv_lock);
			return result;
		}

		*ret = &newguy->sv_absvn;
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return 0;
	}

	/* Didn't exist - create it */
	result = sfs_makeobj(sfs, SFS_TYPE_FILE, &newguy);
	if (result) {
		unreserve_buffers(SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	/* sfs_makeobj loads the inode for us */
	new_dino = sfs_dinode_map(newguy);

	/* We don't currently support file permissions; ignore MODE */
	(void)mode;

	/* Link it into the directory */
	result = sfs_dir_link(sv, name, newguy->sv_ino, NULL);
	if (result) {
		sfs_dinode_unload(newguy);
		lock_release(newguy->sv_lock);
		VOP_DECREF(&newguy->sv_absvn);
		lock_release(sv->sv_lock);
		unreserve_buffers(SFS_BLOCKSIZE);
		return result;
	}

	/* Update the linkcount of the new file */
	new_dino->sfi_linkcount++;

	/* and consequently mark it dirty. */
	sfs_dinode_mark_dirty(newguy);

	*ret = &newguy->sv_absvn;

	sfs_dinode_unload(newguy);
	unreserve_buffers(SFS_BLOCKSIZE);
	lock_release(newguy->sv_lock);
	lock_release(sv->sv_lock);
	return 0;
}

/*
 * Make a hard link to a file.
 * The VFS layer should prevent this being called unless both
 * vnodes are ours.
 *
 * Locking: locks both vnodes, parent first. Since we aren't allowed
 * to hardlink directories, the target can't be an ancestor of the
 * directory we're working in.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_link(struct vnode *dir, const char *name, struct vnode *file)
{
	struct sfs_vnode *sv = dir->vn_data;
	struct sfs_vnode *f = file->vn_data;
	struct sfs_dinode *inodeptr;
	int result;

	KASSERT(file->vn_fs == dir->vn_fs);

	/* Hard links to directories aren't allowed. */
	if (f->sv_type == SFS_TYPE_DIR) {
		return EINVAL;
	}
	KASSERT(file != dir);

	reserve_buffers(SFS_BLOCKSIZE);

	/* directory must be locked first */
	lock_acquire(sv->sv_lock);
	lock_acquire(f->sv_lock);

	result = sfs_dinode_load(f);
	if (result) {
		lock_release(f->sv_lock);
		lock_release(sv->sv_lock);
		unreserve_buffers(SFS_BLOCKSIZE);
		return result;
	}

	/* Create the link */
	result = sfs_dir_link(sv, name, f->sv_ino, NULL);
	if (result) {
		sfs_dinode_unload(f);
		lock_release(f->sv_lock);
		lock_release(sv->sv_lock);
		unreserve_buffers(SFS_BLOCKSIZE);
		return result;
	}

	/* and update the link count, marking the inode dirty */
	inodeptr = sfs_dinode_map(f);
	inodeptr->sfi_linkcount++;
	sfs_dinode_mark_dirty(f);

	sfs_dinode_unload(f);
	lock_release(f->sv_lock);
	lock_release(sv->sv_lock);
	unreserve_buffers(SFS_BLOCKSIZE);
	return 0;
}

/*
 * Create a directory.
 *
 * Locking: Acquires vnode lock on both parent and new directory.
 * Note that the ordering is not significant - nobody can hold the
 * lock on the new directory, because we just created it and nobody
 * else can get to it until we unlock the parent at the end.
 *
 * Requires up to 4 buffers;
 */

static
int
sfs_mkdir(struct vnode *v, const char *name, mode_t mode)
{
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct sfs_vnode *sv = v->vn_data;
	int result;
	uint32_t ino;
	struct sfs_dinode *dir_inodeptr;
	struct sfs_dinode *new_inodeptr;
	struct sfs_vnode *newguy;

	(void)mode;

	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_dinode_load(sv);
	if (result) {
		goto die_early;
	}
	dir_inodeptr = sfs_dinode_map(sv);

	if (dir_inodeptr->sfi_linkcount == 0) {
		result = ENOENT;
		goto die_simple;
	}

	/* Look up the name */
	result = sfs_dir_findname(sv, name, &ino, NULL, NULL);
	if (result!=0 && result!=ENOENT) {
		goto die_simple;
	}

	/* If it exists, fail */
	if (result==0) {
		result = EEXIST;
		goto die_simple;
	}

	/*
	 * If we're trying to create . or .. and we get this far
	 * (meaning the entry in question is missing), the fs is
	 * corrupted. Let's not make it worse. Best at this point to
	 * bail out and run fsck.
	 */
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		panic("sfs: %s: No %s entry in dir %u; please fsck\n",
		      sfs->sfs_sb.sb_volname, name, sv->sv_ino);
	}

	result = sfs_makeobj(sfs, SFS_TYPE_DIR, &newguy);
	if (result) {
		goto die_simple;
	}
	new_inodeptr = sfs_dinode_map(newguy);

	result = sfs_dir_link(newguy, ".", newguy->sv_ino, NULL);
	if (result) {
		goto die_uncreate;
	}

	result = sfs_dir_link(newguy, "..", sv->sv_ino, NULL);
	if (result) {
		goto die_uncreate;
	}

	result = sfs_dir_link(sv, name, newguy->sv_ino, NULL);
	if (result) {
		goto die_uncreate;
	}

        /*
         * Increment link counts (Note: not until after the names are
         * added - that way if one fails, the link count will be zero,
         * and reclaim will dispose of the new directory.
         *
         * Note also that the name in the parent directory gets added
         * last, so there's no case in which we have to go back and
         * remove it.
         */

	new_inodeptr->sfi_linkcount += 2;
	dir_inodeptr->sfi_linkcount++;
	sfs_dinode_mark_dirty(newguy);
	sfs_dinode_mark_dirty(sv);

	sfs_dinode_unload(newguy);
	sfs_dinode_unload(sv);
	lock_release(newguy->sv_lock);
	lock_release(sv->sv_lock);
	VOP_DECREF(&newguy->sv_absvn);

	unreserve_buffers(SFS_BLOCKSIZE);

	KASSERT(result==0);
	return result;

die_uncreate:
	sfs_dinode_unload(newguy);
	lock_release(newguy->sv_lock);
	VOP_DECREF(&newguy->sv_absvn);

die_simple:
	sfs_dinode_unload(sv);

die_early:
	unreserve_buffers(SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);
	return result;
}

/*
 * Delete a directory.
 *
 * Locking: Acquires vnode lock for parent dir and then vnode lock for
 * victim dir. Releases both.
 *
 * Requires 4 buffers.
 */
static
int
sfs_rmdir(struct vnode *v, const char *name)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
	struct sfs_vnode *victim;
	struct sfs_dinode *dir_inodeptr;
	struct sfs_dinode *victim_inodeptr;
	int result, result2;
	int slot;

	/* Cannot remove the . or .. entries from a directory! */
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		return EINVAL;
	}

	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_dinode_load(sv);
	if (result) {
		goto die_loadsv;
	}
	dir_inodeptr = sfs_dinode_map(sv);

	if (dir_inodeptr->sfi_linkcount == 0) {
		result = ENOENT;
		goto die_linkcount;
	}

	result = sfs_lookonce(sv, name, &victim, &slot);
	if (result) {
		goto die_linkcount;
	}

	lock_acquire(victim->sv_lock);
	result = sfs_dinode_load(victim);
	if (result) {
		goto die_loadvictim;
	}
	victim_inodeptr = sfs_dinode_map(victim);

	if (victim->sv_ino == SFS_ROOTDIR_INO) {
		result = EPERM;
		goto die_total;
	}

	/* Only allowed on directories */
	if (victim_inodeptr->sfi_type != SFS_TYPE_DIR) {
		result = ENOTDIR;
		goto die_total;
	}

	result = sfs_dir_checkempty(victim);
	if (result) {
		goto die_total;
	}

	result = sfs_dir_unlink(sv, slot);
	if (result) {
		goto die_total;
	}

	KASSERT(dir_inodeptr->sfi_linkcount > 1);
	KASSERT(victim_inodeptr->sfi_linkcount==2);

	dir_inodeptr->sfi_linkcount--;
	sfs_dinode_mark_dirty(sv);

	victim_inodeptr->sfi_linkcount -= 2;
	sfs_dinode_mark_dirty(victim);

	result = sfs_itrunc(victim, 0);
	if (result) {
		victim_inodeptr->sfi_linkcount += 2;
		dir_inodeptr->sfi_linkcount++;
		result2 = sfs_dir_link(sv, name, victim->sv_ino, NULL);
		if (result2) {
			/* XXX: would be better if this case didn't exist */
			panic("sfs: %s: rmdir: %s; while recovering: %s\n",
			      sfs->sfs_sb.sb_volname, 
			      strerror(result), strerror(result2));
		}
		goto die_total;
	}

die_total:
	sfs_dinode_unload(victim);
die_loadvictim:
	lock_release(victim->sv_lock);
 	VOP_DECREF(&victim->sv_absvn);
die_linkcount:
	sfs_dinode_unload(sv);
die_loadsv:
 	unreserve_buffers(SFS_BLOCKSIZE);
 	lock_release(sv->sv_lock);

	return result;
}

/*
 * Delete a file.
 *
 * Locking: locks the directory, then the file. Unlocks both.
 *   This follows the hierarchical locking order imposed by the directory tree.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_remove(struct vnode *dir, const char *name)
{
	struct sfs_vnode *sv = dir->vn_data;
	struct sfs_vnode *victim;
	struct sfs_dinode *victim_inodeptr;
	struct sfs_dinode *dir_inodeptr;
	int slot;
	int result;

	/* need to check this to avoid deadlock even in error condition */
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		return EISDIR;
	}

	lock_acquire(sv->sv_lock);
	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_dinode_load(sv);
	if (result) {
		goto out_buffers;
	}
	dir_inodeptr = sfs_dinode_map(sv);

	if (dir_inodeptr->sfi_linkcount == 0) {
		result = ENOENT;
		goto out_loadsv;
	}

	/* Look for the file and fetch a vnode for it. */
	result = sfs_lookonce(sv, name, &victim, &slot);
	if (result) {
		goto out_loadsv;
	}

	lock_acquire(victim->sv_lock);
	result = sfs_dinode_load(victim);
	if (result) {
		lock_release(victim->sv_lock);
		VOP_DECREF(&victim->sv_absvn);
		goto out_loadsv;
	}
	victim_inodeptr = sfs_dinode_map(victim);
	KASSERT(victim_inodeptr->sfi_linkcount > 0);

	/* Not allowed on directories */
	if (victim_inodeptr->sfi_type == SFS_TYPE_DIR) {
		result = EISDIR;
		goto out_reference;
	}

	/* Erase its directory entry. */
	result = sfs_dir_unlink(sv, slot);
	if (result) {
		goto out_reference;
	}

	/* Decrement the link count. */
	KASSERT(victim_inodeptr->sfi_linkcount > 0);
	victim_inodeptr->sfi_linkcount--;
	sfs_dinode_mark_dirty(victim);

out_reference:
	/* Discard the reference that sfs_lookonce got us */
	sfs_dinode_unload(victim);
	lock_release(victim->sv_lock);
	VOP_DECREF(&victim->sv_absvn);

out_loadsv:
	sfs_dinode_unload(sv);

out_buffers:
	lock_release(sv->sv_lock);
	unreserve_buffers(SFS_BLOCKSIZE);
	return result;
}

/*
 * Simple helper function for rename.
 */
static
void
recovermsg(const char *vol, int result, int result2)
{
	kprintf("sfs: %s: rename: %s, then while recovering: %s\n", vol,
		strerror(result), strerror(result2));
}

/*
 * Helper function for rename. Make sure COMPARE is not a direct
 * ancestor of (or the same as) CHILD.
 *
 * Note: acquires locks as it goes up.
 */
static
int
check_parent(struct sfs_vnode *lookfor, struct sfs_vnode *failon,
	     struct sfs_vnode *child, int *found)
{
	struct sfs_vnode *up;
	int result;

	*found = 0;

	VOP_INCREF(&child->sv_absvn);
	while (1) {
		if (failon == child) {
			/* Bad */
			VOP_DECREF(&child->sv_absvn);
			return EINVAL;
		}

		if (lookfor == child) {
			*found = 1;
		}

		lock_acquire(child->sv_lock);
		result = sfs_lookonce(child, "..", &up, NULL);
		lock_release(child->sv_lock);

		if (result) {
			VOP_DECREF(&child->sv_absvn);
			return result;
		}
		if (child == up) {
			/* Hit root, done */
			VOP_DECREF(&up->sv_absvn);
			break;
		}
		VOP_DECREF(&child->sv_absvn);
		child = up;
	}

	VOP_DECREF(&child->sv_absvn);
	return 0;
}

/*
 * Rename a file.
 *
 * Locking:
 *    Locks sfs_renamelock.
 *    Calls check_parent, which locks various directories one at a
 *       time.
 *    Locks the target vnodes and their parents in a complex fashion
 *       (described in detail below) which is carefully arranged so
 *       it won't deadlock with rmdir. Or at least I hope so.
 *    Then unlocks everything.
 *
 *    The rationale for all this is complex. See the comments below.
 *
 * Requires up to 7 buffers.
 */
static
int
sfs_rename(struct vnode *absdir1, const char *name1,
	   struct vnode *absdir2, const char *name2)
{
	struct sfs_fs *sfs = absdir1->vn_fs->fs_data;
	struct sfs_vnode *dir1 = absdir1->vn_data;
	struct sfs_vnode *dir2 = absdir2->vn_data;
	struct sfs_vnode *obj1=NULL, *obj2=NULL;
	struct sfs_dinode *dir1_inodeptr, *dir2_inodeptr;
	struct sfs_dinode *obj1_inodeptr, *obj2_inodeptr;
	int slot1=-1, slot2=-1;
	int result, result2;
	struct sfs_direntry sd;
	int found_dir1;

	/* make gcc happy */
	obj2_inodeptr = NULL;

	/* The VFS layer is supposed to enforce this */
	KASSERT(absdir1->vn_fs == absdir2->vn_fs);

	if (!strcmp(name1, ".") || !strcmp(name2, ".") ||
	    !strcmp(name1, "..") || !strcmp(name2, "..")) {
		return EINVAL;
	}

	if (strlen(name2)+1 > sizeof(sd.sfd_name)) {
		return ENAMETOOLONG;
	}

	/*
	 * We only allow one rename to occur at a time. This appears
	 * to be necessary to preserve the consistency of the
	 * filesystem: once you do the parent check (that n1 is not an
	 * ancestor of d2/n2) nothing may be allowed to happen that
	 * might invalidate that result until all of the
	 * rearrangements are complete. If other renames are allowed
	 * to proceed, we'd need to lock every descendent of n1 to
	 * make sure that some ancestor of d2/n2 doesn't get inserted
	 * at some point deep down. This is impractical, so we use one
	 * global lock.
	 *
	 * To prevent certain deadlocks while locking the vnodes we
	 * need, the rename lock goes outside all the vnode locks.
	 */

	reserve_buffers(SFS_BLOCKSIZE);

	lock_acquire(sfs->sfs_renamelock);

	/*
	 * Get the objects we're moving.
	 *
	 * Lock each directory temporarily. We'll check again later to
	 * make sure they haven't disappeared and to find slots.
	 */
	lock_acquire(dir1->sv_lock);
	result = sfs_lookonce(dir1, name1, &obj1, NULL);
	lock_release(dir1->sv_lock);

	if (result) {
		goto out0;
	}

	lock_acquire(dir2->sv_lock);
	result = sfs_lookonce(dir2, name2, &obj2, NULL);
	lock_release(dir2->sv_lock);

	if (result && result != ENOENT) {
		goto out0;
	}

	if (result==ENOENT) {
		/*
		 * sfs_lookonce returns a null vnode with ENOENT in
		 * order to make our life easier.
		 */
		KASSERT(obj2==NULL);
	}

	/*
	 * Prohibit the case where obj1 is a directory and it's a direct
	 * ancestor in the tree of dir2 (or is the same as dir2). If
	 * that were to be permitted, it'd create a detached chunk of
	 * the directory tree, and we don't like that.
	 *
	 * If we see dir1 while checking up the tree, found_dir1 is
	 * set to true. We use this info to choose the correct ordering
	 * for locking dir1 and dir2.
	 *
	 * To prevent deadlocks, the parent check must be done without
	 * holding locks on any other directories.
	 */
	result = check_parent(dir1, obj1, dir2, &found_dir1);
	if (result) {
		goto out0;
	}

	/*
	 * Now check for cases where some of the four vnodes we have
	 * are the same.
	 *
	 * These cases are, in the order they are handled below:
	 *
	 *    dir1 == obj1		Already checked.
	 *    dir2 == obj2		Already checked.
	 *    dir2 == obj1		Already checked.
	 *    dir1 == obj2		Checked below.
	 *    dir1 == dir2		Legal.
	 *    obj1 == obj2		Legal, special handling.
	 */

	/*
	 * A directory should have no entries for itself other than '.'.
	 * Thus, since we explicitly reject '.' above, the names
	 * within the directories should not refer to the directories
	 * themselves.
	 */
	KASSERT(dir1 != obj1);
	KASSERT(dir2 != obj2);

	/*
	 * The parent check should have caught this case.
	 */
	KASSERT(dir2 != obj1);

	/*
	 * Check for dir1 == obj2.
	 *
	 * This is not necessarily wrong if obj1 is the last entry in
	 * dir1 (this is essentially "mv ./foo/bar ./foo") but our
	 * implementation doesn't tolerate it. Because we need to
	 * unlink g2 before linking g1 in the new place, it will
	 * always fail complaining that g2 (sv1) isn't empty. We could
	 * just charge ahead and let this happen, but we'll get into
	 * trouble with our locks if we do, so detect this as a
	 * special case and return ENOTEMPTY.
	 */

	if (obj2==dir1) {
		result = ENOTEMPTY;
		goto out0;
	}


	/*
	 * Now we can begin acquiring locks for real.
	 *
	 * If we saw dir1 while doing the parent check, it means
	 * dir1 is higher in the tree than dir2. Thus, we should
	 * lock dir1 before dir2.
	 *
	 * If on the other hand we didn't see dir1, either dir2 is
	 * higher in the tree than dir1, in which case we should lock
	 * dir2 first, or dir1 and dir2 are on disjoint branches of
	 * the tree, in which case (because there's a single rename
	 * lock for the whole fs) it doesn't matter what order we lock
	 * in.
	 *
	 * If we lock dir1 first, we don't need to lock obj1 before
	 * dir2, since (due to the parent check) obj1 cannot be an
	 * ancestor of dir2.
	 *
	 * However, if we lock dir2 first, obj2 must be locked before
	 * dir1, in case obj2 is an ancestor of dir1. (In this case we
	 * will find that obj2 is not empty and cannot be removed, but
	 * we must lock it before we can check that.)
	 *
	 * Thus we lock in this order:
	 *
	 * dir1   (if found_dir1)
	 * dir2
	 * obj2   (if non-NULL)
	 * dir1   (if !found_dir1)
	 * obj1
	 *
	 * Also, look out for the case where both dirs are the same.
	 * (If this is true, found_dir1 will be set.)
	 */

	if (dir1==dir2) {
		/* This locks "both" dirs */
		lock_acquire(dir1->sv_lock);
		KASSERT(found_dir1);
	}
	else {
		if (found_dir1) {
			lock_acquire(dir1->sv_lock);
		}
		lock_acquire(dir2->sv_lock);
	}

	/*
	 * Now lock obj2.
	 *
	 * Note that we must redo the lookup and get a new obj2, as it
	 * may have changed under us. Since we hold the rename lock
	 * for the whole fs, the fs structure cannot have changed, so
	 * we don't need to redo the parent check or any of the checks
	 * for vnode aliasing with dir1 or dir2 above. Note however
	 * that obj1 and obj2 may now be the same even if they weren't
	 * before.
	 */
	KASSERT(lock_do_i_hold(dir2->sv_lock));
	if (obj2) {
		VOP_DECREF(&obj2->sv_absvn);
		obj2 = NULL;
	}
	result = sfs_lookonce(dir2, name2, &obj2, &slot2);
	if (result==0) {
		KASSERT(obj2 != NULL);
		lock_acquire(obj2->sv_lock);
		result = sfs_dinode_load(obj2);
		if (result) {
			/* ENOENT would confuse us below; but it can't be */
			KASSERT(result != ENOENT);
			lock_release(obj2->sv_lock);
			VOP_DECREF(&obj2->sv_absvn);
			/* continue to check below */
		}
		else {
			obj2_inodeptr = sfs_dinode_map(obj2);
		}
	}
	else if (result==ENOENT) {
		/*
		 * sfs_lookonce returns a null vnode and an empty slot
		 * with ENOENT in order to make our life easier.
		 */
		KASSERT(obj2==NULL);
		KASSERT(slot2>=0);
	}

	if (!found_dir1) {
		lock_acquire(dir1->sv_lock);
	}

	/* Postpone this check to simplify the error cleanup. */
	if (result != 0 && result != ENOENT) {
		goto out1;
	}

	/*
	 * Now reload obj1.
	 */
	KASSERT(lock_do_i_hold(dir1->sv_lock));
	VOP_DECREF(&obj1->sv_absvn);
	obj1 = NULL;
	result = sfs_lookonce(dir1, name1, &obj1, &slot1);
	if (result) {
		goto out1;
	}
	/*
	 * POSIX mandates that if obj1==obj2, we succeed and nothing
	 * happens.  This is somewhat stupid if obj1==obj2 and dir1 != dir2,
	 * but we'll go with POSIX anyway.
	 */
	if (obj1==obj2) {
		result = 0;
		VOP_DECREF(&obj1->sv_absvn);
		obj1 = NULL;
		goto out1;
	}
	lock_acquire(obj1->sv_lock);
	result = sfs_dinode_load(obj1);
	if (result) {
		lock_release(obj1->sv_lock);
		VOP_DECREF(&obj1->sv_absvn);
		obj1 = NULL;
		goto out1;
	}
	obj1_inodeptr = sfs_dinode_map(obj1);

	result = sfs_dinode_load(dir2);
	if (result) {
		goto out2;
	}
	dir2_inodeptr = sfs_dinode_map(dir2);

	result = sfs_dinode_load(dir1);
	if (result) {
		goto out3;
	}
	dir1_inodeptr = sfs_dinode_map(dir1);

	/*
	 * One final piece of paranoia: make sure dir2 hasn't been rmdir'd.
	 * (If dir1 was, the obj1 lookup above would have failed.)
	 */
	if (dir2_inodeptr->sfi_linkcount==0) {
		result = ENOENT;
		goto out4;
	}

	/*
	 * Now we have all the locks we need and we can proceed with
	 * the operation.
	 */

	/* At this point we should have valid slots in both dirs. */
	KASSERT(slot1>=0);
	KASSERT(slot2>=0);

	if (obj2 != NULL) {
		/*
		 * Target already exists.
		 * Must be the same type (file or directory) as the source,
		 * and if a directory, must be empty. Then unlink it.
		 */

		if (obj1_inodeptr->sfi_type == SFS_TYPE_DIR) {
			if (obj2_inodeptr->sfi_type != SFS_TYPE_DIR) {
				result = ENOTDIR;
				goto out4;
			}
			result = sfs_dir_checkempty(obj2);
			if (result) {
				goto out4;
			}

			/* Remove the name */
			result = sfs_dir_unlink(dir2, slot2);
			if (result) {
				goto out4;
			}

			/* Dispose of the directory */
			KASSERT(dir2_inodeptr->sfi_linkcount > 1);
			KASSERT(obj2_inodeptr->sfi_linkcount == 2);
			dir2_inodeptr->sfi_linkcount--;
			obj2_inodeptr->sfi_linkcount -= 2;
			sfs_dinode_mark_dirty(dir2);
			sfs_dinode_mark_dirty(obj2);

			/* ignore errors on this */
			sfs_itrunc(obj2, 0);
		}
		else {
			KASSERT(obj1->sv_type == SFS_TYPE_FILE);
			if (obj2->sv_type != SFS_TYPE_FILE) {
				result = EISDIR;
				goto out4;
			}

			/* Remove the name */
			result = sfs_dir_unlink(dir2, slot2);
			if (result) {
				goto out4;
			}

			/* Dispose of the file */
			KASSERT(obj2_inodeptr->sfi_linkcount > 0);
			obj2_inodeptr->sfi_linkcount--;
			sfs_dinode_mark_dirty(obj2);
		}

		sfs_dinode_unload(obj2);

		lock_release(obj2->sv_lock);
		VOP_DECREF(&obj2->sv_absvn);
		obj2 = NULL;
	}

	/*
	 * At this point the target should be nonexistent and we have
	 * a slot in the target directory we can use. Create a link
	 * there. Do it by hand instead of using sfs_dir_link to avoid
	 * duplication of effort.
	 */
	KASSERT(obj2==NULL);

	bzero(&sd, sizeof(sd));
	sd.sfd_ino = obj1->sv_ino;
	strcpy(sd.sfd_name, name2);
	result = sfs_writedir(dir2, slot2, &sd);
	if (result) {
		goto out4;
	}

	obj1_inodeptr->sfi_linkcount++;
	sfs_dinode_mark_dirty(obj1);

	if (obj1->sv_type == SFS_TYPE_DIR && dir1 != dir2) {
		/* Directory: reparent it */
		result = sfs_readdir(obj1, DOTDOTSLOT, &sd);
		if (result) {
			goto recover1;
		}
		if (strcmp(sd.sfd_name, "..")) {
			panic("sfs: %s: rename: moving dir: .. is not "
			      "in slot %d\n", sfs->sfs_sb.sb_volname,
			      DOTDOTSLOT);
		}
		if (sd.sfd_ino != dir1->sv_ino) {
			panic("sfs: %s: rename: moving dir: .. is i%u "
			      "and not i%u\n", sfs->sfs_sb.sb_volname,
			      sd.sfd_ino, dir1->sv_ino);
		}
		sd.sfd_ino = dir2->sv_ino;
		result = sfs_writedir(obj1, DOTDOTSLOT, &sd);
		if (result) {
			goto recover1;
		}
		dir1_inodeptr->sfi_linkcount--;
		sfs_dinode_mark_dirty(dir1);
		dir2_inodeptr->sfi_linkcount++;
		sfs_dinode_mark_dirty(dir2);
	}

	result = sfs_dir_unlink(dir1, slot1);
	if (result) {
		goto recover2;
	}
	obj1_inodeptr->sfi_linkcount--;
	sfs_dinode_mark_dirty(obj1);

	KASSERT(result==0);

	if (0) {
		/* Only reached on error */
    recover2:
		if (obj1->sv_type == SFS_TYPE_DIR) {
			sd.sfd_ino = dir1->sv_ino;
			result2 = sfs_writedir(obj1, DOTDOTSLOT, &sd);
			if (result2) {
				recovermsg(sfs->sfs_sb.sb_volname,
					   result, result2);
			}
			dir1_inodeptr->sfi_linkcount++;
			sfs_dinode_mark_dirty(dir1);
			dir2_inodeptr->sfi_linkcount--;
			sfs_dinode_mark_dirty(dir2);
		}
    recover1:
		result2 = sfs_dir_unlink(dir2, slot2);
		if (result2) {
			recovermsg(sfs->sfs_sb.sb_volname,
				   result, result2);
		}
		obj1_inodeptr->sfi_linkcount--;
		sfs_dinode_mark_dirty(obj1);
	}

 out4:
 	sfs_dinode_unload(dir1);
 out3:
 	sfs_dinode_unload(dir2);
 out2:
 	sfs_dinode_unload(obj1);
	lock_release(obj1->sv_lock);
 out1:
	if (obj2) {
		sfs_dinode_unload(obj2);
		lock_release(obj2->sv_lock);
	}
	lock_release(dir1->sv_lock);
	if (dir1 != dir2) {
		lock_release(dir2->sv_lock);
	}
 out0:
	if (obj2 != NULL) {
		VOP_DECREF(&obj2->sv_absvn);
	}
	if (obj1 != NULL) {
		VOP_DECREF(&obj1->sv_absvn);
	}

	unreserve_buffers(SFS_BLOCKSIZE);

	lock_release(sfs->sfs_renamelock);

	return result;
}

static
int
sfs_lookparent_internal(struct vnode *v, char *path, struct vnode **ret,
		  char *buf, size_t buflen)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_vnode *next;
	char *s;
	int result;

	VOP_INCREF(&sv->sv_absvn);

	while (1) {
		/* Don't need lock to check vnode type; it's constant */
		if (sv->sv_type != SFS_TYPE_DIR) {
			VOP_DECREF(&sv->sv_absvn);
			return ENOTDIR;
		}

		s = strchr(path, '/');
		if (!s) {
			/* Last component. */
			break;
		}
		*s = 0;
		s++;

		lock_acquire(sv->sv_lock);
		result = sfs_lookonce(sv, path, &next, NULL);
		lock_release(sv->sv_lock);

		if (result) {
			VOP_DECREF(&sv->sv_absvn);
			return result;
		}

		VOP_DECREF(&sv->sv_absvn);
		sv = next;
		path = s;
	}

	if (strlen(path)+1 > buflen) {
		VOP_DECREF(&sv->sv_absvn);
		return ENAMETOOLONG;
	}
	strcpy(buf, path);

	*ret = &sv->sv_absvn;

	return 0;
}

/*
 * lookparent returns the last path component as a string and the
 * directory it's in as a vnode.
 *
 * Locking: gets the vnode lock while calling sfs_lookonce. Doesn't
 *   lock the new vnode, but does hand back a reference to it (so it
 *   won't evaporate).
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_lookparent(struct vnode *v, char *path, struct vnode **ret,
		  char *buf, size_t buflen)
{
	int result;

	reserve_buffers(SFS_BLOCKSIZE);
	result = sfs_lookparent_internal(v, path, ret, buf, buflen);
	unreserve_buffers(SFS_BLOCKSIZE);
	return result;
}

/*
 * Lookup gets a vnode for a pathname.
 *
 * Locking: gets the vnode lock while calling sfs_lookonce. Doesn't
 *   lock the new vnode, but does hand back a reference to it (so it
 *   won't evaporate).
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_lookup(struct vnode *v, char *path, struct vnode **ret)
{
	struct sfs_vnode *sv = v->vn_data;
	struct vnode *dirv;
	struct sfs_vnode *dir;
	struct sfs_vnode *final;
	int result;
	char name[SFS_NAMELEN];

	reserve_buffers(SFS_BLOCKSIZE);

	result = sfs_lookparent_internal(&sv->sv_absvn, path, &dirv, name, sizeof(name));
	if (result) {
		unreserve_buffers(SFS_BLOCKSIZE);
		return result;
	}

	dir = dirv->vn_data;
	lock_acquire(dir->sv_lock);

	result = sfs_lookonce(dir, name, &final, NULL);

	lock_release(dir->sv_lock);
	VOP_DECREF(dirv);

	if (result) {
		unreserve_buffers(SFS_BLOCKSIZE);
		return result;
	}

	*ret = &final->sv_absvn;

	unreserve_buffers(SFS_BLOCKSIZE);
	return 0;
}

////////////////////////////////////////////////////////////
// Ops tables

/*
 * Function table for sfs files.
 */
const struct vnode_ops sfs_fileops = {
	.vop_magic = VOP_MAGIC,	/* mark this a valid vnode ops table */

	.vop_eachopen = sfs_eachopen,
	.vop_reclaim = sfs_reclaim,

	.vop_read = sfs_read,
	.vop_readlink = vopfail_uio_notdir,
	.vop_getdirentry = vopfail_uio_notdir,
	.vop_write = sfs_write,
	.vop_ioctl = sfs_ioctl,
	.vop_stat = sfs_stat,
	.vop_gettype = sfs_gettype,
	.vop_isseekable = sfs_isseekable,
	.vop_fsync = sfs_fsync,
	.vop_mmap = sfs_mmap,
	.vop_truncate = sfs_truncate,
	.vop_namefile = vopfail_uio_notdir,

	.vop_creat = vopfail_creat_notdir,
	.vop_symlink = vopfail_symlink_notdir,
	.vop_mkdir = vopfail_mkdir_notdir,
	.vop_link = vopfail_link_notdir,
	.vop_remove = vopfail_string_notdir,
	.vop_rmdir = vopfail_string_notdir,
	.vop_rename = vopfail_rename_notdir,

	.vop_lookup = vopfail_lookup_notdir,
	.vop_lookparent = vopfail_lookparent_notdir,
};

/*
 * Function table for sfs directories.
 */
const struct vnode_ops sfs_dirops = {
	.vop_magic = VOP_MAGIC,	/* mark this a valid vnode ops table */

	.vop_eachopen = sfs_eachopendir,
	.vop_reclaim = sfs_reclaim,

	.vop_read = vopfail_uio_isdir,
	.vop_readlink = vopfail_uio_inval,
	.vop_getdirentry = sfs_getdirentry,
	.vop_write = vopfail_uio_isdir,
	.vop_ioctl = sfs_ioctl,
	.vop_stat = sfs_stat,
	.vop_gettype = sfs_gettype,
	.vop_isseekable = sfs_isseekable,
	.vop_fsync = sfs_fsync,
	.vop_mmap = vopfail_mmap_isdir,
	.vop_truncate = vopfail_truncate_isdir,
	.vop_namefile = sfs_namefile,

	.vop_creat = sfs_creat,
	.vop_symlink = vopfail_symlink_nosys,
	.vop_mkdir = sfs_mkdir,
	.vop_link = sfs_link,
	.vop_remove = sfs_remove,
	.vop_rmdir = sfs_rmdir,
	.vop_rename = sfs_rename,

	.vop_lookup = sfs_lookup,
	.vop_lookparent = sfs_lookparent,
};
