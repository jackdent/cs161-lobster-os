#include <swap.h>

#define PA_TO_PHYS_PAGE(pa) (pa >> 12)
#define PHYS_PAGE_TO_PA(page) (page << 12)
#define OFFSET_MASK(va) (va & 0xFFF)

#define LOWER_SWAP_BITS 5
#define SWAP_PHYS_PAGE_MASK(swap_id) (swap_id >> LOWER_SWAP_BITS)
#define SWAP_BITS_MASK(swap_id) (swap_id & ((1 << LOWER_SWAP_BITS) - 1))

enum pte_state {
        // The pte is invalid
        S_INVALID,

        // The pte refers to a valid page that has not yet been
        // allocated (i.e. it has no coremap entry or swap id)
        S_LAZY,

        // The pte refers to a valid page in main memory
        S_PRESENT,

        // The pte refers to a valid page in swap space memory
        S_SWAPPED
};

struct pte {
        unsigned int pte_phys_page:20;  // Upper 20 bits of physical address
        unsigned int pte_busy:1;        // 1 if some thread or kernel is
                                        // operating on this entry
        unsigned int pte_swap_tail:5;   // Lower 5 bits of swap offset
                                        // (pte_phys_page is the upper 20)
        enum pte_state pte_state:2;
};


paddr_t pte_va_to_pa(vaddr_t va, struct pte *pte);

/*
 * Extract swap offset from the seperate bit fields in a pte.
 */
swap_id_t pte_get_swap_id(struct pte *pte);
void pte_set_swap_id(struct pte *pte, swap_id_t swap_id);

/*
 * Returns true iff the attempt to acquire the lock on
 * the specified page map entry was successful.
 */
struct pagetable; // Forward declaration for locking

bool pte_attempt_lock(struct pte *pte, struct pagetable *pt);
void pte_acquire_lock(struct pte *pte, struct pagetable *pt);
void pte_release_lock(struct pte *pte, struct pagetable *pt);
