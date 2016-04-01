#include <cme.h>
#include <pte.h>
#include <lib.h>
#include <spinlock.h>

paddr_t
pte_get_pa(struct pte *pte, vaddr_t va)
{
	KASSERT(pte);
	KASSERT(pte->pte_state == S_PRESENT);

	return PHYS_PAGE_TO_PA(pte->pte_phys_page) & OFFSET_MASK(va);
}

cme_id_t
pte_get_cme_id(struct pte *pte)
{
	KASSERT(pte);
	KASSERT(pte->pte_state == S_PRESENT);

	return (cme_id_t)pte->pte_phys_page;
}

void
pte_set_cme_id(struct pte *pte, cme_id_t cme_id)
{
	KASSERT(pte);

	pte->pte_phys_page = cme_id;
}

swap_id_t
pte_get_swap_id(struct pte *pte)
{
	unsigned int upper, lower;

	KASSERT(pte);

	upper = pte->pte_phys_page;
	lower = pte->pte_swap_tail;

	return (swap_id_t)SWAP_ID(upper, lower);
}

void
pte_set_swap_id(struct pte *pte, swap_id_t swap_id)
{
	KASSERT(pte);

	pte->pte_phys_page = SWAP_PHYS_PAGE_MASK(swap_id);
	pte->pte_swap_tail = SWAP_BITS_MASK(swap_id);
}
