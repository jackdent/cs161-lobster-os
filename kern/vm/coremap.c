#include <types.h>
#include <lib.h>
#include <coremap.h>
#include <machine/vm.h>
#include <swap.h>
#include <proctable.h>
#include <addrspace.h>

// Forward declaration, implemented in vm/tlb.c
void tlb_remove(vaddr_t va);

void
cm_init()
{
	unsigned int i, ncoremap_bytes, ncoremap_pages, ncmes;
	paddr_t ram_size, start;

	ram_size = ram_getsize();
	ncmes = (ram_size / PAGE_SIZE);
	ncoremap_bytes = ncmes * sizeof(struct cme);
	ncoremap_pages = ROUNDUP(ncoremap_bytes, PAGE_SIZE);

	start = ram_stealmem(ncoremap_pages);
	if (start == 0) {
		panic("Could not allocate coremap\n");
	}

	// The cmes are now alloc'd
	coremap.cmes = (struct cme*)PADDR_TO_KVADDR(start);

	coremap.cm_size = ncmes;
	spinlock_init(&coremap.cm_busy_spinlock);
	spinlock_init(&coremap.cm_clock_spinlock);
	coremap.cm_clock_hand = 0;

	memset(coremap.cmes, 0, ncmes * sizeof(struct cme));

	// Set the coremap as owned by the kernel
	for (i = 0; i < ncmes; i++) {
		coremap.cmes[i] = cme_create(0, PHYS_PAGE_TO_PA(i), S_KERNEL);
	        coremap.cmes[i].cme_busy = 0;
	}
}

static
void
cm_advance_clock_hand()
{
	coremap.cm_clock_hand = (coremap.cm_clock_hand + 1) % coremap.cm_size;
}

cme_id_t
cm_capture_slot()
{
	unsigned int i;
	cme_id_t slot;
	struct cme entry;

	spinlock_acquire(&coremap.cm_clock_spinlock);

	for (i = 0; i < coremap.cm_size; i++) {
		slot = coremap.cm_clock_hand;
		entry = coremap.cmes[slot];

		cm_advance_clock_hand();

		if (!cm_attempt_lock(slot)) {
			continue;
		}

		coremap.cmes[slot].cme_recent = 0;

		if (entry.cme_state == S_FREE || (entry.cme_recent == 0 && entry.cme_pid != 0)) {
			spinlock_release(&coremap.cm_clock_spinlock);
			return slot;
		}

		cm_release_lock(slot);
	}

	// If we reach the end of the loop without returning, we
	// should evict the entry the clock hand first pointed to
	slot = coremap.cm_clock_hand;

	cm_acquire_lock(slot);
	coremap.cmes[slot].cme_recent = 0;
	cm_advance_clock_hand();

	spinlock_release(&coremap.cm_clock_spinlock);
	return slot;
}

cme_id_t
cm_capture_slots_for_kernel(unsigned int nslots)
{
	cme_id_t i, j;

	spinlock_acquire(&coremap.cm_clock_spinlock);

	i = MIPS_KSEG0 / PAGE_SIZE;

	while (i < coremap.cm_size - nslots) {
		for (j = 0; j < nslots; j++) {
			if (coremap.cmes[i+j].cme_state == S_KERNEL) {
				break;
			}
		}

		if (j == nslots) {
			cm_acquire_locks(i, i + nslots);
			spinlock_release(&coremap.cm_clock_spinlock);
			return i;
		} else {
			i += j + 1;
		}

	}

	panic("Could not capture contiguous slots for kernel allocation\n");
	return 0;
}

/*
 * If the core map entry is free, NOOP. Otherwise, write the page to
 * disk if it is dirty, or if it has never left main memory before.
 * In the latter case, we find a free swap slot and set its index on
 * the page table entry. Finally, we update the page table entry to
 * indicate that it is no longer present in main memory.
 */
void
evict_page(cme_id_t cme_id)
{
        struct addrspace *as;
        struct pte *pte;
        struct proc *proc;
        struct tlbshootdown shootdown;
        struct cme *cme;
        swap_id_t swap;

	cme = &coremap.cmes[cme_id];

	if (cme->cme_state == S_FREE) {
		return;
	}

        proc = proc_table.pt_table[cme->cme_pid];
        as = proc->p_addrspace;

	tlb_remove(OFFSETS_TO_VA(cme->cme_l1_offset, cme->cme_l2_offset));

        // TODO: Shootdown the process if S_FREE or S_DIRTY
        // Make sure memory isn't freed until all complete
        shootdown.ts_flushed_page = cme_id;
        (void)shootdown;
        // ipi_tlbshootdown(proc, &shootdown);

	pte = pagetable_get_pte_from_cme(as->as_pt, cme);
	pt_acquire_lock(as->as_pt, pte);

	if (pte->pte_state == S_INVALID) {
		panic("Trying to evict an invalid page?!");
	}

	pte->pte_state = S_SWAPPED;

	switch(cme->cme_state) {
	case S_CLEAN:
		// If this is the first time we're writing the page
		// out to disk, we grab a free swap entry, and assign
		// its index to the page table entry. The swap id will
		// be stable for this page for the remainder of its
		// lifetime.
		if (cme->cme_swap_id != pte_get_swap_id(pte)) {
			swap = swap_capture_slot();
			pte_set_swap_id(pte, swap);
		}
		else {
			return;
		}

		break;
	case S_DIRTY:
		swap = cme->cme_swap_id;

		if (swap != pte_get_swap_id(pte)) {
			panic("Unstable swap id on a dirty page!\n");
		}

		break;
	default:
		// Must be in state S_KERNEL
		panic("Cannot evict a kernel page\n");
	}

	swap_out(swap, CME_ID_TO_PA(cme_id));
	pt_release_lock(as->as_pt, pte);
}

void
cm_free_page(cme_id_t cme_id)
{
        struct cme *cme;

	cme = &coremap.cmes[cme_id];

	switch(cme->cme_state) {
	case S_FREE:
		panic("Cannot free a page that is already free\n");
	case S_KERNEL:
		// Kernel memory is directly mapped, so can't be in swap
		break;
	case S_CLEAN:
	case S_DIRTY:
		swap_free_slot(cme->cme_swap_id);
		break;
	}

        cme->cme_state = S_FREE;

	// We do not need to send a TLB shootdown since there is no shared
	// user memory
	tlb_remove(OFFSETS_TO_VA(cme->cme_l1_offset, cme->cme_l2_offset));
}

bool
cm_attempt_lock(cme_id_t i)
{
	KASSERT(i < coremap.cm_size);

	bool acquired;

	spinlock_acquire(&coremap.cm_busy_spinlock);
	acquired = (coremap.cmes[i].cme_busy == 0);
	coremap.cmes[i].cme_busy = 1;
	spinlock_release(&coremap.cm_busy_spinlock);

	return acquired;
}

void
cm_acquire_lock(cme_id_t i)
{
	while (1) {
		if (cm_attempt_lock(i)) {
			break;
		}
	}
}

void
cm_release_lock(cme_id_t i)
{
	KASSERT(i < coremap.cm_size);

	spinlock_acquire(&coremap.cm_busy_spinlock);
	KASSERT(coremap.cmes[i].cme_busy == 1);
	coremap.cmes[i].cme_busy = 0;
	spinlock_release(&coremap.cm_busy_spinlock);
}

void
cm_acquire_locks(cme_id_t start, cme_id_t end) {
	KASSERT(start < end);
	KASSERT(start < coremap.cm_size);

	while (start < end) {
		cm_acquire_lock(start);
		start++;
	}
}

void
cm_release_locks(cme_id_t start, cme_id_t end) {
	KASSERT(start < end);
	KASSERT(end < coremap.cm_size);

	while (start < end) {
		cm_release_lock(start);
		start++;
	}
}
