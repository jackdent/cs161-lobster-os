#include <swap.h>

#ifndef cmd_id_t
typedef uint32_t cme_id_t;
#endif

#define PA_TO_PHYS_PAGE(pa) (pa >> 12)
#define PHYS_PAGE_TO_PA(page) (page << 12)
#define OFFSET_MASK(va) (va & 0xFFF)
#define L1_L2_TO_VA(l1, l2) (l1 << 22) | (l2 << 12)

#define LOWER_SWAP_BITS 5
#define SWAP_PHYS_PAGE_MASK(swap_id) (swap_id >> LOWER_SWAP_BITS)
#define SWAP_BITS_MASK(swap_id) (swap_id & ((1 << LOWER_SWAP_BITS) - 1))
#define SWAP_ID(upper, lower) ((upper << LOWER_SWAP_BITS) | lower)

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

paddr_t pte_get_pa(struct pte *pte, vaddr_t va);

/*
 * Extract the physical page number.
 */
paddr_t pte_get_phys_page(struct pte *pte);
void pte_set_phys_page(struct pte *pte, paddr_t pa);

/*
 * Extract the swap offset from the overloaded pte_phys_page and swap_tail.
 */
swap_id_t pte_get_swap_id(struct pte *pte);
void pte_set_swap_id(struct pte *pte, swap_id_t swap_id);
