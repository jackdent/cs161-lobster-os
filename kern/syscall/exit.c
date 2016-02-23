#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <current.h>
#include <kern/wait.h>
#include <proc.h>
#include <proctable.h>
#include <thread.h>

void
sys__exit(int exitcode)
{
	spinlock_acquire(&curproc->p_spinlock);

	/* kproc should adopt all the children *before* we call proc_destroy */
	kproc_adopt_children(curproc);
	curproc->p_exit_status = _MKWAIT_EXIT(exitcode);

	spinlock_release(&curproc->p_spinlock);

	/* Cleanup everything except the proc struct itself, which contains
	   the exit status */
	proc_cleanup(curproc);

	// V() on wait_sem called in thread_exit
	// TODO: consult w/ TF about when to reap
	// This will set threads on the process to 0, so it can be reaped
	thread_exit();
}
