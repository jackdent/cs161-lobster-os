#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <clock.h>
#include <test.h>

/*
 * Unit tests for condition variables.
 *
 *
 * All tests (apart from those that crash) attempt to clean up after
 * running, to avoid leaking memory and leaving extra threads lying
 * around. Tests with a cleanup phase call ok() before going to it in
 * case the cleanup crashes -- this should not happen of course but if
 * it does it should be distinguished from the main part of the test
 * itself dying.
 */

#define NAMESTRING "CV_TEST"



////////////////////////////////////////////////////////////
// tests

/*
 * 1. Testing signaling properties:
 *     - Signaling on a CV that no thread is waiting on 
 		should have no effect on the CV.
 */
int
cvu1(int nargs, char **args)
{

	kprintf("Beginning cv unit test 1\n");

	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	struct cv* test_cv = cv_create(name);
	struct lock* lk = lock_create(name);

	// save attributes before the signal-
	char* name_ptr = test_cv->cv_name;
	struct wchan* cv_wchan_ptr = test_cv->cv_wchan;
	const char* wc_name_ptr = cv_wchan_ptr->wc_name;
	spinlock_data_t splk_lock = test_cv->cv_lock.splk_lock;
	struct cpu* cpu1 = test_cv->cv_lock.splk_holder;


	lock_acquire(lk);
	cv_signal(test_cv, lk);
	lock_release(lk);

	// compare attributes
	KASSERT(name_ptr == test_cv->cv_name);
	KASSERT(cv_wchan_ptr == test_cv->cv_wchan);
	KASSERT(wc_name_ptr == cv_wchan_ptr->wc_name);
	KASSERT(splk_lock == test_cv->cv_lock.splk_lock);
	KASSERT(cpu1 == test_cv->cv_lock.splk_holder);
	KASSERT(test_cv->cv_wchan->wc_threads.tl_count == 0);

	kprintf("cv unit test 1 passed\n");

	/* clean up */
	lock_destroy(lk);
	cv_destroy(test_cv);
	return 0;
}


/*
 * 2. Testing naming properties:
 *     - The name should be a distinct copy of the name passed in.
 */
int
cvu2(int nargs, char **args)
{

	kprintf("Beginning cv unit test 2\n");

	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	struct cv* test_cv = cv_create(name);

	KASSERT(name != test_cv->cv_name);
	KASSERT(strcmp(name, test_cv->cv_name) == 0);
	
	kprintf("cv unit test 2 passed\n");

	/* clean up */
	cv_destroy(test_cv);
	return 0;
}