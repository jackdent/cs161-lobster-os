#include <spinlock.h>
#include <pte.h>
#include <cme.h>

#define PAGE_TABLE_SIZE (1 << 10)

#define L1_PT_MASK(va) (va >> 22)
#define L2_PT_MASK(va) ((va >> 12) & 0x3FF)

struct l2 {
	struct pte l2_ptes[PAGE_TABLE_SIZE];
};

struct l1 {
	struct l2 *l2s[PAGE_TABLE_SIZE];
};

struct pagetable {
	struct l1 pt_l1;                       // l1 page table
	struct spinlock pt_busy_bit_splk;      // for accessing busybits
};

/*
 * Create a empty page table with no l2_pt entries
 */
struct pagetable *pagetable_create(void);

/*
 * Free all pages mapped from the pagetable and the
 * pagetable itself
 */
void pagetable_destroy(struct pagetable *pt);

/*
 * Lookup a pagetable entry based on the supplied l2 and l2 offsets, and
 * return NULL if no entry was found.
 */
struct pte * pagetable_get_pte_from_offsets(struct pagetable *pt, unsigned int l1_offset, unsigned l2_offset);

/*
 * Lookup a pagetable entry based on the supplied virtual address, and
 * return NULL if no entry was found.
 */
struct pte * pagetable_get_pte_from_va(struct pagetable *pt, vaddr_t va);

/*
 * Lookup a pagetable entry based on the supplied core map entry, and
 * return NULL if no entry was found.
 */
struct pte * pagetable_get_pte_from_cme(struct pagetable *pt, struct cme *cme);

/*
 * Clone every entry in the page table. If the entry is in the state
 * S_PRESENT or S_SWAPPED, we create a new slot in the swap space and
 * copy the page over.
 */
void pagetable_clone(struct pagetable *old_pt, struct pagetable *new_pt);
