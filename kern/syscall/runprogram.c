/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than runprogram() does.
 */

#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <proc.h>
#include <kern/errno.h>
#include <copyinout.h>

/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int
runprogram(char *progname, char **args, int argc)
{
	vaddr_t stack_ptr, entry_point;

	int i, padding, result;
	int length;
	size_t offset;
	userptr_t user_dest;
	userptr_t *user_argv;

	KASSERT(proc_getas() == NULL);
	_launch_program(progname, &stack_ptr, &entry_point);

	// Need to copy arguments in
	if (argc > 1) {
		user_argv = kmalloc(sizeof(userptr_t) * argc);
		if (user_argv == NULL) {
			result = ENOMEM;
			goto err1;
		}

		// Copy arg strings in
		offset = 0;
		for (i = argc - 1; i >= 0; i--) {
			length = strlen(args[i]) + 1;
			padding = length % 4 == 0 ? 0 : 4 - (length % 4);
			offset += length + padding;
			user_argv[i] = (userptr_t)(stack_ptr - offset);

			result = copyoutstr((const char*)args[i], user_argv[i], length, NULL);
			if (result){
				goto err2;
			}
		}

		// Copy pointers to arguments in
		user_dest = user_argv[0] - 4 * (argc + 1);
		stack_ptr = (vaddr_t)user_dest;
		for (i = 0; i < argc; i++) {
			result = copyout((const void *)&user_argv[i], user_dest, 4);
			if (result) {
				goto err2;
			}
			user_dest += 4;
		}

		kfree(user_argv);

		enter_new_process(0 /*argc*/, (userptr_t)stack_ptr
			/*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  (vaddr_t)stack_ptr, entry_point);

		panic("runprogram should never return");
		return -1;
	}
	else {
		enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  (vaddr_t)stack_ptr, entry_point);

		panic("runprogram should never return");
		return -1;
	}

	// TODO: Thread destroy should take care of cleaning up
	// the result of _launch_program, right?

	err2:
		kfree(user_argv);
	err1:
		return result;
}
