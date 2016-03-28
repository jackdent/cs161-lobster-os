#include <types.h>
#include <vnode.h>
#include <vfs.h>
#include <bitmap.h>
#include <uio.h>
#include <swap.h>

void
swap_init(void)
{
	int err;
	char *swap_disk_path;

	swap_disk_path = kstrdup("lhd0raw:");
	if (swap_disk_path == NULL) {
		panic("swap_init: could not open swap disk");
	}

	// TODO: use vfs_swapon
	err = vfs_open(swap_disk_path, O_RDWR, 0, &swap_file);
	if (err) {
		panic("swap_init: could not open swap disk");
	}

	swap_map = bitmap_create(SWAP_DISK_PAGES);
	if (swap_map == NULL) {
		panic("swap_init: could not create swap disk map");
	}

	swap_map_lock = lock_create("swap map lock");
	if (swap_map_lock == NULL) {
		panic("swap_init: could not create disk map lock");
	}

	kfree(swap_disk_path);
}

swap_id_t
swap_capture_slot(void)
{
	swap_id_t index;
	int result;

	lock_acquire(swap_map_lock);
	result = bitmap_alloc(swap_map, &index);
	lock_release(swap_map_lock);

	if (result) {
		panic("Ran out of swap space!");
	} else {
		return index;
	}
}

// Free the given swap index.
void
swap_free_slot(swap_id_t slot)
{
	lock_acquire(swap_map_lock);

	KASSERT(bitmap_isset(swap_map, slot));
	bitmap_unmark(swap_map, slot);

	lock_release(swap_map_lock);
}

// Helpers
static
int
write_page_to_disk(void *page, swap_id_t disk_offset)
{
	struct iovec iov;
	struct uio u;

	uio_kinit(&iov, &u, (char*)page, PAGE_SIZE, disk_offset, UIO_WRITE);
	return VOP_WRITE(swap_file, &u);
}

static
int
read_page_from_disk(void *page, swap_id_t disk_offset)
{
	struct iovec iov;
	struct uio u;

	uio_kinit(&iov, &u, (char*)page, PAGE_SIZE, disk_offset, UIO_READ);
	return VOP_READ(swap_file, &u);
}

// Write the page at swap_index on disk to the physical page pp
int
swap_out(swap_id_t swap_index, paddr_t src)
{
	int ret;

	ret = write_page_to_disk((void*)PADDR_TO_KVADDR(src), swap_index * PAGE_SIZE);
	if (!ret) {
		// TODO
	}
	return ret;
}

// Read a page from swap_index into the page at dest
int
swap_in(swap_id_t swap_index, paddr_t dest)
{
	int ret;

	ret = read_page_from_disk((void *)PADDR_TO_KVADDR(dest), swap_index * PAGE_SIZE);

	if (!ret) {
		// TODO
	}
	return ret;
}
