#include <types.h>
#include <syscall.h>

int
sys_write(int fd, userptr_t buf, size_t buflen)
{
        (void)fd;
        (void)buf;
        (void)buflen;
        return 0;
}
