#include <types.h>
#include <limits.h>
#include <proc.h>
#include <spinlock.h>


struct proc_table {
        struct proc *pt_table[PID_MAX];
        struct spinlock pt_spinlock;
};

/* This is the global process table */
struct proc_table proc_table;
void proc_table_init(void);

/* Check if in range and actual process */
int is_valid_pid(pid_t pid);

/* Find a free pid in the global process table and assign it to proc. Return the
   pid if found; otherwise, return -1 */
pid_t assign_proc_to_pid(struct proc *);
void release_pid(pid_t pid);
