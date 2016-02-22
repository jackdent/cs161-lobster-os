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
	tf = sd->trapframe;

	// Tell parent we're done with the setup_data struct
	V(sd->parent_wait_for_child);

	as_activate();
	mips_usermode(&tf);
}


int sys_fork(struct trapframe *tf, pid_t *retval)
{
	unsigned i;
	int err;
	struct proc *child_proc;
	struct trapframe child_tf;
	pid_t tmp;
	bool found;
	pid_t child_pid;


	child_proc = proc_create(curproc->p_name, err);
	if (child_proc == NULL) {
		goto err1;
	}

	child_pid = child_proc->p_pid;

	spinlock_acquire(&curproc->p_lock);

	// Save parent's pid in child_proc
	child_proc->parent_pid = curproc->p_pid;

	// Setup data to be passed into child's setup function
	struct setup_data *sd = kmalloc(sizeof(struct setup_data));
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
	tf.tf_v0 = 0;
	tf.tf_v1 = 0;
	tf.tf_a3 = 0;
	tf.tf_epc += 4;

	// Copy over parent's address space
	err = as_copy(curthread->t_addrspace, &child_as);
	if (err) {
		err = ENOMEM;
		goto err4;
	}

	// Save child's pid in parent proc
	found = false;
	for (i = 0; i < curproc->children->num; i++) {
		tmp = (pid_t) array_get(curproc->children, i);
		if (tmp == -1) {
			array_set(curproc->children, i, (void*)child_pid);
			found = true;
			break;
		}
	}
	if (!found) {
		err = array_add(curproc->children, (void*)child_pid, NULL);
		if (*err) {
			err = ENOMEM;
			goto err5;
		}
	}

	child_proc->p_addrspace = child_as;

	// Need to set up actual child thread
	*err = thread_fork("child", child_proc, child_finish_setup, sd, 0);
	if (*err) {
		goto err6;
	}

	spinlock_release(&curproc->p_lock);

	/*
	TODO: Handle file descriptors here
	*/


	V(sd->child_wait_for_parent);
	P(sd->parent_wait_for_child);

	// Now safe to destroy setup data structs
	sem_destroy(sd->parent_wait_for_child);
	kfree(sd);

	return child_pid;


	err6:
		// Remove child's pid from parent's children array
		for (i = 0; i < curproc->children->num; i++) {
			if ((pid_t)array_get(curproc->children, i) == child_pid) {
				array_set(curproc->children, i, (void*)-1);
				break;
			}
		}
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
		return -1;
}
