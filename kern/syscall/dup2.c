#include <types.h>
#include <kern/errno.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>

/*
 * clone_fd can only return the error EBADF; otherwise, it will succeed
 * It does not return EMFILE or ENFILE, because all of our file descriptor
 * tables have a statically defined fixed size.
 */

int
sys_dup2(int old_fd, int new_fd)
{
        int err;
        struct fd_file *old_file;
        struct fd_table *fd_table;

        fd_table = curproc->p_fd_table;

        KASSERT(fd_table != NULL);
        spinlock_acquire(&fd_table->fdt_spinlock);

        if (valid_fd(fd_table, old_fd)) {
                old_file = fd_table->fdt_table[old_fd];
        } else {
                err = EBADF;
                goto err1;
        }

        /* If old_fd is the same as new_fd, and if they are both valid,
           do nothing */
        if (old_fd == new_fd) {
                spinlock_release(&fd_table->fdt_spinlock);
                return 0;
        }

        if (fd_in_range(new_fd)) {
                if (fd_table->fdt_table[new_fd] != NULL) {
                        fd_file_release(fd_table->fdt_table[new_fd]);
                }

                fd_table->fdt_table[new_fd] = old_file;
                fd_file_reference(old_file);
        } else {
                err = EBADF;
                goto err1;
        }

        spinlock_release(&fd_table->fdt_spinlock);
        return 0;


        err1:
                spinlock_release(&fd_table->fdt_spinlock);
                return err;
}
