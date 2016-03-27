#include <tlb.h>
#include <coremap.h>

/*
 * Processes will only share memory in their user segments with the
 * kernel page flushing daemon. When we receive a tlb shootdown, we
 * should block until the daemon is finished writing to disk, then
 * mark the TLB entry for the original virtual address as clean.
 * The kernel daemon is responsible for marking the core map entry
 * as clean.
 */
void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
        KASSERT(curproc != NULL);

        uint32_t entryhi, entrylo;
        int index;
        struct cme cme;

        // Acquire then immediately release the lock on the cme so
        // that we block until the kernel daemon has finished
        // flushing memory out to disk, to avoid race conditions.
        cme_acquire_lock(ts->ts_flushed_page);
        cme = coremap.cme[ts->ts_flushed_page];
        cme_release_lock(ts->ts_flushed_page);

        if (cme.cme_pid != curproc->p_pid) {
                // The page isn't in the address space for the
                // currently scheduled process and will not
                // be in the TLB, so we NOOP.
                return;
        }

        if (cme.cme_state == S_FREE) {
                panic("Kernel daemon sent a TLB shootdown for an invalid page\n");
        }

        if (cme.cme_state == S_DIRTY) {
                panic("Page still dirty after attempted flush\n");
        }

        entryhi = OFFSETS_TO_VPAGE(cme.cme_l1_offset, cme.cme_l2_offset);
        index = tlb_probe(entryhi, 0);

        if (index < 0) {
                // The page wasn't in the TLB, so we NOOP
                return;
        }

        // Ensure that the TLB entry is clean
        entrylo = CME_ID_TO_PPAGE(ts->ts_flushed_page);
        tlb_write(entryhi, entrylo, (uint32_t)index);
}

/*
 * Implements the Least Recently Added (LRA) algorithm
 * to evict TLB entries. The entries in the TLB are
 * ordered by index according to when they were added.
 * The tlb_lra field on the current cpu marks the index
 * after the most recently added entry, which is the
 * least recently added.
 */
static
void
add_va_to_tlb(struct pte *pte, vaddr_t va)
{
        KASSERT(curthread != NULL);

        uint32_t entryhi, entrylo, lra;
        cme_id_t cme_id;
        struct cme cme;
        int index;

        cme_id = (cme_id_t)pte->pte_phys_page;

        cme_acquire_lock(cme_id);
        cme = coremap.cmes[cme_id];

        if (cme.cme_state == S_FREE) {
                panic("Tried to add a free page to the TLB\n");
        }

        entryhi = VA_TO_VPAGE(va);

        if (cme.cme_dirty == 1) {
                entrylo = CME_ID_TO_WRITEABLE_PPAGE(cme_id);
        } else {
                entrylo = CME_ID_TO_PPAGE(cme_id);
        }

        lra = curthread->t_cpu->c_tlb_lra;
        tlb_write(entryhi, entrylo, lra);
        curthread->t_cpu->c_tlb_lra = (lra + 1) % NUM_TLB;

        cme_release_lock(cme_id);
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
        coremap.cmes[cme_id].cme_dirty = 1;

        entryhi = VA_TO_VPAGE(va);
        index = tlb_probe(entryhi, 0);

        if (index < 0) {
                // TODO: should we add_va_to_tlb instead?
                panic("Tried to mark a non-existent TLB entry as dirty\n");
        }

        entrylo = CME_ID_TO_WRITEABLE_PPAGE(cme_id);
        tlb_write(entryhi, entrylo, (uint32_t)index);

        cme_release_lock(cme_id);
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
