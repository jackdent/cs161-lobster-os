#include <types.h>
#include <kern/wait.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>

void
sys__exit(int exitcode)
{
	proc_exit(curproc, _MKWAIT_EXIT(exitcode));
}
