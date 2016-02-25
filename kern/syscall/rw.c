#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <syscall.h>
#include <synch.h>
#include <uio.h>
#include <proc.h>
#include <copyinout.h>
#include <current.h>

static
int
sys_rw(int fd, userptr_t buf, size_t len, size_t *copied, enum uio_rw rw)
{
        int err;
        struct fd_file *file;
        char *ker_buf;
        struct uio uio;
        struct iovec iov;

        file = get_file_from_fd_table(curproc->p_fd_table, fd);
        if (file == NULL) {
                err = EBADF;
                goto err1;
        }

        lock_acquire(file->fdf_lock);

        ker_buf = kmalloc(len);
        if (ker_buf == NULL) {
                err = ENOMEM;
                goto err2;
        }

        uio_kinit(&iov, &uio, ker_buf, len, file->fdf_offset, rw);

        switch (rw) {
        case UIO_READ:
                if (!(fd_file_check_flag(file, O_RDONLY) ||
                        fd_file_check_flag(file, O_RDWR))) {
                        err = EBADF;
                        goto err3;
                }

                err = VOP_READ(file->fdf_vnode, &uio);
                if (err) {
                        goto err3;
                }

                err = copyout(ker_buf, buf, len);

                break;
        case UIO_WRITE:
                if (!(fd_file_check_flag(file, O_WRONLY) ||
                        fd_file_check_flag(file, O_RDWR))) {
                        err = EBADF;
                        goto err3;
                }

                err = copyin(buf, ker_buf, len);
                if (err) {
                        goto err3;
                }

                err = VOP_WRITE(file->fdf_vnode, &uio);

                break;
        default:
                panic("Invalid rw option");
        }

        if (err) {
                goto err3;
        }

        file->fdf_offset = uio.uio_offset;
        lock_release(file->fdf_lock);
        *copied = len - uio.uio_resid;

        return 0;


        err3:
                kfree(ker_buf);
        err2:
                lock_release(file->fdf_lock);
        err1:
                return err;
}

int
sys_read(int fd, userptr_t buf, size_t len, size_t *read)
{
        return sys_rw(fd, buf, len, read, UIO_READ);
}

int
sys_write(int fd, userptr_t buf, size_t len, size_t *wrote)
{
        return sys_rw(fd, buf, len, wrote, UIO_WRITE);
}
