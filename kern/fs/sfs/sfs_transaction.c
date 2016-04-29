#include <kern/errno.h>
#include "sfsprivate.h"
#include "limits.h"
#include "sfs_transaction.h"

struct sfs_transaction_set *
sfs_transaction_set_create(void)
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

        /*
         * Start at 1 to avoid conflicting with the NULL values in an array
         * when recovering
         */
        tx_set->tx_id_counter = 1;

        return tx_set;
}

void
sfs_transaction_set_destroy(struct sfs_transaction_set *tx)
{
        lock_destroy(tx->tx_lock);
        kfree(tx);
}

struct sfs_transaction *
sfs_transaction_create(struct sfs_transaction_set *tx_tracker)
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

                        tx->tx_id = tx_tracker->tx_id_counter++;
                        tx->tx_lowest_lsn = 0;
                        tx->tx_highest_lsn = 0;
                        tx->tx_committed = 0;
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
sfs_transaction_destroy(struct sfs_transaction *tx)
{
        struct lock *tx_lock;
        int i;

        KASSERT(tx != NULL);

        tx_lock = tx->tx_tracker->tx_lock;

        lock_acquire(tx_lock);
        for (i = 0; i < MAX_TRANSACTIONS; i++) {
                if (tx->tx_tracker->tx_transactions[i] == tx) {
                        tx->tx_tracker->tx_transactions[tx->tx_id] = NULL;
                        lock_release(tx_lock);
                        kfree(tx);
                        return;
                }
        }
        panic("Trying to destroy transactions not in transaction set table?\n");
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

void
sfs_transaction_release_busy_bit(struct sfs_transaction *tx)
{
        lock_acquire(tx->tx_tracker->tx_lock);
        tx->tx_busy_bit = 0;
        lock_release(tx->tx_tracker->tx_lock);
}

static
void
sfs_transaction_add_record(struct sfs_fs *sfs, struct sfs_transaction *tx, struct sfs_record *record, enum sfs_record_type type)
{
        sfs_lsn_t lsn;

        lsn = sfs_record_write_to_journal(sfs, record, type);

        tx->tx_lowest_lsn = (tx->tx_lowest_lsn == 0 ? lsn : tx->tx_lowest_lsn);
        tx->tx_highest_lsn = lsn;

        // We no longer need the record in memory
        kfree(record);
}

void
sfs_current_transaction_add_record(struct sfs_fs *sfs, struct sfs_record *record, enum sfs_record_type type)
{
        struct sfs_transaction *tx;

        if (curthread->t_tx == NULL) {
                tx = sfs_transaction_create(sfs->sfs_transaction_set);

                if (tx == NULL) {
                        panic("TODO\n");
                }

                curthread->t_tx = tx;
        }

        record->r_txid = curthread->t_tx->tx_id;
        sfs_transaction_add_record(sfs, curthread->t_tx, record, type);
}

int
sfs_current_transaction_commit(struct sfs_fs *sfs)
{
        struct sfs_record *record;

        record = kmalloc(sizeof(struct sfs_record));
        if (record == NULL) {
                return ENOMEM;
        }

        KASSERT(curthread->t_tx);
        sfs_current_transaction_add_record(sfs, record, R_TX_COMMIT);
        curthread->t_tx = NULL;

        return 0;
}
