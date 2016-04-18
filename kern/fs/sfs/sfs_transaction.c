#include "sfs_record.h"
#include "sfsprivate.h"

void
sfs_transaction_init(void)
{
        int i;

        tx_tracker.tx_lock = lock_create("transactions lock");
        if (tx_tracker.tx_lock == NULL) {
                panic("Could not initialize transactions lock\n");
        }

        for (i = 0; i < MAX_TRANSACTIONS; i++) {
                tx_tracker.tx_transactions[i] = NULL;
        }
}

struct sfs_transaction *
sfs_create_transaction(void)
{
        struct sfs_transaction *tx;
        int i;

        tx = kmalloc(sizeof(struct sfs_transaction));
        if (tx == NULL) {
                return NULL;
        }

        lock_acquire(tx_tracker.tx_lock);
        for (i = 0; i < MAX_TRANSACTIONS; i++) {
                if (tx_tracker.tx_transactions[i] == NULL) {
                        tx_tracker.tx_transactions[i] = tx;
                        tx->tx_id = i;
                        curthread->t_tx = tx;
                        lock_release(tx_tracker.tx_lock);
                        return tx;
                }
        }

        // could not find free slot for it
        lock_release(tx_tracker.tx_lock);
        kfree(tx);
        return NULL;

}

void
sfs_destroy_transaction(struct sfs_transaction* tx)
{
        KASSERT(tx != NULL);
        KASSERT(tx->tx_id < MAX_TRANSACTIONS);

        lock_acquire(tx_tracker.tx_lock);
        KASSERT(tx_tracker.tx_transactions[tx->tx_id] == tx);
        tx_tracker.tx_transactions[tx->tx_id] = NULL;
        lock_release(tx_tracker.tx_lock);

        VOP_DECREF(tx->tx_device);
        kfree(tx);
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
