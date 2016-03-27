#include <types.h>

// Find num free pages in memory, evicting if needed,
// and return with all of their busy bits
// held so that they are not evicted before the caller needs them
// for_kernel is true if this memory is for the kernel via kmalloc.
// We enforce that a kernel allocation live in contiguous memory if
// it occupies more than 1 page to make kfree()'ing easier.
// num should always be 1 if for_kernel == false. On success, result
// will store the address of the first page and 0 will be returned.
// Otherwise, the return value denotes the error
int find_free_pages(paddr_t *result, bool for_kernel, int num)
{
	KASSERT(for_kernel || num == 1);
}

/*
 * Capture a slot in the core map. If the core map entry is free,
 * NOOP. Otherwise, update the present field on the page table
 * entry to indicate that it is no longer present in main memory.
 *
 * We write the page to disk if it is dirty, or if it has never
 * left main memory before. In latter case, we find a free swap
 * slot and set its index on the page table entry.
 */
cme_id_t
add_page_to_coremap(struct addrspace *as, struct pte *pte)
{
        struct cme cme;
        cme_id_t slot;
        swap_id_t swap;

        slot = cm_capture_slot();

        cme = coremap.cmes[slot];
        if (cme.free == 1) {
                return slot;
        }

        pte->pte_present = 0;

        if (cme.cme_swap_id != pte.pte_phys_page) {
                // If this is the first time we're writing the page
                // out to disk, we grab a free swap entry, and assign
                // its index to the page table entry. The swap id will
                // be stable for this page for the remainder of its
                // lifetime.

                // A page that hasn't been written to disk can never be
                // dirty, free, or owned by the kernel

                // TODO: is this right?
                KASSERT(cme.cme_state == S_CLEAN);

                swap = get_free_swap_index();
                pte_set_swap_id(pte, swap);
        } else if (cme.cme_state == S_DIRTY) {
                swap = cme.cme_swap_id;
        } else {
                // The page isn't dirty, so we NOOP.
        }

        swap_out(swap, CME_ID_TO_PA(slot));

        return slot;
}

/*
 * If the page is already in memory, NOOP. Otherwise, find a slot
 * in the core map and assign it to the page table entry.
 *
 * If the lazy bit is set on the page, we're done. Otherwise,
 * the page was in the swap space on disk, so we copy
 * it into physical memory and set its swap_id on our core map
 * entry.

 * Finally, we set the present bit to indicate the page
 * is now accessible in main memory.
 */
void
ensure_in_memory(struct addrspace *as, vaddr_t va, struct pte *pte)
{
        struct cme cme;

        if (pte->pte_present == 1) {
                return;
        }

        slot = add_page_to_coremap(as, pte);

        KASSERT(curproc != NULL);
        cme = create_cme(curproc->p_pid, va);

        if (pte->pte_lazy == 1) {
                pte->pte_lazy = 0;
        } else {
                // The page was on disk
                swap_in(cme.swap_id, CME_ID_TO_PA(slot));
                cme.swap_id = pte_get_swap_id(pte);
        }

        pte->pte_present = 1;
        coremap.cmes[slot] = cme;
        cme_release_lock(slot);
}
