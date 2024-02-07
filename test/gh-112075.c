// Test that memory_order_release is sufficient for ordering stores to
// dk_nentries and dk_usable in dictobject.c
#include <stdio.h>
#include <threads.h>
#include <stdatomic.h>
#include <model-assert.h>

#include "librace.h"

#define MAX_SIZE 10
#define LOOPS 2

atomic_int dk_usable;
atomic_int dk_nentries;

static int
shared_keys_usable_size(void)
{
    return (atomic_load_explicit(&dk_usable, memory_order_acquire) +
            atomic_load_explicit(&dk_nentries, memory_order_acquire));
}

static void
decrement_usable(void)
{
    int usable = atomic_load_explicit(&dk_usable, memory_order_relaxed);
    int nentries = atomic_load_explicit(&dk_nentries, memory_order_relaxed);

    atomic_store_explicit(&dk_nentries, nentries + 1, memory_order_relaxed);
    atomic_store_explicit(&dk_usable, usable - 1, memory_order_release);
}


static void
thread_one(void *obj)
{
    for (int i = 0; i < LOOPS; i++) {
        int usable_size = shared_keys_usable_size();
        MODEL_ASSERT(usable_size >= MAX_SIZE);
    }
}

static void
thread_two(void *obj)
{
    for (int i = 0; i < LOOPS; i++) {
        decrement_usable();
    }
}

int
user_main(int argc, char **argv)
{
    atomic_init(&dk_usable, MAX_SIZE);
    atomic_init(&dk_nentries, 0);

	thrd_t t1, t2;
    thrd_create(&t1, (thrd_start_t)&thread_one, NULL);
	thrd_create(&t2, (thrd_start_t)&thread_two, NULL);

	thrd_join(t1);
	thrd_join(t2);
	return 0;
}
