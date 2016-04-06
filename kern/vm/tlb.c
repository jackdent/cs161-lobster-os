#include <pagetable.h>
#include <coremap.h>
#include <addrspace.h>
#include <tlb.h>
#include <current.h>
#include <spl.h>
#include <cpu.h>
#include <machine/tlb.h>
#include <proc.h>
#include <kern/errno.h>

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

/*
 * Assumes that the caller holds the core map entry lock.
 */
static
void
tlb_add_readable(vaddr_t va, struct pte *pte, cme_id_t cme_id)
{
        KASSERT(curthread != NULL);
        KASSERT(pte->pte_state == S_PRESENT);

        uint32_t entryhi, entrylo;
        struct cme *cme;

        cme = &coremap.cmes[cme_id];

        entryhi = VA_TO_TLBHI(va);

        switch (cme->cme_state) {
        case S_CLEAN:
                entrylo = CME_ID_TO_RONLY_TLBLO(cme_id);
                break;
        case S_UNSWAPPED:
        case S_DIRTY:
                entrylo = CME_ID_TO_WRITEABLE_TLBLO(cme_id);
                break;
        case S_KERNEL:
                panic("Tried to add a kernel page to the TLB\n");
        default:
                panic("Tried to add a page that isn't in physical memory to the TLB\n");
        }

        tlb_add(entryhi, entrylo);
}

/*
 * Assumes that the caller holds the core map entry lock.
 */
static
void
tlb_add_writeable(vaddr_t va, struct pte *pte, cme_id_t cme_id)
{
        KASSERT(curthread != NULL);
        KASSERT(pte->pte_state == S_PRESENT);

        uint32_t entryhi, entrylo;
        struct cme *cme;

        cme = &coremap.cmes[cme_id];

        if (cme->cme_state == S_CLEAN) {
            cme->cme_state = S_DIRTY;
        }

        entryhi = VA_TO_TLBHI(va);
        entrylo = CME_ID_TO_WRITEABLE_TLBLO(cme_id);

        tlb_add(entryhi, entrylo);
}

void
tlb_set_writeable(vaddr_t va, cme_id_t cme_id, bool writeable)
{
        uint32_t entryhi, entrylo;
        struct cme *cme;
        int spl;
        int index;

        cme = &coremap.cmes[cme_id];

        entryhi = VA_TO_TLBHI(va);

        switch (cme->cme_state) {
        case S_CLEAN:
                if (writeable) {
                        entrylo = CME_ID_TO_WRITEABLE_TLBLO(cme_id);
                        cme->cme_state = S_DIRTY;
                } else {
                        entrylo = CME_ID_TO_RONLY_TLBLO(cme_id);
                }

                break;
        case S_UNSWAPPED:
        case S_DIRTY:
                entrylo = CME_ID_TO_WRITEABLE_TLBLO(cme_id);
                break;
        default:
                panic("Tried to update the TLB write status on a page that isn't in physical memory\n");
        }

        spl = splhigh();
        index = tlb_probe(entryhi, 0);

        if (index < 0) {
                // In case we get a tlb shootdown removing the entry before we
                // get a chance to update it
                tlb_add(entryhi, entrylo);
        } else {
                tlb_write(entryhi, entrylo, (uint32_t)index);
        }

        splx(spl);
}

void
tlb_remove(vaddr_t va)
{
        int i, spl;
        uint32_t entryhi;

        /* Disable interrupts on this CPU while frobbing the TLB. */
        spl = splhigh();

        entryhi = VA_TO_TLBHI(va);
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
cme_id_t
ensure_in_memory(struct pte *pte, vaddr_t va)
{
        KASSERT(curproc != NULL);

        struct cme cme;
        cme_id_t slot;
        paddr_t pa;
        struct addrspace *as;

        if (pte->pte_state == S_INVALID) {
                panic("Cannot ensure than an invalid pte is in memory\n");
        }

        if (pte->pte_state == S_PRESENT) {
                slot = PA_TO_CME_ID(pte_get_pa(pte));
                cm_acquire_lock(slot);
                return slot;
        }

        slot = cm_capture_slot();
        cm_evict_page(slot);

        as = curproc->p_addrspace;
        pa = CME_ID_TO_PA(slot);

        switch(pte->pte_state) {
        case S_LAZY:
                // Actually free memory for the page for the first time
                cme = cme_create(as, va, S_UNSWAPPED);

                // Zero out the memory on the newly allocated page
                memset((void *)PADDR_TO_KVADDR(pa), 0, PAGE_SIZE);

                coremap.cmes[slot] = cme;
                break;
        case S_SWAPPED:
                cme = cme_create(as, va, S_CLEAN);
                cme.cme_swap_id = pte_get_swap_id(pte);

                swap_in(cme.cme_swap_id, slot);

                coremap.cmes[slot] = cme;
                break;
        }

        pte->pte_state = S_PRESENT;
        pte_set_pa(pte, pa);

        return slot;
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
        cme_id_t cme_id;
        paddr_t pa;

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
        if (pte == NULL || pte->pte_state == S_INVALID) {
                return EFAULT;
        }

        pt_acquire_lock(as->as_pt, pte);

        cme_id = ensure_in_memory(pte, faultaddress);

        switch (faulttype) {
        case VM_FAULT_READ:
                tlb_add_readable(faultaddress, pte, cme_id);
                break;
        case VM_FAULT_WRITE:
                tlb_add_writeable(faultaddress, pte, cme_id);
                break;
        case VM_FAULT_READONLY:
                pa = pte_get_pa(pte);
                KASSERT(PA_TO_CME_ID(pa) == cme_id);

                tlb_set_writeable(faultaddress, cme_id, true);
                break;
        default:
                panic("Unknown TLB fault type\n");
                break;
        }

        cm_release_lock(cme_id);
        pt_release_lock(as->as_pt, pte);

        return 0;
}
