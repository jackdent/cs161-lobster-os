#include <types.h>
#include <kern/errno.h>
#include <limits.h>
#include <syscall.h>
#include <copyinout.h>
#include <vfs.h>

int
sys_chdir(userptr_t path)
{
        int result;
        char *path_buf;

        path_buf = kmalloc(PATH_MAX);
        if (path_buf == NULL) {
                result = ENOMEM;
                goto err1;
        }

        result = copyinstr(path, path_buf, PATH_MAX, NULL);
        if (result) {
                goto err1;
        }

        result = vfs_chdir(path_buf);
        if (result) {
                goto err1;
        }

        return 0;


        err1:
                // errno = result;
                return -1;
}
