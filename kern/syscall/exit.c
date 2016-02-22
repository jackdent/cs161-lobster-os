#include <types.h>
#include <lib.h>
#include <thread.h>
#include <syscall.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <current.h>
#include <copyinout.h>
#include <kern/wait.h>
#include <proc.h>
#include <proctable.h>
#include <thread.h>
#include <addrspace.h>

void
sys__exit(int exitcode)
{
	// Mark all children as orphans
	spinlock_acquire(&proc_table.pt_spinlock);

	make_all_children_orphans();

	spinlock_release(&proc_table.pt_spinlock);

	curproc->p_exit_status = _MKWAIT_EXIT(exitcode);

	// Destory/free everything except the proc struct itself, the exit status,
	// and the struct's lock

	thread_exit(); // TODO: consult w/ TF about when to reap

	kfree(curproc->p_name);
	array_destroy(curproc->p_children);
	sem_destroy(curproc->p_wait_sem);
	as_destroy(curproc->p_addrspace);

	// V() on wait_sem called in thread_exit
	thread_exit();
}
