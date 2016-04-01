#include <types.h>
#include <lib.h>
#include <daemon.h>
#include <thread.h>
#include <coremap.h>

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

	cme_id_t cme_id;
	struct cme *cme;

	cme_id = 0;

	while (true) {
		cme_id = (cme_id + 1) % coremap.cm_size;

		if (cm_attempt_lock(cme_id)) {
			cme = &coremap.cmes[cme_id];

			if (cme->cme_state == S_DIRTY) {
				cm_clean_page(cme_id);
			}

			cm_release_lock(cme_id);
		}
	}
}
