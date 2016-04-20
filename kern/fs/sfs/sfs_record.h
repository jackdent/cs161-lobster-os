#include <types.h>
#include <sfs.h>
#include <thread.h>
#include <current.h>
#include "sfs_transaction.h"

#define MAX_META_UPDATE_SIZE 128

/*
 * Record Schema
 */

enum sfs_record_type {
        R_TX_BEGIN,
        R_TX_COMMIT,

        R_FREEMAP_CAPTURE,
        R_FREEMAP_RELEASE,

        R_META_UPDATE,
        R_USER_BLOCK_WRITE
};

struct sfs_meta_update {
        daddr_t block;
        off_t pos;
        size_t len;
        char old_value[MAX_META_UPDATE_SIZE];
        char new_value[MAX_META_UPDATE_SIZE];
};

struct sfs_user_block_write {
        daddr_t block;
        off_t pos;
        size_t len;
        uint32_t checksum;
};

// Journal record (directly serialized)

struct sfs_record {
        txid_t r_txid;
        union {
                struct sfs_meta_update meta_update;
                struct sfs_user_block_write userblock_write;
        } r_parameters;
};

sfs_lsn_t sfs_record_write_to_journal(struct sfs_record, enum sfs_record_type, struct sfs_fs *);

/*
 * Recovery operations
 */

void sfs_record_undo(struct sfs_record, enum sfs_record_type);
void sfs_record_redo(struct sfs_record, enum sfs_record_type);
