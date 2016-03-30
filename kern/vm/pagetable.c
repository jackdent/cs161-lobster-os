#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <proc.h>
#include <spinlock.h>
#include <swap.h>

struct pagetable *
pagetable_create(void)
{
	struct pagetable *pt;

	pt = kmalloc(sizeof(struct pagetable));
	if (pt == NULL) {
		return NULL;
	}

	spinlock_init(&pt->pt_busy_spinlock);

	// Ensure all l2_pt pointers are NULL initially
	memset(&pt->pt_l1, 0, sizeof(pt->pt_l1));

	return pt;
}

void
pagetable_destroy(struct pagetable *pt)
{
	KASSERT(pt != NULL);

	int i, j;
	struct l2 *l2;

	// Walk through all entries and free them
	for (i = 0; i < PAGE_TABLE_SIZE; i++) {
		if (pt->pt_l1.l2s[i] == NULL) {
			continue;
		}

		l2 = pt->pt_l1.l2s[i];
		for (j = 0; j < PAGE_TABLE_SIZE; j++) {
			free_upage(&l2->l2_ptes[j], L1_L2_TO_VA(i, j));
		}

		kfree(l2);
	}

	spinlock_cleanup(&pt->pt_busy_spinlock);
	kfree(pt);
}

struct pte *
pagetable_get_pte_from_offsets(struct pagetable *pt, unsigned int l1_offset, unsigned l2_offset)
{
	struct l2 *l2;
	struct pte *pte;

	l2 = pt->pt_l1.l2s[l1_offset];
	if (l2 == NULL) {
		return NULL;
	}

	pte = &l2->l2_ptes[l2_offset];
	if (pte->pte_phys_page == 0) {
		return NULL;
	}
	return pte;
}

struct pte *
pagetable_get_pte_from_va(struct pagetable *pt, vaddr_t va)
{
	return pagetable_get_pte_from_offsets(pt, L1_PT_MASK(va), L2_PT_MASK(va));
}

struct pte *
pagetable_get_pte_from_cme(struct pagetable *pt, struct cme *cme)
{
	return pagetable_get_pte_from_offsets(pt, cme->cme_l1_offset, cme->cme_l2_offset);
}

static
swap_id_t
pagetable_clone_pte(struct pte *old_pte, struct pte *new_pte)
{
	swap_id_t new_slot;

	pte_get_swap_id(old_pte);
	new_slot = swap_capture_slot();

	pte_set_swap_id(new_pte, new_slot);

	return new_slot;
}

int
pagetable_clone(struct pagetable *old_pt, struct pagetable *new_pt)
{
	struct l1 *old_l1, *new_l1;
	struct l2 *old_l2, *new_l2;
	struct pte *old_pte, *new_pte;
	swap_id_t old_slot, new_slot;
	int i, j;

	old_l1 = &old_pt->pt_l1;
	new_l1 = &new_pt->pt_l1;

	// Copy over the pages
	for (i = 0; i < PAGE_TABLE_SIZE; ++i) {
		if (old_l1->l2s[i] == NULL) {
			continue;
		}

		new_l1->l2s[i] = kmalloc(sizeof(struct l2));
		if (new_l1->l2s[i] == NULL) {
			return ENOMEM; // caller will handle cleanup
		}

		old_l2 = old_l1->l2s[i];
		new_l2 = new_l1->l2s[i];

		for (j = 0; j < PAGE_TABLE_SIZE; j++) {
			old_pte = &old_l2->l2_ptes[j];
			new_pte = &new_l2->l2_ptes[j];

			*new_pte = *old_pte;

			// So old_pte doesn't get evicted
			pte_acquire_lock(old_pte, old_pt);

			switch (old_pte->pte_state) {
			case S_INVALID:
			case S_LAZY:
				break;
			case S_PRESENT:
				new_slot = pagetable_clone_pte(old_pte, new_pte);
				swap_out(new_slot, PHYS_PAGE_TO_PA(new_pte->pte_phys_page));
				new_pte->pte_state = S_SWAPPED;
				break;
			case S_SWAPPED:
				old_slot = old_pte->pte_phys_page;
				new_slot = pagetable_clone_pte(old_pte, new_pte);
				swap_copy(old_slot, new_slot);
				break;
			}
			pte_release_lock(old_pte, old_pt);
		}
	}
	return 0;
}
