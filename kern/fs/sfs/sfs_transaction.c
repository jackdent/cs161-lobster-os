#include "sfs-record.h"

static
void
sfs_transaction_apply(struct sfs_transaction *tx, void (*fn)(struct sfs_record, enum sfs_record_type))
{
        int err;
        sfs_jiter *ji;

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

                if (record.r_txid == tx->txid) {
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
