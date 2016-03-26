#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <synch.h>

#define SWAP_DISK_PAGES 2000 // TODO: assign this based on size of disk
#define DISK_OFFSET(index) (index * PAGE_SIZE)

/* An index for pages that is stable over their lifetime, assigned the first
   time they are evicted from main memory to disk */
typedef swap_id_t unsigned uint32_t;

struct vnode *swap_file;
struct bitmap *swap_map;
struct lock *swap_map_lock;

// Initialize the swap file, swap map, and swap map lock
// Panic if any fail
void swap_init(void);

// Find, acquire, and return a free index in swap.
swap_id_t get_free_swap_index(void);

// Free the given swap index.
void free_swap_index(swap_id_t index);

// Write the page at swap_index on disk to the physical page pp
int swap_out(swap_id_t index, paddr_t src);

// Read a page from swap_index into the page at dest
int swap_in(swap_id_t index, paddr_t dest);

// Helpers
int write_page_to_disk(void *page, swap_id_t disk_offset);
int read_page_from_disk(void *page, swap_id_t offset);
