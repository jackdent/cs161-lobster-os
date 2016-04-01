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

	// Set all pointers to NULL initially
	memset(fd_table->fdt_table, 0, sizeof(fd_table->fdt_table));

	fd_table->fdt_lock = lock_create("fd_table lock");

	return fd_table;
};

void
fd_table_destroy(struct fd_table *fd_table)
{
        KASSERT(fd_table != NULL);

        lock_destroy(fd_table->fdt_lock);
        kfree(fd_table);
}

bool
fd_in_range(int fd)
{
        return fd >= 0 && fd < FD_MAX;
}

// Expects caller to hold fd_table lock
bool
valid_fd(struct fd_table *fd_table, int fd)
{
        return fd_in_range(fd) && fd_table->fdt_table[fd] != NULL;
}

int
add_file_to_fd_table(struct fd_table *fd_table, struct fd_file *file)
{
        KASSERT(fd_table != NULL);
        KASSERT(file != NULL);

        lock_acquire(fd_table->fdt_lock);

        for (int i = 0; i < FD_MAX; i++) {
                if (fd_table->fdt_table[i] == NULL) {
                        fd_table->fdt_table[i] = file;
                        lock_release(fd_table->fdt_lock);
                        return i;
                }
        }

        lock_release(fd_table->fdt_lock);
        return -1;
}

struct fd_file *
get_file_from_fd_table(struct fd_table *fd_table, int fd)
{
        struct fd_file *file;

        KASSERT(fd_table != NULL);

        if (fd_in_range(fd)) {
                lock_acquire(fd_table->fdt_lock);
                file = fd_table->fdt_table[fd];
                lock_release(fd_table->fdt_lock);
                return file;
        }

        return NULL;
}

void
clone_fd_table(struct fd_table *src, struct fd_table *dest)
{
        KASSERT(src != NULL);
        KASSERT(dest != NULL);

        lock_acquire(src->fdt_lock);
        lock_acquire(dest->fdt_lock);

        for (int i = 0; i < FD_MAX; i++) {

                if (src->fdt_table[i] != NULL) {
                        dest->fdt_table[i] = src->fdt_table[i];
                        fd_file_reference(src->fdt_table[i]);
                }
        }

        lock_release(src->fdt_lock);
        lock_release(dest->fdt_lock);
}

int
release_fd_from_fd_table(struct fd_table *fd_table, int fd)
{
        int result;

        KASSERT(fd_table != NULL);

        lock_acquire(fd_table->fdt_lock);

        if (valid_fd(fd_table, fd)) {
                fd_file_release(fd_table->fdt_table[fd]);
                fd_table->fdt_table[fd] = NULL;
                result = 0;
        } else {
                result = EBADF;
        }

        lock_release(fd_table->fdt_lock);
        return result;
}
