#include <types.h>
#include <kern/errno.h>
#include <limits.h>
#include <syscall.h>
#include <copyinout.h>
#include <vfs.h>

int
sys_chdir(userptr_t path)
{
        int err;
        char *path_buf;

        path_buf = kmalloc(PATH_MAX);
        if (path_buf == NULL) {
                err = ENOMEM;
                goto err1;
        }

        err = copyinstr(path, path_buf, PATH_MAX, NULL);
        if (err) {
                goto err2;
        }

        err = vfs_chdir(path_buf);
        if (err) {
                goto err2;
        }

        return 0;


        err2:
                kfree(path_buf);
        err1:
                return err;
}
