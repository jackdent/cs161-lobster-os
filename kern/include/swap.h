#include <types.h>

#define SWAP_DISK_PAGES 2000 // ~8.2 MB
#define DISK_OFFSET(index) (index * PAGE_SIZE)

/*
 * An index for pages that is stable over their lifetime, assigned
 * the first time they are evicted from main memory to disk.
 */
typedef uint32_t swap_id_t;

struct vnode *swap_file;
struct bitmap *swap_map;
struct lock *swap_map_lock;

/*
 * Initialize the swap file, swap map, and swap map lock.
 * Panic if any fail.
 */
void swap_init(void);

/*
 * Find, acquire, and return a free index in swap.
 */
swap_id_t swap_capture_slot(void);

/*
 * Free the given swap index.
 */
void swap_free_slot(swap_id_t slot);

/*
 * Write the page at swap_index on disk to the physical page pp.
 */
void swap_out(swap_id_t index, paddr_t src);

/*
 * Read a page from swap_index into the page at dest.
 */
void swap_in(swap_id_t index, paddr_t dest);

/*
 * Copy the memory from one slot in the swap space to another.
 */
int swap_copy(swap_id_t from, swap_id_t to);
