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
add_pte_to_tlb(struct pte pte)
{
        (void) pte;
        // TODO
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
        struct pte pte;
        uint32_t ehi, elo;
        vaddr_t stack_start, heap_end;

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

        heap_end = as->as_heap_base + as->as_heap_size;
        stack_start = stacfaultaddress < USERSTACK - as->as_stack_size
        if (faultaddress > heap_end && faultaddress < stack_start) {
                return EFAULT;
        }

        pte = get_pte(faultaddress);
        ehi = faultaddress & TLBHI_VPAGE;
        elo = tlb_probe(ehi, 0);
        if (elo < 0) {
                //
        } else {

        }

        switch (faulttype) {
        case VM_FAULT_READ:
                if (pte.pte_valid == 0) {
                        // allocate physical memory for the page
                        // add it to the page table
                        add_pte_to_tlb(pte);
                } else if (pte.pte_present == 0) {
                        // swap from disk into RAM
                } else {
                        panic("What?!\n");
                }

                break;
        case VM_FAULT_WRITE:
                mark_tlb_entry_dirty(faultaddress);
                break;
        case VM_FAULT_READONLY:
                return EFAULT;
        }

        return 0;
}
