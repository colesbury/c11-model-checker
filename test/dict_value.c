// Simplified model for storing dict values.
// Demonstrates that we need "release" for assigning the value even if
// we lock the dict mutex.
#include <stdio.h>
#include <threads.h>
#include <stdatomic.h>
#include <model-assert.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "librace.h"

struct PyMutex {
    atomic_int v;
};

struct PyLongObject {
    atomic_int ob_value;
};

struct PyDictObject {
    struct PyMutex mutex;
    atomic_address value;
};

struct PyLongObject long_obj;
struct PyDictObject dict;

static void
LOCK(struct PyMutex *mutex)
{
    int expected;
    do {
        expected = 0;
    }
    while (!atomic_compare_exchange_strong(&mutex->v, &expected, 1));
}

static void
UNLOCK(struct PyMutex *mutex)
{
    int old = atomic_exchange(&mutex->v, 0);
    MODEL_ASSERT(old == 1);
}

static void
thread1(void *arg)
{
    atomic_store_explicit(&long_obj.ob_value, 1234, memory_order_relaxed);
    LOCK(&dict.mutex);
    atomic_store_explicit(&dict.value, &long_obj, memory_order_release); // This needs to be RELEASE!
    UNLOCK(&dict.mutex);
}


static void
thread2(void *arg)
{
    struct PyLongObject *value = atomic_load_explicit(&dict.value, memory_order_acquire);
    if (value != NULL) {
        int field = atomic_load_explicit(&value->ob_value, memory_order_relaxed);
        MODEL_ASSERT(field == 1234);
    }
}

int
user_main(int argc, char **argv)
{
    atomic_init(&dict.mutex.v, 0);
    atomic_init(&dict.value, 0);
    atomic_init(&long_obj.ob_value, 0);

    thrd_t threads[2];
    thrd_create(&threads[0], thread1, NULL);
    thrd_create(&threads[1], thread2, NULL);

    thrd_join(threads[0]);
    thrd_join(threads[1]);

	return 0;
}
