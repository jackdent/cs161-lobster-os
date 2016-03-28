#include <types.h>
#include <kern/errno.h>
#include <syscall.h>
#include <addrspace.h>
#include <proc.h>
#include <current.h>

int
sys_sbrk(int32_t amount, int32_t *retval)
{
        struct addrspace *as;
        vaddr_t old_break, new_break;

        as = curproc->p_addrspace;
        old_break = as->as_heap_end;
        new_break = old_break + amount;

        if (new_break < as->as_heap_base) {
                return EINVAL;
        } else if (new_break > as->as_stack_end) {
                return ENOMEM;
        }

        // TODO: free or add pages to pagetable.
        // NB, we  probably want to round new_break up to
        // the nearest page, because otherwise we could
        // read from an invalid address in the last heap
        // page if the break is in the middle

        as->as_heap_end = new_break;
        *retval = old_break;

        return 0;
}
