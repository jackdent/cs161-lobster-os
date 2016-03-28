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
#include <pagetable.h>
#include <coremap.h>

void
vm_bootstrap(void)
{
        cm_init();
}

vaddr_t
alloc_kpages(unsigned npages)
{
        cme_id_t start, curr;
        vaddr_t addr;
        struct cme cme;

        start = cm_capture_slots_for_kernel(npages);

        while (int i = 0; i < npages; ++i) {
                curr = start + i;

                evict_page(curr);

                // We don't really need to do this because kernel
                // memory is directy mapped, but it may be helpful
                // when debugging
                addr = CME_ID_TO_PA(curr);
                KASSERT(addr >= MIPS_KSEG0);

                cme = cme_create(kproc->p_pid, addr, S_KERNEL);

                coremap.cmes[curr] = cme;
        }

        cm_release_locks(start, start + nslots);

        return CME_ID_TO_PA(start);
}

void
free_kpages(vaddr_t addr)
{
        (void)addr;

        /* TODO */
}

void
map_upages(struct pagetable *pt, vaddr_t start, unsigned int npages)
{
        KASSERT(start % PAGE_SIZE == 0);

        unsigned int i;
        struct pte *pte;

        for (i = 0; i < npages; ++i) {
                pte = pagetable_get_pte_from_va(start + i * PAGE_SIZE);
                pte->pte_state = S_LAZY;
        }
}

void
unmap_upages(struct pagetable *pt, vaddr_t start, unsigned int npages)
{
        KASSERT(start % PAGE_SIZE == 0);

        unsigned int i;
        vaddr_t va;
        struct pte *pte;
        struct cme cme;
        cme_id_t cme_id;

        // TODO: synchronisation
        for (i = 0; i < npages; ++i) {
                va = start + i * PAGE_SIZE;
                pte = pagetable_get_pte_from_va(va);

                pte->pte_state = S_INVALID;

                tlb_remove(va);

                cme_id = (cme_id_t)pte->pte_phys_page;
                cme = coremap.cmes[cme_id];

                KASSERT(cme.cme_state != S_KERNEL);
                coremap.cmes[cme_id].cme_state = S_FREE;
        }
}
