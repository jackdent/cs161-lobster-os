#include <types.h>
#include <limits.h>
#include <kern/errno.h>
#include <syscall.h>
#include <copyinout.h>
#include <vfs.h>
#include <proc.h>
#include <current.h>

int
sys_open(userptr_t filename, int flags)
{
        int result, fd;
        char *filename_buf;
        struct vnode *vnode;
        struct fd_file *file;

        KASSERT(filename != NULL);

        filename_buf = kmalloc(PATH_MAX);
        if (filename_buf == NULL) {
                result = ENOMEM;
                goto err1;
        }

        result = copyinstr(filename, filename_buf, PATH_MAX, NULL);
        if (result) {
                // TODO: do we need to do anything to result
                goto err2;
        }

        // TODO: validate flags and permissions

        result = vfs_open(filename_buf, flags, 0, &vnode);
        if (result) {
                // TODO: do we need to do anything to result
                goto err2;
        }

        // TODO: if it doesn't exists, create the vnode

        file = fd_file_create(vnode, flags);
        if (file == NULL) {
                result = ENOMEM;
                goto err2;
        }

        fd = add_file_to_fd_table(curproc->p_fd_table, file);
        if (fd < 0) {
                result = ENOMEM;
                goto err2;
        }

        return fd;


        err2:
                kfree(filename_buf);
        err1:
                return result;
}
