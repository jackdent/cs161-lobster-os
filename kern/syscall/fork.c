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

static
void
child_finish_setup(void *child_tf, unsigned long n)
{
	(void)n;

	as_activate(); 	/* TODO: thread_fork may call as_activate? */
	// TODO: when does child_tf get freed?
	mips_usermode((struct trapframe *)child_tf);
}

int sys_fork(struct trapframe *parent_tf, pid_t *retval)
{
	int err;
	struct proc *child_proc;
	struct addrspace *child_as;
	struct trapframe *child_tf;

	spinlock_acquire(&curproc->p_spinlock);

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

	child_tf = kmalloc(sizeof(*child_tf));
	memcpy(child_tf, parent_tf, sizeof(*child_tf));
	if (child_tf == NULL) {
		err = ENOMEM;
		goto err3;
	}

	child_tf->tf_v0 = 0; // fork should return 0 to child
	child_tf->tf_a3 = 0; // No error has occured
	child_tf->tf_epc += 4; // Advance the child's program counter

	clone_fd_table(curproc->p_fd_table, child_proc->p_fd_table);

	err = as_copy(curproc->p_addrspace, &child_as);
	if (err) {
		err = ENOMEM;
		goto err4;
	}

	child_proc->p_addrspace = child_as;

	child_proc->p_cwd = curproc->p_cwd;
	VOP_INCREF(child_proc->p_cwd);

	err = thread_fork("child", child_proc, child_finish_setup, child_tf, 0);
	if (err) {
		goto err5;
	}

	spinlock_release(&curproc->p_spinlock);

	*retval = child_proc->p_pid;
	return 0;


	err5:
		as_destroy(child_as);
	err4:
		kfree(child_tf);
	err3:
		remove_child_pid_from_parent(curproc, child_proc->p_pid);
	err2:
		proc_destroy(child_proc);
	err1:
		spinlock_release(&curproc->p_spinlock);
		return err;
}
