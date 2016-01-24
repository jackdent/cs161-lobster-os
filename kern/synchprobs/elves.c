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
 * SYNCHRONIZATION PROBLEM 1: KEEBLER ELVES
 *
 * The Keebler Cookie Factory is staffed by one supervisor and many
 * elves. Each elf must complete some series of tasks before it can
 * leave for the day (implemented in the `work` function). Whenever
 * an elf completes one of its tasks, it announces what it just did
 * (implemented as the `kprintf` in the `work` function). When an elf
 * has completed all of its work the supervisor dismisses the elf by
 * saying "Thanks for your work, Elf N!" where N corresponds to the N-th elf.
 *
 * At the beginning of the day, the supervisor (a supervisor thread)
 * opens the factory and lets the elves inside (starts their threads).
 * At any given moment, there is a single supervisor and possibly
 * multiple elves working. The supervisor is not allowed to dismiss an
 * elf until that elf has finished working. Your solution CANNOT wait
 * for ALL the elves to finish before starting to dismiss them.
 */

#include <types.h>
#include <lib.h>
#include <wchan.h>
#include <thread.h>
#include <synch.h>
#include <test.h>
#include <kern/errno.h>
#include "common.h"

#define NUM_TASKS 16

static const char *tasks[NUM_TASKS] = {
	"Stirred the marshmallow mush",
	"Melted the dark chocolate",
	"Salted the caramel",
	"Fluffed the meringue",
	"Counted the butterscotch chips",
	"Chopped up the mint",
	"Chopped up the sprinkles",
	"Whipped up the cream",
	"Tasted the toffee",
	"Cooled the fudge",
	"Mixed the molasses",
	"Froze the frosting",
	"Sliced the sugar cookies",
	"Baked the apples",
	"Melted the candy coating",
	"Perfected the plum sauce",
};

/*
 * Do not modify this!
 */
static
void
work(unsigned elf_num)
{
	int r;

	r = random() % NUM_TASKS;
	while (r != 0) {
		kprintf("Elf %3u: %s\n", elf_num, tasks[r]);
		r = random() % NUM_TASKS;
		thread_yield(); // cause some interleaving!
	}
}

// One of these structs should be passed from the main driver thread
// to the supervisor thread.
struct supervisor_args {
	unsigned num_elves;
	// The supervisor thread will signal this semaphore
	// when it is done
	struct semaphore *supervisor_exit;
};

// One of these structs should be passed from the supervisor thread
// to each of the elf threads.
struct elf_args {
	struct semaphore *supervisor_ready;
	struct semaphore *elf_done;
	volatile unsigned exited_elf;
};

static
void
elf(void *args, unsigned long id)
{
	struct elf_args *eargs = (struct elf_args *) args;
	work(id);

	// Wait until the supervisor is ready to receive our report
	P(eargs->supervisor_ready);

	// Tell the supervisor our ID and wake them up
	eargs->exited_elf = id;
	V(eargs->elf_done);
}

static
void
supervisor(void *args, unsigned long junk)
{
	struct supervisor_args *sargs = (struct supervisor_args *) args;
	(void)junk;

	struct elf_args eargs = {
		.supervisor_ready = sem_create("supervisor_ready",0),
		.elf_done = sem_create("elf_done",0)
	};
	KASSERT( (eargs.supervisor_ready != NULL && eargs.elf_done != NULL) &&
		"Could not create elf semaphores");


	// Fill in the elf worker array
	for (unsigned i = 0; i < sargs->num_elves; ++i) {
		if (thread_fork("elf", NULL, elf, &eargs, i) != 0)
			panic("Could not fork elf. Try asking the dwarves.");
	}

	for (unsigned i = 0; i < sargs->num_elves; ++i) {
		V(eargs.supervisor_ready);
		P(eargs.elf_done);
		kprintf("Thanks for your work ELF %3u\n", eargs.exited_elf);
	}

	// We're done for the day
	V(sargs->supervisor_exit);

	// Clean up
	sem_destroy(eargs.supervisor_ready);
	sem_destroy(eargs.elf_done);
}

int
elves(int nargs, char **args)
{
	// if an argument is passed, use that as the number of elves
	unsigned num_elves = 10;
	if (nargs == 2) {
		num_elves = atoi(args[1]);
	}

	// Setup the arguments for the supervisor thread.
	struct supervisor_args sargs = {
	  .num_elves = num_elves,
	  .supervisor_exit = sem_create("supervisor_exit", 0)
	};

	KASSERT( (sargs.supervisor_exit != NULL) && "Failed to create supervisor_exit sem");
	// CLARIFICATION: Do we want the students to make the supervisor a separate
	// thread, if so, I think we need to state that more clearly
	if (thread_fork("supervisor", NULL, supervisor, &sargs, 0) != 0)
		panic("Failed to create supervisor thread");

	// Wait until the supervisor is done before returning to the menu
	P(sargs.supervisor_exit);

	// Clean up
	sem_destroy(sargs.supervisor_exit);

	return 0;
}
