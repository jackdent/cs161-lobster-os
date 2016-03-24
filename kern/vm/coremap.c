#include <coremap.h>
#include <lib.h>

bool cm_entry_attempt_lock(unsigned int i)
{
        bool acquired;

        KASSERT(i < CM_SIZE);

        spinlock_acquire(&coremap.cm_entry_spinlock);
        acquired = (coremap.cm_entries[i].cme_busy == 0);
        coremap.cm_entries[i].cme_busy = 1;
        spinlock_release(&coremap.cm_entry_spinlock);

        return acquired;
}

void cm_entry_acquire_lock(unsigned int i)
{
        while (1) {
                if (cm_entry_attempt_lock(i)) {
                        break;
                }
        }
}

void cm_entry_release_lock(unsigned int i)
{
        KASSERT(i < CM_SIZE);

        spinlock_acquire(&coremap.cm_entry_spinlock);
        KASSERT(coremap.cm_entries[i].cme_busy == 1);
        coremap.cm_entries[i].cme_busy = 0;
        spinlock_release(&coremap.cm_entry_spinlock);
}

void cm_init()
{
        spinlock_init(&coremap.cm_entry_spinlock);
        spinlock_init(&coremap.cm_clock_spinlock);
        coremap.cm_clock_hand = 0;
}

static
void
cm_advance_clock_hand()
{
        coremap.cm_clock_hand = (coremap.cm_clock_hand + 1) % CM_SIZE;
}

bool cm_add_entry(struct cm_entry new_entry, struct cm_entry *evicted_entry)
{
        unsigned int i;
        struct cm_entry current;
        bool insert, evicted;

        spinlock_acquire(&coremap.cm_clock_spinlock);

        for (i = 0; i < CM_SIZE; ++i) {
                current = coremap.cm_entries[coremap.cm_clock_hand];

                if (!cm_entry_attempt_lock(coremap.cm_clock_hand)) {
                        continue;
                }

                if (current.cme_free == 1) {
                        evicted = false;
                        insert = true;
                } else if (current.cme_recent == 0) {
                        evicted = true;
                        insert = true;
                        *evicted_entry = current;
                } else {
                        insert = false;
                }

                if (insert) {
                        coremap.cm_entries[coremap.cm_clock_hand] = new_entry;
                        cm_entry_release_lock(coremap.cm_clock_hand);

                        cm_advance_clock_hand();
                        break;
                }

                coremap.cm_entries[coremap.cm_clock_hand].cme_recent = 0;
                cm_entry_release_lock(coremap.cm_clock_hand);

                cm_advance_clock_hand();
        }

        // Evict the entry the clock hand first pointed to
        if (i == CM_SIZE) {
                cm_entry_acquire_lock(coremap.cm_clock_hand);
                evicted = true;
                *evicted_entry = coremap.cm_entries[coremap.cm_clock_hand];
                coremap.cm_entries[coremap.cm_clock_hand] = new_entry;
                cm_entry_release_lock(coremap.cm_clock_hand);

                cm_advance_clock_hand();
        }

        spinlock_release(&coremap.cm_clock_spinlock);

        return evicted;
}
