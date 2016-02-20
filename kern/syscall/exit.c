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
	pid_t pid;
	unsigned i;

	// Mark all children as orphans
	spinlock_acquire(&proc_table.pt_spinlock);

	for (i = 0; i < curproc->children->num; i++) {
		pid = (pid_t) array_get(curproc->children, i);
		if (pid != -1) {
			spinlock_acquire(&proc_table.pt_table[pid]->p_lock);
			proc_table.pt_table[pid]->parent_pid = -1;
			spinlock_release(&proc_table.pt_table[pid]->p_lock);
		}
	}

	spinlock_release(&proc_table.pt_spinlock);

	curproc->exit_status = _MKWAIT_EXIT(exitcode);

	// Destory/free everything except the proc struct itself, the exit status,
	// and the struct's lock
	thread_exit(); // TODO more cleanup here

	kfree(curproc->p_name);
	array_destroy(curproc->children);
	sem_destroy(curproc->wait_sem);
	as_destroy(curproc->p_addrspace);

	// V() on wait_sem called in thread_exit
	thread_exit();

}
