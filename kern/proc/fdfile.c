#include <fdfile.h>
#include <vfs.h>
#include <kern/fcntl.h>

struct fd_file *
fd_file_create(struct vnode *vnode, int flags)
{
        struct fd_file *file;
        struct lock *lock;

        KASSERT(vnode != NULL);

        file = kmalloc(sizeof(struct fd_file));
        if (file == NULL) {
                goto err1;
        }

        lock = lock_create("fd_file");
        if (lock == NULL) {
                goto err2;
        }

        file->fdf_vnode = vnode;
        file->fdf_lock = lock;
        file->fdf_flags = flags;
        file->fdf_offset = 0;
        file->fdf_refcount = 1;

        return file;


        err2:
                kfree(file);
        err1:
                return NULL;
}

void
fd_file_destroy(struct fd_file *file)
{
        vfs_close(file->fdf_vnode);
        lock_destroy(file->fdf_lock);
        kfree(file);
}

void
fd_file_reference(struct fd_file *file)
{
        lock_acquire(file->fdf_lock);
        file->fdf_refcount++;
        lock_release(file->fdf_lock);
}

bool
fd_file_check_flag(struct fd_file *file, int flag)
{
        return (file->fdf_flags & RD_FLAG_MASK) == flag;
}

void
fd_file_release(struct fd_file *file)
{
        lock_acquire(file->fdf_lock);

        if (file->fdf_refcount == 1) {
                lock_release(file->fdf_lock);
                fd_file_destroy(file);
        } else {
                file->fdf_refcount--;
                lock_release(file->fdf_lock);
        }
}

