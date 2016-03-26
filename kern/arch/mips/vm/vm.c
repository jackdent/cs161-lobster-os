#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <pagetable.h>
#include <coremap.h>

void
vm_bootstrap(void)
{
        cm_init();
}

vaddr_t
alloc_kpages(unsigned npages)
{
        (void)npages;

        /* TODO */
        return 0;
}

void
free_kpages(vaddr_t addr)
{
        (void)addr;

        /* TODO */
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
        (void)ts;

        /* TODO */
}

static
void
add_pte_to_tlb(struct pte *pte)
{
        (void)pte;

        /* TODO */
        // paddr_t pa;
        // pa = va_to_pa(faultaddress, pte);
}

static
void
mark_tlb_entry_dirty(vaddr_t addr)
{
        tlb_write(addr & TLBHI_VPAGE);
}

/*
 * Called on TLB exceptions
 * Returns EFAULT if address isn't mapped
 */
int
vm_fault(int faulttype, vaddr_t faultaddress)
{
        struct addrspace *as;
        struct pte *pte;

        if (curproc == NULL) {
                /*
                 * No process. This is probably a kernel fault early
                 * in boot. Return EFAULT so as to panic instead of
                 * getting into an infinite faulting loop.
                 */
                return EFAULT;
        }

        as = proc_getas();
        if (as == NULL) {
                /*
                 * No address space set up. This is probably also a
                 * kernel fault early in boot.
                 */
                return EFAULT;
        }

        if (!va_in_bounds(as, faultaddress)) {
            return EFAULT;
        }

        if (!va_in_pagetable(faultaddress, as->as_pt, &pte)) {
            return EFAULT;
        }

        ensure_in_memory(as, faultaddress, pte);

        switch (faulttype) {
        case VM_FAULT_READ:
                add_pte_to_tlb(pte);
                break;
        case VM_FAULT_WRITE:
                panic("Tried to write to a non-existent TLB entry");
                break;
        case VM_FAULT_READONLY:
                make_va_writeable(as, va);
                break;
        default:
                panic("Unknown TLB fault type\n");
                break;
        }

        return 0;
}
