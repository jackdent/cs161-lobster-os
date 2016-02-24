#include <types.h>
#include <kern/errno.h>
#include <limits.h>
#include <syscall.h>
#include <copyinout.h>
#include <vfs.h>
#include <proc.h>
#include <current.h>

int
sys_open(userptr_t filename, int flags, int *fd)
{
	int err;
	char *filename_buf;
	struct vnode *vnode;
	struct fd_file *file;

	if (filename == NULL) {
		err = EFAULT;
		goto err1;
	}

	filename_buf = kmalloc(PATH_MAX);
	if (filename_buf == NULL) {
		err = ENOMEM;
		goto err1;
	}

	err = copyinstr(filename, filename_buf, PATH_MAX, NULL);
	if (err) {
		goto err2;
	}

	/* Checks the flags are valid, and creates the file if it
	   does not exist */
	err = vfs_open(filename_buf, flags, 0, &vnode);
	if (err) {
		goto err2;
	}

	file = fd_file_create(vnode, flags);
	if (file == NULL) {
		err = ENOMEM;
		goto err3;
	}

	*fd = add_file_to_fd_table(curproc->p_fd_table, file);
	if (*fd < 0) {
		err = EMFILE;
		goto err4;
	}

	return 0;


	err4:
		fd_file_destroy(file);
	err3:
		/* N.B. if the file was created, then vfs_close will not
		   delete it */
		vfs_close(vnode);
	err2:
		kfree(filename_buf);
	err1:
		return err;
}
