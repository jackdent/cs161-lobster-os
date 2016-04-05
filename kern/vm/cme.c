#include <cme.h>

struct cme
cme_create(struct addrspace *as, vaddr_t va, enum cme_state state)
{
        struct cme cme;

        cme.cme_as = as;
        cme.cme_l1_offset = L1_PT_MASK(va);
        cme.cme_l2_offset = L2_PT_MASK(va);
        cme.cme_swap_id = 0;
        cme.cme_busy = 1; // So we can unset it when we're finished
        cme.cme_recent = 1;
        cme.cme_state = state;

        return cme;
}
