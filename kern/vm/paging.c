

// Find num free pages in memory, evicting if needed,
// and return with all of their busy bits
// held so that they are not evicted before the caller needs them
// for_kernel is true if this memory is for the kernel via kmalloc.
// We enforce that a kernel allocation live in contiguous memory if
// it occupies more than 1 page to make kfree()'ing easier.
// num should always be 1 if for_kernel == false. On success, result
// will store the address of the first page and 0 will be returned.
// Otherwise, the return value denotes the error
int find_free_pages(paddr_t *result, bool for_kernel, int num)
{
	KASSERT(for_kernel || num == 1);



}

int ensure_in_memory
