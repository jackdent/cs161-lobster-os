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
#include <thread.h>
#include <daemon.h>

int daemon_index = 0;

void
daemon_init(void)
{
	int err;
	char *daemon_name;

	if (!USE_DAEMON) {
		return;
	}

	daemon_name = kstrdup("writeback daemon:");
	if (daemon_name == NULL) {
		panic("daemon_init: could not launch thread");
	}

	err = thread_fork(daemon_name, NULL, daemon_thread, NULL, 0);
	if (err) {
		panic("daemon_init: could not launch thread");
	}
}

void
daemon_thread(void *data1, unsigned long data2)
{
	(void)data1;
	(void)data2;

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
