#include <types.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>

int
sys_dup2(int old_fd, int new_fd)
{
        int result;

        result = clone_fd(curproc->p_fd_table, old_fd, new_fd);
        if (result) {
                // errno = result;
                return -1;
        }

        return new_fd;
}
