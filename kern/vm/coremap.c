#include <types.h>
#include <lib.h>
#include <coremap.h>
#include <machine/vm.h>
#include <swap.h>
#include <synch.h>
#include <proctable.h>
#include <addrspace.h>
#include <thread.h>
#include <current.h>
#include <array.h>
#include <cpu.h>
#include <pagetable.h>

// Global coremap struct
struct cm coremap;

// Forward declaration, implemented in vm/tlb.c
void tlb_remove(vaddr_t va);
void tlb_set_writeable(vaddr_t va, cme_id_t cme_id, bool writeable);

void
cm_init()
{
	unsigned int i, ncoremap_bytes, ncoremap_pages, ncmes;
	paddr_t ram_size, start;

	ram_size = ram_getsize();
	ncmes = (ram_size / PAGE_SIZE);
	ncoremap_bytes = ncmes * sizeof(struct cme);;
	ncoremap_pages = DIVROUNDUP(ncoremap_bytes, PAGE_SIZE);

	start = ram_stealmem(ncoremap_pages);
	if (start == 0) {
		panic("Could not allocate coremap\n");
	}

	// The cmes are now alloc'd
	coremap.cmes = (struct cme*)PADDR_TO_KVADDR(start);
	base = start;

	coremap.cm_size = ncmes - PA_TO_PHYS_PAGE(base);
	spinlock_init(&coremap.cm_busy_spinlock);
	spinlock_init(&coremap.cm_clock_spinlock);
	coremap.cm_clock_hand = 0;

	memset(coremap.cmes, 0, ncmes * sizeof(struct cme));

	// Set the coremap as owned by the kernel
	for (i = 0; i < ncoremap_pages; i++) {
		coremap.cmes[i] = cme_create(0, 0, S_KERNEL);
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

		if (entry.cme_state == S_FREE || (entry.cme_recent == 0 && entry.cme_state != S_KERNEL)) {
			spinlock_release(&coremap.cm_clock_spinlock);
			return slot;
		}

		cm_release_lock(slot);
	}

	// If we reach the end of the loop without returning, we
	// should evict the entry the clock hand first pointed to,
	// unless it's a kernel page
	for (i = 0; i < coremap.cm_size; i++) {
		slot = coremap.cm_clock_hand;
		entry = coremap.cmes[slot];

		cm_advance_clock_hand();

		if (!cm_attempt_lock(slot)) {
			continue;
		}

		coremap.cmes[slot].cme_recent = 0;

		if (entry.cme_state != S_KERNEL) {
			cm_advance_clock_hand();
			spinlock_release(&coremap.cm_clock_spinlock);
			return slot;
		}
		cm_release_lock(slot);
	}
	panic("Cannot capture coremap slot: all memory pages are kernel?\n");
	return 0;
}

cme_id_t
cm_capture_slots_for_kernel(unsigned int nslots)
{
	cme_id_t i, j;
	spinlock_acquire(&coremap.cm_clock_spinlock);
	i = 0;

	while (i < coremap.cm_size - nslots) {
		for (j = 0; j < nslots; j++) {
			if (!cm_attempt_lock(i + j)) {
				break;
			}
			if (coremap.cmes[i + j].cme_state != S_FREE) {
				cm_release_lock(i + j);
				break;
			}
		}

		if (j == nslots) {
			spinlock_release(&coremap.cm_clock_spinlock);
			return i;
		} else {
			cm_release_locks(i, i + j);
			i += j + 1;
		}
	}

	panic("Could not capture contiguous slots for kernel allocation\n");
	return 0;
}

static
void
cm_tlb_shootdown(vaddr_t va, cme_id_t cme_id, enum tlbshootdown_type type)
{
	unsigned int numcpus, i, j;
	struct cpu *cpu;

	numcpus = cpuarray_num(&allcpus);

	lock_acquire(tlbshootdown.ts_lock);
	tlbshootdown.ts_flushed_cme_id = cme_id;
	tlbshootdown.ts_flushed_va = va;
	tlbshootdown.ts_type = type;

	for (i = 0; i < cpuarray_num(&allcpus); i++) {
		cpu = cpuarray_get(&allcpus, i);
		if (cpu != curcpu->c_self) {
			ipi_tlbshootdown(cpu, &tlbshootdown);
		}
	}

	// Don't wait on self
	for (j = 0; j < numcpus - 1; j++) {
		// Will reset the semaphore to 0
		P(tlbshootdown.ts_sem);
	}

	lock_release(tlbshootdown.ts_lock);
}

/*
 * If the core map entry is free, NOOP. Otherwise, write the page to
 * disk if it is dirty, or if it has never left main memory before.
 * In the latter case, we find a free swap slot and set its index on
 * the page table entry. Finally, we update the page table entry to
 * indicate that it is no longer present in main memory.
 */
void
cm_evict_page(cme_id_t cme_id)
{
        struct addrspace *as;
        struct pte *pte;
        struct proc *proc;
        struct cme *cme;
        vaddr_t va;
        swap_id_t swap_id;

	cme = &coremap.cmes[cme_id];

	if (cme->cme_state == S_FREE) {
		return;
	}

	proc = proc_table.pt_table[cme->cme_pid];
	KASSERT(proc != NULL);

        as = proc->p_addrspace;
	KASSERT(as != NULL);

	pte = pagetable_get_pte_from_cme(as->as_pt, cme);
	KASSERT(pte != NULL);

	pt_acquire_lock(as->as_pt, pte);
	KASSERT(pte->pte_state == S_PRESENT);

	va = OFFSETS_TO_VA(cme->cme_l1_offset, cme->cme_l2_offset);

	tlb_remove(va);
	cm_tlb_shootdown(va, cme_id, TS_EVICT);

	switch (cme->cme_state) {
	case S_KERNEL:
		panic("Cannot evict a kernel page\n");
	case S_UNSWAPPED:
		// If this is the first time we're writing the page
		// out to disk, we grab a free swap entry, and assign
		// its index to the page table entry. The swap id will
		// be stable for this page for the remainder of its
		// lifetime.
		swap_id = swap_capture_slot();
		pte_set_swap_id(pte, swap_id);
		swap_out(swap_id, cme_id);
		break;
	case S_CLEAN:
		pte_set_swap_id(pte, cme->cme_swap_id);
		break;
	case S_DIRTY:
		swap_id = cme->cme_swap_id;
		pte_set_swap_id(pte, cme->cme_swap_id);
		swap_out(swap_id, cme_id);
		break;
	}

	cme->cme_state = S_CLEAN;
	pte->pte_state = S_SWAPPED;
	pt_release_lock(as->as_pt, pte);
}

/*
 * Mark the TLB entry as unwriteable, write the page out to disk,
 * and mark the core map entry as clean.
 */
void
cm_clean_page(cme_id_t cme_id)
{
	struct cme *cme;
	vaddr_t va;

	cme = &coremap.cmes[cme_id];
	KASSERT(cme->cme_state == S_DIRTY);

	va = OFFSETS_TO_VA(cme->cme_l1_offset, cme->cme_l2_offset);

	tlb_set_writeable(va, cme_id, false);
	cm_tlb_shootdown(va, cme_id, TS_CLEAN);

	cme->cme_state = S_CLEAN;
	swap_out(cme->cme_swap_id, cme_id);
}

void
cm_free_page(cme_id_t cme_id)
{
	struct cme *cme;

	cme = &coremap.cmes[cme_id];

	// We do not need to send a TLB shootdown since there is no shared
	// user memory
	tlb_remove(OFFSETS_TO_VA(cme->cme_l1_offset, cme->cme_l2_offset));

	switch(cme->cme_state) {
	case S_FREE:
		panic("Cannot free a page that is already free\n");
	case S_KERNEL:
		// Kernel memory is directly mapped, so can't be in swap
		break;
	case S_UNSWAPPED:
		// The page has never left main memory, so there is no
		// swap entry to release
		break;
	case S_CLEAN:
	case S_DIRTY:
		swap_free_slot(cme->cme_swap_id);
		break;
	}

	cme->cme_state = S_FREE;
}

bool
cm_attempt_lock(cme_id_t i)
{
	KASSERT(i < coremap.cm_size);

	bool acquired;
	spinlock_acquire(&coremap.cm_busy_spinlock);
	acquired = (coremap.cmes[i].cme_busy == 0);
	if (acquired) {
		coremap.cmes[i].cme_busy = 1;
	}
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
	KASSERT(start <= end);
	KASSERT(start < coremap.cm_size);
	KASSERT(end < coremap.cm_size);

	while (start < end) {
		cm_acquire_lock(start);
		start++;
	}
}

void
cm_release_locks(cme_id_t start, cme_id_t end) {
	KASSERT(start <= end);
	KASSERT(start < coremap.cm_size);
	KASSERT(end < coremap.cm_size);

	while (start < end) {
		cm_release_lock(start);
		start++;
	}
}
