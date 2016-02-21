#include <types.h>
#include <kern/errno.h>
#include <limits.h>
#include <vfs.h>
#include <syscall.h>
#include <uio.h>
#include <copyinout.h>
#include <proc.h>
#include <current.h>

int
sys___getcwd(userptr_t buf, size_t len, size_t *copied)
{
        int err;
        struct uio uio;
        struct iovec iov;
        char *ker_buf;

        ker_buf = kmalloc(PATH_MAX);
        if (ker_buf == NULL) {
                err = ENOMEM;
                goto err1;
        }

        uio_kinit(&iov, &uio, ker_buf, PATH_MAX, 0, UIO_READ);
        vfs_getcwd(&uio);

        err = copyoutstr(ker_buf, buf, len, copied);
        if (err) {
                goto err2;
        }

        return 0;


        err2:
                kfree(ker_buf);
        err1:
                return err;
}
