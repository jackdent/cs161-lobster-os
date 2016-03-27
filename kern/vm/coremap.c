#include <coremap.h>
#include <lib.h>

struct cme
cme_create(pid_t pid, vaddr_t va)
{
        struct cme cme;

        cme.pid = pid;
        cme.cme_l1_offset = L1_PT_MASK(va);
        cme.cme_l2_offset = L2_PT_MASK(va);
        cme.cme_swap_id = 0;
        cme.cme_state = S_FREE;
        cme.cme_busy = 0;
        cme.cme_recent = 1;

        return cme;
}

bool
cme_attempt_lock(cme_id_t i)
{
        bool acquired;

        KASSERT(i < CM_SIZE);

        spinlock_acquire(&coremap.cme_spinlock);
        acquired = (coremap.cmes[i].cme_busy == 0);
        coremap.cmes[i].cme_busy = 1;
        spinlock_release(&coremap.cme_spinlock);

        return acquired;
}

void
cme_acquire_lock(cme_id_t i)
{
        while (1) {
                if (cme_attempt_lock(i)) {
                        break;
                }
        }
}

void
cme_release_lock(cme_id_t i)
{
        KASSERT(i < CM_SIZE);

        spinlock_acquire(&coremap.cme_spinlock);
        KASSERT(coremap.cmes[i].cme_busy == 1);
        coremap.cmes[i].cme_busy = 0;
        spinlock_release(&coremap.cme_spinlock);
}

void
cm_init()
{
        spinlock_init(&coremap.cme_spinlock);
        spinlock_init(&coremap.cm_clock_spinlock);
        coremap.cm_clock_hand = 0;
}

static
void
cm_advance_clock_hand()
{
        coremap.cm_clock_hand = (coremap.cm_clock_hand + 1) % CM_SIZE;
}

cme_id_t
cm_capture_slot()
{
        unsigned int i;
        cme_id_t slot;
        struct cme entry;

        spinlock_acquire(&coremap.cm_clock_spinlock);

        for (i = 0; i < CM_SIZE; ++i) {
                slot = coremap.cm_clock_hand;
                entry = coremap.cmes[slot];

                cm_advance_clock_hand();

                if (!cme_attempt_lock(slot)) {
                        continue;
                }

                coremap.cmes[slot].cme_recent = 0;

                if (entry.cme_state == S_FREE || entry.cme_recent == 0) {
                        spinlock_release(&coremap.cm_clock_spinlock);
                        return slot;
                }

                cme_release_lock(slot);
        }

        // If we reach the end of the loop without returning, we
        // should evict the entry the clock hand first pointed to
        slot = coremap.cm_clock_hand;

        cme_acquire_lock(slot);
        coremap.cmes[slot].cme_recent = 0;
        cm_advance_clock_hand();

        spinlock_release(&coremap.cm_clock_spinlock);
        return slot;
}
