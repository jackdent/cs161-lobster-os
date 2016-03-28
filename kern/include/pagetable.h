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
 * Returns true if and only if the virtual address is in the pagetable,
 * and assigns the pte argument to a pointer to the discovered pte.
 */
bool pagetable_contains_va(vaddr_t va, struct pagetable *pt, struct pte **pte);

/*
 * Lookup a pagetable entry based on the supplied core map entry, and
 * returns NULL if no entry was found.
 */
struct pte * pagetable_get_pte_from_cme(struct pagetable *pt, struct cme *cme);

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
 */
 int map_pa_to_va(paddr_t pa, vaddr_t va, struct pagetable *pt);

 /*
 * Opposite of map_pa_to_va. Returns nothing, but KASSERTS that
 * the mapping exists, as if it didn't, our pagetable is corrupted somehow
 * Should be coupled with a TLB shootdown of some sort
 */
void unmap_va(vaddr_t va, struct pagetable *pt);
