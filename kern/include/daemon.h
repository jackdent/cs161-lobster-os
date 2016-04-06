#define USE_DAEMON false

// Above this fraction of main memory used, the daemon is woken up.
// Integers, since no floating point arithmatic on MIPS
#define USE_DAEMON_FRAC_NUMER 60
#define USE_DAEMON_FRAC_DENOM 100

struct daemon {
	struct cv *d_cv;
	struct lock *d_lock;
	int d_memory_threshold;
	bool d_awake;		// To prevent repeated signaling from coremap
};

struct daemon daemon;

void daemon_init(void);

/* Writeback daemon that runs in the background */
void daemon_thread(void *data1, unsigned long data2);
