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
 * More file-related system call implementations.
 */

#include <types.h>
#include <limits.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <vfs.h>
#include <vnode.h>
#include <syscall.h>

/*
 * sync - call vfs_sync
 */
int
sys_sync(void)
{
	int err;

	err = vfs_sync();
	if (err==EIO) {
		/* This is the only likely failure case */
		kprintf("Warning: I/O error during sync\n");
	}
	else if (err) {
		kprintf("Warning: sync: %s\n", strerror(err));
	}
	/* always succeed */
	return 0;
}

/*
 * mkdir - call vfs_mkdir
 */
int
sys_mkdir(userptr_t path, mode_t mode)
{
	char *pathbuf;
	int err;

	pathbuf = kmalloc(PATH_MAX);
	if (pathbuf == NULL) {
		return ENOMEM;
	}

	err = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (err) {
		kfree(pathbuf);
		return err;
	}

	err = vfs_mkdir(pathbuf, mode);
	kfree(pathbuf);
	return err;
}

/*
 * rmdir - call vfs_rmdir
 */
int
sys_rmdir(userptr_t path)
{
	char *pathbuf;
	int err;

	pathbuf = kmalloc(PATH_MAX);
	if (pathbuf == NULL) {
		return ENOMEM;
	}

	err = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (err) {
		kfree(pathbuf);
		return err;
	}

	err = vfs_rmdir(pathbuf);
	kfree(pathbuf);
	return err;
}

/*
 * remove - call vfs_remove
 */
int
sys_remove(userptr_t path)
{
	char *pathbuf;
	int err;

	pathbuf = kmalloc(PATH_MAX);
	if (pathbuf == NULL) {
		return ENOMEM;
	}

	err = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (err) {
		kfree(pathbuf);
		return err;
	}

	err = vfs_remove(pathbuf);
	kfree(pathbuf);
	return err;
}

/*
 * link - call vfs_link
 */
int
sys_link(userptr_t oldpath, userptr_t newpath)
{
	char *oldbuf;
	char *newbuf;
	int err;

	oldbuf = kmalloc(PATH_MAX);
	if (oldbuf == NULL) {
		return ENOMEM;
	}

	newbuf = kmalloc(PATH_MAX);
	if (newbuf == NULL) {
		kfree(oldbuf);
		return ENOMEM;
	}

	err = copyinstr(oldpath, oldbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = copyinstr(newpath, newbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = vfs_link(oldbuf, newbuf);
 fail:
	kfree(newbuf);
	kfree(oldbuf);
	return err;
}

/*
 * rename - call vfs_rename
 */
int
sys_rename(userptr_t oldpath, userptr_t newpath)
{
	char *oldbuf;
	char *newbuf;
	int err;

	oldbuf = kmalloc(PATH_MAX);
	if (oldbuf == NULL) {
		return ENOMEM;
	}

	newbuf = kmalloc(PATH_MAX);
	if (newbuf == NULL) {
		kfree(oldbuf);
		return ENOMEM;
	}

	err = copyinstr(oldpath, oldbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = copyinstr(newpath, newbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = vfs_rename(oldbuf, newbuf);
 fail:
	kfree(newbuf);
	kfree(oldbuf);
	return err;
}

/*
 * getdirentry - call VOP_GETDIRENTRY
 */
int
sys_getdirentry(int fd, userptr_t buf, size_t buflen, int *retval)
{
	struct iovec iov;
	struct uio useruio;
	struct fd_file *file;
	int err;

	/* better be a valid file descriptor */

	file = get_file_from_fd_table(curproc->p_fd_table, fd);
	if (file == NULL) {
		return EBADF;
	}

	/* all directories should be seekable */
	KASSERT(VOP_ISSEEKABLE(file->fdf_vnode));

	lock_acquire(file->fdf_lock);

	/* fdf_flags should have only the O_ACCMODE bits in it */
	KASSERT((file->fdf_flags & O_ACCMODE) == file->fdf_flags);

	/* Dirs shouldn't be openable for write at all, but be safe... */
	if (file->fdf_flags == O_WRONLY) {
		lock_release(file->fdf_lock);
		return EBADF;
	}

	/* set up a uio with the buffer, its size, and the current offset */
	uio_uinit(&iov, &useruio, buf, buflen, file->fdf_offset, UIO_READ);

	/* do the read */
	err = VOP_GETDIRENTRY(file->fdf_vnode, &useruio);
	if (err) {
		lock_release(file->fdf_lock);
		return err;
	}

	/* set the offset to the updated offset in the uio */
	file->fdf_offset = useruio.uio_offset;

	lock_release(file->fdf_lock);

	/*
	 * the amount read is the size of the buffer originally, minus
	 * how much is left in it. Note: it is not correct to use
	 * uio_offset for this!
	 */
	*retval = buflen - useruio.uio_resid;

	return 0;
}

/*
 * fstat - call VOP_FSTAT
 */
int
sys_fstat(int fd, userptr_t statptr)
{
	struct stat kbuf;
	struct fd_file *file;
	int err;

	file = get_file_from_fd_table(curproc->p_fd_table, fd);
	if (file == NULL) {
		return EBADF;
	}

	/*
	 * No need to lock the openfile - it cannot disappear under us,
	 * and we're not using any of its non-constant fields.
	 */

	err = VOP_STAT(file->fdf_vnode, &kbuf);
	if (err) {
		return err;
	}

	return copyout(&kbuf, statptr, sizeof(struct stat));
}

/*
 * fsync - call VOP_FSYNC
 */
int
sys_fsync(int fd)
{
	struct fd_file *file;
	int err;

	file = get_file_from_fd_table(curproc->p_fd_table, fd);
	if (file == NULL) {
		return EBADF;
	}

	/*
	 * No need to lock the openfile - it cannot disappear under us,
	 * and we're not using any of its non-constant fields.
	 */

	err = VOP_FSYNC(file->fdf_vnode);
	return err;
}

/*
 * ftruncate - call VOP_TRUNCATE
 */
int
sys_ftruncate(int fd, off_t len)
{
	struct fd_file *file;
	int err;

	if (len < 0) {
		return EINVAL;
	}

	file = get_file_from_fd_table(curproc->p_fd_table, fd);
	if (file == NULL) {
		return EBADF;
	}

	/* fdf_flags should have the O_ACCMODE bits in it */
	//KASSERT((file->fdf_flags & O_ACCMODE) == O_ACCMODE);

	if (file->fdf_flags == O_RDONLY) {
		return EBADF;
	}

	/*
	 * No need to lock the openfile - it cannot disappear under us,
	 * and we're not using any of its non-constant fields.
	 */

	err = VOP_TRUNCATE(file->fdf_vnode, len);
	return err;
}
