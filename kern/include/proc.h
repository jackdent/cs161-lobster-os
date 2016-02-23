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

#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <array.h>
#include <spinlock.h>
#include <fdtable.h>
#include <synch.h>

struct addrspace;
struct thread;
struct vnode;


/*
 * Process structure.
 *
 * Note that we only count the number of threads in each process.
 * (And, unless you implement multithreaded user processes, this
 * number will not exceed 1 except in kproc.) If you want to know
 * exactly which threads are in the process, e.g. for debugging, add
 * an array and a sleeplock to protect it. (You can't use a spinlock
 * to protect an array because arrays need to be able to call
 * kmalloc.)
 *
 * You will most likely be adding stuff to this structure, so you may
 * find you need a sleeplock in here for other reasons as well.
 * However, note that p_addrspace must be protected by a spinlock:
 * thread_switch needs to be able to fetch the current address space
 * without sleeping.
 */
struct proc {
        /* These fields are all initialised by proc_create */
        pid_t p_pid;                    /* This process's pid */
        pid_t p_parent_pid;             /* Parent's pid */
        char *p_name;                   /* Name of this process */
        int p_exit_status;              /* exit status */
        unsigned p_numthreads;          /* Number of threads in this process. If num_threads
                                           is 0, either a thread never ran in the process or
                                           the process has completed, so the proc can be reaped */
        struct lock *p_lock;         /* Lock for this structure */
        struct semaphore *p_wait_sem;   /* Call V() when exited so parent can P() on it */
        struct array *p_children;       /* Array for keeping track of children pids
                                           -1 indicates an open slot in the array */

        /* This is initialised by proc_create, but does not bind STDIN, STDOUT or STDERR */
        struct fd_table *p_fd_table;    /* File descriptor table */

        /* These fields are NOT initialised by proc_create */
        struct addrspace *p_addrspace;  /* virtual address space */
        struct vnode *p_cwd;            /* current working directory */
};

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* This is the global process table */
extern struct proc_table proc_table;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);

/* Call once during system startup to bind STDIN/OUT/ERR to kproc. */
void kproc_stdio_bootstrap(void);

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Create a process. */
struct proc *proc_create(const char *name, int *err);

/* Cleanup a process for reaping. */
void proc_cleanup(struct proc *proc);

/* Reap a process at some point in the future, after calling after cleanup. */
void proc_reap(struct proc *proc);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Exit a process */
void proc_exit(struct proc *proc, int exitcode);

/* Attach a thread to a process. Must not already have a process. */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);

/* Add a child pid to a parent's children array */
int add_child_pid_to_parent(struct proc *parent, pid_t child_pid);

/* Remove a child pid from a parent's children array */
void remove_child_pid_from_parent(struct proc *parent, pid_t child_pid);

/* Assign all nonzombie children to kproc, and kfree all zombies */
void kproc_adopt_children(struct proc *proc);

/* Check if proc has any children */
bool proc_has_children(struct proc *proc);

/* Check if pid is a child of proc */
bool proc_has_child(struct proc *proc, pid_t pid);

#endif /* _PROC_H_ */
