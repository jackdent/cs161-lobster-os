#include <types.h>
#include <limits.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>

int
sys_close(int fd)
{
        return release_fd_from_fd_table(curproc->p_fd_table, fd);
}
