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
#include <coremap.h>
#include <tlb.h>

void
vm_bootstrap(void)
{
        cm_init();
}

vaddr_t
alloc_kpages(unsigned npages)
{
        unsigned i;
        cme_id_t start, curr;
        vaddr_t addr;
        struct cme cme;

        start = cm_capture_slots_for_kernel(npages);

        for (i = 0; i < npages; i++) {
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

        cm_release_locks(start, start + npages);

        return CME_ID_TO_PA(start);
}

void
free_kpages(vaddr_t addr)
{
        (void)addr;

        /* TODO */
}

void
alloc_upages(struct pagetable *pt, vaddr_t start, unsigned int npages)
{
        KASSERT(start % PAGE_SIZE == 0);

        unsigned int i;
        struct pte *pte;

        for (i = 0; i < npages; ++i) {
                pte = pagetable_get_pte_from_va(pt, start + (vaddr_t)(i * PAGE_SIZE));
                pte->pte_state = S_LAZY;
        }
}

void
free_upage(struct pte *pte, vaddr_t va)
{
        cme_id_t cme_id;
        swap_id_t swap_id;
        struct cme cme;

        switch (pte->pte_state) {
        case S_INVALID:
        case S_LAZY:
                break;
        case S_PRESENT:
                tlb_remove(va);

                cme_id = (cme_id_t)pte->pte_phys_page;
                cme = coremap.cmes[cme_id];

                KASSERT(cme.cme_state != S_KERNEL);

                coremap.cmes[cme_id].cme_state = S_FREE;
                break;
        case S_SWAPPED:
                swap_id = pte_get_swap_id(pte);
                swap_free_slot(swap_id);
                break;
        };

        pte->pte_state = S_INVALID;
}

void
free_upages(struct pagetable *pt, vaddr_t start, unsigned int npages)
{
        KASSERT(start % PAGE_SIZE == 0);

        unsigned int i;
        vaddr_t va;
        struct pte *pte;

        // TODO: synchronisation
        for (i = 0; i < npages; ++i) {
                va = start + i * PAGE_SIZE;
                pte = pagetable_get_pte_from_va(pt, va);

                free_upage(pte, va);
        }
}
