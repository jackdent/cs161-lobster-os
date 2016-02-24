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
	int result;

	struct array *argv;
	struct array *argv_lens;

	KASSERT(proc_getas() == NULL);
	_launch_program(progname, &stack_ptr, &entry_point);

	// Need to copy arguments in
	if (argc > 1) {
		argv = array_create();
		if (!argv) {
			result = ENOMEM;
			goto err1;
		}

		argv_lens = array_create();
		if (!argv_lens) {
			result = ENOMEM;
			goto err2;
		}

		result = extract_args((userptr_t) args, NULL, argv, argv_lens, false);
		if (result) {
			goto err2;
		}

		copy_args_to_stack(&stack_ptr, argv, argv_lens);

		enter_new_process(0 /*argc*/, (userptr_t)stack_ptr /*userspace addr of argv*/,
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
		array_destroy(argv);
	err1:
		array_destroy(argv_lens);
		return result;
}
