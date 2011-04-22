/*
 * lock-free-alloc.c: Lock free allocator.
 *
 * (C) Copyright 2011 Novell, Inc
 */

#include <glib.h>
#include <unistd.h>
#include <stdlib.h>

#include "mono-mmap.h"
#include "mono-membar.h"
#include "hazard.h"
#include "atomic.h"
#include "queue.h"
#include "sgen-gc.h"

#include "lock-free-alloc.h"

//#define DESC_AVAIL_DUMMY

//#define LAST_BYTE_DEBUG

#ifdef LAST_BYTE_DEBUG
#define LAST_BYTE(p,s)	(*((unsigned char*)p + (s) - 1))
#endif

static volatile gboolean stop_threads = FALSE;

#define ASSERT(x)	do {				\
		if (!(x)) {				\
			stop_threads = TRUE;		\
			mono_memory_write_barrier ();	\
			g_assert_not_reached ();	\
		}					\
	} while (0)

enum {
	STATE_FULL,
	STATE_PARTIAL,
	STATE_EMPTY
};

typedef union {
	guint64 value;
	struct {
		guint64 avail : 10;
		guint64 count : 10;
		guint64 state : 2;
		guint64 tag : 42;
	} data;
} Anchor;

typedef struct _MonoLockFreeAllocDescriptor Descriptor;
struct _MonoLockFreeAllocDescriptor {
	MonoLockFreeQueueNode node;
	MonoLockFreeAllocator *heap;
	Anchor anchor;
	unsigned int slot_size;
	unsigned int max_count;
	gpointer sb;
#ifndef DESC_AVAIL_DUMMY
	Descriptor *next;
#endif
	gboolean in_use;
};

#define NUM_DESC_BATCH	64

#define SB_SIZE		16384
#define SB_HEADER_SIZE	16
#define SB_USABLE_SIZE	(SB_SIZE - SB_HEADER_SIZE)
#define MAX_SMALL_SIZE	(8192 - 8)

#define SB_HEADER_FOR_ADDR(a)	((gpointer)((mword)(a) & ~(mword)(SB_SIZE-1)))
#define DESCRIPTOR_FOR_ADDR(a)	(*(Descriptor**)SB_HEADER_FOR_ADDR (a))

static gpointer
alloc_sb (Descriptor *desc)
{
	gpointer sb_header = mono_sgen_alloc_os_memory_aligned (SB_SIZE, SB_SIZE, TRUE);
	ASSERT (sb_header == SB_HEADER_FOR_ADDR (sb_header));
	DESCRIPTOR_FOR_ADDR (sb_header) = desc;
	//g_print ("sb %p for %p\n", sb_header, desc);
	return (char*)sb_header + SB_HEADER_SIZE;
}

static void
free_sb (gpointer sb)
{
	gpointer sb_header = SB_HEADER_FOR_ADDR (sb);
	ASSERT ((char*)sb_header + SB_HEADER_SIZE == sb);
	mono_sgen_free_os_memory (sb_header, SB_SIZE);
	//g_print ("free sb %p\n", sb_header);
}

#ifndef DESC_AVAIL_DUMMY
static Descriptor * volatile desc_avail;

static Descriptor*
desc_alloc (void)
{
	MonoThreadHazardPointers *hp = mono_hazard_pointer_get ();
	Descriptor *desc;

	for (;;) {
		gboolean success;

		desc = mono_thread_hazardous_load ((gpointer * volatile)&desc_avail, hp, 1);
		if (desc) {
			Descriptor *next = desc->next;
			success = (InterlockedCompareExchangePointer ((gpointer * volatile)&desc_avail, next, desc) == desc);
		} else {
			size_t desc_size = sizeof (Descriptor);
			Descriptor *d;
			int i;

			desc = mono_sgen_alloc_os_memory (desc_size * NUM_DESC_BATCH, TRUE);

			/* Organize into linked list. */
			d = desc;
			for (i = 0; i < NUM_DESC_BATCH; ++i) {
				Descriptor *next = (i == (NUM_DESC_BATCH - 1)) ? NULL : (Descriptor*)((char*)desc + ((i + 1) * desc_size));
				d->next = next;
				mono_lock_free_queue_node_init (&d->node, TRUE);
				d = next;
			}

			mono_memory_write_barrier ();

			success = (InterlockedCompareExchangePointer ((gpointer * volatile)&desc_avail, desc->next, NULL) == NULL);

			if (!success)
				mono_sgen_free_os_memory (desc, desc_size * NUM_DESC_BATCH);
		}

		mono_hazard_pointer_clear (hp, 1);

		if (success)
			break;
	}

	ASSERT (!desc->in_use);
	desc->in_use = TRUE;

	return desc;
}

static void
desc_enqueue_avail (gpointer _desc)
{
	Descriptor *desc = _desc;
	Descriptor *old_head;

	ASSERT (desc->anchor.data.state == STATE_EMPTY);
	ASSERT (!desc->in_use);

	do {
		old_head = desc_avail;
		desc->next = old_head;
		mono_memory_write_barrier ();
	} while (InterlockedCompareExchangePointer ((gpointer * volatile)&desc_avail, desc, old_head) != old_head);
}

static void
desc_retire (Descriptor *desc)
{
	ASSERT (desc->anchor.data.state == STATE_EMPTY);
	ASSERT (desc->in_use);
	desc->in_use = FALSE;
	free_sb (desc->sb);
	mono_thread_hazardous_free_or_queue (desc, desc_enqueue_avail);
}
#else
MonoLockFreeQueue available_descs;

static Descriptor*
desc_alloc (void)
{
	Descriptor *desc = (Descriptor*)mono_lock_free_queue_dequeue (&available_descs);

	if (desc)
		return desc;

	return calloc (1, sizeof (Descriptor));
}

static void
desc_retire (Descriptor *desc)
{
	free_sb (desc->sb);
	mono_lock_free_queue_enqueue (&available_descs, &desc->node);
}
#endif

static Descriptor*
list_get_partial (MonoLockFreeAllocSizeClass *sc)
{
	for (;;) {
		Descriptor *desc = (Descriptor*) mono_lock_free_queue_dequeue (&sc->partial);
		if (!desc)
			return NULL;
		if (desc->anchor.data.state != STATE_EMPTY)
			return desc;
		desc_retire (desc);
	}
}

static void
desc_put_partial (gpointer _desc)
{
	Descriptor *desc = _desc;

	ASSERT (desc->anchor.data.state != STATE_FULL);

	mono_lock_free_queue_node_free (&desc->node);
	mono_lock_free_queue_enqueue (&desc->heap->sc->partial, &desc->node);
}

static void
list_put_partial (Descriptor *desc)
{
	ASSERT (desc->anchor.data.state != STATE_FULL);
	mono_thread_hazardous_free_or_queue (desc, desc_put_partial);
}

static void
list_remove_empty_desc (MonoLockFreeAllocSizeClass *sc)
{
	int num_non_empty = 0;
	for (;;) {
		Descriptor *desc = (Descriptor*) mono_lock_free_queue_dequeue (&sc->partial);
		if (!desc)
			return;
		/*
		 * We don't need to read atomically because we're the
		 * only thread that references this descriptor.
		 */
		if (desc->anchor.data.state == STATE_EMPTY) {
			desc_retire (desc);
		} else {
			g_assert (desc->heap->sc == sc);
			mono_thread_hazardous_free_or_queue (desc, desc_put_partial);
			if (++num_non_empty >= 2)
				return;
		}
	}
}

static Descriptor*
heap_get_partial (MonoLockFreeAllocator *heap)
{
	return list_get_partial (heap->sc);
}

static void
heap_put_partial (Descriptor *desc)
{
	list_put_partial (desc);
}

static gboolean
set_anchor (Descriptor *desc, Anchor old_anchor, Anchor new_anchor)
{
	if (old_anchor.data.state == STATE_EMPTY)
		ASSERT (new_anchor.data.state == STATE_EMPTY);

	return atomic64_cmpxchg ((volatile gint64*)&desc->anchor.value, old_anchor.value, new_anchor.value) == old_anchor.value;
}

static gpointer
alloc_from_active_or_partial (MonoLockFreeAllocator *heap)
{
	Descriptor *desc;
	Anchor old_anchor, new_anchor;
	gpointer addr;

 retry:
	desc = heap->active;
	if (desc) {
		if (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->active, NULL, desc) != desc)
			goto retry;
	} else {
		desc = heap_get_partial (heap);
		if (!desc)
			return NULL;
	}

	/* Now we own the desc. */

	do {
		unsigned int next;

		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((volatile gint64*)&desc->anchor);
		if (old_anchor.data.state == STATE_EMPTY) {
			/* We must free it because we own it. */
			desc_retire (desc);
			goto retry;
		}
		ASSERT (old_anchor.data.state == STATE_PARTIAL);
		ASSERT (old_anchor.data.count > 0);

		addr = (char*)desc->sb + old_anchor.data.avail * desc->slot_size;

		mono_memory_read_barrier ();

		next = *(unsigned int*)addr;
		ASSERT (next < SB_USABLE_SIZE / desc->slot_size);

		new_anchor.data.avail = next;
		--new_anchor.data.count;
		++new_anchor.data.tag;

		if (new_anchor.data.count == 0)
			new_anchor.data.state = STATE_FULL;
	} while (!set_anchor (desc, old_anchor, new_anchor));

	/* If the desc is partial we have to give it back. */
	if (new_anchor.data.state == STATE_PARTIAL) {
		if (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->active, desc, NULL) != NULL)
			heap_put_partial (desc);
	}

	return addr;
}

static gpointer
alloc_from_new_sb (MonoLockFreeAllocator *heap)
{
	unsigned int slot_size, count, i;
	Descriptor *desc = desc_alloc ();

	desc->sb = alloc_sb (desc);

	slot_size = desc->slot_size = heap->sc->slot_size;
	count = SB_USABLE_SIZE / slot_size;

	/* Organize blocks into linked list. */
	for (i = 1; i < count - 1; ++i)
		*(unsigned int*)((char*)desc->sb + i * slot_size) = i + 1;

	desc->heap = heap;
	/*
	 * Setting avail to 1 because 0 is the block we're allocating
	 * right away.
	 */
	desc->anchor.data.avail = 1;
	desc->anchor.data.tag = 0;
	desc->slot_size = heap->sc->slot_size;
	desc->max_count = count;

	desc->anchor.data.count = desc->max_count - 1;
	desc->anchor.data.state = STATE_PARTIAL;

	mono_memory_write_barrier ();

	/* Make it active or free it again. */
	if (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->active, desc, NULL) == NULL) {
		return desc->sb;
	} else {
		desc->anchor.data.state = STATE_EMPTY;
		desc_retire (desc);
		return NULL;
	}
}

#define TEST_SIZE	64

static MonoLockFreeAllocSizeClass test_sc;
static MonoLockFreeAllocator test_heap;

static void
init_heap (void)
{
	mono_lock_free_queue_init (&test_sc.partial);
	test_sc.slot_size = TEST_SIZE;
	test_heap.sc = &test_sc;
}

gpointer
mono_lock_free_alloc (MonoLockFreeAllocator *heap)
{
	gpointer addr;

	for (;;) {

		addr = alloc_from_active_or_partial (heap);
		if (addr)
			break;

		addr = alloc_from_new_sb (heap);
		if (addr)
			break;
	}

#ifdef LAST_BYTE_DEBUG
	ASSERT (!LAST_BYTE (addr, heap->sc->slot_size));
	LAST_BYTE (addr, heap->sc->slot_size) = 1;
#endif

	return addr;
}

void
mono_lock_free_free (gpointer ptr)
{
	Anchor old_anchor, new_anchor;
	Descriptor *desc;
	gpointer sb;
	MonoLockFreeAllocator *heap = NULL;

	desc = DESCRIPTOR_FOR_ADDR (ptr);
	sb = desc->sb;
	ASSERT (SB_HEADER_FOR_ADDR (ptr) == SB_HEADER_FOR_ADDR (sb));

#ifdef LAST_BYTE_DEBUG
	ASSERT (LAST_BYTE (ptr, desc->slot_size));
	LAST_BYTE (ptr, desc->slot_size) = 0;
#endif

	do {
		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((volatile gint64*)&desc->anchor);
		*(unsigned int*)ptr = old_anchor.data.avail;
		new_anchor.data.avail = ((char*)ptr - (char*)sb) / desc->slot_size;
		ASSERT (new_anchor.data.avail < SB_USABLE_SIZE / desc->slot_size);

		if (old_anchor.data.state == STATE_FULL)
			new_anchor.data.state = STATE_PARTIAL;

		if (++new_anchor.data.count == desc->max_count) {
			heap = desc->heap;
			new_anchor.data.state = STATE_EMPTY;
		}
	} while (!set_anchor (desc, old_anchor, new_anchor));

	if (new_anchor.data.state == STATE_EMPTY) {
		ASSERT (old_anchor.data.state != STATE_EMPTY);

		if (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->active, NULL, desc) == desc) {
			/* We own it, so we free it. */
			desc_retire (desc);
		} else {
			/*
			 * Somebody else must free it, so we do some
			 * freeing for others.
			 */
			list_remove_empty_desc (heap->sc);
		}
	} else if (old_anchor.data.state == STATE_FULL) {
		/*
		 * Nobody owned it, now we do, so we need to give it
		 * back.
		 */

		g_assert (new_anchor.data.state == STATE_PARTIAL);

		if (InterlockedCompareExchangePointer ((gpointer * volatile)&desc->heap->active, desc, NULL) != NULL)
			heap_put_partial (desc);
	}
}

#define ASSERT_OR_PRINT(c, format, ...)	do {				\
		if (!(c)) {						\
			if (print)					\
				g_print ((format), ## __VA_ARGS__);	\
			else						\
				ASSERT (FALSE);				\
		}							\
	} while (0)

static void
descriptor_check_consistency (Descriptor *desc, gboolean print)
{
	int count = desc->anchor.data.count;
	int max_count = SB_USABLE_SIZE / desc->slot_size;
	gboolean linked [max_count];
	int i, last;
	unsigned int index;

#ifndef DESC_AVAIL_DUMMY
	Descriptor *avail;

	for (avail = desc_avail; avail; avail = avail->next)
		ASSERT_OR_PRINT (desc != avail, "descriptor is in the available list\n");
#endif

	ASSERT_OR_PRINT (desc->slot_size == desc->heap->sc->slot_size, "slot size doesn't match size class\n");

	if (print)
		g_print ("descriptor %p is ", desc);

	switch (desc->anchor.data.state) {
	case STATE_FULL:
		if (print)
			g_print ("full\n");
		ASSERT_OR_PRINT (count == 0, "count is not zero: %d\n", count);
		break;
	case STATE_PARTIAL:
		if (print)
			g_print ("partial\n");
		ASSERT_OR_PRINT (count < max_count, "count too high: is %d but must be below %d\n", count, max_count);
		break;
	case STATE_EMPTY:
		if (print)
			g_print ("empty\n");
		ASSERT_OR_PRINT (count == max_count, "count is wrong: is %d but should be %d\n", count, max_count);
		break;
	default:
		ASSERT_OR_PRINT (FALSE, "invalid state\n");
	}

	for (i = 0; i < max_count; ++i)
		linked [i] = FALSE;

	index = desc->anchor.data.avail;
	last = -1;
	for (i = 0; i < count; ++i) {
		gpointer addr = (char*)desc->sb + index * desc->slot_size;
		ASSERT_OR_PRINT (index >= 0 && index < max_count,
				"index %d for %dth available slot, linked from %d, not in range [0 .. %d)\n",
				index, i, last, max_count);
		ASSERT_OR_PRINT (!linked [index], "%dth available slot %d linked twice\n", i, index);
		if (linked [index])
			break;
#ifdef LAST_BYTE_DEBUG
		ASSERT_OR_PRINT (!LAST_BYTE (addr, desc->slot_size), "debug byte on %dth available slot %d set\n", i, index);
#endif
		linked [index] = TRUE;
		last = index;
		index = *(unsigned int*)addr;
	}

	for (i = 0; i < max_count; ++i) {
		gpointer addr = (char*)desc->sb + i * desc->slot_size;
		if (linked [i])
			continue;
#ifdef LAST_BYTE_DEBUG
		ASSERT_OR_PRINT (LAST_BYTE (addr, desc->slot_size), "debug byte on non-available slot %d not set\n", i);
#endif
	}
}

static void
heap_check_consistency (MonoLockFreeAllocator *heap)
{
	Descriptor *active = heap->active;
	Descriptor *desc;
	if (active) {
		ASSERT (active->anchor.data.state == STATE_PARTIAL);
		descriptor_check_consistency (active, FALSE);
	}
	while ((desc = (Descriptor*)mono_lock_free_queue_dequeue (&heap->sc->partial))) {
		ASSERT (desc->anchor.data.state == STATE_PARTIAL || desc->anchor.data.state == STATE_EMPTY);
		descriptor_check_consistency (desc, FALSE);
	}

	g_print ("heap consistent\n");
	exit (0);
}

/* Test code */

#ifdef TEST_ALLOC

#define NUM_THREADS	4

enum {
	ACTION_NONE,
	ACTION_ALLOC,
	ACTION_FREE
};

#define ACTION_BUFFER_SIZE	16

typedef struct {
	int action;
	int index;
	gpointer p;
} ThreadAction;

typedef struct {
	pthread_t thread;
	int increment;
	volatile gboolean have_attached;
	ThreadAction action_buffer [ACTION_BUFFER_SIZE];
	int next_action_index;
} ThreadData;

static ThreadData thread_datas [NUM_THREADS];

#define NUM_ENTRIES	1024
#define NUM_ITERATIONS	100000000

static gpointer entries [NUM_ENTRIES];

static volatile guint64 atomic_test;

static void
log_action (ThreadData *data, int action, int index, gpointer p)
{
	data->action_buffer [data->next_action_index].action = action;
	data->action_buffer [data->next_action_index].index = index;
	data->action_buffer [data->next_action_index].p = p;

	data->next_action_index = (data->next_action_index + 1) % ACTION_BUFFER_SIZE;
}

static void
dump_action_logs (void)
{
	int i, j;

	for (i = 0; i < NUM_THREADS; ++i) {
		g_print ("action log for thread %d:\n\n", i);

		j = thread_datas [i].next_action_index;
		do {
			ThreadAction *action = &thread_datas [i].action_buffer [j];
			switch (action->action) {
			case ACTION_NONE:
				break;

			case ACTION_ALLOC:
				g_print ("%6d %p alloc\n", action->index, action->p);
				break;

			case ACTION_FREE:
				g_print ("%6d %p free\n", action->index, action->p);
				break;

			default:
				g_assert_not_reached ();
			}

			j = (j + 1) % ACTION_BUFFER_SIZE;
		} while (j != thread_datas [i].next_action_index);

		g_print ("\n\n");
	}
}

static void
wait_for_threads_to_attach (void)
{
	int i;
 retry:
	for (i = 0; i < NUM_THREADS; ++i) {
		if (!thread_datas [i].have_attached) {
			usleep (5000);
			goto retry;
		}
	}
}

static void*
thread_func (void *_data)
{
	ThreadData *data = _data;
	int increment = data->increment;
	int i, index;

	mono_thread_attach ();
	data->have_attached = TRUE;
	wait_for_threads_to_attach ();

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		gpointer p;
	retry:
		if (stop_threads) {
			for (;;) {
				usleep (1000000);
				g_print ("stopped and sleeped\n");
			}
		}

		p = entries [index];
		if (p) {
			if (InterlockedCompareExchangePointer ((gpointer * volatile)&entries [index], NULL, p) != p)
				goto retry;
			ASSERT (*(int*)p == index << 10);
			*(int*)p = -1;
			mono_lock_free_free (p);

			log_action (data, ACTION_FREE, index, p);
		} else {
			p = mono_lock_free_alloc (&test_heap);

			/*
			int j;

			for (j = 0; j < NUM_ENTRIES; ++j)
				ASSERT (entries [j] != p);
			*/

			*(int*)p = index << 10;

			log_action (data, ACTION_ALLOC, index, p);

			if (InterlockedCompareExchangePointer ((gpointer * volatile)&entries [index], p, NULL) != NULL) {
				//g_print ("immediate free %p\n", p);
				*(int*)p = -1;
				mono_lock_free_free (p);

				log_action (data, ACTION_FREE, index, p);

				goto retry;
			}
		}

		index += increment;
		while (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;

		guint64 a = atomic_test;
		ASSERT ((a & 0xffffffff) == (a >> 32));
		guint64 new_a = (index | ((guint64)index << 32));
		atomic64_cmpxchg ((volatile gint64*)&atomic_test, a, new_a);

		if (i % (NUM_ITERATIONS / 20) == 0)
			g_print ("thread %d: %d\n", increment, i);
	}

	return NULL;
}

int
main (void)
{
	int i;

	ASSERT (sizeof (Anchor) <= 8);

	mono_thread_hazardous_init ();

	mono_thread_attach ();

	init_heap ();
	mono_lock_free_alloc (&test_heap);

	thread_datas [0].increment = 1;
	if (NUM_THREADS >= 2)
		thread_datas [1].increment = 2;
	if (NUM_THREADS >= 3)
		thread_datas [2].increment = 3;
	if (NUM_THREADS >= 4)
		thread_datas [3].increment = 5;

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_create (&thread_datas [i].thread, NULL, thread_func, &thread_datas [i]);

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_join (thread_datas [i].thread, NULL);

	mono_thread_hazardous_try_free_all ();

	mono_thread_hazardous_print_stats ();

	heap_check_consistency (&test_heap);

	return 0;
}

#endif