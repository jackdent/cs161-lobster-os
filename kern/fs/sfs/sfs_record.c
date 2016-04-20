#include "sfs_record.h"

sfs_lsn_t
sfs_record_write_to_journal(struct sfs_record *record, enum sfs_record_type type, struct sfs_fs *fs)
{
        return sfs_jphys_write(fs, NULL, NULL, type, record, sizeof(struct sfs_record));
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
