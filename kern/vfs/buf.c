/*
 * Copyright (c) 2009, 2014
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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <array.h>
#include <clock.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <mainbus.h>
#include <vfs.h>
#include <fs.h>
#include <buf.h>

/* Uncomment this to enable printouts of the syncer state. */
//#define SYNCER_VERBOSE

DECLARRAY(buf, static __UNUSED inline);
DEFARRAY(buf, static __UNUSED inline);

/*
 * The required size for all buffers. This is the size SFS uses. In a
 * real system you wouldn't have this restriction, but for us managing
 * buffers of different sizes just creates complications and would
 * serve no purpose.
 */
#define ONE_TRUE_BUFFER_SIZE		512

/*
 * Illegal array index.
 */
#define INVALID_INDEX ((unsigned)-1)

/*
 * This constant is used as an error return by buffer_mark_busy() to
 * indicate that the buffer we were trying to get disappeared under us
 * (was invalidated, evicted, etc.) and we need to try again. The most
 * natural error code for this is EAGAIN, but we need to be able to
 * distinguish this case from a return from FSOP_WRITEBLOCK that means
 * "please write out some other buffer first", where EAGAIN is also
 * natural; and since the latter is external and student-facing we'll
 * use a different error code for the former case. For this we'll
 * misappropriate EBADF; ESTALE might be a more sensible choice, but
 * it isn't defined in OS/161. Either way, this error code and
 * condition should never escape from this file...
 */
#define EDEADBUF EBADF

/*
 * One buffer.
 */
struct buf {
	/* maintenance */
	unsigned b_tableindex;	/* index into {de,at}tached_buffers */
	unsigned b_dirtyindex;	/* index into dirty_buffers */
	unsigned b_bucketindex;	/* index into buffer_hash bucket */
	unsigned b_dirtyepoch;	/* when we became dirty */

	/* status flags */
	unsigned b_attached:1;	/* key fields are valid */
	unsigned b_busy:1;	/* currently in use */
	unsigned b_valid:1;	/* contains real data */
	unsigned b_dirty:1;	/* data needs to be written to disk */
	unsigned b_fsmanaged:1;	/* managed by file system */
	struct thread *b_holder; /* who did buffer_mark_busy() */
	struct timespec b_timestamp; /* when it became dirty */

	/* key */
	struct fs *b_fs;	/* file system buffer belongs to */
	daddr_t b_physblock;	/* physical block number */

	/* value */
	void *b_data;
	size_t b_size;

	void *b_fsdata;		/* fs-specific metadata */
};

/*
 * Buffer hash table.
 */
struct bufhash {
	unsigned bh_numbuckets;
	struct bufarray *bh_buckets;
};

/*
 * Global state.
 *
 * The main table of buffers is attached_buffers[]. This is an
 * LRU-ordered array. All buffers in attached_buffers[] should be
 * attached (that is, they are associated with a specific fs and
 * block), and should also be in buffer_hash.
 *
 * Buffers that are dirty *also* appear in dirty_buffers[]; this array
 * is ordered by how recently the buffer was *first* modified.
 *
 * Buffers that are not attached appear (only) in detached_buffers[],
 * which is not ordered.
 *
 * Space in all three arrays is preallocated when buffers are created
 * so insert ops won't fail on the fly.
 *
 * The ordered arrays (attached_buffers and dirty_buffers) are
 * preallocated with extra space (and may contain NULL entries) and
 * are compacted only when the extra space runs out.
 */

static struct bufhash buffer_hash;

static struct bufarray attached_buffers;
static unsigned attached_buffers_first;   /* hint for first empty element */
static unsigned attached_buffers_thresh;  /* size limit before compacting */

static struct bufarray dirty_buffers;
static unsigned dirty_buffers_first;      /* hint for first empty element */
static unsigned dirty_buffers_thresh;     /* size limit before compacting */

static struct bufarray detached_buffers;

/*
 * Epochs and generations.
 *
 * The dirty_epoch is incremented whenever an explicit sync call is
 * made, and is used to know when to stop syncing. The
 * dirty_buffers_generation, conversely, is incremented whenever the
 * dirty_buffers table is compacted so syncs in progress know they
 * need to restart from the beginning of it. The attached_buffers
 * generation is the same, but for the attached_buffers table.
 */

static unsigned dirty_epoch;
static unsigned dirty_buffers_generation;
static unsigned attached_buffers_generation;

/*
 * Counters.
 */

static unsigned attached_buffers_count;
static unsigned busy_buffers_count;
static unsigned dirty_buffers_count;

static unsigned num_reserved_buffers;
static unsigned num_total_buffers;
static unsigned max_total_buffers;

static unsigned num_total_gets;
static unsigned num_valid_gets;
static unsigned num_read_gets;
static unsigned num_total_writeouts;
static unsigned num_total_evictions;
static unsigned num_dirty_evictions;

/*
 * Syncer state. (This is file-static so it's easily visible from the
 * debugger.)
 */
static bool syncer_under_load;
static bool syncer_needs_help;
static struct thread *syncer_thread;

/*
 * Lock
 */

static struct lock *buffer_lock;

/*
 * CVs
 */
static struct cv *buffer_busy_cv;
static struct cv *buffer_reserve_cv;

/*
 * Magic numbers (also search the code for "voodoo:")
 *
 * Note that these are put here like this so they're easy to find and
 * tune. There's no particular reason one shouldn't or couldn't
 * completely change the way they're computed. One could, for example,
 * factor buffer reservation calls into some of these decisions somehow.
 */

/* Number of buffers to reserve for each file system operation. */
#define RESERVE_BUFFERS		8

/* Factor for choosing attached_buffers_thresh. */
#define ATTACHED_THRESH_NUM	3
#define ATTACHED_THRESH_DENOM	2

/* Factor for choosing dirty_buffers_thresh. */
#define DIRTY_THRESH_NUM	5
#define DIRTY_THRESH_DENOM	4

/* Proportion of buffers we want to keep always clean. */
#define SYNCER_ALWAYS_NUM	1
#define SYNCER_ALWAYS_DENOM	5

/* Proportion of buffers we want clean if older than one second. */
#define SYNCER_IFOLD_NUM	2
#define SYNCER_IFOLD_DENOM	5

/* Age at which a buffer should be synced unconditionally. (seconds) */
#define SYNCER_TARGET_AGE	2

/* Buffer age at which the syncer considers itself under load. (seconds) */
#define SYNCER_LOAD_AGE		4

/* Buffer age at which the syncer considers itself in trouble. (seconds) */
#define SYNCER_HELP_AGE		8

#if 0
/* Threshold proportion (of bufs dirty) for starting the syncer */
#define SYNCER_DIRTY_NUM	1
#define SYNCER_DIRTY_DENOM	2

/* Target proportion (of total bufs) for syncer to clean in one run */
#define SYNCER_TARGET_NUM	1
#define SYNCER_TARGET_DENOM	4

/* Limit on the previous proportion, as proportion of dirty buffers */
#define SYNCER_LIMIT_NUM	1
#define SYNCER_LIMIT_DENOM	2
#endif

/* Overall limit on fraction of main memory to use for buffers */
#define BUFFER_MAXMEM_NUM	1
#define BUFFER_MAXMEM_DENOM	4

/* Macro for applying a NUM/DENOM pair. */
#define SCALE(x, K) (((x) * K##_NUM) / K##_DENOM)

/*
 * Forward declaration (XXX: reorg to make this go away)
 */
static void buffer_release_internal(struct buf *b);

////////////////////////////////////////////////////////////
// state invariants

/*
 * Check consistency of the global state.
 */
static
void
bufcheck(void)
{
	KASSERT(attached_buffers_count <= bufarray_num(&attached_buffers));
	KASSERT(attached_buffers_first <= bufarray_num(&attached_buffers));
	KASSERT(bufarray_num(&attached_buffers) <= attached_buffers_thresh);

	KASSERT(dirty_buffers_count <= bufarray_num(&dirty_buffers));
	KASSERT(dirty_buffers_first <= bufarray_num(&dirty_buffers));
	KASSERT(bufarray_num(&dirty_buffers) <= dirty_buffers_thresh);

	KASSERT(bufarray_num(&detached_buffers) + attached_buffers_count
		== num_total_buffers);
	// This is not true any more, because busy_buffers_count now
	// includes buffers marked busy by syncing.
	//KASSERT(busy_buffers_count <= num_reserved_buffers);
	KASSERT(num_reserved_buffers <= max_total_buffers);
	KASSERT(num_total_buffers <= max_total_buffers);
}

////////////////////////////////////////////////////////////
// supplemental array ops

/*
 * Remove an entry from a bufarray, and (unlike bufarray_remove) don't
 * preserve order.
 *
 * Use fixup() to correct the index stored in the object that gets
 * moved. Sigh.
 */
static
void
bufarray_remove_unordered(struct bufarray *a, unsigned index,
			  void (*fixup)(struct buf *,
					unsigned oldix, unsigned newix))
{
	unsigned num;
	struct buf *b;
	int result;

	num = bufarray_num(a);
	if (index < num-1) {
		b = bufarray_get(a, num-1);
		fixup(b, num-1, index);
		bufarray_set(a, index, b);
	}
	result = bufarray_setsize(a, num-1);
	/* shrinking, should not fail */
	KASSERT(result == 0);
}

/*
 * Remove null entries from a bufarray, preserving order.
 *
 * Use fixup() to correct the index stored in the objects that get
 * moved.
 */
static
void
bufarray_compact(struct bufarray *a,
		 unsigned *firstp,
		 void (*fixup)(struct buf *, unsigned oldix, unsigned newix))
{
	unsigned i, j, num;
	struct buf *b;
	int result;

	num = bufarray_num(a);
	for (i = j = *firstp; i<num; i++) {
		b = bufarray_get(a, i);
		if (b != NULL) {
			if (j < i) {
				fixup(b, i, j);
				bufarray_set(a, j++, b);
			}
			else {
				j++;
			}
		}
	}
	KASSERT(j <= num);
	result = bufarray_setsize(a, j);
	/* shrinking, shouldn't fail */
	KASSERT(result == 0);
	*firstp = j;
}

/*
 * Routines for that fixup()...
 */
static
void
buf_fixup_bucketindex(struct buf *b, unsigned oldix, unsigned newix)
{
	KASSERT(b->b_bucketindex == oldix);
	b->b_bucketindex = newix;
}

static
void
buf_fixup_dirtyindex(struct buf *b, unsigned oldix, unsigned newix)
{
	KASSERT(b->b_dirtyindex == oldix);
	b->b_dirtyindex = newix;
}

static
void
buf_fixup_tableindex(struct buf *b, unsigned oldix, unsigned newix)
{
	KASSERT(b->b_tableindex == oldix);
	b->b_tableindex = newix;
}

////////////////////////////////////////////////////////////
// bufhash

/*
 * Set up a bufhash.
 */
static
int
bufhash_init(struct bufhash *bh, unsigned numbuckets)
{
	unsigned i;

	bh->bh_buckets = kmalloc(numbuckets*sizeof(*bh->bh_buckets));
	if (bh->bh_buckets == NULL) {
		return ENOMEM;
	}
	for (i=0; i<numbuckets; i++) {
		bufarray_init(&bh->bh_buckets[i]);
	}
	bh->bh_numbuckets = numbuckets;
	return 0;
}

#if 0 /* not used */
/*
 * Destroy a bufhash.
 */
static
int
bufhash_cleanup(struct bufhash *bh)
{
	...
}
#endif /* 0 -- not used */

/*
 * Hash function.
 */
static
unsigned
buffer_hashfunc(struct fs *fs, daddr_t physblock)
{
	unsigned val = 0;

	/* there is nothing particularly special or good about this */
	val = 0xfeeb1e;
	val ^= ((uintptr_t)fs) >> 6;
	val ^= physblock;
	return val;
}

/*
 * Add a buffer to a bufhash.
 */
static
int
bufhash_add(struct bufhash *bh, struct buf *b)
{
	unsigned hash, bn;

	KASSERT(b->b_bucketindex == INVALID_INDEX);

	hash = buffer_hashfunc(b->b_fs, b->b_physblock);
	bn = hash % bh->bh_numbuckets;
	return bufarray_add(&bh->bh_buckets[bn], b, &b->b_bucketindex);
}

/*
 * Remove a buffer from a bufhash.
 */
static
void
bufhash_remove(struct bufhash *bh, struct buf *b)
{
	unsigned hash, bn;

	hash = buffer_hashfunc(b->b_fs, b->b_physblock);
	bn = hash % bh->bh_numbuckets;

	KASSERT(bufarray_get(&bh->bh_buckets[bn], b->b_bucketindex) == b);
	bufarray_set(&bh->bh_buckets[bn], b->b_bucketindex, NULL);
	bufarray_remove_unordered(&bh->bh_buckets[bn], b->b_bucketindex,
				  buf_fixup_bucketindex);
	b->b_bucketindex = INVALID_INDEX;
}

/*
 * Find a buffer in a bufhash.
 */
static
struct buf *
bufhash_get(struct bufhash *bh, struct fs *fs, daddr_t physblock)
{
	unsigned hash, bn;
	unsigned num, i;
	struct buf *b;

	hash = buffer_hashfunc(fs, physblock);
	bn = hash % bh->bh_numbuckets;

	num = bufarray_num(&bh->bh_buckets[bn]);
	for (i=0; i<num; i++) {
		b = bufarray_get(&bh->bh_buckets[bn], i);
		KASSERT(b->b_bucketindex == i);
		if (b->b_fs == fs && b->b_physblock == physblock) {
			/* found */
			return b;
		}
	}
	return NULL;
}

////////////////////////////////////////////////////////////
// buffer tables

/*
 * Preallocate the buffer lists so adding things to them on the fly
 * can't blow up.
 */
static
int
preallocate_buffer_arrays(unsigned newtotal)
{
	int result;
	unsigned newathresh, newdthresh;

	newathresh = (newtotal*ATTACHED_THRESH_NUM)/ATTACHED_THRESH_DENOM;
	newdthresh = (newtotal*DIRTY_THRESH_NUM)/DIRTY_THRESH_DENOM;

	result = bufarray_preallocate(&detached_buffers, newtotal);
	if (result) {
		return result;
	}

	result = bufarray_preallocate(&attached_buffers, newathresh);
	if (result) {
		return result;
	}
	attached_buffers_thresh = newathresh;

	result = bufarray_preallocate(&dirty_buffers, newdthresh);
	if (result) {
		return result;
	}
	dirty_buffers_thresh = newdthresh;

	return 0;
}

/*
 * Go through the attached_buffers array and close up gaps.
 */
static
void
compact_attached_buffers(void)
{
	bufarray_compact(&attached_buffers, &attached_buffers_first,
			 buf_fixup_tableindex);
	KASSERT(attached_buffers_count == bufarray_num(&attached_buffers));

	/* it does not matter if this overflows */
	attached_buffers_generation++;
}

/*
 * Go through the dirty_buffers array and close up gaps.
 */
static
void
compact_dirty_buffers(void)
{
	bufarray_compact(&dirty_buffers, &dirty_buffers_first,
			 buf_fixup_dirtyindex);
	KASSERT(dirty_buffers_count == bufarray_num(&dirty_buffers));

	/* it does not matter if this overflows */
	dirty_buffers_generation++;
}

/*
 * Get a buffer from the pool of detached buffers.
 */
static
struct buf *
buffer_remove_detached(void)
{
	struct buf *b;
	unsigned num;
	int result;

	num = bufarray_num(&detached_buffers);
	if (num > 0) {
		b = bufarray_get(&detached_buffers, num-1);
		KASSERT(b->b_tableindex == num-1);
		b->b_tableindex = INVALID_INDEX;

		/* shrink array (should not fail) */
		result = bufarray_setsize(&detached_buffers, num-1);
		KASSERT(result == 0);

		return b;
	}

	return NULL;
}

/*
 * Put a buffer into the pool of detached buffers.
 */
static
void
buffer_insert_detached(struct buf *b)
{
	int result;

	KASSERT(b->b_attached == 0);
	KASSERT(b->b_busy == 0);
	KASSERT(b->b_tableindex == INVALID_INDEX);

	result = bufarray_add(&detached_buffers, b, &b->b_tableindex);
	/* arrays are preallocated to avoid failure here */
	KASSERT(result == 0);
}

/*
 * Remove a buffer from the attached (LRU) list.
 */
static
void
buffer_remove_attached(struct buf *b, unsigned expected_busy)
{
	unsigned ix;

	KASSERT(b->b_attached == 1);
	KASSERT(b->b_busy == expected_busy);

	ix = b->b_tableindex;

	KASSERT(bufarray_get(&attached_buffers, ix) == b);

	/* Remove from table, leave NULL behind (compact lazily, later) */
	bufarray_set(&attached_buffers, ix, NULL);
	b->b_tableindex = INVALID_INDEX;

	/* cache the first empty slot  */
	if (ix < attached_buffers_first) {
		attached_buffers_first = ix;
	}

	attached_buffers_count--;
}

/*
 * Put a buffer into the attached (LRU) list, always at the end.
 */
static
void
buffer_insert_attached(struct buf *b)
{
	unsigned num;
	int result;

	KASSERT(b->b_attached == 1);
	KASSERT(b->b_tableindex == INVALID_INDEX);

	num = bufarray_num(&attached_buffers);
	if (num >= attached_buffers_thresh) {
		compact_attached_buffers();
	}

	result = bufarray_add(&attached_buffers, b, &b->b_tableindex);
	/* arrays are preallocated to avoid failure here */
	KASSERT(result == 0);
	attached_buffers_count++;
}

/*
 * Get a buffer out of the dirty list.
 */
static
void
buffer_remove_dirty(struct buf *b)
{
	unsigned ix;

	KASSERT(b->b_attached == 1);
	// not necessarily true, e.g. in buffer_drop()
	//KASSERT(b->b_busy == 1);

	ix = b->b_dirtyindex;

	KASSERT(bufarray_get(&dirty_buffers, ix) == b);

	/* Remove from table, leave NULL behind (compact lazily, later) */
	bufarray_set(&dirty_buffers, ix, NULL);
	b->b_dirtyindex = INVALID_INDEX;

	/* cache the first empty slot  */
	if (ix < dirty_buffers_first) {
		dirty_buffers_first = ix;
	}
}

/*
 * Put a buffer into the dirty list.
 */
static
void
buffer_insert_dirty(struct buf *b)
{
	unsigned num;
	int result;

	KASSERT(b->b_attached == 1);
	KASSERT(b->b_busy == 1);
	KASSERT(b->b_dirtyindex == INVALID_INDEX);

	num = bufarray_num(&dirty_buffers);
	if (num >= dirty_buffers_thresh) {
		compact_dirty_buffers();
	}

	result = bufarray_add(&dirty_buffers, b, &b->b_dirtyindex);
	/* arrays are preallocated to avoid failure here */
	KASSERT(result == 0);
}

////////////////////////////////////////////////////////////
// ops on buffers

/*
 * Create a fresh buffer.
 */
static
struct buf *
buffer_create(void)
{
	struct buf *b;
	int result;

	result = preallocate_buffer_arrays(num_total_buffers+1);
	if (result) {
		return NULL;
	}

	b = kmalloc(sizeof(*b));
	if (b == NULL) {
		return NULL;
	}

	b->b_data = kmalloc(ONE_TRUE_BUFFER_SIZE);
	if (b->b_data == NULL) {
		kfree(b);
		return NULL;
	}

	b->b_tableindex = INVALID_INDEX;
	b->b_dirtyindex = INVALID_INDEX;
	b->b_bucketindex = INVALID_INDEX;
	b->b_dirtyepoch = 0;
	b->b_attached = 0;
	b->b_busy = 0;
	b->b_valid = 0;
	b->b_dirty = 0;
	b->b_fsmanaged = 0;
	b->b_holder = NULL;
	b->b_timestamp.tv_sec = 0;
	b->b_timestamp.tv_nsec = 0;
	b->b_fs = NULL;
	b->b_physblock = 0;
	b->b_size = ONE_TRUE_BUFFER_SIZE;
	b->b_fsdata = NULL;
	num_total_buffers++;
	return b;
}

/*
 * Attach a buffer to a given key (fs and block number)
 */
static
int
buffer_attach(struct buf *b, struct fs *fs, daddr_t block)
{
	int result;

	KASSERT(b->b_busy == 0);
	KASSERT(b->b_attached == 0);
	KASSERT(b->b_valid == 0);
	KASSERT(b->b_busy == 0);
	KASSERT(b->b_fsdata == NULL);
	b->b_attached = 1;
	b->b_fs = fs;
	b->b_physblock = block;

	result = bufhash_add(&buffer_hash, b);
	if (result) {
		b->b_attached = 0;
		b->b_fs = NULL;
		b->b_physblock = 0;
		return result;
	}
	return 0;
}

/*
 * Detach a buffer from a particular key.
 */
static
void
buffer_detach(struct buf *b)
{
	KASSERT(b->b_attached == 1);
	KASSERT(b->b_busy == 0);
	bufhash_remove(&buffer_hash, b);

	if (b->b_fsdata != NULL) {
		kprintf("vfs: %s left behind fs-specific buffer data\n",
			FSOP_GETVOLNAME(b->b_fs));
		b->b_fsdata = NULL;
	}
	b->b_attached = 0;
	b->b_fs = NULL;
	b->b_physblock = 0;
	cv_broadcast(buffer_busy_cv, buffer_lock);
}

/*
 * Mark a buffer busy, waiting if necessary.
 *
 * Returns EDEADBUF if the buffer gets detached (or worse, detached
 * and reattached) under us, which can happen if it gets released and
 * then gets evicted before we wake up. If it gets detached and
 * reattached to the same block, we won't notice, but in that case we
 * probably don't care either.
 */
static
int
buffer_mark_busy(struct buf *b)
{
	struct fs *fs;
	daddr_t block;

	KASSERT(b->b_holder != curthread);
	fs = b->b_fs;
	block = b->b_physblock;
	while (b->b_busy) {
		if (!b->b_attached || fs != b->b_fs ||
		    block != b->b_physblock) {
			return EDEADBUF;
		}
		cv_wait(buffer_busy_cv, buffer_lock);
	}
	if (!b->b_attached || fs != b->b_fs || block != b->b_physblock) {
		return EDEADBUF;
	}
	b->b_busy = 1;
	KASSERT(b->b_fsmanaged == 0);
	b->b_holder = curthread;
	busy_buffers_count++;
	return 0;
}

/*
 * Unmark a buffer busy, awakening waiters.
 */
static
void
buffer_unmark_busy(struct buf *b)
{
	KASSERT(b->b_busy != 0);
	b->b_busy = 0;
	if (b->b_fsmanaged) {
		b->b_fsmanaged = false;
	}
	else {
		KASSERT(b->b_holder == curthread);
	}
	b->b_holder = NULL;
	busy_buffers_count--;
	cv_broadcast(buffer_busy_cv, buffer_lock);
}

/*
 * I/O: disk to buffer
 */
static
int
buffer_readin(struct buf *b)
{
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	KASSERT(b->b_attached);
	KASSERT(b->b_busy);
	KASSERT(b->b_fs != NULL);

	if (b->b_valid) {
		return 0;
	}

	lock_release(buffer_lock);
	result = FSOP_READBLOCK(b->b_fs, b->b_physblock, b->b_data, b->b_size);
	lock_acquire(buffer_lock);
	if (result == 0) {
		b->b_valid = 1;
	}
	return result;
}

/*
 * I/O: buffer to disk
 *
 * Note: releases lock to do I/O; busy bit should be set to protect
 *
 * buffer_writeout differs from buffer_sync in that buffer_writeout
 * always writes the buffer, and buffer_sync is specifically for
 * syncing and checks b_fsmanaged. (Also, buffer_writeout requires a
 * buffer that is already held by the caller, and buffer_sync one that
 * is not.)
 */
static
int
buffer_writeout_internal(struct buf *b)
{
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();

	KASSERT(b->b_attached);
	KASSERT(b->b_valid);
	KASSERT(b->b_busy);
	KASSERT(b->b_fs != NULL);

	if (!b->b_dirty) {
		return 0;
	}

	num_total_writeouts++;
	lock_release(buffer_lock);
	result = FSOP_WRITEBLOCK(b->b_fs, b->b_physblock, b->b_fsdata,
				 b->b_data, b->b_size);
	lock_acquire(buffer_lock);
	if (result == 0) {
		dirty_buffers_count--;
		b->b_dirty = 0;
		buffer_remove_dirty(b);
	}
	return result;
}

int
buffer_writeout(struct buf *b)
{
	int result;

	lock_acquire(buffer_lock);
	result = buffer_writeout_internal(b);
	lock_release(buffer_lock);
	return result;
}

/*
 * Fetch buffer pointer (external op)
 *
 * no lock necessary because of busy bit
 */
void *
buffer_map(struct buf *b)
{
	KASSERT(b->b_busy);
	return b->b_data;
}

/*
 * Check if buffer is dirty. (external op)
 */
bool
buffer_is_dirty(struct buf *b)
{
	KASSERT(b->b_busy);
	KASSERT(b->b_valid);

	return b->b_dirty;
}

/*
 * Check if buffer is valid. (external op)
 */
bool
buffer_is_valid(struct buf *b)
{
	KASSERT(b->b_busy);

	return b->b_valid;
}

/*
 * Mark buffer dirty (external op, for after messing with buffer pointer)
 */
void
buffer_mark_dirty(struct buf *b)
{
	KASSERT(b->b_busy);
	KASSERT(b->b_valid);

	lock_acquire(buffer_lock);
	if (b->b_dirty) {
		/* nothing to do */
		lock_release(buffer_lock);
		return;
	}

	b->b_dirty = 1;
	b->b_dirtyepoch = dirty_epoch;
	gettime(&b->b_timestamp);

	/* XXX: should we avoid putting fsmanaged buffers on the dirty list? */

	buffer_insert_dirty(b);
	dirty_buffers_count++;
	/* Here we might prod the syncer, but currently it doesn't need it */
	lock_release(buffer_lock);
}

/*
 * Mark buffer valid (external op, for after messing with buffer pointer)
 */
void
buffer_mark_valid(struct buf *b)
{
	KASSERT(b->b_busy);
	b->b_valid = 1;
}

////////////////////////////////////////////////////////////
// buffer get/release

/*
 * Write a buffer out.
 *
 * buffer_writeout differs from buffer_sync in that buffer_writeout
 * always writes the buffer, and buffer_sync is specifically for
 * syncing and checks b_fsmanaged. (Also, buffer_writeout requires a
 * buffer that is already held by the caller, and buffer_sync one that
 * is not.)
 */
static
int
buffer_sync(struct buf *b)
{
	int result;

	KASSERT(b->b_valid == 1);
	KASSERT(b->b_dirty == 1);

	if (b->b_fsmanaged) {
		KASSERT(b->b_busy);
		/* Succeed without doing anything; buffer remains dirty. */
		return 0;
	}

	/*
	 * Mark it busy while we do I/O.
	 */
	result = buffer_mark_busy(b);
	if (result) {
		/* may be EDEADBUF */
		return result;
	}
	KASSERT(b->b_valid == 1);
	if (!b->b_dirty) {
		/* Someone else wrote it out while we were waiting */
		buffer_unmark_busy(b);
		return 0;
	}

	result = buffer_writeout_internal(b);
	/*
	 * The caller needs to be able to distinguish buffer_mark_busy
	 * failing (which requires specific handling) from any failure
	 * that can happen writing the buffer out. Therefore,
	 * buffer_writeout_internal isn't allowed to return EDEADBUF.
	 */
	KASSERT(result != EDEADBUF);

	buffer_unmark_busy(b);

	return result;
}

/*
 * Write out one buffer from the dirty_buffers queue.
 *
 * If the syncer has signalled for help, this is called on every
 * buffer_get until the dirty_buffers queue gets back to a manageable
 * state.
 *
 * We don't attempt to sync buffers that are currently busy, because
 * that might deadlock; we'll let the syncer deal with those.
 *
 * (Similarly, we don't want to wait for the syncer, in case the
 * syncer is waiting for a buffer we hold.)
 */
static
void
sync_one_old_buffer(void)
{
	unsigned i;
	struct buf *b;
	int result;

	for (i=0; i < bufarray_num(&dirty_buffers); i++) {
		b = bufarray_get(&dirty_buffers, i);
		if (b == NULL) {
			continue;
		}
		if (b->b_fsmanaged) {
			continue;
		}
		if (b->b_busy) {
			continue;
		}
		KASSERT(b->b_dirty);

		/* could check the buffer age here, but let's not bother */

		result = buffer_sync(b);
		if (result) {
			/* wasn't busy -> didn't wait -> can't disappear */
			KASSERT(result != EDEADBUF);
			/* let the syncer deal with it */
			(void)result;
		}
		break;
	}
}

/*
 * Clean out a buffer for reuse and detach it.
 *
 * Does not put it on the detached list; the caller should do that if
 * desired.
 */
static
void
buffer_clean(struct buf *b)
{
	int result;

	KASSERT(b->b_busy == 0);
	result = buffer_mark_busy(b);
	/* not busy, won't sleep, can't fail */
	KASSERT(result == 0);

	lock_release(buffer_lock);
	FSOP_DETACHBUF(b->b_fs, b->b_physblock, b);
	lock_acquire(buffer_lock);
	buffer_unmark_busy(b);

	buffer_remove_attached(b, 0);
	b->b_valid = 0;
	if (b->b_dirty) {
		b->b_dirty = 0;
		dirty_buffers_count--;
		buffer_remove_dirty(b);
	}
	buffer_detach(b);
}

/*
 * Evict a buffer.
 */
static
int
buffer_evict(struct buf **ret)
{
	unsigned num, i;
	struct buf *b, *db;
	int result;

	/*
	 * Find a target buffer.
	 */

 tryagain:
	num = bufarray_num(&attached_buffers);
	b = db = NULL;
	for (i=0; i<num; i++) {
		if (i >= num/2 && db != NULL) {
			/*
			 * voodoo: avoid preferring very recent clean
			 * buffers to older dirty buffers.
			 */
			break;
		}
		b = bufarray_get(&attached_buffers, i);
		if (b == NULL) {
			continue;
		}
		if (b->b_busy == 1) {
			b = NULL;
			continue;
		}
		/* fsmanaged buffers are always busy */
		KASSERT(b->b_fsmanaged == 0);
		if (b->b_dirty == 1) {
			if (db == NULL) {
				/* remember first dirty buffer we saw */
				db = b;
			}
			b = NULL;
			continue;
		}
		break;
	}
	if (b == NULL && db != NULL) {
		b = db;
	}
	if (b == NULL) {
		/* No buffers at all...? */
		kprintf("buffer_evict: no targets!?\n");
		return EAGAIN;
	}

	/*
	 * Flush the buffer out if necessary.
	 */
	num_total_evictions++;
	if (b->b_dirty) {
		num_dirty_evictions++;
		KASSERT(b->b_busy == 0);
		/* lock may be released here */
		result = buffer_sync(b);
		if (result) {
			/* it wasn't busy, so it can't disappear */
			KASSERT(result != EDEADBUF);

			/* urgh... get another buffer */
			kprintf("buffer_evict: warning: %s\n",
				strerror(result));
			buffer_remove_attached(b, 0);
			buffer_insert_attached(b);
			goto tryagain;
		}
	}

	KASSERT(b->b_dirty == 0);

	/*
	 * Detach it from its old key, and return it in a state where
	 * it can be reattached properly.
	 */
	buffer_clean(b);

	*ret = b;
	return 0;
}

static
struct buf *
buffer_find(struct fs *fs, daddr_t physblock)
{
	return bufhash_get(&buffer_hash, fs, physblock);
}

/*
 * Find a buffer for the given block, if one already exists; otherwise
 * attach one but don't bother to read it in. Set fsmanaged mode if
 * FSMANAGED is true.
 */
static
int
buffer_get_internal(struct fs *fs, daddr_t block, size_t size, bool fsmanaged,
		    struct buf **ret)
{
	struct buf *b;
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);
	if (!fsmanaged) {
		KASSERT(curthread->t_did_reserve_buffers == true);
	}

	if (!fsmanaged && syncer_needs_help) {
		sync_one_old_buffer();
	}

	num_total_gets++;

again:
	b = buffer_find(fs, block);
	if (b != NULL) {
		result = buffer_mark_busy(b);
		if (result) {
			KASSERT(result == EDEADBUF);
			goto again;
		}
		num_valid_gets++;
		buffer_remove_attached(b, 1);

		/* move it to the tail (recent end) of the LRU list */
		buffer_insert_attached(b);
	}
	else {
		b = buffer_remove_detached();
		if (b == NULL && num_total_buffers < max_total_buffers) {
			/* Can create a new buffer... */
			b = buffer_create();
		}
		if (b == NULL) {
			result = buffer_evict(&b);
			if (result) {
				return result;
			}
			KASSERT(b != NULL);
		}

		KASSERT(b->b_size == ONE_TRUE_BUFFER_SIZE);
		result = buffer_attach(b, fs, block);
		if (result) {
			buffer_insert_detached(b);
			return result;
		}
		KASSERT(b->b_busy == 0);
		result = buffer_mark_busy(b);
		/* b wasn't busy, so we didn't wait and it didn't disappear */
		KASSERT(result == 0);

		/* move it to the tail (recent end) of the LRU list */
		buffer_insert_attached(b);

		/*
		 * Call the FS's buffer attach routine. We do this
		 * after buffer_attach (rather than in it) so we can
		 * do it safely with the buffer marked busy and
		 * without holding buffer_lock, as buffer_lock isn't
		 * supposed to be exposed to file system code.
		 *
		 * Note: b_fsmanaged, if requested, hasn't been set
		 * yet.  There's some chance that this might confuse
		 * FS code, in which case it should be set here
		 * instead; I haven't done this because that requires
		 * duplicating the code.
		 */

		lock_release(buffer_lock);
		result = FSOP_ATTACHBUF(b->b_fs, block, b);
		lock_acquire(buffer_lock);
		if (result) {
			buffer_unmark_busy(b);
			buffer_insert_detached(b);
			return result;
		}
	}

	/* crosscheck that we got what we asked for */
	KASSERT(b->b_fs == fs && b->b_physblock == block);


	if (fsmanaged) {
		b->b_fsmanaged = 1;
	}

	*ret = b;
	return 0;
}

/*
 * Same as buffer_get_internal but does a read so the resulting buffer
 * always contains valid data.
 */
static
int
buffer_read_internal(struct fs *fs, daddr_t block, size_t size, bool fsmanaged,
		     struct buf **ret)
{
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));

	result = buffer_get_internal(fs, block, size, fsmanaged, ret);
	if (result) {
		lock_release(buffer_lock);
		*ret = NULL;
		return result;
	}

	if (!(*ret)->b_valid) {
		num_read_gets++;
		/* may lose (and then re-acquire) lock here */
		result = buffer_readin(*ret);
		if (result) {
			buffer_release_internal(*ret);
			*ret = NULL;
			return result;
		}
	}

	return 0;
}

/*
 * Find a buffer for the given block, if one already exists; otherwise
 * attach one but don't bother to read it in.
 */
int
buffer_get(struct fs *fs, daddr_t block, size_t size, struct buf **ret)
{
	int result;

	lock_acquire(buffer_lock);
	result = buffer_get_internal(fs, block, size, false/*fsmanaged*/, ret);
	lock_release(buffer_lock);

	return result;
}

/*
 * Same as buffer_get but does a read so the resulting buffer always
 * contains valid data.
 */
int
buffer_read(struct fs *fs, daddr_t block, size_t size, struct buf **ret)
{
	int result;

	lock_acquire(buffer_lock);
	result = buffer_read_internal(fs, block, size, false/*fsmanaged*/,ret);
	lock_release(buffer_lock);

	return result;
}

/*
 * The fsmanaged version of buffer_get.
 */
int
buffer_get_fsmanaged(struct fs *fs, daddr_t block, size_t size,
		     struct buf **ret)
{
	int result;

	lock_acquire(buffer_lock);
	result = buffer_get_internal(fs, block, size, true/*fsmanaged*/, ret);
	lock_release(buffer_lock);

	return result;
}

/*
 * The fsmanaged version of buffer_read.
 */
int
buffer_read_fsmanaged(struct fs *fs, daddr_t block, size_t size,
		      struct buf **ret)
{
	int result;

	lock_acquire(buffer_lock);
	result = buffer_read_internal(fs, block, size, true/*fsmanaged*/, ret);
	lock_release(buffer_lock);

	return result;
}

/*
 * Shortcut combination of buffer_get and buffer_writeout that writes
 * out any existing buffer if it's dirty and otherwise does nothing.
 *
 * This is one of the tools FSes can use to manage fsmanaged buffers,
 * so we explicitly use buffer_writeout and not buffer_sync, as
 * buffer_sync ignores fsmanaged buffers.
 */
int
buffer_flush(struct fs *fs, daddr_t block, size_t size)
{
	struct buf *b;
	int result = 0;

	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	b = buffer_find(fs, block);
	if (b == NULL) {
		goto done;
	}
	KASSERT(b->b_valid);

	if (!b->b_dirty) {
		/* Not dirty; don't need to do anything. */
		goto done;
	}

	result = buffer_mark_busy(b);
	if (result) {
		KASSERT(result == EDEADBUF);
		/* Buffer disappeared; no longer need to write it */
		result = 0;
		goto done;
	}

	if (!b->b_dirty) {
		/* Someone else wrote it out. */
		buffer_unmark_busy(b);
		goto done;
	}

	/* crosscheck that we got what we asked for */
	KASSERT(b->b_fs == fs && b->b_physblock == block);

	result = buffer_writeout_internal(b);
	/* as per the call in buffer_sync */
	KASSERT(result != EDEADBUF);

	buffer_unmark_busy(b);
done:
	lock_release(buffer_lock);
	return result;
}

/*
 * Shortcut combination of buffer_get and buffer_release_and_invalidate
 * that invalidates any existing buffer and otherwise does nothing.
 */
void
buffer_drop(struct fs *fs, daddr_t block, size_t size)
{
	struct buf *b;
	int result;

	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	b = buffer_find(fs, block);
	if (b != NULL) {
		/*
		 * While the FS shouldn't ever drop a buffer that it's also
		 * actively using, the buffer might be getting synced. So
		 * wait for it, then release it again. Because we're locked,
		 * nobody else can get it at that point until we finish.
		 */
		result = buffer_mark_busy(b);
		if (result == EDEADBUF) {
			/* someone else already dropped it */
			lock_release(buffer_lock);
			return;
		}
		KASSERT(result == 0);
		buffer_unmark_busy(b);

		buffer_clean(b);
		buffer_insert_detached(b);
	}
	lock_release(buffer_lock);
}

static
void
buffer_release_internal(struct buf *b)
{
	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();

	if (!b->b_fsmanaged) {
		/* buffers must be released while still reserved */
		KASSERT(curthread->t_did_reserve_buffers == true);
	}

	buffer_unmark_busy(b);

	if (!b->b_valid) {
		/* detach it */
		buffer_clean(b);
		buffer_insert_detached(b);
	}
	else {
		/* move it to the end of the LRU list */
		buffer_remove_attached(b, 0);
		buffer_insert_attached(b);
	}
}

/*
 * Let go of a buffer obtained with buffer_get or buffer_read.
 */
void
buffer_release(struct buf *b)
{
	lock_acquire(buffer_lock);
	buffer_release_internal(b);
	lock_release(buffer_lock);
}

/*
 * Same as buffer_release, but also invalidates the buffer.
 */
void
buffer_release_and_invalidate(struct buf *b)
{
	lock_acquire(buffer_lock);
	bufcheck();

	b->b_valid = 0;
	buffer_release_internal(b);
	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// user data

void *
buffer_get_fsdata(struct buf *buf)
{
	return buf->b_fsdata;
}

void *
buffer_set_fsdata(struct buf *buf, void *newfsd)
{
	void *oldfsd;

	oldfsd = buf->b_fsdata;
	buf->b_fsdata = newfsd;
	return oldfsd;
}

////////////////////////////////////////////////////////////
// explicit sync

int
sync_fs_buffers(struct fs *fs)
{
	unsigned i;
	struct buf *b;
	unsigned my_epoch, my_generation;
	int result;

	lock_acquire(buffer_lock);
	bufcheck();

	my_epoch = dirty_epoch++;
	if (dirty_epoch == 0) {
		/*
		 * Handling this instead of dying is not that
		 * difficult, but for OS/161 it's not really worth the
		 * trouble.
		 */
		panic("vfs: buffer cache syncer epoch wrapped around\n");
	}

	my_generation = dirty_buffers_generation;

	/* Don't cache the array size; it might change as we work. */
	for (i=0; i<bufarray_num(&dirty_buffers); i++) {
		b = bufarray_get(&dirty_buffers, i);
		if (b == NULL || b->b_fs != fs) {
			continue;
		}
		if (b->b_dirtyepoch > my_epoch) {
			/*
			 * This buffer became dirty after we started
			 * syncing. Not only do we not need to write
			 * it, but we can stop completely as any
			 * subsequent buffers will be even newer.
			 */
			break;
		}

		KASSERT(b->b_valid);
		KASSERT(b->b_dirty);

		/* lock may be released (and then re-acquired) here */
		result = buffer_sync(b);
		if (result == EDEADBUF) {
			/*
			 * The buffer was invalidated/evicted while we
			 * were waiting to sync it. It no longer needs
			 * syncing, so do nothing and just go on.
			 */
		}
		else if (result) {
			lock_release(buffer_lock);
			return result;
		}

		if (my_generation != dirty_buffers_generation) {
			/* compact_dirty_buffers ran; restart loop */
			i = 0;
			my_generation = dirty_buffers_generation;
			/* compensate for the i++ */
			i--;
		}
	}

	lock_release(buffer_lock);
	return 0;
}

////////////////////////////////////////////////////////////
// for unmounting

/*
 * Invalidate and detach all buffers belonging to a filesystem. Every
 * fs should do this as part of its unmount routine once it's sure
 * that the fs is idle.
 *
 * Panic if we hit a dirty buffer, as sync should just have been
 * called and no buffers should get dirty again after that.
 */
void
drop_fs_buffers(struct fs *fs)
{
	unsigned i;
	struct buf *b;
	unsigned my_generation;

	lock_acquire(buffer_lock);
	bufcheck();

	my_generation = attached_buffers_generation;
	/* Don't cache the array size; it might change as we work. */
	for (i=0; i<bufarray_num(&attached_buffers); i++) {
		b = bufarray_get(&attached_buffers, i);
		if (b == NULL || b->b_fs != fs) {
			continue;
		}

		KASSERT(b->b_valid);
		if (b->b_dirty) {
			panic("drop_fs_buffers: buffer did not get synced\n");
		}
		if (b->b_busy) {
			panic("drop_fs_buffers: buffer is busy\n");
		}

		buffer_clean(b);
		buffer_insert_detached(b);

		if (my_generation != attached_buffers_generation) {
			/* compact_attached_buffers ran; restart loop */
			i = 0;
			my_generation = attached_buffers_generation;
			/* compensate for the i++ */
			i--;
		}
	}

	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// syncer

/*
 * The syncer has two goals: first, to ensure a steady supply of old,
 * clean buffers for eviction; and second, to make sure no buffer
 * remains dirty for too long (no matter how heavily used it is) to
 * avoid data loss in a crash.
 *
 * Pursuant to this, there are two work functions, one for working
 * the queue of least-recently-used buffers (attached_buffers) and
 * one for working the queue of old dirty buffers (dirty_buffers).
 *
 * We balance work between them as follows:
 *    - Under normal circumstances, we work attached_buffers first and
 *      then dirty_buffers.
 *    - Each of the work functions has a goal after which it stops;
 *      but it limits itself to some fixed maximum number of buffers
 *      before returning, in order to bound the amount of time before
 *      the outer loop reconsiders the situation.
 *    - Under write load, we switch to working dirty_buffers first, in
 *      order to attempt to bound data loss in a crash. Because client
 *      threads will fall back to synchronous evictions from the LRU
 *      list, under these conditions the syncer should concentrate on
 *      old buffers.
 *    - Under heavy write load, we set a flag to make client threads
 *      write out old buffers.
 *    - "Write load" and "heavy write load" are defined by whether the
 *      syncer is managing to keep up with the dirty buffer load; or
 *      more precisely, by how far behind it is on dirty_buffers
 *      relative to where it wants to be.
 */

/*
 * Sync buffers from the LRU list (attached_buffers)
 *
 * When activated, we write out:
 *    - any of the N least recently used buffers that are dirty;
 *    - any of the N+K least recently used buffers that are dirty and
 *      are older than one second.
 *
 * Any buffers that can still be allocated (max_total_buffers -
 * num_total_buffers) are counted as very old clean buffers, so at
 * first we don't sync anything at all until one of the time limits
 * kicks in.
 *
 * Note that "age" (via b_timestamp) is the time since the buffer
 * means was first marked dirty, which may differ substantially
 * from how recently it has been used.
 */
static
bool
sync_lru_buffers(void)
{
	struct timespec started, now, age;
	unsigned sync_always; /* N */
	unsigned sync_ifold; /* N + K */
	unsigned seenbuffers;
	unsigned my_generation;
	unsigned loops;
	unsigned i;
	struct buf *b;
	bool finished;
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();
	KASSERT(dirty_buffers_count > 0);

	gettime(&started);
	finished = false;

	sync_always = SCALE(max_total_buffers, SYNCER_ALWAYS);
	sync_ifold = SCALE(max_total_buffers, SYNCER_IFOLD);
	seenbuffers = 0;

	/*
	 * Buffers not allocated yet are buffers we have effectively
	 * already processed.
	 */
	seenbuffers += max_total_buffers - num_total_buffers;

	my_generation = attached_buffers_generation;
	loops = 0;
	i = 0;
	while (1) {
		/* Don't cache the array size; it might change as we work. */
		if (i >= bufarray_num(&attached_buffers)) {
			/* no more buffers to look at */
			finished = true;
			break;
		}
		if (seenbuffers >= sync_ifold) {
			/* checked enough */
			finished = true;
			break;
		}

		b = bufarray_get(&attached_buffers, i);
		i++;
		if (b == NULL) {
			continue;
		}
		seenbuffers++;
		if (!b->b_dirty) {
			continue;
		}

		gettime(&now);
		timespec_sub(&started, &now, &age);
		if (age.tv_sec > 0) {
			/*
			 * Return back to the outer syncer loop if
			 * we've been running for more than 1 second.
			 */
			break;
		}

		if (seenbuffers >= sync_always) {
			timespec_sub(&now, &b->b_timestamp, &age);
			if (age.tv_sec < 1) {
				/* buffer is less than a second old */
				continue;
			}
		}

		/* This can sleep */
		result = buffer_sync(b);
		if (result == EDEADBUF) {
			/*
			 * The buffer was invalidated/evicted while we
			 * were waiting to mark it busy. It no longer
			 * needs syncing, so carry on.
			 */
		}
		else if (result) {
			/*
			 * XXX we should probably do something to
			 * avoid retrying it over and over.
			 */
			kprintf("syncer: %s: block %u: Warning: %s\n",
				FSOP_GETVOLNAME(b->b_fs), b->b_physblock,
				strerror(result));
		}

		if (my_generation != attached_buffers_generation) {
			/* compact_attached_buffers ran; restart loop */
			loops++;
			if (loops > 15) {
				/* limit 15 tries, then go on */
				break;
			}
			i = 0;
			seenbuffers = 0;
			seenbuffers += max_total_buffers - num_total_buffers;
			my_generation = attached_buffers_generation;
			continue;
		}
	}
	return finished;
}

/*
 * Update the syncer state flags based on the age of a buffer we're
 * about to write out.
 */
static
void
syncer_adjust_state(unsigned age)
{
	COMPILE_ASSERT(SYNCER_LOAD_AGE < SYNCER_HELP_AGE);

	if (age >= SYNCER_HELP_AGE) {
		/*
		 * Don't assert this; it might not be true, e.g. if a
		 * fsmanaged buffer gets buffer_release'd and thereby
		 * ceases to be fsmanaged.
		 */
		if (!syncer_under_load) {
			syncer_under_load = true;
		}
		if (!syncer_needs_help) {
			syncer_needs_help = true;
#ifdef SYNCER_VERBOSE
			kprintf("syncer: under heavy load\n");
#endif
		}
	}
	else if (age >= SYNCER_LOAD_AGE) {
		if (syncer_needs_help) {
			KASSERT(syncer_under_load);
			syncer_needs_help = false;
#ifdef SYNCER_VERBOSE
			kprintf("syncer: under load\n");
#endif
		}
		if (!syncer_under_load) {
			syncer_under_load = true;
#ifdef SYNCER_VERBOSE
			kprintf("syncer: under load\n");
#endif
		}
	}
	else {
		if (syncer_needs_help) {
			KASSERT(syncer_under_load);
			syncer_needs_help = false;
		}
		if (syncer_under_load) {
			syncer_under_load = false;
#ifdef SYNCER_VERBOSE
			kprintf("syncer: normal state\n");
#endif
		}
	}
}

/*
 * Sync buffers from the age-sorted list of dirty buffers.
 *
 * We write out any dirty buffers that are older than two seconds.
 */
static
bool
sync_old_buffers(void)
{
	struct timespec started, now, age;
	unsigned my_generation;
	unsigned i;
	struct buf *b;
	bool finished;
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();
	KASSERT(dirty_buffers_count > 0);

	gettime(&started);
	finished = false;

	my_generation = dirty_buffers_generation;
	i = 0;
	while (1) {
		/* Don't cache the array size; it might change as we work. */
		if (i >= bufarray_num(&dirty_buffers)) {
			finished = true;
			break;
		}
		b = bufarray_get(&dirty_buffers, i);
		i++;
		if (b == NULL) {
			continue;
		}
		KASSERT(b->b_dirty);
		gettime(&now);
		timespec_sub(&started, &now, &age);
		if (age.tv_sec > 0) {
			/*
			 * If we've been running for more than one
			 * second, return to the outer syncer loop.
			 */
			break;
		}
		timespec_sub(&now, &b->b_timestamp, &age);
		if (age.tv_sec < SYNCER_TARGET_AGE) {
			/*
			 * Because buffers are added to dirty[] in
			 * order and it's never reshuffled, once we
			 * see one buffer newer than we need to force
			 * out, all the rest will be newer too. So we
			 * can stop iterating.
			 */
			finished = true;
			break;
		}

		/* If we're seeing sufficiently old buffers, take steps */
		syncer_adjust_state(age.tv_sec);

		result = buffer_sync(b);
		if (result == EDEADBUF) {
			/* as above */
		}
		else if (result) {
			/*
			 * XXX we should probably do something to
			 * avoid retrying it over and over.
			 */
			kprintf("syncer: %s: block %u: Warning: %s\n",
				FSOP_GETVOLNAME(b->b_fs), b->b_physblock,
				strerror(result));
		}

		if (my_generation != dirty_buffers_generation) {
			/* compact_dirty_buffers ran; restart loop */
			i = 0;
			my_generation = dirty_buffers_generation;
			continue;
		}
	}
	if (finished && syncer_under_load) {
		/* If we finished, the age of the "next" buffer is 0. */
		syncer_adjust_state(0);
	}
	return finished;
}

/*
 * If OS/161 had a more powerful clock system, we might arrange to run
 * the syncer either when enough buffers become dirty or every second
 * or two when dirty buffers exist. But we don't really have the
 * facilities for that, so instead we'll just run once a second.
 */
static
void
syncer(void *x1, unsigned long x2)
{
	bool lru_finished, old_finished;

	(void)x1;
	(void)x2;

	lock_acquire(buffer_lock);
	syncer_thread = curthread;

	lru_finished = true;
	old_finished = true;
	while (1) {
		if (lru_finished && old_finished) {
			lock_release(buffer_lock);
			clocksleep(1);
			lock_acquire(buffer_lock);
		}

		if (syncer_needs_help) {
			old_finished = sync_old_buffers();
			lru_finished = false;
		}
		else if (syncer_under_load) {
			old_finished = sync_old_buffers();
			lru_finished = sync_lru_buffers();
		}
		else if (dirty_buffers_count > 0) {
			lru_finished = sync_lru_buffers();
			old_finished = sync_old_buffers();
		}
		else {
			lru_finished = true;
			old_finished = true;
		}
	}
	syncer_thread = NULL;
	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// reservation

/*
 * Reserve some buffers of size SIZE.
 *
 * This does not allocate buffers or mark buffers busy; it registers
 * the intent, and thereby claims the right, to do so.
 *
 * This should be called only once per file system operation, at the
 * beginning, and unreserve_buffers should not be called until the
 * operation is done and all buffers have been released. The idea is
 * that it blocks until enough buffers are available that the
 * operation will be able to get all the buffers it needs to complete.
 * Otherwise one can get deadlocks where all threads have N buffers,
 * all are trying to get another, and none are left.
 *
 * The number of buffers to reserve is fixed; we could pass in the
 * number (and in fact used to) but counting the exact numbers of
 * buffers required is not worthwhile.
 */
void
reserve_buffers(size_t size)
{
	unsigned count = RESERVE_BUFFERS;

	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	/* All buffer reservations must be done up front, all at once. */
	KASSERT(curthread->t_did_reserve_buffers == false);

	while (num_reserved_buffers + count > max_total_buffers) {
		cv_wait(buffer_reserve_cv, buffer_lock);
	}
	num_reserved_buffers += count;
	curthread->t_did_reserve_buffers = true;
	lock_release(buffer_lock);
}

/*
 * Release reservation of COUNT buffers.
 */
void
unreserve_buffers(size_t size)
{
	unsigned count = RESERVE_BUFFERS;

	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	KASSERT(curthread->t_did_reserve_buffers == true);
	KASSERT(count <= num_reserved_buffers);

	curthread->t_did_reserve_buffers = false;
	num_reserved_buffers -= count;
	cv_broadcast(buffer_reserve_cv, buffer_lock);

	lock_release(buffer_lock);
}

void
reserve_fsmanaged_buffers(unsigned count, size_t size)
{
	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	while (num_reserved_buffers + count > max_total_buffers) {
		cv_wait(buffer_reserve_cv, buffer_lock);
	}
	num_reserved_buffers += count;
	lock_release(buffer_lock);
}

void
unreserve_fsmanaged_buffers(unsigned count, size_t size)
{
	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);
	KASSERT(count <= num_reserved_buffers);

	num_reserved_buffers -= count;
	cv_broadcast(buffer_reserve_cv, buffer_lock);

	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// print stats

void
buffer_printstats(void)
{
	lock_acquire(buffer_lock);

	kprintf("Buffers: %u of %u allocated\n",
		num_total_buffers, max_total_buffers);
	kprintf("   %u detached, %u attached\n",
		bufarray_num(&detached_buffers), attached_buffers_count);
	kprintf("   %u reserved\n", num_reserved_buffers);
	kprintf("   %u busy\n", busy_buffers_count);
	kprintf("   %u dirty\n", dirty_buffers_count);

	kprintf("Buffer operations:\n");
	kprintf("   %u gets (%u hits, %u reads)\n",
		num_total_gets, num_valid_gets, num_read_gets);
	kprintf("   %u writeouts\n",
		num_total_writeouts);
	kprintf("   %u evictions (%u when dirty)\n",
		num_total_evictions, num_dirty_evictions);

	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// bootstrap

void
buffer_bootstrap(void)
{
	size_t max_buffer_mem;
	int result;

	attached_buffers_count = 0;
	dirty_buffers_count = 0;

	num_reserved_buffers = 0;
	num_total_buffers = 0;

	/* Limit total memory usage for buffers */
	max_buffer_mem =
		(mainbus_ramsize() * BUFFER_MAXMEM_NUM) / BUFFER_MAXMEM_DENOM;
	max_total_buffers = max_buffer_mem / ONE_TRUE_BUFFER_SIZE;

	kprintf("buffers: max count %lu; max size %luk\n",
		(unsigned long) max_total_buffers,
		(unsigned long) max_buffer_mem/1024);

	num_total_gets = 0;
	num_valid_gets = 0;
	num_read_gets = 0;
	num_total_writeouts = 0;
	num_total_evictions = 0;
	num_dirty_evictions = 0;

	bufarray_init(&detached_buffers);
	bufarray_init(&attached_buffers);
	bufarray_init(&dirty_buffers);
	attached_buffers_first = 0;
	attached_buffers_thresh = 0;
	dirty_buffers_first = 0;
	dirty_buffers_thresh = 0;

	result = bufhash_init(&buffer_hash, max_total_buffers/16);
	if (result) {
		panic("Creating buffer_hash failed\n");
	}

	buffer_lock = lock_create("buffer cache lock");
	if (buffer_lock == NULL) {
		panic("Creating buffer cache lock failed\n");
	}

	buffer_busy_cv = cv_create("bufbusy");
	if (buffer_busy_cv == NULL) {
		panic("Creating buffer_busy_cv failed\n");
	}

	buffer_reserve_cv = cv_create("bufreserve");
	if (buffer_reserve_cv == NULL) {
		panic("Creating buffer_reserve_cv failed\n");
	}

	result = thread_fork("syncer", NULL, syncer, NULL, 0);
	if (result) {
		panic("Starting syncer failed\n");
	}
}
