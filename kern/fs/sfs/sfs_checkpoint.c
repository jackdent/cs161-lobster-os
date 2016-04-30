#include <buf.h>
#include <limits.h>
#include <synch.h>
#include <types.h>
#include <thread.h>
#include "sfs_transaction.h"
#include "sfs_checkpoint.h"

static
void
checkpoint(struct sfs_fs *fs)
{
	unsigned i;
	sfs_lsn_t min_buf_lowest_lsn, min_tx_lowest_lsn;
	struct sfs_transaction *tx;
	struct sfs_transaction_set *tx_set;

	KASSERT(fs != NULL);

	/* Step 1: Find minimum lowest_lsn across all buffers */

	min_buf_lowest_lsn = buffer_get_min_low_lsn(&fs->sfs_absfs);

	/* Step 2: Remove any transactions we know made it to disk
	and find the minimum tx_low_lsn across all remaining */

	tx_set = fs->sfs_transaction_set;

	lock_acquire(tx_set->tx_lock);

	min_tx_lowest_lsn = ULLONG_MAX;
	for (i = 0; i < MAX_TRANSACTIONS; i++) {
		tx = tx_set->tx_transactions[i];

		if (tx == NULL) {
			continue;
		}

		if (tx->tx_committed && tx->tx_highest_lsn < min_buf_lowest_lsn) {
			sfs_transaction_destroy(tx);
		}
		else if (tx->tx_lowest_lsn < min_tx_lowest_lsn) {
			min_tx_lowest_lsn = tx->tx_lowest_lsn;
		}
	}

	lock_release(tx_set->tx_lock);

	/* Step 3: Trim journal records before min_tx_lowest_lsn */
	if (min_buf_lowest_lsn != ULLONG_MAX) {
		sfs_jphys_trim(fs, min_buf_lowest_lsn);
	}
}

void
checkpoint_thread(void *data1, unsigned long data2)
{
	struct sfs_fs *fs;

	(void)data2;

	fs = (struct sfs_fs *) data1;
	/* we checkpoint when the journal is > 1/4 full */

	while (1) {
		if (fs->sfs_checkpoint_exit) {
			checkpoint(fs);
			/* tell unmounter that we got the message */
			fs->sfs_checkpoint_exit = 0;
			thread_exit();
		}
		checkpoint(fs);
		thread_yield();
	}
}
