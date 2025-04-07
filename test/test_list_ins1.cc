// Is it safe to use a relaxed store when copying a list element to a
// different location?
//
// ./run.sh test/test_list_ins1.o
#include <stdio.h>
#include <threads.h>
#include <stdatomic.h>
#include <model-assert.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

#include "librace.h"

#define LIST_CAPACITY 10

typedef struct PyObject {
    atomic_int data;
} PyObject;

typedef struct PyListObject {
    std::mutex *mutex;
    atomic_int size;
    atomic_address ob_items;
} PyListObject;

typedef struct list_items {
    atomic_int capacity;
    atomic_address items[LIST_CAPACITY];
} list_items;

static PyListObject my_list;
static list_items my_list_items;
static PyObject obj1;

static void
thread1(void *arg)
{
    // Initialize data outside the lock
    atomic_store_explicit(&obj1.data, 1, memory_order_relaxed);

    my_list.mutex->lock();
    atomic_store_explicit(&my_list_items.items[0], &obj1, memory_order_release);
    my_list.mutex->unlock();
}

static void
thread2(void *arg)
{
    my_list.mutex->lock();

    // NOTE: We either need the release fence at (1) or the store at (2) must
    // be release (or seq_cst).
    atomic_thread_fence(memory_order_release); // (1)

    void *ptr = atomic_load_explicit(&my_list_items.items[0], memory_order_relaxed);
    if (ptr != NULL) {
        MODEL_ASSERT(atomic_load_explicit(&((PyObject*)ptr)->data, memory_order_relaxed) == 1);
        atomic_store_explicit(&my_list_items.items[1], ptr, memory_order_relaxed); // (2)
    }
    my_list.mutex->unlock();
}

static void
thread3(void *arg)
{
    void *ptr = atomic_load_explicit(&my_list_items.items[1], memory_order_acquire);
    if (ptr != NULL) {
        PyObject *obj = (PyObject *)ptr;
        int data = atomic_load_explicit(&obj->data, memory_order_relaxed);
        MODEL_ASSERT(data == 1);
    }
}

int
user_main(int argc, char **argv)
{
    atomic_init(&my_list_items.capacity, 10);
    for (int i = 0; i < LIST_CAPACITY; i++) {
        atomic_init(&my_list_items.items[i], NULL);
    }
    atomic_init(&my_list.ob_items, &my_list_items);
    atomic_init(&obj1.data, 0);
    my_list.mutex = new std::mutex();

    thrd_t t1, t2, t3;

    thrd_create(&t1, thread1, NULL);
    thrd_create(&t2, thread2, NULL);
    thrd_create(&t3, thread3, NULL);
    thrd_join(t1);
    thrd_join(t2);
    thrd_join(t3);

    return 0;
}
