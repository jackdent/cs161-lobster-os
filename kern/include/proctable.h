#include <proc.h>
#include <spinlock.h>
#include <limits.h>


struct proc_table {
        struct proc *pt_table[PID_MAX];
        struct spinlock *pt_spinlock;
};

/* This is the global process table */
struct proc_table proc_table;

/* Find a free pid in the global process table and assign it to proc. Return the
   pid if found; otherwise, return -1 */
pid_t assign_pid_to_proc(struct proc *);
void release_pid(pid_t pid);
