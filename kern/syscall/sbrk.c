#include <types.h>
#include <kern/errno.h>
#include <syscall.h>
#include <addrspace.h>
#include <proc.h>
#include <current.h>
#include <machine/vm.h>

/*
 * Amount must be a multiple of PAGE_SIZE, otherwise
 * sbrk wil lreturn EINVAL
 */
int
sys_sbrk(int32_t amount, int32_t *retval)
{
        struct addrspace *as;
        vaddr_t old_break, new_break;
        unsigned int npages;

        as = curproc->p_addrspace;
        old_break = as->as_heap_end;
        new_break = old_break + amount;

        npages = amount / PAGE_SIZE;
        *retval = old_break;

        if (amount == 0) {
                return 0;
        }

        // The new heap break must be page aligned
        if (amount % PAGE_SIZE != 0) {
                return EINVAL;
        }

        if (new_break < as->as_heap_base) {
                return EINVAL;
        }

        if (new_break > as->as_stack_end) {
                return ENOMEM;
        }

        if (new_break > old_break) {
                alloc_upages(as->as_pt, old_break, npages);
        } else {
                free_upages(as->as_pt, new_break, npages);
        }

        as->as_heap_end = new_break;

        return 0;
}
