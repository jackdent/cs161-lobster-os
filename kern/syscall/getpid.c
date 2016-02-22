#include <types.h>
#include <current.h>
#include <proc.h>
#include <synch.h>
#include <syscall.h>

// Cannot fail
void
sys_getpid(pid_t* retval)
{
	spinlock_acquire(&curproc->p_lock);
	*retval = curproc->p_pid;
	spinlock_release(&curproc->p_lock);
}
