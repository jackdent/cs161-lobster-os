#define USE_DAEMON true

void daemon_init(void);

/* Writeback daemon that runs in the background */
void daemon_thread(void *data1, unsigned long data2);
