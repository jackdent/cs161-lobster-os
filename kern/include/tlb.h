#include <machine/vm.h>
#include <pagetable.h>

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown *);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

void tlb_add(vaddr_t va, struct pte *pte);
void tlb_make_writeable(vaddr_t va, struct pte *pte);
void tlb_remove(vaddr_t va);
void tlb_flush(void);
