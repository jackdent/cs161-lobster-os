#include <types.h>
#include <syscall.h>

int
sys_lseek(int fd, off_t pos, int whence)
{
        (void)fd;
        (void)pos;
        (void)whence;
        return 0;
}
