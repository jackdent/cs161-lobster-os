#include <types.h>

typedef uint64_t txid_t;

struct sfs_transaction {
        txid_t txid;                     // equal to the LSN of the transaction's first record
        uint32_t tx_dirty_buf_count;     // 0 when transaction has completed all side-effects
        uint32_t tx_volume;              // the volume the transaction is relevant to
};

void sfs_transaction_redo(struct sfs_transaction *tx);
void sfs_transaction_undo(struct sfs_transaction *tx);
