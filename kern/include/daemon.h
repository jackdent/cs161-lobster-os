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

int daemon_index = 0;

void daemon_thread(void);
