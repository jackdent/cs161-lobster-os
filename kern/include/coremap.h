#include <types.h>
#include <spinlock.h>
#include <cme.h>

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

/*
 * Finds n contiguous free slots in the kernel portion of the
 * coremap, acquires the lock on all of those slots, and returns
 * the index of the first slot. Panics if the kernel could not
 * find any such memory.
 *
 * Expects the caller to call release every acquired lock by
 * callign cme_release_locks.
 */
cme_id_t cm_capture_slots_for_kernel(unsigned int nslots);

/*
 * Evicts a page from main memory to disk, if necessary.
 *
 * Assumes that the caller holds the core map entry lock.
 */
void evict_page(cme_id_t cme_id);
