#include <types.h>

/*
 * Evict a page from main memory to disk, if necessary.
 *
 * Assumes that the caller holds the core map entry lock.
 */
void evict_page(cme_id_t cme_id);
