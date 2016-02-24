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

int
sys_waitpid(pid_t pid, int *status, int options, pid_t *retval)
{
	int err;

	// Lock current proc struct
	lock_acquire(curproc->p_lock);

	// Don't support any options
	if (options != 0) {
		err = EINVAL;
		goto err1;
	}

	// Validate passed in pid
	if (!(is_valid_pid(pid))) {
		err = ESRCH;
		goto err1;
	}

	if (!proc_has_child(curproc, pid)) {
		err = ECHILD;
		goto err1;
	}

	P(proc_table.pt_table[pid]->p_wait_sem);

	// Save exit value to status if not NULL
	if (status) {
		err = copyout(&(proc_table.pt_table[pid]->p_exit_status),
			(userptr_t)status, sizeof(int));
		if (err) {
			goto err2;
		}

	}
	else {
		err = 0;
	}

	// Finish cleaning up the child proc
	spinlock_acquire(&proc_table.pt_spinlock);
	sem_destroy(proc_table.pt_table[pid]->p_wait_sem);
	kfree(proc_table.pt_table[pid]);
	proc_table.pt_table[pid] = NULL;
	spinlock_release(&proc_table.pt_spinlock);

	// Remove pid from array of children
	remove_child_pid_from_parent(curproc, pid);

	lock_release(curproc->p_lock);

	*retval = pid;
	return err;


	err2:
		// Undo our P() call
		V(proc_table.pt_table[pid]->p_wait_sem);
	err1:
		lock_release(curproc->p_lock);
		return err;
}
