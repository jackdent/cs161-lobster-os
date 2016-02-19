#include <types.h>
#include <kern/errno.h>
#include <syscall.h>
#include <synch.h>
#include <uio.h>
#include <proc.h>
#include <current.h>

static
int
sys_rw(int fd, userptr_t buf, size_t len, enum uio_rw rw)
{
        int result;
        struct fd_file *file;
        struct uio uio;
        struct iovec iov;

        file = get_file_from_fd_table(curproc->p_fd_table, fd);
        if (file == NULL) {
                result = EBADF;
                goto err1;
        }

        lock_acquire(file->fdf_lock);

        /* check read/write flags on file?? */

        /* NB security? buf should not overwrite memory in the kernel */
        uio_kinit(&iov, &uio, buf, len, file->fdf_offset, rw);
        result = uiomove(buf, len, &uio);
        if (result) {
                goto err2;
        }

        file->fdf_offset = uio.uio_offset;
        lock_release(file->fdf_lock);
        return len - uio.uio_resid;


        err2:
                lock_release(file->fdf_lock);
        err1:
                // errno = result;
                return -1;
}

int
sys_read(int fd, userptr_t buf, size_t len)
{
        return sys_rw(fd, buf, len, UIO_READ);
}

int
sys_write(int fd, userptr_t buf, size_t len)
{
        return sys_rw(fd, buf, len, UIO_WRITE);
}
