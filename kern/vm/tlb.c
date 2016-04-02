#include <cme.h>
#include <coremap.h>
#include <addrspace.h>
#include <tlb.h>
#include <current.h>
#include <spl.h>
#include <cpu.h>
#include <machine/tlb.h>
#include <proc.h>

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
tlb_add(uint32_t entryhi, uint32_t entrylo)
{
        uint32_t lra;
        int spl;

        spl = splhigh();

        lra = curthread->t_cpu->c_tlb_lra;
        tlb_write(entryhi, entrylo, lra);
        curthread->t_cpu->c_tlb_lra = (lra + 1) % NUM_TLB;

        splx(spl);
}

static
void
tlb_add_readable(vaddr_t va, struct pte *pte)
{
        KASSERT(curthread != NULL);

        uint32_t entryhi, entrylo;
        cme_id_t cme_id;
        struct cme *cme;

        KASSERT(pte->pte_state == S_PRESENT);
        cme_id = pte_get_cme_id(pte);

        cm_acquire_lock(cme_id);
        cme = &coremap.cmes[cme_id];

        entryhi = VA_TO_VPAGE(va);

        switch (cme->cme_state) {
        case S_UNSWAPPED:
        case S_CLEAN:
                entrylo = CME_ID_TO_PPAGE(cme_id);
                break;
        case S_DIRTY:
                entrylo = CME_ID_TO_WRITEABLE_PPAGE(cme_id);
                break;
        case S_KERNEL:
                panic("Tried to add a kernel page to the TLB\n");
        default:
                panic("Tried to add a page that isn't in physical memory to the TLB\n");
        }

        tlb_add(entryhi, entrylo);

        cm_release_lock(cme_id);
}

static
void
tlb_add_writeable(vaddr_t va, struct pte *pte)
{
        KASSERT(curthread != NULL);

        uint32_t entryhi, entrylo;
        cme_id_t cme_id;
        struct cme *cme;

        KASSERT(pte->pte_state == S_PRESENT);
        cme_id = pte_get_cme_id(pte);

        cm_acquire_lock(cme_id);

        cme = &coremap.cmes[cme_id];
        cme->cme_state = S_DIRTY;

        entryhi = VA_TO_VPAGE(va);
        entrylo = CME_ID_TO_WRITEABLE_PPAGE(cme_id);

        tlb_add(entryhi, entrylo);

        cm_release_lock(cme_id);
}

void
tlb_set_writeable(vaddr_t va, cme_id_t cme_id, bool writeable)
{
        uint32_t entryhi, entrylo;
        struct cme *cme;
        int spl;
        int index;

        cm_acquire_lock(cme_id);
        cme = &coremap.cmes[cme_id];

        entryhi = VA_TO_VPAGE(va);

        switch (cme->cme_state) {
        case S_UNSWAPPED:
        case S_CLEAN:
                if (writeable) {
                        cme->cme_state = S_DIRTY;
                        entrylo = CME_ID_TO_WRITEABLE_PPAGE(cme_id);
                } else {
                        entrylo = CME_ID_TO_PPAGE(cme_id);
                }

                break;
        case S_DIRTY:
                entrylo = CME_ID_TO_WRITEABLE_PPAGE(cme_id);
                break;
        default:
                panic("Tried to update the TLB write status on a page that isn't in physical memory\n");
        }

        spl = splhigh();
        index = tlb_probe(entryhi, 0);

        if (index < 0) {
                panic("Tried to mark a non-existent TLB entry as dirty\n");
        }

        tlb_write(entryhi, entrylo, (uint32_t)index);

        splx(spl);
}

void
tlb_remove(vaddr_t va)
{
        int i, spl;
        uint32_t entryhi;

        /* Disable interrupts on this CPU while frobbing the TLB. */
        spl = splhigh();

        entryhi = VA_TO_VPAGE(va);
        i = tlb_probe(entryhi, 0);

        if (i < 0) {
                // The page wasn't in the TLB, so we NOOP
                splx(spl);
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

        curcpu->c_tlb_lra = 0;

        for (i = 0; i < NUM_TLB; i++) {
                tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
        }

        splx(spl);
}

/*
 * The caller function is responsible for marking as clean or dirty
 * in the pte. If ts->ts_type is T_CLEAN, then we rewrite the TLB entry
 * so as to catch the next write to the page. If ts->ts_type is TS_EVICT
 * then we flush it from the TLB.
 */
void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
        KASSERT(curproc != NULL);

        if (ts->ts_type == TS_CLEAN) {
                tlb_set_writeable(ts->ts_flushed_va, ts->ts_flushed_cme_id, false);
        } else {
                tlb_remove(ts->ts_flushed_va);
        }

        V(tlbshootdown.ts_sem);
}

/*
 * If the page is already in memory, NOOP. Otherwise, find a slot
 * in the core map and assign it to the page table entry.
 *
 * If the page was in the swap space on disk, we copy it into
 * physical memory and set its swap_id on our core map entry.
 *
 * Finally, we set the present bit to indicate the page
 * is now accessible in main memory.
 *
 * Assumes that the caller has validated the virtual address, and
 * that it holds the pte lock.
 */
static
void
ensure_in_memory(struct pte *pte, vaddr_t va)
{
        KASSERT(curproc != NULL);

        struct cme cme;
        cme_id_t slot;
        paddr_t pa;

        if (pte->pte_state == S_INVALID) {
                panic("Cannot ensure than an invalid pte is in memory\n");
        }

        if (pte->pte_state == S_PRESENT) {
                return;
        }

        slot = cm_capture_slot();
        cm_evict_page(slot);

        pa = CME_ID_TO_PA(slot);

        switch(pte->pte_state) {
        case S_LAZY:
                // Actually free memory for the page for the first time
                cme = cme_create(curproc->p_pid, va, S_UNSWAPPED);

                // Zero out the memory on the newly allocated page
                memset((void *)PADDR_TO_KVADDR(pa), 0, PAGE_SIZE);

                break;
        case S_SWAPPED:
                cme = cme_create(curproc->p_pid, va, S_CLEAN);
                cme.cme_swap_id = pte_get_swap_id(pte);

                swap_in(cme.cme_swap_id, slot);
                break;
        }

        coremap.cmes[slot] = cme;
        cm_release_lock(slot);

        pte->pte_state = S_PRESENT;
        pte_set_cme_id(pte, slot);
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
        int EFAULT = 1;

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

        pte = pagetable_get_pte_from_va(as->as_pt, faultaddress);
        if (pte == NULL) {
                return EFAULT;
        }

        pt_acquire_lock(as->as_pt, pte);

        ensure_in_memory(pte, faultaddress);

        switch (faulttype) {
        case VM_FAULT_READ:
                tlb_add_readable(faultaddress, pte);
        case VM_FAULT_WRITE:
                tlb_add_writeable(faultaddress, pte);
                break;
        case VM_FAULT_READONLY:
                tlb_set_writeable(faultaddress, pte_get_cme_id(pte), true);
                break;
        default:
                panic("Unknown TLB fault type\n");
                break;
        }

        pt_release_lock(as->as_pt, pte);

        return 0;
}
