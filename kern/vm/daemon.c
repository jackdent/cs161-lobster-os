#include <types.h>
#include <vnode.h>
#include <vfs.h>
#include <bitmap.h>
#include <uio.h>
#include <swap.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <synch.h>
#include <machine/vm.h>
#include <addrspace.h>
#include <coremap.h>
#include <daemon.h>

void
daemon_thread(void)
{
	struct cme *cme;

	while (true) {
		daemon_index = (daemon_index + 1) % coremap.cm_size;
		cm_acquire_lock(daemon_index);
		cme = &coremap.cmes[daemon_index];
		if (cme->cme_state != S_DIRTY) {
			cm_release_lock(daemon_index);
			continue;
		}
		swap_out(cme->cme_swap_id, CME_ID_TO_PA(daemon_index));
		cme->cme_state = S_CLEAN;
		cm_release_lock(daemon_index);
	}
}
