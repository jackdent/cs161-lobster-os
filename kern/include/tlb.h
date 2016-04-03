#include <machine/vm.h>
#include <cme.h>
#include <addrspace.h>

/*
 * TLB shootdown handling called from interprocessor_interrupt.
 */
void vm_tlbshootdown(const struct tlbshootdown *);

/*
 * Fault handling function called by trap code.
 */
int vm_fault(int faulttype, vaddr_t faultaddress);

/*
 * Assumes that the caller holds the core map entry lock.
 */
void tlb_set_writeable(vaddr_t va, cme_id_t cme_id, bool writeable);

void tlb_remove(vaddr_t va);
void tlb_flush(void);
