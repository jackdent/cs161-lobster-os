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

#define SWAP_DISK_PAGES 2000 // TODO: is there a good number to use?
#define DISK_OFFSET(index) (index * PAGE_SIZE)

struct vnode *swap_file;
struct bitmap *swap_map;
struct lock *swap_map_lock;


// Initialize the swap file, swap map, and swap map lock
// Panic if any fail
void swap_init(void);

// Find a free index in swap, acquire it, and return it
// Returns -1 if none are available.
int get_swap_index(void);

// Free the given swap index.
void free_swap_index(unsigned index);


// Write the page at swap_index on disk to the physical page pp
int swap_out(unsigned swap_index, paddr_t src);

// Read a page from swap_index into the page at dest
int swap_in(unsigned swap_index, paddr_t dest);

// Helpers
int write_page_to_disk(void *page, unsigned disk_offset);
int read_page_from_disk(void *page, unsigned offset);
