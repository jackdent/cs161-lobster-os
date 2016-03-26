#include <spinlock.h>

#define PAGE_TABLE_ENTRIES (1 << 10)
#define KUSEG_START 0x20000000

#define L1_PT_MASK(va) (va >> 22)
#define L2_PT_MASK(va) ((va >> 12) & 0x3FF)
#define OFFSET_MASK(va) (va & 0xFFF)

#define PI_TO_PHYS_PAGE(pa) (pa >> 12)
#define PHYS_PAGE_TO_PI(page) (page << 12)

#define LOWER_SWAP_BITS 5
#define SWAP_PHYS_PAGE_MASK(swap_id) (swap_id >> LOWER_SWAP_BITS)
#define SWAP_BITS_MASK(swap_id) (swap_id & ((1 << LOWER_SWAP_BITS) - 1))

// Will live inside the address space struct of a process

struct pte {
	unsigned int pte_phys_page:20;	// upper 20 bits of physical address
        unsigned int pte_valid:1;       // 1 if page is allocated for this process
	unsigned int pte_lazy:1;        // 1 if page is acquired but not allocated
					// (i.e. it has no coremap entry or swap id)
	unsigned int pte_present:1;     // 1 if page is in main memory
	unsigned int pte_busy_bit:1;	// 1 if some thread or kernel is operating
					// on this entry
	unsigned int pte_swap_tail:5;	// Lower 5 bits of swap offset
					// (pte_phys_page) is the upper 20
};


struct l2 {
	struct pte l2_ptes[PAGE_TABLE_ENTRIES];
};


struct l1 {
	struct l2 *l2s[PAGE_TABLE_ENTRIES];
};


struct pagetable {
	struct l1 pt_l1;                       // l1 page table
	struct spinlock pt_busy_bit_splk;      // for accessing busybits
};

// Create a empty page table with no l2_pt entries
struct pagetable *create_pagetable(void);

/*
 * Free all pages mapped from the pagetable and the
 * pagetable itself
 */
void destroy_pagetable(struct pagetable *pt);

/*
 * Returns true if and only if the virtual address is in the pagetable,
 * and assigns the pte argument to a pointer to the discovered pte.
 */
bool va_in_pagetable(vaddr_t va, struct pagetable *pt, struct pte **pte);

/*
 * Map a physical address pa to a virtual address va in the given
 * pagetable. Returns 0 on success, error value on failure
 * Failure can occur if a new l2_pt cannot be allocated (ENOMEM),
 *
 * It KASSERTS that no mapping exists, as if one did, our pagetable is
 * corrupted somehow
 *
 * This should be done while the corresponding core map entry is locked,
 * so that an eviction cannot take place will the pagetable entry is
 * being filled in an may not have its busy bit set yet.
 *
 * The valid and present bits will be set and the busy bit will not be set
 * upon returning. TODO: is this what we want?
 * int map_pa_to_va(paddr_t pa, vaddr_t va, struct pagetable *pt);
 *
 * Opposite of map_pa_to_va. Returns nothing, but KASSERTS that
 * the mapping exists, as if it didn't, our pagetable is corrupted somehow
 * Should be coupled with a TLB shootdown of some sort
 */
void unmap_va(vaddr_t va, struct pagetable *pt);

paddr_t va_to_pa(vaddr_t va, struct pte *pte);
swap_id_t swap_id(struct pte *pte);

/*
 * Extract swap offset from the seperate bit fields in a pte
 */
swap_id_t pte_get_swap_id(struct pte *pte);
void pte_set_swap_id(struct pte *pte, swap_id_t swap_id);

void acquire_busy_bit(struct pte *pte, struct pagetable *pt);
void release_busy_bit(struct pte *pte, struct pagetable *pt);
