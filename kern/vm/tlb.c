#include <tlb.h>

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
        (void)ts;

        /* TODO */
}

// TODO: better eviction policy
static
void
add_va_to_tlb(struct pte *pte, vaddr_t va)
{
        uint32_t entryhi, entrylo;
        cme_id_t cme_id;

        cme_id = (cme_id_t)pte->pte_phys_page;

        entryhi = VA_TO_VPAGE(va);
        entrylo = CME_ID_TO_PPAGE(cme_id);

        tlb_random(entryhi, entrylo);
}

static
void
make_va_writeable(struct pte *pte, vaddr_t va)
{
        uint32_t entryhi, entrylo;
        cme_id_t cme_id;
        int index;

        cme_id = (cme_id_t)pte->pte_phys_page;
        cme_acquire_lock(cme_id);
        coremap[cme_id].cme_dirty = 1;
        cme_release_lock(cme_id);

        entryhi = VA_TO_VPAGE(va);
        entrylo = WRITEABLE_CME(cme_id);

        index = tlb_probe(entryhi, entrylo);
        if (index < 0) {
                // TODO: should we add_va_to_tlb instead?
                panic("Tried to mark a non-existent TLB entry as dirty\n");
        }
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
                add_va_to_tlb(faultaddress, pte);
                break;
        case VM_FAULT_WRITE:
                panic("Tried to write to a non-existent TLB entry");
                break;
        case VM_FAULT_READONLY:
                make_va_writeable(faultaddress, pte);
                break;
        default:
                panic("Unknown TLB fault type\n");
                break;
        }

        return 0;
}
