#include <spinlock.h>
#include <cme.h>

struct cm {
        unsigned int cm_size;
        // Slots above the kernel break will be reserved for
        // user pages
        unsigned int cm_kernel_break;
        struct cme *cmes;
        struct spinlock cm_busy_spinlock;
        struct spinlock cm_clock_busy_spinlock;
        bool cm_clock_busy;
        cme_id_t cm_clock_hand;
        struct spinlock cm_page_count_spinlock;
        int cm_allocated_pages; // # of pages allocated, either in swap or RAM
        unsigned int cm_total_pages;	 // # of pages in swap + RAM
};

extern struct cm coremap;

// We should always have at least as many pages allocated as
// the number of pages for the coremap itself
int min_allocated_pages;

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
void cm_evict_page(cme_id_t cme_id);

/*
 * Writes a dirty page from main memory to disk.
 *
 * Assumes that the caller holds the core map entry lock.
 */
void cm_clean_page(cme_id_t cme_id);

/*
 * Frees the page in the coremap.
 *
 * Assumes that the caller holds the core map entry lock.
 */
void cm_free_page(cme_id_t cme_id);

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

/*
 * For keeping track of allocation counts
 */
bool cm_try_raise_page_count(unsigned int npages);
void cm_lower_page_count(unsigned int npages);
int cm_get_page_count(void);
