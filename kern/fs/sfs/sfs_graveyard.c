#include <types.h>
#include <lib.h>
#include <sfs.h>
#include "sfsprivate.h"
#include "sfs_graveyard.h"

static
struct sfs_vnode *
graveyard_get(struct sfs_fs *sfs)
{
        int err;
        struct sfs_vnode *graveyard;

        err = sfs_loadvnode(sfs, SFS_GRAVEYARD_INO, SFS_TYPE_DIR, &graveyard);
        if (err) {
                panic("Could not load graveyard\n");
        }

        return graveyard;
}

/*
 * The corresponding dinode should have linkcount == 0
 */
void
graveyard_add(struct sfs_fs *sfs, uint32_t ino)
{
        int err, slot;
        struct sfs_direntry sd;
        struct sfs_vnode *graveyard;

        /* Initialize directory entry */

        bzero(&sd, sizeof(sd));
        sd.sfd_ino = ino;
        snprintf(sd.sfd_name, SFS_NAMELEN, "%d", ino);

        /* Write entry to graveyard */

        graveyard = graveyard_get(sfs);

        err = sfs_dir_findname(graveyard, NULL, NULL, NULL, &slot);
        if (err || slot < 0) {
                panic("Could not find empty slot in graveyard\n");
        }

        err = sfs_writedir(graveyard, slot, &sd);
        if (err) {
                panic("Could not add inode to graveyard");
        }
}

void
graveyard_remove(struct sfs_fs *sfs, uint32_t ino)
{
        int err, slot;
        struct sfs_direntry sd;
        uint32_t entry;
        struct sfs_vnode *graveyard;

        /* Initialize directory entry */

        bzero(&sd, sizeof(sd));
        sd.sfd_ino = SFS_NOINO;
        // Use directory entry buffer to store name
        snprintf(sd.sfd_name, SFS_NAMELEN, "%d", ino);

        /* Remove directory entry from graveyard */

        graveyard = graveyard_get(sfs);

        err = sfs_dir_findname(graveyard, (const char *)&sd.sfd_name, &entry, &slot, NULL);
        if (err || slot < 0) {
                panic("Could not find slot when removing inode from graveyard\n");
        }

        KASSERT(entry == ino);

        err = sfs_writedir(graveyard, slot, &sd);
        if (err) {
                panic("Could not remove inode from graveyard");
        }
}
