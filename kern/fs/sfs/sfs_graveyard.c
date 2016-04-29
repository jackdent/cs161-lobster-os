#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <sfs.h>
#include <synch.h>
#include "sfsprivate.h"
#include "sfs_graveyard.h"

static
struct sfs_vnode *
graveyard_get(struct sfs_fs *sfs)
{
        int err;
        struct sfs_vnode *graveyard;

        err = sfs_loadvnode(sfs, SFS_GRAVEYARD_INO, SFS_TYPE_INVAL, &graveyard);
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

        lock_acquire(graveyard->sv_lock);

        slot = -1;
        err = sfs_dir_findname(graveyard, "", NULL, NULL, &slot);
        if (err != ENOENT) {
                panic("Graveyard corrupted with empty string file?\n");
        }
        /* If we didn't get an empty slot, add the entry at the end. */
        else if (slot < 0) {
                err = sfs_dir_nentries(graveyard, &slot);
                if (err) {
                        panic("Could not find empty slot in graveyard\n");
                }
        }

        err = sfs_writedir(graveyard, slot, &sd);
        if (err) {
                panic("Could not add inode to graveyard\n");
        }

        lock_release(graveyard->sv_lock);
        sfs_reclaim(&graveyard->sv_absvn);
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

        /* Find the slot */

        graveyard = graveyard_get(sfs);

        lock_acquire(graveyard->sv_lock);

        err = sfs_dir_findname(graveyard, (const char *)&sd.sfd_name, &entry, &slot, NULL);
        if (err || slot < 0) {
                panic("Could not find slot when removing inode from graveyard\n");
        }

        KASSERT(entry == ino);

        /* Remove directory entry from graveyard */

        bzero(&sd, sizeof(sd));
        sd.sfd_ino = SFS_NOINO;

        err = sfs_writedir(graveyard, slot, &sd);
        if (err) {
                panic("Could not remove inode from graveyard");
        }

        lock_release(graveyard->sv_lock);
        sfs_reclaim(&graveyard->sv_absvn);
}

void
graveyard_flush(struct sfs_fs *sfs)
{
        int err, nentries, i;
        struct sfs_vnode *graveyard;
        struct sfs_direntry sd;
        struct sfs_vnode *sv;

        graveyard = graveyard_get(sfs);
        lock_acquire(graveyard->sv_lock);

        err = sfs_dir_nentries(graveyard, &nentries);
        if (err) {
                panic("Could not read slots while flushing graveyard\n");
        }

        /* For each slot... */
        for (i = 0; i < nentries; i++) {
                err = sfs_readdir(graveyard, i, &sd);
                if (err) {
                        panic("Could not read direntry from slot while flushing graveyard\n");
                }

                if (sd.sfd_ino != SFS_NOINO) {
                        err = sfs_loadvnode(sfs, sd.sfd_ino, 0, &sv);
                        if (err) {
                                panic("Could not load vnode for graveyard entry");
                        }

                        lock_release(graveyard->sv_lock);
                        sfs_reclaim(&sv->sv_absvn);
                        lock_acquire(graveyard->sv_lock);
                }
        }

        lock_release(graveyard->sv_lock);
        sfs_reclaim(&graveyard->sv_absvn);
}
