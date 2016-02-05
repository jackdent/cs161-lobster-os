#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <clock.h>
#include <test.h>

/*
 * Unit tests for locks.
 *
 *
 * All tests (apart from those that crash) attempt to clean up after
 * running, to avoid leaking memory and leaving extra threads lying
 * around. Tests with a cleanup phase call ok() before going to it in
 * case the cleanup crashes -- this should not happen of course but if
 * it does it should be distinguished from the main part of the test
 * itself dying.
 */

#define NAMESTRING "LOCK_TEST"



////////////////////////////////////////////////////////////
// tests

/*
 * 1. Testing naming properties:
 *     - lk_holder should be NULL initially
 *     - lk_holder should be the holder's name after acquiring it
 *     - lk_holder should be NULL after releasing
 */
int
locku1(int nargs, char **args)
{

	kprintf("Beginning lock unit test 1\n");

	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	struct lock* lk = lock_create(name);
	KASSERT(!lk->lk_holder);

	lock_acquire(lk);
	KASSERT(lk->lk_holder == curthread);

	lock_release(lk);
	KASSERT(!lk->lk_holder);

	kprintf("Lock unit test 1 passed\n");

	/* clean up */
	lock_destroy(lk);
	return 0;
}


/*
 * 2. Testing ownership properties:
 *     - lock_do_i_hold should return true if the thread has 
 		the lock and false otherwise
 */
int
locku2(int nargs, char **args)
{

	kprintf("Beginning lock unit test 2\n");

	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	struct lock* lk = lock_create(name);
	KASSERT(!lock_do_i_hold(lk));

	lock_acquire(lk);
	KASSERT(lock_do_i_hold(lk));

	lock_release(lk);
	KASSERT(!lock_do_i_hold(lk));

	kprintf("Lock unit test 2 passed\n");
	
	/* clean up */
	lock_destroy(lk);
	return 0;
}


/*
 * 3. Testing contention properties:
 *     - After a thread tries to acquire a lock already held, 
 		lock->lk_holder should still be the original holder's name.
 */


// helper functions thread1 and thread2
static
void
thread1(void *lk, unsigned long junk)
{
	(void)junk;

	struct lock* lk1 = (struct lock*) lk;

	lock_acquire(lk1);
	clocksleep(2);

	KASSERT(lk1->lk_holder == curthread);
	lock_release(lk1);

	return;
}

static
void
thread2(void *lk, unsigned long junk)
{
	(void)junk;

	struct lock* lk1 = (struct lock*) lk;

	clocksleep(1);
	lock_acquire(lk1);
	lock_release(lk1);

	kprintf("Lock unit test 3 passed\n");
	return;
}


int
locku3(int nargs, char **args)
{

	kprintf("Beginning lock unit test 3\n");

	const char *name = NAMESTRING;
	int result;

	(void)nargs; (void)args;

	struct lock* lk = lock_create(name);
	
	result = thread_fork("thread1", NULL, thread1, (void*)lk, 0);
	if (result) {
		panic("locku3: thread_fork failed\n");
	}
	result = thread_fork("thread2", NULL, thread2, (void*)lk, 0);
	if (result) {
		panic("locku3: thread_fork failed\n");
	}

	clocksleep(3);

	/* clean up */
	lock_destroy(lk);
	return 0;
}