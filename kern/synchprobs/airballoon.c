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
 * SYNCHRONIZATION PROBLEM 2: AIR BALLOON
 *
 * After a war erupts in their kingdom, Princess Marigold must help
 * Prince Dandelion (her younger brother) escape from danger. Marigold places
 * Dandelion in a hot air balloon, which is connected to the ground by
 * NROPES ropes -- each rope is connected to a hook on the balloon as well as
 * a stake in the ground. Marigold and Dandelion must work together to sever all
 * of these ropes so that Dandelion can escape. Marigold unties the ropes from
 * the ground stakes while Dandelion unhooks the ropes from the balloon.
 *
 * Unfortunately, one of Princess Marigold and Prince Dandelion's enemies,
 * Lord FlowerKiller, is also at work. FlowerKiller is rearranging the ropes
 * to thwart Princess Marigold and Prince Dandelion. He will randomly unhook
 * a rope from one stake and move it to another stake. This leads to chaos!
 *
 * Without Lord FlowerKiller's dastardly, behavior, there would be a simple
 * 1:1 correspondence between balloon_hooks and ground_stakes (each hook in
 * balloon_hooks has exactly one corresponding entry in ground_stakes, and
 * each stake in ground_stakes has exactly one corresponding entry in
 * balloon_hooks). However, while Lord FlowerKiller is around, this perfect
 * 1:1 correspondence may not exist.
 *
 * As Marigold and Dandelion cut ropes, they must delete mappings, so that they
 * remove all the ropes as efficiently as possible (that is, once Marigold has
 * severed a rope, she wants to communicate that information to Dandelion, so
 * that he can work on severing different ropes). They will each use NTHREADS
 * to sever the ropes and udpate the mappings. Dandelion selects ropes to sever
 * by generating a random balloon_hook index, and Marigold selects ropes by
 * generating a random ground_stake index.
 *
 * Lord FlowerKiller has only a single thread. He is on the ground, so like
 * Marigold, he selects ropes by their ground_stake index.
 *
 * Consider this example:
 * Marigold randomly selects the rope attached to ground_stake 7 to sever. She
 * consults the mapping for ground_stake 7, sees that it is still mapped, and
 * sees that the other end of the rope attaches to balloon_hook 11. To cut the
 * rope, she must free the mappings in both ground_stake 7 and balloon_hook 11.
 * Imagine that Dandelion randomly selects balloon_hook index 11 to delete. He
 * determines that it is still mapped, finds that the corresponding ground_stake
 * index is 7. He will want to free the mappings in balloon_hook 11 and
 * ground_stake 7. It's important that Dandelion and Marigold don't get in each
 * other's way. Worse yet, Lord FlowerKiller might be wreaking havoc with the
 * same ropes.  For example, imagine that he decides to swap ground_stake 7
 * with ground_stake 4 at the same time.  Now, all of a sudden, balloon_hook 11
 * is no longer associated with ground_stake 7 but with ground_stake 4.
 *
 * Without proper synchronization, Marigold and Dandelion can encounter:
 * - a race condition, where multiple threads attempt to sever the same rope at
 *   the same time (e.g., two different Marigold threads attempt to sever the
 *   rope attached to ground_stake 7).
 * - a deadlock, where two threads select the same rope, but accessing it from
 *   different directions (e.g., Dandelion gets at the rope from balloon_hook 11
 *   while Marigold gets at the rope from ground_stake 7).
 *
 * Your solution must satisfy these conditions:
 *  - Avoid race conditions.
 *  - Guarantee no deadlock can occur. Your invariants and comments should
 *  provide a convincing proof of this.
 *  HINT: This includes ensuring that Lord FlowerKiller's behavior does not
 *  cause any race conditions or deadlocks by adding the appropriate
 *  synchronization to his thread as well.
 *  HINT: You should insert well-placed thread_yield() calls in your code to
 *  convince yourself that your synchronization is working.
 *  - When Marigold and Dandelion select ropes to cut, you may choose to ignore
 *    a particular choice and generate a new one, however, all mappings must
 *    eventually be deleted.
 *  HINT: Use this to your advantage to introduce some asymmetry to the
 *  problem.
 *  - Permit multiple Marigold/Dandelion threads to sever ropes concurrently
 *  (no "big lock" solutions)
 *
 */

#include <types.h>
#include <lib.h>
#include <wchan.h>
#include <thread.h>
#include <synch.h>
#include <test.h>
#include <kern/errno.h>
#include "common.h"

#define NROPES 128
#define NTHREADS 10 // number of threads for each person

volatile unsigned num_deleted = 0; // number of deleted mappings

struct balloon_hook {
	volatile unsigned ground_ndx;
	volatile bool is_mapped; // indicates this hook is still in use
};

struct ground_stake {
	volatile unsigned balloon_ndx;
	volatile bool is_mapped; // indicates this stake is still in use
	// A lock to protect the mapping. We always acquire this first before
	// modifying anything.
	struct lock *lk;
};

// Global semaphore to block the main thread
// while the airballon is trying to escape.
// This will be signaled 2*NTHREADS + 1 times,
// once by each thread.
struct semaphore *exit_sem;

// Lock to synchronize access to num_deleted
struct lock *num_deleted_lk;

// Contains balloon <-> ground mapping
struct balloon_hook balloon_hooks[NROPES];
struct ground_stake ground_stakes[NROPES];

/*
 * Indicates who is deleting the current mapping. Used for
 * print_deleted_mapping.
 */
enum person {
	DANDELION,
	MARIGOLD,
};

/*
 * DO NOT MODIFY THIS. Call this function after every delete.
 */
static
void
print_deleted_mapping(enum person who,
                      unsigned balloon_index,
                      unsigned ground_index,
                      unsigned num_deleted_current) {
	const char *name = (who == DANDELION) ? "dandelion" : "marigold";
	kprintf("{who: %s, balloon: %u, ground: %u, deleted: %u}\n",
            name, balloon_index, ground_index, num_deleted_current);
	thread_yield(); // cause some interleaving
}

/*
 * Do not modify this!
 */
static
void
init_mappings(void) {
	unsigned balloon_ndx, ground_ndx, i;
	unsigned array[NROPES];

	for (i = 0; i < NROPES; i++) {
		array[i] = i;
	}

	// generate a random bijection between balloon indices and ground indices
	shuffle(array, NROPES);
	for (i = 0; i < NROPES; i++) {
		balloon_ndx = i;
		ground_ndx = array[i];

		balloon_hooks[balloon_ndx].ground_ndx = ground_ndx;
		balloon_hooks[balloon_ndx].is_mapped = true;

		ground_stakes[ground_ndx].balloon_ndx = balloon_ndx;
		ground_stakes[ground_ndx].is_mapped = true;
	}
}

static
void
dandelion(void *data, unsigned long junk) {
	// Local variables
	unsigned balloon_ndx, ground_ndx;
	unsigned num_deleted_current;

	// Do you know why these are here?
	(void) data;
	(void) junk;

	while (1) {
		// Check if there are any more hooks left to deal with
		// protect access to num_deleted
		lock_acquire(num_deleted_lk);
		if (num_deleted == NROPES) {
			lock_release(num_deleted_lk);
			break;
		}
		lock_release(num_deleted_lk);

		// generate random balloon index to delete
		balloon_ndx = random() % NROPES;
		if (!balloon_hooks[balloon_ndx].is_mapped) {
			continue;
		}

		// Acquire the lock on the ballon hook
		ground_ndx = balloon_hooks[balloon_ndx].ground_ndx;

		// If this isn't a valid ground index, the mapping is
		// probably deleted. Otherwise, it doesn't matter if we get,
		// say a partial write as long as bail out if we're not connected
		// to what we think we're connected to.
		if (ground_ndx >= NROPES)
			continue;

		lock_acquire(ground_stakes[ground_ndx].lk);
		// If we caught a partial write when reading the ground_ndx
		// above, or if the enemy exchanged indicies on us while we
		// were waiting for the lock, just try again.
		// Additionally, this might have been deleted while we were waiting
		// for the lock. In that case move on to avoid double counting.
		if (ground_stakes[ground_ndx].balloon_ndx != balloon_ndx ||
			!ground_stakes[ground_ndx].is_mapped) {
			lock_release(ground_stakes[ground_ndx].lk);
			continue;
		}

		/*
		 * Do the actual deletion. The value 0xDEADBEEF has
		 * historically been used to represent freed memory
		 * locations; we use it (and friends) here for your amusement.
		 */
		balloon_hooks[balloon_ndx].is_mapped = false;
		ground_stakes[ground_ndx].is_mapped = false;
		balloon_hooks[balloon_ndx].ground_ndx = 0xDEADBEEF;
		ground_stakes[ground_ndx].balloon_ndx = 0xBAADBEEF;
		// protect access to num_deleted
		lock_acquire(num_deleted_lk);
		num_deleted_current = ++num_deleted;
		lock_release(num_deleted_lk);

		// We're done modifying the data structure, so release
		// the lock now. No need to hold it while printing.
		lock_release(ground_stakes[ground_ndx].lk);

		print_deleted_mapping(DANDELION,
		    balloon_ndx, ground_ndx, num_deleted_current);
    }

    V(exit_sem);
}

static
void
marigold(void *data, unsigned long junk) {
	// Local variables
	unsigned balloon_ndx, ground_ndx;
	unsigned num_deleted_current;

	// Do you know why these are here?
	(void) data;
	(void) junk;

	// TODO: add synchronization
	while (1) {
		// Check if there are any more hooks left to deal with
		// protect access to num_deleted
		lock_acquire(num_deleted_lk);
		if (num_deleted == NROPES) {
			lock_release(num_deleted_lk);
			break;
		}
		lock_release(num_deleted_lk);

		// generate random ground index to delete
		ground_ndx = random() % NROPES;
		if (!ground_stakes[ground_ndx].is_mapped) {
			continue;
		}

		lock_acquire(ground_stakes[ground_ndx].lk);
		// This mapping might have been deleted while we were waiting
		// for the lock. Don't double count it in that case.
		if (!ground_stakes[ground_ndx].is_mapped) {
			lock_release(ground_stakes[ground_ndx].lk);
			continue;
		}

		// Get the ballon index. Since we did this after getting
		// the lock we can be sure that KillerFlower isn't swapping
		// ropes.
		balloon_ndx = ground_stakes[ground_ndx].balloon_ndx;

		// actually do the deletion
		ground_stakes[ground_ndx].is_mapped = false;
		balloon_hooks[balloon_ndx].is_mapped = false;
		ground_stakes[ground_ndx].balloon_ndx = 0xBEEFDEAD;
		balloon_hooks[balloon_ndx].ground_ndx = 0xFEEDBEEF;
		// protect access to num_deleted
		lock_acquire(num_deleted_lk);
		num_deleted_current = ++num_deleted;
		lock_release(num_deleted_lk);

		// Same as above, release the lock before printing
		lock_release(ground_stakes[ground_ndx].lk);

		print_deleted_mapping(MARIGOLD,
		    balloon_ndx, ground_ndx, num_deleted_current);
	}

	// Signal the main thread that we are done
	V(exit_sem);
}

static
void
KillerFlower(void *data, unsigned long junk){
	// Local variables
	unsigned gndx_a, gndx_b, i, mappings_to_change, temp;

    // Do you know why these are here?
	(void) data;
	(void) junk;

	mappings_to_change = random() % NROPES;

	for (i = 0; i < mappings_to_change; i++) {
		//it is possible to get a no-op here, but don't count on those
		gndx_a = random() % NROPES;
		gndx_b = random() % NROPES;

		// Make sure to not deadlock ourselves
		if (gndx_a == gndx_b) {
			// Swapping an index with itself is a noop
			continue;
		}

		lock_acquire(ground_stakes[gndx_a].lk);
		lock_acquire(ground_stakes[gndx_b].lk);

		//swap the connections to balloon indices a and b if both
		// ties have not been severed
		if (ground_stakes[gndx_a].is_mapped &&
		    ground_stakes[gndx_b].is_mapped) {

			// Swap where the balloon hooks mappings
			balloon_hooks[ground_stakes[gndx_a].balloon_ndx].ground_ndx
			    = gndx_b;
			balloon_hooks[ground_stakes[gndx_b].balloon_ndx].ground_ndx
			    = gndx_a;

			// Now swap the ground_stake mappings
			temp = ground_stakes[gndx_a].balloon_ndx;

			ground_stakes[gndx_a].balloon_ndx =
			    ground_stakes[gndx_b].balloon_ndx;
			ground_stakes[gndx_b].balloon_ndx = temp;
		}

		lock_release(ground_stakes[gndx_a].lk);
		lock_release(ground_stakes[gndx_b].lk);
	}

	V(exit_sem);
}

int
airballoon(int nargs, char **args) {
	unsigned i;

	(void) nargs;
	(void) args;
	init_mappings();
	num_deleted = 0;

	for (i = 0; i < NROPES; ++i) {
		ground_stakes[i].lk = lock_create("ground lock");
		KASSERT( ground_stakes[i].lk != NULL );
	}

	num_deleted_lk = lock_create("num_deleted lock");
	KASSERT( num_deleted_lk != NULL );
	exit_sem = sem_create("exit_sem",0);
	KASSERT( exit_sem != NULL );
	// Spawn FlowerKiller thread.
	thread_fork_or_panic("FlowerKiller", NULL, KillerFlower, NULL, 0);

	// Spawn Dandelion's and Marigold's threads
	for (i = 0; i < NTHREADS; i++) {
        	thread_fork_or_panic("Dandelion", NULL, dandelion, NULL, 0);
        	thread_fork_or_panic("Marigold", NULL, marigold, NULL, 0);
	}

	for (i = 0; i < 2*NTHREADS + 1; ++i)
		P(exit_sem);

	// cleanup
	num_deleted = 0;
	sem_destroy(exit_sem);
	lock_destroy(num_deleted_lk);
	for (i = 0; i < NROPES; ++i) {
		lock_destroy(ground_stakes[i].lk);
	}

	return 0;
}
