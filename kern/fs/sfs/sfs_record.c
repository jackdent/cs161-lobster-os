#include <kern/errno.h>
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

/*
static
void
sfs_undo_inode_release(struct inode_release inode_release)
{
        // set inode_release.ino to inode_release.old_inode
}

static
void
sfs_undo_directory_remove(struct sfs_directory_remove directory_remove)
{
        // set directory_remove.slot to directory_remove.ino in directory_remove.dirno
}

static
void
sfs_undo_freemap_release(struct sfs_freemap_release freemap_release)
{
        // set freemap[freemap_release.block] to 1
}

void
sfs_record_undo(struct sfs_record record, enum sfs_record_type record_type)
{
        switch (record_type) {
        case R_INODE_RELEASE:
                sfs_undo_inode_release(record.r_parameters.inode_release);
                break;
        case R_DIRECTORY_REMOVE:
                sfs_undo_directory_remove(record.r_parameters.directory_remove);
                break;
        case R_FREEMAP_RELEASE:
                sfs_undo_freemap_release(record.r_parameters.freemap_release);
                break;
        default:
                panic("Undo unsupported for record type\n");
        }
}
*/
/*
 * Redo operations
 */
/*
static
void
sfs_redo_inode_release(struct inode_release inode_release)
{
        // mark inode_release.ino as free
}

static
void
sfs_redo_directory_remove(struct sfs_directory_remove directory_remove)
{
        // mark directory_remove.slot as free in directory_remove.dirno
}

static
void
sfs_redo_freemap_release(struct sfs_freemap_release freemap_release)
{
        // set freemap[freemap_release.block] to 0
}

void
sfs_record_redo(struct sfs_record record, enum sfs_record_type record_type)
{
        switch (record_type) {
        case R_INODE_RELEASE:
                sfs_redo_inode_release(record.r_parameters.inode_release);
                break;
        case R_DIRECTORY_REMOVE:
                sfs_redo_directory_remove(record.r_parameters.directory_remove);
                break;
        case R_FREEMAP_RELEASE:
                sfs_redo_freemap_release(record.r_parameters.freemap_release);
                break;
        default:
                panic("Redo unsupported for record type\n");
        }
}
*/
