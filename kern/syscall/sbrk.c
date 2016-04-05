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
        int err;

        as = curproc->p_addrspace;
        old_break = as->as_heap_end;
        new_break = old_break + amount;

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

        if (new_break > HEAP_MAX) {
                return ENOMEM;
        }

        if (new_break > as->as_stack_end) {
                return ENOMEM;
        }

        if (new_break > old_break) {
                npages = amount / PAGE_SIZE;

                err = alloc_upages(old_break, npages);
                if (err) {
                        return err;
                }
        } else {
                npages = (amount * -1) / PAGE_SIZE;
                free_upages(new_break, npages);
        }

        as->as_heap_end = new_break;

        return 0;
}
