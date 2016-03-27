#include <types.h>
#include <spinlock.h>

/*
 * Coremap entry
 */

/* An index for pages that is *not* stable over their lifetime */
typedef cme_id_t unsigned uint32_t;

#define CME_ID_TO_PA(cme_id) (cme_id * PAGE_SIZE)

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

struct cme cme_create(pid_t pid);
bool cme_attempt_lock(cme_id_t i);
void cme_acquire_lock(cme_id_t i);
void cme_release_lock(cme_id_t i);

/*
 * Coremap
 */

// TODO: set to size of main memory
#define CM_SIZE (1 << 20)

struct cm {
        struct cme cmes[CM_SIZE];
        struct spinlock cme_spinlock;
        struct spinlock cm_clock_spinlock;
        cme_id_t cm_clock_hand;
};

// Global coremap
struct cm coremap;

void cm_init(void);

/*
 * Implements the LRU page eviction algorithm.
 *
 * Finds a free slot in the coremap, acquires the lock on that
 * slot, and returns the slot's index.
 *
 * Expects the caller to release the lock on the cme
 */
cme_id_t cm_capture_slot(void);
