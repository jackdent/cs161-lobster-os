#include <types.h>
#include <kern/errno.h>
#include <bitmap.h>
#include "sfs_record.h"
#include "sfsprivate.h"
#include "buf.h"

sfs_lsn_t
sfs_record_write_to_journal(struct sfs_fs *fs, struct sfs_record *record, enum sfs_record_type type)
{
        return sfs_jphys_write(fs, NULL, NULL, type, record, sizeof(struct sfs_record));
}

struct sfs_record *
sfs_record_create_metadata(daddr_t block, off_t pos, size_t len, char *old_value, char *new_value)
{
        struct sfs_record *record;
        struct sfs_meta_update *meta_update;

        record = kmalloc(sizeof(struct sfs_record));
        if (record == NULL) {
                return NULL;
        }

        meta_update = &record->r_parameters.meta_update;

        meta_update->block = block;
        meta_update->pos = pos;
        meta_update->len = len;
        memcpy((void*)meta_update->old_value, (void*)&old_value, len);
        memcpy((void*)meta_update->new_value, (void*)&new_value, len);

        return record;
}

/*
 * Modified version of Fletcher's checksum, from Wikipedia
 */
static
uint32_t
sfs_record_user_data_checksum(char *data, size_t len)
{
        KASSERT(len < SFS_BLOCKSIZE);

        uint32_t sum1, sum2;
        uint32_t mask;
        size_t i;

        sum1 = 0;
        sum2 = 0;
        mask = (1 << 16) - 1;

        for (i = 0; i < len; i++) {
                sum1 = (sum1 + data[i]) % mask;
                sum2 = (sum2 + sum1) % mask;
        }

        return (sum2 << 16) | sum1;
}

/*
 * Assumes caller has reserved 1 buffer
 */
bool
sfs_record_check_user_block_write(struct sfs_fs *sfs, struct sfs_record *record)
{
        int result;
        struct sfs_user_block_write *user_block_write;
        struct buf *iobuffer;
        char *ioptr;
        uint32_t checksum;

        user_block_write = &record->r_parameters.user_block_write;

        result = buffer_read(&sfs->sfs_absfs, user_block_write->block, SFS_BLOCKSIZE, &iobuffer);
        if (result) {
                panic("Could not read from buffer associated with record\n");
        }

        ioptr = buffer_map(iobuffer);
        checksum = sfs_record_user_data_checksum(ioptr + user_block_write->pos, user_block_write->len);

        buffer_release(iobuffer);

        return checksum == user_block_write->checksum;
}

struct sfs_record *
sfs_record_create_user_block_write(daddr_t block, off_t pos, size_t len, char *data)
{
        KASSERT(pos < SFS_BLOCKSIZE);
        KASSERT(len < SFS_BLOCKSIZE);

        struct sfs_record *record;
        struct sfs_user_block_write *user_block_write;

        record = kmalloc(sizeof(struct sfs_record));
        if (record == NULL) {
                return NULL;
        }

        user_block_write = &record->r_parameters.user_block_write;

        user_block_write->block = block;
        user_block_write->pos = pos;
        user_block_write->len = len;

        user_block_write->checksum = sfs_record_user_data_checksum(data, len);

        return record;
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
        void *ptr;
        char *meta;

        err = buffer_read(&sfs->sfs_absfs, meta_update.block, SFS_BLOCKSIZE, &buf);
        if (err) {
                panic("Tried to undo meta update to invalid block\n");
        }

        ptr = buffer_map(buf);

        if (redo) {
                meta = meta_update.new_value;
        } else {
                meta = meta_update.old_value;
        }

        memcpy(meta, ptr + meta_update.pos, meta_update.len);

        buffer_release(buf);
}

void
sfs_record_undo(struct sfs_fs *sfs, struct sfs_record record, enum sfs_record_type record_type)
{
        switch (record_type) {
        case R_FREEMAP_CAPTURE:
                bitmap_unmark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                break;
        case R_FREEMAP_RELEASE:
                bitmap_mark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                break;
        case R_META_UPDATE:
                sfs_meta_update(sfs, record.r_parameters.meta_update, false);
                break;
        case R_TX_BEGIN:
        case R_TX_COMMIT:
                // NOOP
                break;
        case R_USER_BLOCK_WRITE:
                panic("Tried to undo user write\n");
        default:
                panic("Undo unsupported for record type\n");
        }
}

void
sfs_record_redo(struct sfs_fs *sfs, struct sfs_record record, enum sfs_record_type record_type)
{
        switch (record_type) {
        case R_FREEMAP_CAPTURE:
                bitmap_mark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                // TODO: we need to call sfs_clearblock(sfs, block, NULL)
                // here, right? How this be undone?
                break;
        case R_FREEMAP_RELEASE:
                bitmap_unmark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                break;
        case R_META_UPDATE:
                sfs_meta_update(sfs, record.r_parameters.meta_update, true);
                break;
        case R_TX_BEGIN:
        case R_TX_COMMIT:
                // NOOP
                break;
        case R_USER_BLOCK_WRITE:
                panic("Tried to redo user write\n");
        default:
                panic("Undo unsupported for record type\n");
        }
}
