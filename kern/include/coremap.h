#include <spinlock.h>
#include <cme.h>

#define CM_ENTRIES 1000 // TODO: make this equal to # of pages

struct cm {
        unsigned int cm_size;
        struct cme cmes[CM_ENTRIES];
        struct spinlock cm_busy_spinlock;
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

/*
 * Returns true iff the attempt to acquire the lock on
 * the specified core map entry was successful.
 */
bool cm_attempt_lock(cme_id_t i);
void cm_acquire_lock(cme_id_t i);
void cm_release_lock(cme_id_t i);

/*
 * Acquire/release all locks between start (inclusive)
 * and end (exclusive).
 */
void cm_acquire_locks(cme_id_t start, cme_id_t end);
void cm_release_locks(cme_id_t start, cme_id_t end);
