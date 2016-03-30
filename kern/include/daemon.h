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


#define USE_DAEMON true

void daemon_init(void);

/* Writeback daemon that runs in the background */
void daemon_thread(void *data1, unsigned long data2);
