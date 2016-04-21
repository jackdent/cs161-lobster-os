#include <kern/errno.h>
#include <bitmap.h>
#include "sfs_record.h"
#include "sfsprivate.h"
#include "buf.h"

sfs_lsn_t
sfs_record_write_to_journal(struct sfs_record *record, enum sfs_record_type type, struct sfs_fs *fs)
{
        return sfs_jphys_write(fs, NULL, NULL, type, record, sizeof(struct sfs_record));
}

int
sfs_record_linkcount_change(struct sfs_vnode *vnode, struct sfs_dinode *dinode, int old_linkcount, int new_linkcount)
{

        struct sfs_record *record;
        struct sfs_meta_update *meta_update;

        record = kmalloc(sizeof(struct sfs_record));
        if (record == NULL) {
                return ENOMEM;
        }

        meta_update = &record->r_parameters.meta_update;

        meta_update->block = buffer_get_block_number(vnode->sv_dinobuf);
        meta_update->pos = (void*)&dinode->sfi_linkcount - (void*)dinode;
        meta_update->len = sizeof(uint32_t);
        memcpy((void*)meta_update->old_value, (void*)&old_linkcount, sizeof(uint32_t));
        memcpy((void*)meta_update->new_value, (void*)&new_linkcount, sizeof(uint32_t));

        sfs_current_transaction_add_record(record, R_META_UPDATE);

        return 0;
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
sfs_meta_update(struct fs *fs, struct sfs_meta_update meta_update, bool redo)
{
        struct buf *buf;
        int err;
        void *ptr;
        char *meta;

        err = buffer_read(fs, meta_update.block, SFS_BLOCKSIZE, &buf);
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

// TODO: after recovery, flush freemap and buffer

void
sfs_record_undo(struct fs *fs, struct sfs_record record, enum sfs_record_type record_type)
{
        struct sfs_fs *sfs;

        sfs = fs->fs_data;

        switch (record_type) {
        case R_FREEMAP_CAPTURE:
                bitmap_unmark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                break;
        case R_FREEMAP_RELEASE:
                bitmap_mark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                break;
        case R_META_UPDATE:
                sfs_meta_update(fs, record.r_parameters.meta_update, false);
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
sfs_record_redo(struct fs *fs, struct sfs_record record, enum sfs_record_type record_type)
{
        struct sfs_fs *sfs;

        sfs = fs->fs_data;

        switch (record_type) {
        case R_FREEMAP_CAPTURE:
                bitmap_mark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                break;
        case R_FREEMAP_RELEASE:
                bitmap_unmark(sfs->sfs_freemap, record.r_parameters.freemap_update.block);
                break;
        case R_META_UPDATE:
                sfs_meta_update(fs, record.r_parameters.meta_update, true);
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
