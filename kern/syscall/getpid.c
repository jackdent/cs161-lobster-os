#include <types.h>
#include <current.h>
#include <proc.h>
#include <synch.h>
#include <syscall.h>

// Cannot fail
int
sys_getpid(pid_t* retval)
{
	struct proc* blah = curproc;
	lock_acquire(blah->p_lock);
	*retval = blah->p_pid;
	lock_release(blah->p_lock);
	return 0;
}
