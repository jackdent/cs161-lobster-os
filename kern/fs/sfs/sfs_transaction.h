#include <types.h>
#include <synch.h>
#include "lib.h"

#define MAX_TRANSACTIONS 64	// TODO: this should be fine, right?

typedef uint64_t txid_t;

struct sfs_transaction {
        txid_t tx_id;                   // equal to the slot number in the global array
        uint32_t tx_lowest_LSN;		// For checkpointing
        uint32_t tx_dirty_buf_count;    // 0 when transaction has completed all side-effects
        struct vnode *tx_device;	// the volume the transaction is relevant to
};

struct sfs_transaction_tracker {
	struct sfs_transaction *tx_transactions[MAX_TRANSACTIONS];
	struct lock *tx_lock;
};

struct sfs_transaction_tracker tx_tracker;


void sfs_transaction_init(void);
struct sfs_transaction *sfs_create_transaction(void);
void sfs_destroy_transaction(struct sfs_transaction* tx);

void sfs_transaction_redo(struct sfs_transaction *tx);
void sfs_transaction_undo(struct sfs_transaction *tx);
