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

	daemon.d_cv = cv_create("daemon cv");
	if (daemon.d_cv == NULL) {
		panic("daemon_init: could not launch thread");
	}

	daemon.d_lock = lock_create("daemon lock");
	if (daemon.d_lock == NULL) {
		panic("daemon_init: could not launch thread");
	}

	daemon.d_awake = true;

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

	if (!USE_DAEMON) {
		cv_destroy(daemon.d_cv);
		lock_destroy(daemon.d_lock);
		return;
	}

	int allocated_pages;
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

		allocated_pages = cm_get_page_count();
		if (allocated_pages <= daemon.d_memory_threshold) {
			lock_acquire(daemon.d_lock);
			daemon.d_awake = false;
			cv_wait(daemon.d_cv, daemon.d_lock);
			daemon.d_awake = true;
			lock_release(daemon.d_lock);
		}
	}
}
