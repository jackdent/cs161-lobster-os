#include <types.h>
#include <sfs.h>
#include <thread.h>
#include <current.h>
#include "sfs_transaction.h"

/*
 * Record Schema
 */

// Record types

enum sfs_record_type {
        R_TX_BEGIN,
        R_TX_COMMIT,

        R_INODE_CAPTURE,
        R_INODE_UPDATE,
        R_INODE_RELEASE,

        R_DIRECTORY_ADD,
        R_DIRECTORY_REMOVE,

        R_BLOCK_WRITE,

        R_FREEMAP_CAPTURE,
        R_FREEMAP_RELEASE
};

// inode records

struct sfs_inode_capture {
        uint32_t ino;
};

// May need to split this up into link/unlink,
// increase/decrease size, and add/remove block
struct sfs_inode_update {
        uint32_t ino;
        struct sfs_dinode old_inode;
        struct sfs_dinode new_inode;
};

struct sfs_inode_release {
        uint32_t ino;
        struct sfs_dinode old_inode;
};

// Directory records

// adding or removing
struct sfs_directory {
        uint32_t parent_ino;
        int slot;
        uint32_t ino;
};

// Block records

struct sfs_block_write {
        daddr_t block;
        off_t pos;
        size_t len;
        uint32_t checksum;
};

// Freemap records

struct sfs_freemap_capture {
        uint32_t block;
};

struct sfs_freemap_release {
        uint32_t block;
};

// Journal record (directly serialized)

struct sfs_record {
        txid_t r_txid;
        enum sfs_record_type r_type;
        union {
                struct sfs_inode_capture inode_capture;
                struct sfs_inode_update inode_update;
                struct sfs_inode_release inode_release;
                struct sfs_directory directory;
                struct sfs_block_write block_write;
                struct sfs_freemap_capture freemap_capture;
                struct sfs_freemap_release freemap_release;
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

void sfs_record_undo(struct sfs_record record, enum sfs_record_type);
void sfs_record_redo(struct sfs_record record, enum sfs_record_type);
