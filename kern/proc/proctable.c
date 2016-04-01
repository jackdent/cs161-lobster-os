#include <kern/errno.h>
#include <proctable.h>
#include <lib.h>

void
proc_table_init()
{
        spinlock_init(&proc_table.pt_spinlock);
}

int is_valid_pid(pid_t pid)
{
        int err;

        if (pid < PID_MIN || pid >= PID_MAX) {
                return 0;
        }

        spinlock_acquire(&proc_table.pt_spinlock);

        if (!proc_table.pt_table[pid]) {
                err = 0;
        } else {
                err = 1;
        }

        spinlock_release(&proc_table.pt_spinlock);

        return err;
}

/*
 * Looks for a pid that is NULL, or a pid that belongs to an
 * inactive process whose parent is kproc.
 */
pid_t
assign_proc_to_pid(struct proc *proc)
{
        struct proc *pt_proc;

        KASSERT(proc != NULL);

        spinlock_acquire(&proc_table.pt_spinlock);

        for (pid_t i = 0; i < PID_MAX; i++) {
                pt_proc = proc_table.pt_table[i];

                if (pt_proc != NULL
                    && pt_proc->p_numthreads == 0
                    && pt_proc->p_parent_pid == kproc->p_pid) {
                        proc_reap(pt_proc);
                        proc_table.pt_table[i] = NULL;
                }

                if (pt_proc == NULL) {
                        proc->p_pid = i;
                        proc_table.pt_table[i] = proc;
                        spinlock_release(&proc_table.pt_spinlock);
                        return i;
                }
        }

        spinlock_release(&proc_table.pt_spinlock);
        return -1;
}

void
release_pid(pid_t pid)
{
        KASSERT(pid >= 0 && pid < PID_MAX);

        spinlock_acquire(&proc_table.pt_spinlock);
        proc_table.pt_table[pid] = NULL;
        spinlock_release(&proc_table.pt_spinlock);
}
