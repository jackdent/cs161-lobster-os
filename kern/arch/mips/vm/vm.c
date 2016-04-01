#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <pagetable.h>
#include <coremap.h>

static
void
tlbshootdown_init()
{
        tlbshootdown.ts_lock = lock_create("tlbshootdown lock");
        if (tlbshootdown.ts_lock == NULL) {
                panic("Could not create tlbshootdown lock");
        }

        tlbshootdown.ts_sem = sem_create("tlbshootdown sem", 0);
        if (tlbshootdown.ts_sem == NULL) {
                panic("Could not create tlbshootdown sem");
        }
}

void
vm_bootstrap(void)
{
        cm_init();

        // We can now use kmalloc
        tlbshootdown_init();
}

vaddr_t
alloc_kpages(unsigned npages)
{
        unsigned i;
        cme_id_t start, curr;
        vaddr_t addr;
        paddr_t start_pa;
        struct cme cme;

        start = cm_capture_slots_for_kernel(npages);

        for (i = 0; i < npages; i++) {
                curr = start + i;
                cm_evict_page(curr);

                // We don't really need to do this because kernel
                // memory is directy mapped, but it may be helpful
                // when debugging
                addr = CME_ID_TO_PA(curr);
                KASSERT(addr >= MIPS_KSEG0);

                cme = cme_create(kproc->p_pid, addr, S_KERNEL);
                cme.cme_swap_id = 0;

                coremap.cmes[curr] = cme;
        }

        // Since we never swap out kernel pages, we can use this
        // for storing the size of the allocation
        coremap.cmes[start].cme_swap_id = npages;

        cm_release_locks(start, start + npages);

        start_pa = CME_ID_TO_PA(start);
        return PADDR_TO_KVADDR(start_pa);
}

void
free_kpages(vaddr_t addr)
{
        unsigned int npages, i;
        paddr_t start_pa;
        cme_id_t start, end;

        start_pa = KVADDR_TO_PADDR(addr);
        start = PA_TO_CME_ID(start_pa);

        npages = coremap.cmes[start].cme_swap_id;
        if (npages == 0) {
                panic("Tried to free a kernel page that did not start the allocation.\n");
        }

        end = start + npages;
        cm_acquire_locks(start, end);

        for (i = 0; i < npages; i++) {
                KASSERT(coremap.cmes[start + i].cme_state == S_KERNEL);
                cm_free_page(start + i);
        }

        cm_release_locks(start, end);
}

void
alloc_upages(vaddr_t start, unsigned int npages)
{
        KASSERT(start % PAGE_SIZE == 0);

        struct addrspace *as;
        unsigned int i;
        struct pte *pte;
        vaddr_t va;

        as = curproc->p_addrspace;

        for (i = 0; i < npages; i++) {
                va =  start + i * PAGE_SIZE;
                pte = pagetable_get_pte_from_va(as->as_pt, va);

                pt_acquire_lock(as->as_pt, pte);
                KASSERT(pte->pte_state == S_INVALID);
                pte->pte_state = S_LAZY;
                pt_release_lock(as->as_pt, pte);
        }
}

void
free_upage(vaddr_t va)
{
        struct addrspace *as;
        struct pte *pte;
        cme_id_t cme_id;
        swap_id_t swap_id;

        as = curproc->p_addrspace;
        pte = pagetable_get_pte_from_va(as->as_pt, va);

        KASSERT(pte != NULL);

        pt_acquire_lock(as->as_pt, pte);

        switch (pte->pte_state) {
        case S_INVALID:
                panic("Tried to free an invalid user page.\n");
        case S_LAZY:
                break;
        case S_PRESENT:
                cme_id = pte_get_cme_id(pte);

                cm_acquire_lock(cme_id);
                cm_free_page(cme_id);
                cm_release_lock(cme_id);
                break;
        case S_SWAPPED:
                swap_id = pte_get_swap_id(pte);
                swap_free_slot(swap_id);
                break;
        }

        pte->pte_state = S_INVALID;
        pt_release_lock(as->as_pt, pte);
}

void
free_upages(vaddr_t start, unsigned int npages)
{
        KASSERT(start % PAGE_SIZE == 0);

        unsigned int i;
        vaddr_t va;

        for (i = 0; i < npages; i++) {
                va = start + i * PAGE_SIZE;
                free_upage(va);
        }
}
