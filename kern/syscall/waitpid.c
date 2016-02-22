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
	spinlock_acquire(&curproc->p_spinlock);

	// Don't support any options
	if (options != 0) {
		goto err1;
		err = EINVAL;
	}

	// Validate passed in pid
	if (!(is_valid_pid(pid))) {
		goto err1;
		err = ESRCH;
	}

	if (!is_child(pid)) {
		goto err1;
		err = ECHILD;
	}

	P(proc_table.pt_table[pid]->p_wait_sem);

	// Save exit value to status if not NULL
	if (status) {
		err = copyout(&(proc_table.pt_table[pid]->p_exit_status),
			(userptr_t)status, sizeof(int));
		if (err) {
			goto err1;
		}

	}
	else {
		err = 0;
	}

	// Finish cleaning up the child proc
	spinlock_acquire(&proc_table.pt_spinlock);
	sem_destroy(proc_table.pt_table[pid]->p_wait_sem);
	spinlock_cleanup(&proc_table.pt_table[pid]->p_spinlock);
	kfree(proc_table.pt_table[pid]);
	proc_table.pt_table[pid] = NULL;

	// Remove pid from array of children
	remove_child_pid_from_parent(pid);

	spinlock_release(&curproc->p_spinlock);

	*retval = pid;
	return err;

	err1:
		spinlock_release(&curproc->p_spinlock);
		return err;
}
