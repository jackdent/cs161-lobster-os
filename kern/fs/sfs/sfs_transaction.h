#include <types.h>
#include <synch.h>
#include "lib.h"
#include "sfsprivate.h"

#define MAX_TRANSACTIONS 64	// TODO: this should be fine, right?

typedef uint32_t txid_t;
struct sfs_transaction_set;
struct sfs_record;
enum sfs_record_type;

struct sfs_transaction {
        txid_t tx_id;                   // equal to the slot number in the per-device array
        sfs_lsn_t tx_lowest_lsn;	// For checkpointing
        sfs_lsn_t tx_highest_lsn;	// For checkpointing
        uint32_t tx_committed:1;		// 0 when transaction has completed all side-effects
        struct sfs_transaction_set *tx_tracker;
        uint32_t tx_busy_bit:1;		// Locking, accessed via tx_lock
};

// Per-device struct that lives in the sfs_fs struct
struct sfs_transaction_set {
	struct sfs_transaction *tx_transactions[MAX_TRANSACTIONS];
	struct lock *tx_lock;
};

struct sfs_transaction_set *sfs_transaction_set_create(void);
void sfs_transaction_set_destroy(struct sfs_transaction_set *tx);

void sfs_transaction_init(void);
struct sfs_transaction *sfs_transaction_create(struct sfs_transaction_set *tx_tracker);
void sfs_transaction_destroy(struct sfs_transaction *tx);

void sfs_transaction_acquire_busy_bit(struct sfs_transaction *tx);
void sfs_transaction_release_busy_bit(struct sfs_transaction *tx);

/*
 * Create a new transaciton and assign it to curthread, if no current transaction exists.
 */
void sfs_current_transaction_add_record(struct sfs_record *, enum sfs_record_type);

void sfs_transaction_redo(struct sfs_transaction *tx);
void sfs_transaction_undo(struct sfs_transaction *tx);
