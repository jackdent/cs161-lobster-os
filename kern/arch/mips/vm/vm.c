#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <coremap.h>

void
vm_bootstrap(void)
{
        cm_init();
}

vaddr_t
alloc_kpages(unsigned npages)
{
        (void)npages;

        /* TODO */
        return 0;
}

void
free_kpages(vaddr_t addr)
{
        (void)addr;

        /* TODO */
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
        (void)ts;

        /* TODO */
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
        (void)faulttype; (void)faultaddress;

        /* TODO */
        return 0;
}
