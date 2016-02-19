#include <fdfile.h>
#include <vfs.h>

struct fd_file *
fd_file_create(struct vnode *vnode, int flags)
{
        struct fd_file *file;
        struct lock *lock;

        KASSERT(vnode != NULL);

        file = kmalloc(sizeof(struct fd_file));
        if (file == NULL) {
                return NULL;
        }

        lock = lock_create("file");
        if (lock == NULL) {
                kfree(file);
                return NULL;
        }

        file->fdf_vnode = vnode;
        file->fdf_lock = lock;
        file->fdf_flags = flags;
        file->fdf_offset = 0;
        file->fdf_refcount = 1;

        return file;
}

void
fd_file_reference(struct fd_file *file)
{
        lock_acquire(file->fdf_lock);
        file->fdf_refcount++;
        lock_release(file->fdf_lock);
}

static
void
fd_file_destroy(struct fd_file *file)
{
        vfs_close(file->fdf_vnode);
        lock_destroy(file->fdf_lock);
        kfree(file);
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

