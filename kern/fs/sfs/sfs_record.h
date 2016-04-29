#include <types.h>
#include <sfs.h>
#include <kern/sfs.h>
#include <thread.h>
#include <current.h>
#include "sfsprivate.h"

struct sfs_record *sfs_record_create_meta_update(daddr_t block, off_t pos, size_t len, char *old_value, char *new_value);
struct sfs_record *sfs_record_create_user_block_write(daddr_t, char *);

/*
 * Journal operations
 */

sfs_lsn_t sfs_record_write_to_journal(struct sfs_fs *, struct sfs_record *, enum sfs_record_type);

/*
 * Recovery operations
 */

void sfs_record_undo(struct sfs_fs *, struct sfs_record, enum sfs_record_type);
void sfs_record_redo(struct sfs_fs *, struct sfs_record, enum sfs_record_type);
