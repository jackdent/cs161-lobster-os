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


// To let child finish setting up its trapframe
static
void
child_init(struct trapframe *child_tf) {
        *child_tf.tf_v0 = 0;
        *child_tf.tf_v1 = 0;
        *child_tf.tf_a3 = 0;
        *child_tf.tf_epc += 4; // Advance program counter
        as_activate(curthread->t_addrspace);

        mips_usermode(child_tf);
}


pid_t sys_fork(struct trapframe *tf, int *err){
        int i;
        struct proc *child;
        struct addrspace child_as;
        pid_t tmp;
        bool found;
        pid_t child_pid;
        struct trapframe child_tf;

        child = proc_create(curproc->p_name);
        if (child == NULL) {
                goto err1;
        }

        child_pid = child->p_pid;

        spinlock_acquire(curproc->p_lock);

        // Save parent's pid in child
        child->parent_pid = curproc->p_pid;

        // Copy trapframe from parent
        child_tf = *tf;
        s->child_tf = &child_tf;

        // Copy over parent's address space
        *err = as_copy(curthread->t_addrspace, &child_as);
        if (*err) {
                *err = ENOMEM;
                goto err2;
        }

        found = false;
        for (i = 0; i < curproc->children->num; i++) {
                tmp = (pid_t) array_get(curproc->children, i);
                if (tmp == -1) {
                        array_set(curproc->children, (void*)child_pid, i);
                        found = true;
                        break;
                }
        }
        if (!found) {
                *err = array_add(curproc->children, (void*)child_pid, NULL);
                if (*err) {
                        *err = ENOMEM;
                        goto err3;
                }
        }

        struct thread *child_thread;

        /*
        TODO: Handle file descriptors
        */

        // Need to set up child thread, and some more stuff

        *err = thread_fork("child", child_init, child_tf, 0, &child_thread);
        if (*err) {
                goto err4;
        }

        return child_pid;



        err4;

        err3:

        err2:

        err1:
                return -1;
}
