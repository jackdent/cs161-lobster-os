#include <types.h>
#include <spinlock.h>

#define CM_SIZE 100

/* Coremap entry */

struct cm_entry {
        unsigned int cme_pid:15;
        unsigned int cme_l1_offset:10;
        unsigned int cme_l2_offset:10;
        unsigned int cme_swap_offset:25;
        unsigned int cme_free:1;
        unsigned int cme_dirty:1;
        unsigned int cme_busy:1;
        unsigned int cme_recent:1;
};

bool cm_entry_attempt_lock(unsigned int i);
void cm_entry_acquire_lock(unsigned int i);
void cm_entry_release_lock(unsigned int i);

/* Coremap */

struct cm {
        struct cm_entry cm_entries[CM_SIZE];
        struct spinlock cm_entry_spinlock;
        struct spinlock cm_clock_spinlock;
        unsigned int cm_clock_hand;
};

// Global coremap
struct cm coremap;

void cm_init(void);

/*
 * Adds the supplied argument to a free slot in the coremap,
 * returning false.
 *
 * If there are no free slots available, finds a cm_entry
 * to evict and assigns it to evicted_entry, returning true.
 */
bool cm_add_entry(struct cm_entry new_entry, struct cm_entry *evicted_entry);
