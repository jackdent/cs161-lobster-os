#include <addrspace.h>

void tlb_add(vaddr_t va, struct pte *pte);
void tlb_make_writeable(vaddr_t va, struct pte *pte);
void tlb_remove(vaddr_t va);
void tlb_flush(void);
