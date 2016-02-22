/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <kern/fcntl.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <proc.h>
#include <proctable.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <vfs.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 * Create a proc structure.
 */

struct proc *
proc_create(const char *name, int *err) {
	struct proc *proc;
	pid_t pid;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		*err = ENOMEM;
		goto err1;
	}

	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		*err = ENOMEM;
		goto err2;
	}

	pid = assign_proc_to_pid(proc);
	if (pid < 0) {
		/* N.B. our kernel only has one user so
		   ENPROC and EMPROC are semantically
		   equivalent */
		*err = ENPROC;
		goto err3;
	}

	proc->p_fd_table = fd_table_create();
	if (proc->p_fd_table == NULL) {
		goto err4;
	}

	proc->p_children = array_create();
	if (proc->p_children == NULL) {
		*err = ENOMEM;
		goto err5;
	}

	proc->p_wait_sem = sem_create(name, 0);
	if (proc->p_wait_sem == NULL) {
		*err = ENOMEM;
		goto err6;
	}

	spinlock_init(&proc->p_lock);

	proc->p_parent_pid = -1; // To be set by caller
	proc->p_numthreads = 0;
	proc->p_exit_status = -1;
	proc->p_addrspace = NULL; // Probably need to change
	proc->p_cwd = NULL; // Probably need to change

	*err = 0;
	return proc;


	err6:
		array_destroy(proc->p_children);
	err5:
		fd_table_destroy(proc->p_fd_table);
	err4:
		release_pid(pid);
	err3:
		kfree(proc->p_name);
	err2:
		kfree(proc);
	err1:
		return NULL;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	KASSERT(proc->p_numthreads == 0);

	spinlock_cleanup(&proc->p_lock);
	sem_destroy(proc->p_wait_sem);
	array_destroy(proc->p_children);
	fd_table_destroy(proc->p_fd_table);
	release_pid(proc->p_pid);
	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel, and initialise
 * the global process table lock
 */
void
proc_bootstrap(void)
{
	int err;

	proc_table_init();

	kproc = proc_create("[kernel]", &err);
	if (err) {
		panic("proc_create for kproc failed\n");
	}
}

/* Bind the console to STDIN, STDOUT and STDERR */
void
kproc_stdio_bootstrap(void)
{
	int err1, err2, err3;
	struct vnode *stdin, *stdout, *stderr;
	struct fd_file *stdin_f, *stdout_f, *stderr_f;

	char *con1 = kstrdup("con:");
	char *con2 = kstrdup("con:");
	char *con3 = kstrdup("con:");

	// TODO: do we need a different mode?
	err1 = vfs_open(con1, O_RDONLY, 0, &stdin);
	err2 = vfs_open(con2, O_WRONLY, 0, &stdout);
	err3 = vfs_open(con3, O_WRONLY, 0, &stderr);

	if (err1 || err2 || err3) {
		panic("vfs_open for console devices during STDIO initialisation failed\n");
	}

	stdin_f = fd_file_create(stdin, O_RDONLY);
	stdout_f = fd_file_create(stdout, O_WRONLY);
	stderr_f = fd_file_create(stderr, O_WRONLY);

        if (stdin_f == NULL || stdout_f == NULL || stderr_f == NULL) {
		panic("fd_file_create for STDIO failed\n");
        }

	curproc->p_fd_table->fdt_table[STDIN_FILENO] = stdin_f;
	curproc->p_fd_table->fdt_table[STDOUT_FILENO] = stdout_f;
	curproc->p_fd_table->fdt_table[STDERR_FILENO] = stderr_f;
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;
	int err;

	newproc = proc_create(name, &err);
	if (err) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}



int is_valid_pid(pid_t pid)
{
	int err;

	if (!(pid >= PID_MIN && pid < PID_MAX)) {
		return ESRCH;
	}

	spinlock_acquire(&proc_table.pt_spinlock);
	if (!proc_table.pt_table[pid]) {
		err = ESRCH;
	}
	else {
		err = 0;
	}
	spinlock_release(&proc_table.pt_spinlock);
	return err;
}

// Assumes curproc is already locked
// Return 0 on success, ENOMEM on error
int
add_child_pid_to_parent(pid_t child_pid)
{
	unsigned i;
	pid_t tmp;

	for (i = 0; i < curproc->p_children->num; i++) {
		tmp = (pid_t) array_get(curproc->p_children, i);
		if (tmp == -1) {
			array_set(curproc->p_children, i, (void*)child_pid);
			return 0;
		}
	}
	return array_add(curproc->p_children, (void*)child_pid, NULL);
}

// Assumes curproc is already locked, and the child_pid
// is actually in the children array
void
remove_child_pid_from_parent(pid_t child_pid)
{
	unsigned i;

	for (i = 0; i < curproc->p_children->num; i++) {
		if ((pid_t)array_get(curproc->p_children, i) == child_pid) {
			array_set(curproc->p_children, i, (void*)-1);
			break;
		}
	}
}

// Assumes curproc is already locked
void
make_all_children_orphans(void)
{
	unsigned i;
	pid_t pid;

	for (i = 0; i < curproc->p_children->num; i++) {
		pid = (pid_t) array_get(curproc->p_children, i);
		if (pid != (pid_t)-1) {
			spinlock_acquire(&proc_table.pt_table[pid]->p_lock);
			proc_table.pt_table[pid]->p_parent_pid = 0;
			spinlock_release(&proc_table.pt_table[pid]->p_lock);
		}
	}
}

// Assumes curproc is already locked
// Check if pid is a child of curproc
bool
is_child(pid_t pid)
{
	unsigned i;

	for (i = 0; i < curproc->p_children->num; i++) {
		if ((pid_t) array_get(curproc->p_children, i) == pid) {
			return true;
		}
	}

	return false;
}
