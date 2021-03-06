#include <types.h>
#include <vnode.h>
#include <vfs.h>
#include <bitmap.h>
#include <uio.h>
#include <swap.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <synch.h>
#include <machine/vm.h>
#include <coremap.h>
#include <kern/stat.h>

void
swap_init(void)
{
	int err;
	char *swap_disk_path;
	struct stat stat;

	swap_disk_path = kstrdup("lhd0:");
	if (swap_disk_path == NULL) {
		panic("swap_init: could not open swap disk");
	}

	err = vfs_swapon(swap_disk_path, &swap.swap_file);
	if (err) {
		panic("swap_init: could not open swap disk");
	}

	err = VOP_STAT(swap.swap_file, &stat);
	if (err) {
		panic("swap_init: could not open swap disk");
	}
	swap.swap_slots = stat.st_size / PAGE_SIZE;
	coremap.cm_total_pages += swap.swap_slots;

	swap.swap_map = bitmap_create(swap.swap_slots);
	if (swap.swap_map == NULL) {
		panic("swap_init: could not create swap disk map");
	}

	swap.swap_map_lock = lock_create("swap map lock");
	if (swap.swap_map_lock == NULL) {
		panic("swap_init: could not create disk map lock");
	}

	kfree(swap_disk_path);
}

swap_id_t
swap_capture_slot(void)
{
	swap_id_t index;
	int err;

	lock_acquire(swap.swap_map_lock);
	err = bitmap_alloc(swap.swap_map, &index);
	lock_release(swap.swap_map_lock);

	if (err) {
		panic("Ran out of swap space!");
	} else {
		return index;
	}
}

// Free the given swap index.
void
swap_free_slot(swap_id_t slot)
{
	lock_acquire(swap.swap_map_lock);

	(void)slot;
	KASSERT(bitmap_isset(swap.swap_map, slot));
	bitmap_unmark(swap.swap_map, slot);

	lock_release(swap.swap_map_lock);
}

// Helpers
static
int
write_page_to_disk(void *page, swap_id_t swap_index)
{
	struct iovec iov;
	struct uio u;

	uio_kinit(&iov, &u, (char*)page, PAGE_SIZE, DISK_OFFSET(swap_index), UIO_WRITE);
	return VOP_WRITE(swap.swap_file, &u);
}

static
int
read_page_from_disk(void *page, swap_id_t swap_index)
{
	struct iovec iov;
	struct uio u;

	uio_kinit(&iov, &u, (char*)page, PAGE_SIZE, DISK_OFFSET(swap_index), UIO_READ);
	return VOP_READ(swap.swap_file, &u);
}

// Write the page at src to the disk at swap_index
void
swap_out(swap_id_t swap_index, cme_id_t src)
{
	int err;

	err = write_page_to_disk((void*)PADDR_TO_KVADDR(CME_ID_TO_PA(src)), swap_index);
	if (err != 0) {
		// Nothing else we can really do here
		panic("Disk error when writing from RAM to swap\n");
	}
}

// Read the page from swap_index on disk into the page at dest
void
swap_in(swap_id_t swap_index, cme_id_t dest)
{
	int err;

	err = read_page_from_disk((void*)PADDR_TO_KVADDR(CME_ID_TO_PA(dest)), swap_index);

	if (err != 0) {
		// Nothing else we can really do here
		panic("Disk error when reading from swap to RAM\n");
	}
}

int
swap_copy(swap_id_t from, swap_id_t to)
{
	int err;
	void *buf;

	buf = kmalloc(PAGE_SIZE);
	if (buf == NULL) {
		return ENOMEM;
	}

	err = read_page_from_disk((void*)buf, from);
	if (err != 0) {
		panic("Disk error when reading from swap to RAM\n");
	}

	err = write_page_to_disk((void*)buf, to);
	if (err != 0) {
		panic("Disk error when writing from RAM to swap\n");
	}

	return 0;
}
