#include <types.h>
#include <syscall.h>
#include <uio.h>
#include <proc.h>
#include <current.h>

int
sys___getcwd(userptr_t buf, size_t len)
{
        struct vnode *cwd;
        struct uio uio;
        struct iovec iov;

        cwd = curproc->p_cwd;

        // TODO: when can this fail?
        uio_kinit(&iov, &uio, buf, len, 0, UIO_READ);
        VOP_NAMEFILE(cwd, &uio);

        return len - uio.uio_resid;
}
