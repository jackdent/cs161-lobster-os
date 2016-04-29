#include <types.h>
#include <kern/errno.h>
#include <bitmap.h>
#include <lib.h>
#include "sfs_record.h"
#include "sfsprivate.h"
#include "buf.h"

sfs_lsn_t
sfs_record_write_to_journal(struct sfs_fs *fs, struct sfs_record *record, enum sfs_record_type type)
{
        return sfs_jphys_write(fs, NULL, NULL, type, record, sizeof(struct sfs_record));
}

struct sfs_record *
sfs_record_create_meta_update(daddr_t block, off_t pos, size_t len, char *old_value, char *new_value)
{
        struct sfs_record *record;
        struct sfs_meta_update *meta_update;

        record = kmalloc(sizeof(struct sfs_record));
        if (record == NULL) {
                return NULL;
        }

        meta_update = &record->data.meta_update;
        meta_update->block = block;
        meta_update->pos = pos;
        meta_update->len = len;
        memcpy((void*)meta_update->old_value, (void*)old_value, len);
        memcpy((void*)meta_update->new_value, (void*)new_value, len);

        /* set rest of buffers to 0 for debugging purposes */
        bzero((void*)meta_update->old_value + len, MAX_META_UPDATE_SIZE - len);
        bzero((void*)meta_update->new_value + len, MAX_META_UPDATE_SIZE - len);

        return record;
}

/*
 * Modified version of Fletcher's checksum, from Wikipedia
 */
static
uint32_t
sfs_record_user_data_checksum(char *data)
{
        uint32_t sum1, sum2;
        uint32_t mask;
        size_t i;

        sum1 = 0;
        sum2 = 0;
        mask = (1 << 16) - 1;

        for (i = 0; i < SFS_BLOCKSIZE; i++) {
                sum1 = (sum1 + data[i]) % mask;
                sum2 = (sum2 + sum1) % mask;
        }

        return (sum2 << 16) | sum1;
}

struct sfs_record *
sfs_record_create_user_block_write(daddr_t block, char *data)
{
        struct sfs_record *record;
        struct sfs_user_block_write *user_block_write;

        record = kmalloc(sizeof(struct sfs_record));
        if (record == NULL) {
                return NULL;
        }

        user_block_write = &record->data.user_block_write;
        user_block_write->block = block;
        user_block_write->checksum = sfs_record_user_data_checksum(data);

        return record;
}

/*
 * Assumes caller has reserved 1 buffer
 */
static
void
sfs_record_redo_user_block_write(struct sfs_fs *sfs, struct sfs_user_block_write user_block_write)
{
        int result;
        struct buf *buf;
        char *ioptr;
        uint32_t checksum;

        result = buffer_read(&sfs->sfs_absfs, user_block_write.block, SFS_BLOCKSIZE, &buf);
        if (result) {
                panic("Could not read from buffer associated with record\n");
        }

        ioptr = buffer_map(buf);
        checksum = sfs_record_user_data_checksum(ioptr);

        // If the data is stale
        if (checksum == user_block_write.checksum) {
                bzero(ioptr, SFS_BLOCKSIZE);
        }

        buffer_mark_dirty(buf);
        buffer_release(buf);
}

/*
 * Undo operations
 */

// TODO: what if someone else has already claimed the old slots?
// Will we just undo that claim when we step through the log?
// We can't quite do that, because the other transaction may have
// committed while this transaction may not have.

static
void
sfs_meta_update(struct sfs_fs *sfs, struct sfs_meta_update meta_update, bool redo)
{
        struct buf *buf;
        int err;
        void *ioptr;
        char *meta;

        err = buffer_read(&sfs->sfs_absfs, meta_update.block, SFS_BLOCKSIZE, &buf);
        if (err) {
                panic("Tried to undo meta update to invalid block\n");
        }

        ioptr = buffer_map(buf);

        if (redo) {
                meta = meta_update.new_value;
        } else {
                meta = meta_update.old_value;
        }

        memcpy(ioptr + meta_update.pos, meta, meta_update.len);

        buffer_mark_dirty(buf);
        buffer_release(buf);
}

static
void
sfs_freemap_update(struct sfs_fs *sfs, struct sfs_freemap_update freemap_update, bool capture)
{
        bool occupied;

        occupied = bitmap_isset(sfs->sfs_freemap, freemap_update.block);

        if (capture && !occupied) {
                bitmap_mark(sfs->sfs_freemap, freemap_update.block);
        } else if (!capture && occupied) {
                bitmap_unmark(sfs->sfs_freemap, freemap_update.block);
        }
}

void
sfs_record_undo(struct sfs_fs *sfs, struct sfs_record record, enum sfs_record_type record_type)
{
        switch (record_type) {
        case R_FREEMAP_CAPTURE:
                sfs_freemap_update(sfs, record.data.freemap_update, false);
                break;
        case R_FREEMAP_RELEASE:
                sfs_freemap_update(sfs, record.data.freemap_update, true);
                break;
        case R_META_UPDATE:
                sfs_meta_update(sfs, record.data.meta_update, false);
                break;
        case R_USER_BLOCK_WRITE:
        case R_TX_BEGIN:
        case R_TX_COMMIT:
                // NOOP
                break;
        default:
                panic("Undo unsupported for record type\n");
        }
}

void
sfs_record_redo(struct sfs_fs *sfs, struct sfs_record record, enum sfs_record_type record_type)
{
        switch (record_type) {
        case R_FREEMAP_CAPTURE:
                sfs_freemap_update(sfs, record.data.freemap_update, true);
                break;
        case R_FREEMAP_RELEASE:
                sfs_freemap_update(sfs, record.data.freemap_update, false);
                break;
        case R_META_UPDATE:
                sfs_meta_update(sfs, record.data.meta_update, true);
                break;
        case R_USER_BLOCK_WRITE:
                sfs_record_redo_user_block_write(sfs, record.data.user_block_write);
        case R_TX_BEGIN:
        case R_TX_COMMIT:
                // NOOP
                break;
        default:
                panic("Undo unsupported for record type\n");
        }
}
