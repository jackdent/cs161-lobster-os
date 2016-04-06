#include <types.h>
#include <pagetable_masks.h>
#include <addrspace.h>

#define CME_ID_TO_PA(cme_id) ((cme_id * PAGE_SIZE) + base)
#define PA_TO_CME_ID(pa) ((pa - base) / PAGE_SIZE)
#define OFFSETS_TO_VA(l1, l2) ((l1 << 10 | l2) << 12)

paddr_t base;

/*
 * An index for pages that is *not* stable over their lifetime
 */
typedef uint32_t cme_id_t;

/*
 * We use an enumerated type, since these states are mutually
 * exclusive
 */

#ifndef CME_H_
#define CME_H_

enum cme_state {
        // The page is not owned by a user process
        // or the kernel
        S_FREE,

        // The page is owned by the kernel
        S_KERNEL,

        // The page is not owned by the kernel,
        // and has never been swapped to disk
        S_UNSWAPPED,

        // The page is not owned by the kernel,
        // and has been swapped to disk in the past
        S_DIRTY,
        S_CLEAN
};

struct cme {
        struct addrspace *cme_as;
        unsigned int cme_l1_offset:10;
        unsigned int cme_l2_offset:10;
        unsigned int cme_swap_id:24;
        unsigned int cme_busy:1;
        unsigned int cme_recent:1;
        enum cme_state cme_state:3;
};

struct cme cme_create(struct addrspace *as, vaddr_t va, enum cme_state state);
bool cme_is_equal_to(struct cme *cme, struct cme *other);

#endif
