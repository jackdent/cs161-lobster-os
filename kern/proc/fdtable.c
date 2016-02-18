#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <fdtable.h>

struct fd_table *
fd_table_create()
{
        struct fd_table *fd_table;

        fd_table = kmalloc(sizeof(struct fd_table));
        if (fd_table == NULL) {
                return NULL;
        }

        return fd_table;
};

void
fd_table_destroy(struct fd_table *fd_table)
{
        KASSERT(fd_table != NULL);

        spinlock_cleanup(&fd_table->fdt_spinlock);
        kfree(fd_table);
}

int
add_file_to_fd_table(struct fd_table *fd_table, struct fd_file *file)
{
        KASSERT(fd_table != NULL);
        KASSERT(file != NULL);

        spinlock_acquire(&fd_table->fdt_spinlock);

        for (int i = 0; i < FD_MAX; ++i) {
                if (fd_table->fdt_table[i] == NULL) {
                        fd_table->fdt_table[i] = file;
                        spinlock_release(&fd_table->fdt_spinlock);
                        return i;
                }
        }

        spinlock_release(&fd_table->fdt_spinlock);
        return -1;
}

int
release_fd_from_fd_table(struct fd_table *fd_table, int fd)
{
        if (fd < 0 || fd >= FD_MAX || fd_table->fdt_table[fd] == NULL) {
                return EBADF;
        } else {
                spinlock_acquire(&fd_table->fdt_spinlock);
                fd_file_release(fd_table->fdt_table[fd]);
                fd_table->fdt_table[fd] = NULL;
                spinlock_release(&fd_table->fdt_spinlock);
                return 0;
        }
}
