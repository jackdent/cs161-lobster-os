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

bool
cme_is_equal_to(struct cme *cme, struct cme *other)
{
        KASSERT(cme != NULL);
        KASSERT(other != NULL);

        if (cme->cme_as != other->cme_as
         || cme->cme_l1_offset != other->cme_l1_offset
         || cme->cme_l2_offset != other->cme_l2_offset
         || cme->cme_swap_id != other->cme_swap_id
         || cme->cme_busy != other->cme_busy
         || cme->cme_recent != other->cme_recent
         || cme->cme_state != other->cme_state) {
                return false;
        }

        return true;
}
