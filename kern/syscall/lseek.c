#include <types.h>
#include <kern/errno.h>
#include <kern/seek.h>
#include <stat.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>

int
sys_lseek(int fd, off_t pos, off_t *new_pos, int whence)
{
        int err;
        struct fd_file *file;
        struct stat stat;

        file = get_file_from_fd_table(curproc->p_fd_table, fd);
        if (file == NULL) {
                err = EBADF;
                goto err1;
        }

        lock_acquire(file->fdf_lock);

        if (!VOP_ISSEEKABLE(file->fdf_vnode)) {
                err = EINVAL;
                goto err2;
        }

        switch (whence) {
        case SEEK_SET:
                *new_pos = pos;
                break;
        case SEEK_CUR:
                *new_pos = file->fdf_offset + pos;
                break;
        case SEEK_END:
                VOP_STAT(file->fdf_vnode, &stat);
                // TODO: is there an off by one err here?
                *new_pos = stat.st_size + pos;
                break;
        default:
                err = EINVAL;
                goto err2;
        }

        KASSERT(*new_pos >= 0);

        file->fdf_offset = *new_pos;
        lock_release(file->fdf_lock);

        return 0;


        err2:
                lock_release(file->fdf_lock);
        err1:
                return err;
}
