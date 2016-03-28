#include <cme.h>

struct cme
cme_create(pid_t pid, vaddr_t va, enum cme_state state)
{
        struct cme cme;

        cme.cme_pid = pid;
        cme.cme_l1_offset = L1_PT_MASK(va);
        cme.cme_l2_offset = L2_PT_MASK(va);
        cme.cme_swap_id = 0;
        cme.cme_busy = 1; // So we can unset it when we're finished
        cme.cme_recent = 1;
        cme.cme_state = state;

        return cme;
}

bool
cme_attempt_lock(cme_id_t i)
{
        KASSERT(i < CM_SIZE);

        bool acquired;

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
cme_acquire_locks(cme_id_t start, cme_id_t end) {
        KASSERT(start < end);
        KASSERT(start < CM_SIZE);

        while (start < end) {
                cme_acquire_lock(start);
                start++;
        }
}

void
cme_release_locks(cme_id_t start, cme_id_t end) {
        KASSERT(start < end);
        KASSERT(end < CM_SIZE);

        while (start < end) {
                cme_release_lock(start);
                start++;
        }
}
