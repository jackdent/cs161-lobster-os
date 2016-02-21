#include <types.h>
#include <lib.h>
#include <vnode.h>
#include <synch.h>

struct fd_file {
        struct vnode *fdf_vnode;
        struct lock *fdf_lock;
        int fdf_flags;
        int fdf_refcount;
        off_t fdf_offset;
};

struct fd_file *fd_file_create(struct vnode *vnode, int flags);
void fd_file_destroy(struct fd_file *file);
void fd_file_reference(struct fd_file *file);
bool fd_file_check_flag(struct fd_file *file, int);
void fd_file_release(struct fd_file *file);
