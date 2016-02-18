#include <types.h>
#include <lib.h>
#include <proctable.h>


pid_t
assign_pid_to_proc(struct proc *proc)
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
