#include "sfs_record.h"
#include "sfsprivate.h"
#include "limits.h"

struct sfs_transaction_set *
sfs_create_transaction_set(void)
{
        struct sfs_transaction_set *tx_set;
        int i;

        tx_set = kmalloc(sizeof(struct sfs_transaction_set));
        if (tx_set == NULL) {
                return NULL;
        }

        tx_set->tx_lock = lock_create("transaction set lock");
        if (tx_set->tx_lock == NULL) {
                kfree(tx_set);
                return NULL;
        }

        for (i = 0; i < MAX_TRANSACTIONS; i++) {
                tx_set->tx_transactions[i] = NULL;
        }

        return tx_set;
}

void
sfs_destroy_transaction_set(struct sfs_transaction_set *tx)
{
        lock_destroy(tx->tx_lock);
        kfree(tx);
}

struct sfs_transaction *
sfs_create_transaction(struct sfs_transaction_set *tx_tracker)
{
        struct sfs_transaction *tx;
        int i;

        tx = kmalloc(sizeof(struct sfs_transaction));
        if (tx == NULL) {
                return NULL;
        }

        lock_acquire(tx_tracker->tx_lock);
        for (i = 0; i < MAX_TRANSACTIONS; i++) {
                if (tx_tracker->tx_transactions[i] == NULL) {
                        tx_tracker->tx_transactions[i] = tx;

                        tx->tx_id = i;
                        tx->tx_lowest_LSN = UINT_MAX;
                        tx->tx_highest_LSN = UINT_MAX;
                        tx->tx_commited = 0;
                        tx->tx_busy_bit = 0;

                        curthread->t_tx = tx;
                        lock_release(tx_tracker->tx_lock);
                        return tx;
                }
        }

        // could not find free slot for it
        lock_release(tx_tracker->tx_lock);
        kfree(tx);
        return NULL;
}

void
sfs_destroy_transaction(struct sfs_transaction *tx)
{

        struct lock *tx_lock;

        KASSERT(tx != NULL);
        KASSERT(tx->tx_id < MAX_TRANSACTIONS);

        tx_lock = tx->tx_tracker->tx_lock;

        lock_acquire(tx_lock);
        KASSERT(tx->tx_tracker->tx_transactions[tx->tx_id] == tx);
        tx->tx_tracker->tx_transactions[tx->tx_id] = NULL;
        lock_release(tx_lock);

        kfree(tx);
}

static
bool
sfs_transaction_attempt_busy_bit(struct sfs_transaction *tx)
{
        bool result;

        lock_acquire(tx->tx_tracker->tx_lock);
        result = tx->tx_busy_bit == 0;
        if (result) {
                tx->tx_busy_bit = 1;
        }
        lock_release(tx->tx_tracker->tx_lock);

        return result;
}

void
sfs_transaction_acquire_busy_bit(struct sfs_transaction *tx)
{
        while (!sfs_transaction_attempt_busy_bit(tx)) {
                continue;
        }
}

void sfs_transaction_release_busy_bit(struct sfs_transaction *tx)
{
        lock_acquire(tx->tx_tracker->tx_lock);
        tx->tx_busy_bit = 0;
        lock_release(tx->tx_tracker->tx_lock);
}

/*
static
void
sfs_transaction_apply(struct sfs_transaction *tx, void (*fn)(struct sfs_record, enum sfs_record_type))
{
        int err;
        struct sfs_jiter *ji;

        enum sfs_record_type record_type;
        void *record_ptr;
        size_t record_len;
        struct sfs_record record;

        err = sfs_jiter_fwdcreate(sfs, &ji);
        if (err) {
                panic("Error while reading journal\n");
        }

        while (!sfs_jiter_done(ji)) {
                record_type = sfs_jiter_type(ji);

                record_ptr = sfs_jiter_rec(ji, &record_len);
                record = memcpy(&record, record_ptr, record_len);

                if (record.r_txid == tx->tx_id) {
                        fn(record, record_type);
                }

                err = sfs_jiter_next(sfs, ji);
                if (err) {
                        panic("Error while reading journal\n");
                }
        }

        sfs_jiter_destroy(ji);
}

void
sfs_transaction_undo(struct sfs_transaction *tx)
{
        KASSERT(tx != NULL);

        sfs_transaction_apply(tx, sfs_record_undo);
}

void
sfs_transaction_redo(struct sfs_transaction *tx)
{
        KASSERT(tx != NULL);

        sfs_transaction_apply(tx, sfs_record_redo);
}
*/
