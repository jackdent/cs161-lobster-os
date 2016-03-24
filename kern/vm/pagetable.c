#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <spinlock.h>
#include <pagetable.h>

struct pagetable*
create_pagetable(void)
{
	pagetable *pt;

	pt = kmalloc(sizeof(struct pagetable));
	if (pt == NULL):
		return NULL;

	spinlock_init(&pt->busy_bit_splk);

	// ensure all l2_pt pointers are NULL initially
	memset(&pt->l1_pt, 0, sizeof(pt->l1_pt));

	return pt;
}


void
destroy_pagetable(struct pagetable *pt)
{
	KASSERT(pt != NULL);

	int i, j;
	struct l2_pt *l2_pt;

	// walk through all entries and free them
	for (i = 0; i < PAGE_TABLE_ENTRIES; i++) {
		if (pt->l1_pt[i] == NULL)
			continue;

		l2_pt = pt->l1_pt[i]
		for (j = 0; j < PAGE_TABLE_ENTRIES; j++) {
			if (l2_pt[j] != 0) {
				//FREE_PAGE(l2_pt[j].phys_page) // TODO: fill in with actual function
			}
		}
		kfree(l2_pt);
	}
	spinlock_cleanup(&pt->busy_bit_splk);
	kfree(pt);
}

int
map_pa_to_va(paddr_t pa, vaddr_t va, struct pagetable *pt)
{
	KASSERT(va > KUSEG_START);
	KASSERT(pt != NULL);

	unsigned l1_index, l2_index;
	struct l2_pt l2_pt;

	l1_index = L1_PT_MASK(va);
	l2_index = L2_PT_MASK(va);

	// Already have an l2 pagetable
	if (pt->l1_pt[l1_index] != NULL) {
		l2_pt = pt->l1_pt[l1_index];
		// Cannot be already taken
		KASSERT(l2_pt[l2_index] == 0)
		l2_pt[l2_index].phys_page = PHYS_PAGE_MASK(pa);
		l2_pt[l2_index].valid = 1;
		l2_pt[l2_index].present = 1;
		l2_pt[l2_index].busy_bit = 0;
	}
	// Need to make an l2 pagetable
	else {
		l2_pt = kmalloc(sizeof(l2_pt));
		if (l2_pt == NULL)
			return = ENOMEM;
		pt->l1_pt[l1_index] = l2_pt;
		l2_pt[l2_index].phys_page = PHYS_PAGE_MASK(pa);
		l2_pt[l2_index].valid = 1;
		l2_pt[l2_index].present = 1;
		l2_pt[l2_index].busy_bit = 0;
	}
	return 0;
}

void
unmap_va(vaddr_t va, struct pagetable *pt)
{
	KASSERT(va > KUSEG_START);
	KASSERT(pt != NULL);

	unsigned l1_index, l2_index;
	struct l2_pt l2_pt;

	l1_index = L1_PT_MASK(va);
	l2_index = L2_PT_MASK(va);

	KASSERT(pt->l1_pt[l1_index] != NULL);

	l2_pt = pt->l1_pt[l1_index];

	KASSERT(l2_pt[l2_index] != 0);
	// 0 out entire entry
	spinlock_acquire(&pt->busy_bit_splk);
	l2_pt[l2_index] = 0;
	spinlock_release(&pt->busy_bit_splk);
}


void
acquire_busy_bit(struct pte *pte, struct pagetable *pt)
{
	KASSERT(pt);
	KASSERT(pte);
	spinlock_acquire(&pt->busy_bit_splk);
	pte->busy_bit = 1;
	spinlock_release(&pt->busy_bit_splk);
}
void
release_busy_bit(struct pte *pte, struct pagetable *pt)
{
	KASSERT(pt);
	KASSERT(pte);
	spinlock_acquire(&pt->busy_bit_splk);
	pte->busy_bit = 0;
	spinlock_release(&pt->busy_bit_splk);
}

