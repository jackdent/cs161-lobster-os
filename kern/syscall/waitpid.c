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
#include <spinlock.h>


pid_t
sys_waitpid(pid_t pid, int *status, int options, int *err)
{

	// Local helper variables
	unsigned i;
	bool found;

	// Lock current proc struct
	spinlock_acquire(&curproc->p_lock);

	// Don't support any options
	if (options != 0) {
		*err = EINVAL;
		return -1;
	}

	// Validate passed in pid
	if (pid < PID_MIN || pid > PID_MAX){
		*err = ESRCH;
		return -1;
	}

	spinlock_acquire(&proc_table.pt_spinlock);
	if (!proc_table.pt_table[pid]) {
		*err = ESRCH;
		return -1;
	}
	spinlock_release(&proc_table.pt_spinlock);


	found = false;
	for (i = 0; i < curproc->children.num; i++) {
		if ((pid_t) array_get(&curproc->children, i) == pid) {
			found = true;
			break;
		}
	}

	if (!found){
		*err = ECHILD;
		return -1;
	}

	P(&proc_table.pt_table[pid]->wait_sem);

	// Save exit value to status if not NULL
	if (status) {
		*err = copyout(&(proc_table.pt_table[pid]->exit_status), (userptr_t)status, sizeof(int));
	}
	else {
		*err = 0;
	}

	// Finish cleaning up the child proc
	spinlock_acquire(&proc_table.pt_spinlock);
	sem_destroy(&proc_table.pt_table[pid]->wait_sem);
	spinlock_cleanup(&proc_table.pt_table[pid]->p_lock);
	kfree(proc_table.pt_table[pid]);
	proc_table.pt_table[pid] = NULL;
	spinlock_release(&proc_table.pt_spinlock);

	// Remove pid from array of children
	array_set(&curproc->children, i, (void*) -1);

	spinlock_release(&curproc->p_lock);

	return pid;
}
