#include <types.h>
#include <sfs.h>
#include <thread.h>
#include <current.h>
#include "sfs_transaction.h"

/*
 * Record Schema
 */

enum sfs_record_type {
        R_TX_BEGIN,
        R_TX_COMMIT,

        R_INODE_CAPTURE,
        R_INODE_RESIZE,
        R_INODE_UPDATE_SLOT,
        R_INODE_RELEASE,

        R_DIRECTORY_ADD,
        R_DIRECTORY_REMOVE,

        R_FREEMAP_CAPTURE,
        R_FREEMAP_RELEASE,

        R_USER_BLOCK_WRITE
};

struct sfs_update_directory {
        uint32_t parent_ino;
        int slot;
        uint32_t ino;
        char name[SFS_NAMELEN];
};

struct sfs_meta_update_word {
        daddr_t block;
        off_t pos;
        uint32_t old_value;
        uint32_t new_value;
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
        enum sfs_record_type r_type;
        union {
                struct sfs_update_directory update_directory;
                struct sfs_meta_update_word meta_update_word;
                struct sfs_user_block_write userblock_write;
        } r_parameters;
};

/*
 * Record creation
 */

struct sfs_record *sfs_create_dir_record(uint32_t parent_ino, int slot, uint32_t ino);
struct sfs_record *sfs_create_commit_record(void);


/*
 * Recovery operations
 */

void sfs_record_undo(struct sfs_record, enum sfs_record_type);
void sfs_record_redo(struct sfs_record, enum sfs_record_type);
