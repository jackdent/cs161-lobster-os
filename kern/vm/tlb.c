#include <cme.h>
#include <coremap.h>
#include <tlb.h>
#include <current.h>
#include <spl.h>
#include <cpu.h>

// TODO: when do we want to disable interrupts?

/*
 * Implements the Least Recently Added (LRA) algorithm
 * to evict TLB entries. The entries in the TLB are
 * ordered by index according to when they were added.
 * The tlb_lra field on the current cpu marks the index
 * after the most recently added entry, which is the
 * least recently added.
 */
void
tlb_add(vaddr_t va, struct pte *pte)
{
        KASSERT(curthread != NULL);

        uint32_t entryhi, entrylo, lra;
        cme_id_t cme_id;
        struct cme cme;
        int index;

        cme_id = (cme_id_t)pte->pte_phys_page;

        cm_acquire_lock(cme_id);
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

        cm_release_lock(cme_id);
}

void
tlb_make_writeable(vaddr_t va, struct pte *pte)
{
        uint32_t entryhi, entrylo;
        cme_id_t cme_id;
        int index;

        cme_id = (cme_id_t)pte->pte_phys_page;

        cm_acquire_lock(cme_id);
        coremap.cmes[cme_id].cme_dirty = 1;

        entryhi = VA_TO_VPAGE(va);
        index = tlb_probe(entryhi, 0);

        if (index < 0) {
                // TODO: should we tlb_add instead?
                panic("Tried to mark a non-existent TLB entry as dirty\n");
        }

        entrylo = CME_ID_TO_WRITEABLE_PPAGE(cme_id);
        tlb_write(entryhi, entrylo, (uint32_t)index);

        cm_release_lock(cme_id);
}

void()
tlb_remove(vaddr_t va)
{
    int i, spl;
    uint32_t entryhi;

    /* Disable interrupts on this CPU while frobbing the TLB. */
    spl = splhigh();

    entryhi = VA_TO_VPAGE(va);
    index = tlb_probe(entryhi, 0);

    if (i < 0) {
            // The page wasn't in the TLB, so we NOOP
            return;
    }

    tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);

    splx(spl);
}

void
tlb_flush()
{
    int i, spl;

    /* Disable interrupts on this CPU while frobbing the TLB. */
    spl = splhigh();

    &curcpu->c_tlb_lra = 0;

    for (i = 0; i < NUM_TLB; ++i) {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }

    splx(spl);
}

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
        cm_acquire_lock(ts->ts_flushed_page);
        cme = coremap.cme[ts->ts_flushed_page];
        cm_release_lock(ts->ts_flushed_page);

        if (cme.cme_pid != curproc->p_pid) {
                // The page isn't in the address space for the
                // currently scheduled process and will not
                // be in the TLB, so we NOOP.
                return;
        }

        if (cme.cme_state != S_CLEAN) {
                panic("Kernel daemon sent a TLB shootdown for an invalid page\n");
        }

        entryhi = VA_TO_VPAGE(OFFSETS_TO_VA(cme.cme_l1_offset, cme.cme_l2_offset));
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
 * If the page is already in memory, NOOP. Otherwise, find a slot
 * in the core map and assign it to the page table entry.
 *
 * If the page is in the lazy state, we're done. Otherwise,
 * the page was in the swap space on disk, so we copy
 * it into physical memory and set its swap_id on our core map
 * entry.
 *
 * Finally, we set the present bit to indicate the page
 * is now accessible in main memory.
 *
 * Assumes that the caller has validated the virtual address.
 */
static
void
ensure_in_memory(struct pte *pte, vaddr_t va)
{
        KASSERT(curproc != NULL);

        struct cme cme;
        cme_id_t slot;

        cme = cme_create(curproc->p_pid, va, S_CLEAN);

        switch(pte->pte_state) {
        case S_PRESENT:
                return;
        case S_INVALID:
                panic("Cannot ensure than an invalid pte is in memory\n");
        case S_LAZY:
                break;
        case S_SWAPPED:
                slot = cm_capture_slot();

                evict_page(slot);

                cme.swap_id = pte_get_swap_id(pte);
                swap_in(cme.swap_id, CME_ID_TO_PA(slot));

                break;
        }


        pte->pte_state = S_PRESENT;
        coremap.cmes[slot] = cme;
        cm_release_lock(slot);
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

        if (!va_in_as_bounds(as, faultaddress)) {
            return EFAULT;
        }

        if (!pagetable_contains_va(as->as_pt, faultaddress, &pte)) {
            return EFAULT;
        }

        ensure_in_memory(pte, faultaddress);

        switch (faulttype) {
        case VM_FAULT_READ:
                tlb_add(faultaddress, pte);
                break;
        case VM_FAULT_READONLY:
                tlb_make_writeable(faultaddress, pte);
                break;
        case VM_FAULT_WRITE:
                panic("Tried to write to a non-existent TLB entry");
                break;
        default:
                panic("Unknown TLB fault type\n");
                break;
        }

        return 0;
}
