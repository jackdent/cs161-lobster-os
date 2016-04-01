#include <types.h>
#include <lib.h>
#include <daemon.h>
#include <thread.h>

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

	while (true) {
	}
}
