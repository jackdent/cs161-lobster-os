#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <spinlock.h>

struct pagetable*
create_pagetable(void)
{
	struct pagetable *pt;

	pt = kmalloc(sizeof(struct pagetable));
	if (pt == NULL) {
		return NULL;
	}

	spinlock_init(&pt->pt_busy_bit_splk);

	// ensure all l2_pt pointers are NULL initially
	memset(&pt->pt_l1, 0, sizeof(pt->pt_l1));

	return pt;
}


void
destroy_pagetable(struct pagetable *pt)
{
	KASSERT(pt != NULL);

	int i, j;
	struct l2 *l2;

	// walk through all entries and free them
	for (i = 0; i < PAGE_TABLE_ENTRIES; i++) {
		if (pt->pt_l1.l2s[i] == NULL) {
			continue;
		}

		l2 = pt->pt_l1.l2s[i];
		for (j = 0; j < PAGE_TABLE_ENTRIES; j++) {
			if (l2->l2_ptes[j].pte_valid != 0) {
				//FREE_PAGE(l2[j].phys_page) // TODO: fill in with actual function
			}
		}
		kfree(l2);
	}

	spinlock_cleanup(&pt->pt_busy_bit_splk);
	kfree(pt);
}

bool
va_in_pagetable(vaddr_t va, struct pagetable *pt, struct pte **pte)
{
	unsigned int l1_index, l2_index;
	struct l2 *l2;

	l1_index = L1_PT_MASK(va);
	l2_index = L2_PT_MASK(va);

	l2 = pt->pt_l1.l2s[l1_index];
	if (l2 == NULL) {
		return false;
	}

	*pte = l2->l2_ptes[l2_index];
	if (pte.pte_valid == 0) {
		return false;
	}

	return true;
}

int
map_pa_to_va(paddr_t pa, vaddr_t va, struct pagetable *pt)
{
	KASSERT(va > KUSEG_START);
	KASSERT(pt != NULL);

	unsigned l1_index, l2_index;
	struct l2 *l2;

	l1_index = L1_PT_MASK(va);
	l2_index = L2_PT_MASK(va);

	// Already have an l2 pagetable
	if (pt->pt_l1.l2s[l1_index] != NULL) {
		l2 = pt->pt_l1.l2s[l1_index];
		// Cannot be already taken
		KASSERT(l2->l2_ptes[l2_index].pte_valid == 0);
		l2->l2_ptes[l2_index].pte_phys_page = PA_TO_PHYS_PAGE(pa);
		l2->l2_ptes[l2_index].pte_valid = 1;
		l2->l2_ptes[l2_index].pte_present = 1;
		l2->l2_ptes[l2_index].pte_busy_bit = 0;
		l2->l2_ptes[l2_index].pte_swap_bits = 0;
	}
	// Need to make an l2 pagetable
	else {
		l2 = kmalloc(sizeof(l2));
		if (l2 == NULL)
			return ENOMEM;
		pt->pt_l1.l2s[l1_index] = l2;
		l2->l2_ptes[l2_index].pte_phys_page = PA_TO_PHYS_PAGE(pa);
		l2->l2_ptes[l2_index].pte_valid = 1;
		l2->l2_ptes[l2_index].pte_present = 1;
		l2->l2_ptes[l2_index].pte_busy_bit = 0;
		l2->l2_ptes[l2_index].pte_swap_bits = 0;
	}

	return 0;
}

void
unmap_va(vaddr_t va, struct pagetable *pt)
{
	KASSERT(va > KUSEG_START);
	KASSERT(pt != NULL);

	unsigned int l1_index, l2_index;
	struct l2 *l2;

	l1_index = L1_PT_MASK(va);
	l2_index = L2_PT_MASK(va);

	KASSERT(pt->pt_l1.l2s[l1_index] != NULL);
	l2 = pt->pt_l1.l2s[l1_index];
	KASSERT(l2->l2_ptes[l2_index].pte_valid != 0);

	// 0 out entire entry
	spinlock_acquire(&pt->pt_busy_bit_splk);
	l2->l2_ptes[l2_index].pte_valid = 0;
	spinlock_release(&pt->pt_busy_bit_splk);
}

paddr_t
va_to_pa(vaddr_t va, struct pte *pte)
{
        return PHYS_PAGE_TO_PI(pte->pte_phys_page) & OFFSET_MASK(va);
}

struct pte *
get_pte_from_cme(struct pagetable *pt, struct cme cme)
{
	struct l2 *l2;

	l2 = pt->pt_l1.l2s[cme.cme_l1_offset];
	if (l2 == NULL) {
		return NULL;
	}

	return l2->l2_ptes[cme.cme_l2_offset];
}

swap_id_t
pte_get_swap_id(struct pte *pte)
{
	unsigned int upper, lower;

	KASSERT(pte);
	upper = pte->pte_phys_page;
	lower = pte->pte_swap_bits;

	return (upper << LOWER_SWAP_BITS) | lower;
}

void
pte_set_swap_id(struct pte *pte, swap_id_t swap_id)
{
	pte->pte_phys_page = SWAP_PHYS_PAGE_MASK(swap_id);
	pte->pte_swap_bits = SWAP_BITS_MASK(swap_id);
}

void
acquire_busy_bit(struct pte *pte, struct pagetable *pt)
{
	KASSERT(pt);
	KASSERT(pte);

	spinlock_acquire(&pt->pt_busy_bit_splk);
	KASSERT(pte->pte_busy_bit == 0);
	pte->pte_busy_bit = 1;
	spinlock_release(&pt->pt_busy_bit_splk);
}
void
release_busy_bit(struct pte *pte, struct pagetable *pt)
{
	KASSERT(pt);
	KASSERT(pte);

	spinlock_acquire(&pt->pt_busy_bit_splk);
	KASSERT(pte->pte_busy_bit == 1);
	pte->pte_busy_bit = 0;
	spinlock_release(&pt->pt_busy_bit_splk);
}
