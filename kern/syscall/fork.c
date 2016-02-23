#include <types.h>
#include <lib.h>
#include <thread.h>
#include <copyinout.h>
#include <syscall.h>
#include <vfs.h>
#include <vnode.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <current.h>
#include <kern/iovec.h>
#include <uio.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <limits.h>
#include <proc.h>


// Child V()'s on signal_to_parent to let parent know it is
// safe to free the setup_data struct

struct setup_data {
	struct trapframe* child_tf;
	struct semaphore* signal_to_parent;
};

static
void
child_finish_setup(void *ptr, unsigned long n)
{
	(void)n;

	struct setup_data *sd;
	struct trapframe copied_child_tf;

	sd = (struct setup_data*) ptr;
	copied_child_tf = *sd->child_tf;

	as_activate();
	V(sd->signal_to_parent);

	mips_usermode(&copied_child_tf);
}

int sys_fork(struct trapframe *parent_tf, pid_t *retval)
{
	int err;
	struct proc *child_proc;
	struct addrspace *child_as;
	struct trapframe *child_tf;
	struct setup_data* sd;

	int j = 4;
	for (int i = 0; i < 32000000; i++) {
		j++;
	}

	lock_acquire(curproc->p_lock);

	child_proc = proc_create(curproc->p_name, &err);
	if (child_proc == NULL) {
		err = ENOMEM;
		goto err1;
	}

	child_proc->p_parent_pid = curproc->p_pid;
	err = add_child_pid_to_parent(curproc, child_proc->p_pid);
	if (err) {
		goto err2;
	}

	sd = kmalloc(sizeof(*sd));
	if (sd == NULL) {
		err = ENOMEM;
		goto err3;
	}

	sd->signal_to_parent = sem_create("signal_to_parent", 0);
	if (sd->signal_to_parent == NULL) {
		err = ENOMEM;
		goto err4;
	}

	child_tf = kmalloc(sizeof(*child_tf));
	if (child_tf == NULL) {
		err = ENOMEM;
		goto err5;
	}

	*child_tf = *parent_tf;
	sd->child_tf = child_tf;

	child_tf->tf_v0 = 0; // fork should return 0 to child
	child_tf->tf_a3 = 0; // No error has occured
	child_tf->tf_epc += 4; // Advance the child's program counter

	clone_fd_table(curproc->p_fd_table, child_proc->p_fd_table);

	err = as_copy(curproc->p_addrspace, &child_as);
	if (err) {
		err = ENOMEM;
		goto err6;
	}

	child_proc->p_addrspace = child_as;

	child_proc->p_cwd = curproc->p_cwd;
	VOP_INCREF(child_proc->p_cwd);

	err = thread_fork("child", child_proc, child_finish_setup, sd, 0);
	if (err) {
		goto err7;
	}

	P(sd->signal_to_parent);
	sem_destroy(sd->signal_to_parent);
	kfree(child_tf);
	kfree(sd);


	lock_release(curproc->p_lock);

	*retval = child_proc->p_pid;
	return 0;

	err7:
		as_destroy(child_as);
	err6:
		kfree(child_tf);
	err5:
		sem_destroy(sd->signal_to_parent);
	err4:
		kfree(sd);
	err3:
		remove_child_pid_from_parent(curproc, child_proc->p_pid);
	err2:
		proc_destroy(child_proc);
	err1:
		lock_release(curproc->p_lock);
		return err;
}
