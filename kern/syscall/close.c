#include <types.h>
#include <limits.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>

int
sys_close(int fd)
{
        int result;

        result = release_fd_from_fd_table(curproc->p_fd_table, fd);
        if (result) {
                // errno = result;
                return -1;
        }

        return 0;
}
