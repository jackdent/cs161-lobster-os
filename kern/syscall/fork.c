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

/*
Use 1 semaphores to communicate between parent and child threads
for setup. When the child has copied its trapframe over to a local
struct variable, it signals to the parent that it can continue executing
*/

struct setup_data {
	struct semaphore *parent_wait_for_child;
	struct trapframe child_tf;
};

// To let child get its trapframe
static
void
child_finish_setup(void *p, unsigned long n)
{
	(void)n;
	struct setup_data *sd;
	struct trapframe tf;

	sd = (struct setup_data *)p;
	tf = sd->child_tf;

	// Tell parent we're done with the setup_data struct
	V(sd->parent_wait_for_child);

	as_activate();
	mips_usermode(&tf);
}

int sys_fork(struct trapframe *tf, pid_t *retval)
{
	int err;
	struct proc *child_proc;
	struct setup_data *sd;
	pid_t child_pid;
	struct addrspace *child_as;


	child_proc = proc_create(curproc->p_name, &err);
	if (child_proc == NULL) {
		goto err1;
	}

	child_pid = child_proc->p_pid;

	spinlock_acquire(&curproc->p_lock);

	// Save parent's pid in child_proc
	child_proc->p_parent_pid = curproc->p_pid;

	// Setup data to be passed into child's setup function
	sd = kmalloc(sizeof(struct setup_data));
	if (sd == NULL){
		err = ENOMEM;
		goto err2;
	}

	sd->parent_wait_for_child = sem_create("parent wait for child", 0);
	if (sd->parent_wait_for_child == NULL) {
		err = ENOMEM;
		goto err3;
	}


	// Alter child's return value
	sd->child_tf = *tf;
	sd->child_tf.tf_v0 = 0;
	sd->child_tf.tf_v1 = 0;
	sd->child_tf.tf_a3 = 0;
	sd->child_tf.tf_epc += 4;

	// Copy over parent's address space
	err = as_copy(curthread->t_addrspace, &child_as);
	if (err) {
		err = ENOMEM;
		goto err4;
	}

	err = add_child_pid_to_parent(child_pid);
	if (err) {
		goto err5;
	}

	child_proc->p_addrspace = child_as;

	// TODO: copy parent's file table to child
	reference_each_file(curproc->p_fd_table);

	// Need to set up actual child thread
	err = thread_fork("child", child_proc, child_finish_setup, sd, 0);
	if (err) {
		goto err6;
	}

	spinlock_release(&curproc->p_lock);

	// Wait for child to get its trapframe
	P(sd->parent_wait_for_child);

	// Now safe to destroy setup data structs
	sem_destroy(sd->parent_wait_for_child);
	kfree(sd);

	*retval = child_pid;

	KASSERT(err == 0);
	return err;


	err6:
		remove_child_pid_from_parent(child_pid);
		spinlock_release(&curproc->p_lock);
	err5:
		as_destroy(child_as);
	err4:
		sem_destroy(sd->parent_wait_for_child);
	err3:
		kfree(sd);
	err2:
		proc_destroy(child_proc);
	err1:
		return err;
}
