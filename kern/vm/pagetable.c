#include <types.h>
#include <kern/errno.h>
#include <pagetable.h>
#include <vm.h>
#include <lib.h>
#include <proc.h>
#include <spinlock.h>
#include <current.h>


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
pagetable_destroy(struct pagetable *pt, struct addrspace *as)
{
	KASSERT(pt != NULL);

	int i, j;
	struct l2 *l2;
	struct pte *pte;

	KASSERT(as != NULL);

	// Walk through all entries and free them
	for (i = 0; i < PAGE_TABLE_SIZE; i++) {
		l2 = pt->pt_l1.l2s[i];
		if (l2 == NULL) {
			continue;
		}

		for (j = 0; j < PAGE_TABLE_SIZE; j++) {
			pte = &l2->l2_ptes[j];
			if (pte->pte_state != S_INVALID) {
				free_upage(L1_L2_TO_VA(i, j), as);
			}
		}

		kfree(l2);
	}

	spinlock_cleanup(&pt->pt_busy_spinlock);
	kfree(pt);
}

struct pte *
pagetable_get_pte_from_offsets(struct pagetable *pt, unsigned int l1_offset, unsigned int l2_offset)
{
	struct l2 *l2;
	struct pte *pte;

	l2 = pt->pt_l1.l2s[l1_offset];
	if (l2 == NULL) {
		return NULL;
	}

	pte = &l2->l2_ptes[l2_offset];

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
struct l2 *
pagetable_create_l2(struct l1 *l1, unsigned int offset)
{
	KASSERT(l1->l2s[offset] == NULL);

	struct l2 *l2;

	l2 = kmalloc(sizeof(struct l2));
	if (l2 == NULL) {
		return NULL;
	}

	l1->l2s[offset] = l2;

	// Set all l2 pte's to 0 initially
	memset((void*)l2, 0, sizeof(struct l2));

	return l2;
}

struct pte *
pagetable_create_pte_from_va(struct pagetable *pt, vaddr_t va)
{
	struct l1 *l1;
	struct l2 *l2;
	unsigned int l1_offset, l2_offset;
	struct pte *pte;

	l1_offset = L1_PT_MASK(va);
	l2_offset = L2_PT_MASK(va);

	l1 = &pt->pt_l1;
	l2 = l1->l2s[l1_offset];

	if (l2 == NULL) {
		l2 = pagetable_create_l2(l1, l1_offset);
		if (l2 == NULL) {
			panic("Could not create l2 pagetable\n");
		}
	}

	pte = &l2->l2_ptes[l2_offset];

	return pte;
}

static
swap_id_t
pagetable_assign_swap_slot_to_pte(struct pte *pte)
{
	swap_id_t slot;

	slot = swap_capture_slot();
	pte_set_swap_id(pte, slot);

	return slot;
}

int
pagetable_clone(struct pagetable *old_pt, struct pagetable *new_pt)
{
	struct l1 *old_l1, *new_l1;
	struct l2 *old_l2, *new_l2;
	struct pte *old_pte, *new_pte;
	swap_id_t old_slot, new_slot;
	cme_id_t old_cme_id;
	int i, j;

	old_l1 = &old_pt->pt_l1;
	new_l1 = &new_pt->pt_l1;

	// Copy over the pages
	for (i = 0; i < PAGE_TABLE_SIZE; i++) {
		if (old_l1->l2s[i] == NULL) {
			continue;
		}

		old_l2 = old_l1->l2s[i];
		new_l2 = pagetable_create_l2(new_l1, i);

		if (new_l2 == NULL) {
			return ENOMEM; // caller will handle cleanup
		}

		for (j = 0; j < PAGE_TABLE_SIZE; j++) {
			old_pte = &old_l2->l2_ptes[j];
			new_pte = &new_l2->l2_ptes[j];

			// So old_pte doesn't get evicted
			pt_acquire_lock(old_pt, old_pte);

			*new_pte = *old_pte;

			switch (old_pte->pte_state) {
			case S_INVALID:
			case S_LAZY:
				break;
			case S_PRESENT:
				new_slot = pagetable_assign_swap_slot_to_pte(new_pte);
				old_cme_id = PA_TO_CME_ID(pte_get_phys_page(old_pte));
				swap_out(new_slot, old_cme_id);
				new_pte->pte_state = S_SWAPPED;
				break;
			case S_SWAPPED:
				new_slot = pagetable_assign_swap_slot_to_pte(new_pte);
				old_slot = pte_get_swap_id(old_pte);
				swap_copy(old_slot, new_slot);
				break;
			}

			pt_release_lock(new_pt, new_pte);
			pt_release_lock(old_pt, old_pte);
		}
	}

	return 0;
}

bool
pt_attempt_lock(struct pagetable *pt, struct pte *pte)
{
	KASSERT(pt != NULL);
	KASSERT(pte != NULL);

	bool acquired;

	spinlock_acquire(&pt->pt_busy_spinlock);
	acquired = (pte->pte_busy == 0);
	if (acquired) {
		pte->pte_busy = 1;
	}
	spinlock_release(&pt->pt_busy_spinlock);

	return acquired;
}

void
pt_acquire_lock(struct pagetable *pt, struct pte *pte)
{
	while (1) {
		if (pt_attempt_lock(pt, pte)) {
			break;
		}
	}
}

void
pt_release_lock(struct pagetable *pt, struct pte *pte)
{
	KASSERT(pt != NULL);
	KASSERT(pte != NULL);

	spinlock_acquire(&pt->pt_busy_spinlock);
	KASSERT(pte->pte_busy == 1);
	pte->pte_busy = 0;
	spinlock_release(&pt->pt_busy_spinlock);
}
