#include <types.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>

int
sys_dup2(int old_fd, int new_fd)
{
        int err;

        err = clone_fd(curproc->p_fd_table, old_fd, new_fd);

        return err || 0;
}
