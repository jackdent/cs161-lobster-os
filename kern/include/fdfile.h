#include <types.h>
#include <lib.h>
#include <vnode.h>
#include <synch.h>

struct fd_file {
        struct vnode *fdf_vnode;
        struct lock *fdf_lock;
        int fdf_flags;
        int fdf_offset;
        int fdf_refcount;
};

struct fd_file * fd_file_create(struct vnode *vnode, int flags);
void fd_file_release(struct fd_file *file);
