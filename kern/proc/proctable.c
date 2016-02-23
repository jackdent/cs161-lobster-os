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
                return ESRCH;
        }

        spinlock_acquire(&proc_table.pt_spinlock);

        if (!proc_table.pt_table[pid]) {
                err = ESRCH;
        } else {
                err = 0;
        }

        spinlock_release(&proc_table.pt_spinlock);

        return err;
}

pid_t
assign_proc_to_pid(struct proc *proc)
{
        KASSERT(proc != NULL);

        spinlock_acquire(&proc_table.pt_spinlock);

        for (pid_t i = 0; i < PID_MAX; ++i) {
                if (proc_table.pt_table[i] == NULL) {
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
