#include <cme.h>
#include <pte.h>
#include <lib.h>
#include <spinlock.h>

paddr_t
pte_get_pa(struct pte *pte)
{
	KASSERT(pte);
	KASSERT(pte->pte_state == S_PRESENT);

	return PHYS_PAGE_TO_PA(pte->pte_phys_page);
}

void
pte_set_pa(struct pte *pte, paddr_t pa)
{
	KASSERT(pte);

	pte->pte_phys_page = PA_TO_PHYS_PAGE(pa);
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
	pte->pte_swap_tail = SWAP_TAIL_MASK(swap_id);
}
