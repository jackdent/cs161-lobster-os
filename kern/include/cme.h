#include <types.h>
#include <proc.h>

#define CME_ID_TO_PA(cme_id) (cme_id * PAGE_SIZE)

/*
 * An index for pages that is *not* stable over their lifetime
 */
typedef uint32_t cme_id_t;

/*
 * We use an enumerated type, since these states are mutually
 * exclusive
 */
enum cme_state {
        // The page is not owned by a user process
        // or the kernel
        S_FREE,

        // The page is owned by the kernel
        S_KERNEL,

        // The page is not owned by the kernel
        S_DIRTY,
        S_CLEAN
};

struct cme {
        unsigned int cme_pid:15;
        unsigned int cme_l1_offset:10;
        unsigned int cme_l2_offset:10;
        unsigned int cme_swap_id:24;
        unsigned int cme_busy:1;
        unsigned int cme_recent:1;
        enum cme_state cme_state:2;
};

struct cme cme_create(pid_t pid, vaddr_t va, enum cme_state state);
