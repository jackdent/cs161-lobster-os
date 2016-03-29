#include <pagetable.h>
#include <lib.h>
#include <spinlock.h>


paddr_t
pte_va_to_pa(struct pte *pte, vaddr_t va)
{
	return PHYS_PAGE_TO_PA(pte->pte_phys_page) & OFFSET_MASK(va);
}

swap_id_t
pte_get_swap_id(struct pte *pte)
{
	unsigned int upper, lower;

	KASSERT(pte);
	upper = pte->pte_phys_page;
	lower = pte->pte_swap_tail;

	return (upper << LOWER_SWAP_BITS) | lower;
}

void
pte_set_swap_id(struct pte *pte, swap_id_t swap_id)
{
	pte->pte_phys_page = SWAP_PHYS_PAGE_MASK(swap_id);
	pte->pte_swap_tail = SWAP_BITS_MASK(swap_id);
}

bool
pte_attempt_lock(struct pte *pte, struct pagetable *pt)
{
	KASSERT(pt != NULL);
	KASSERT(pte != NULL);

	bool acquired;

	spinlock_acquire(&pt->pt_busy_spinlock);
	acquired = (pte->pte_busy == 0);
	pte->pte_busy = 1;
	spinlock_release(&pt->pt_busy_spinlock);

	return acquired;
}

void
pte_acquire_lock(struct pte *pte, struct pagetable *pt)
{
	while (1) {
		if (pte_attempt_lock(pte, pt)) {
			break;
		}
	}
}

void
pte_release_lock(struct pte *pte, struct pagetable *pt)
{
	KASSERT(pt != NULL);
	KASSERT(pte != NULL);

	spinlock_acquire(&pt->pt_busy_spinlock);
	KASSERT(pte->pte_busy == 1);
	pte->pte_busy = 0;
	spinlock_release(&pt->pt_busy_spinlock);
}
