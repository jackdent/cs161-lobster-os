#include <types.h>

/*
 * Chooses a page to evict from main memory and writes it to
 * the swap space. Returns the core map index of the evicted page.
 *
 * Assumes the caller holds a lock on the page table entry.
 */
cme_id_t add_page_to_coremap(struct addrspace *as, cme_id_t cme_id);

/*
 * Assumes that the caller has validated the virtual address.
 */
void ensure_in_memory(struct addrspace *as, vaddr_t va, struct pte *pte);

/*
 * Assumes that the caller has validated the virtual address.
 */
void make_va_writeable(struct addrspace *as, struct pte *pte);
