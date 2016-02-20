#include <types.h>
#include <current.h>
#include <proc.h>
#include <syscall.h>

pid_t
sys_getpid(void)
{
	return curproc->p_pid;
}
