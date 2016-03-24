#include <spinlock.h>

#define PAGE_TABLE_ENTRIES (1 << 10)
#define KUSEG_START 0x20000000

#define L1_PT_MASK(va) (va >> 22)
#define L2_PT_MASK(va) ((va >> 12) & 0x3FF)
#define OFFSET_MASK(va) (va & 0xFFF)

#define PHYS_PAGE_MASK(pa) (pa >> 12)

// Will live inside the address space struct of a process

struct pte {
	unsigned pte_phys_page:20;	// upper 20 bits of physical address
	unsigned pte_valid:1;		// 1 if page is allocated for this process
	unsigned pte_present:1;		// 1 if page is in main memory
	unsigned pte_busy_bit:1;	// 1 if some thread or kernel is operating
					// on this entry
};


struct l2 {
	struct pte l2_ptes[PAGE_TABLE_ENTRIES];
};


struct l1 {
	struct l2 *l2s[PAGE_TABLE_ENTRIES];
};


struct pagetable {
	struct l1 pt_l1;			// l1 page table
	struct spinlock pt_busy_bit_splk; 	// for accessing busybits
};

// Create a empty page table with no l2_pt entries
struct pagetable *create_pagetable(void);

// Free all pages mapped from the pagetable and the
// pagetable itself
void destroy_pagetable(struct pagetable *pt);

// Map a physical address pa to a virtual address va in the given
// pagetable. Returns 0 on success, error value on failure
// Failure can occur if a new l2_pt cannot be allocated (ENOMEM),

// It KASSERTS that no mapping exists, as if one did, our pagetable is
// corrupted somehow

// This should be done while the corresponding core map entry is locked,
// so that an eviction cannot take place will the pagetable entry is
// being filled in an may not have its busy bit set yet.

// The valid and present bits will be set and the busy bit will not be set
// upon returning. TODO: is this what we want?
int map_pa_to_va(paddr_t pa, vaddr_t va, struct pagetable *pt);

// Opposite of map_pa_to_va. Returns nothing, but KASSERTS that
// the mapping exists, as if it didn't, our pagetable is corrupted somehow
// Should be coupled with a TLB shootdown of some sort
void unmap_va(vaddr_t va, struct pagetable *pt);

void acquire_busy_bit(struct pte *pte, struct pagetable *pt);
void release_busy_bit(struct pte *pte, struct pagetable *pt);


