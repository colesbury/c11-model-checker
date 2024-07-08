// ./run.sh  test/seqlock.o -m 10
#include <stdio.h>
#include <threads.h>
#include <stdatomic.h>
#include <model-assert.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "pyatomic.h"
#include "librace.h"

typedef struct _object {
    int id;
} PyObject;

typedef struct {
    PyObject base;
    atomic_uint tp_version_tag;
} PyTypeObject;


typedef struct {
    atomic_uint32 sequence;
} _PySeqLock;

struct type_cache_entry {
    atomic_uint version;  // initialized from type->tp_version_tag
   _PySeqLock sequence;
    atomic_ptr name;        // reference to exactly a str or None
    atomic_ptr value;       // borrowed reference or NULL
};

static PyTypeObject type_obj;

static PyObject name1 = { .id = 1 };
static PyObject name2 = { .id = 2 };
static PyObject value1 = { .id = 3 };
static PyObject value2 = { .id = 4 };

struct type_cache_entry entry_obj;
struct type_cache_entry *entry = &entry_obj;


#define SEQLOCK_IS_UPDATING(sequence) (sequence & 0x01)

void _PySeqLock_LockWrite(_PySeqLock *seqlock)
{
    // lock by moving to an odd sequence number
    uint32_t prev = _Py_atomic_load_uint32_relaxed(&seqlock->sequence);
    while (1) {
        if (SEQLOCK_IS_UPDATING(prev)) {
            // Someone else is currently updating the cache
            _Py_yield();
            prev = _Py_atomic_load_uint32_relaxed(&seqlock->sequence);
        }
        else if (_Py_atomic_compare_exchange_uint32(&seqlock->sequence, &prev, prev + 1)) {
            // We've locked the cache
            _Py_atomic_fence_release();
            break;
        }
        else {
            _Py_yield();
        }
    }
}

// load-acquire is like: load() + acquire fence
// store-release is like: release fence + store()

void _PySeqLock_AbandonWrite(_PySeqLock *seqlock)
{
    uint32_t new_seq = _Py_atomic_load_uint32_relaxed(&seqlock->sequence) - 1;
    assert(!SEQLOCK_IS_UPDATING(new_seq));
    _Py_atomic_store_uint32(&seqlock->sequence, new_seq);
}

void _PySeqLock_UnlockWrite(_PySeqLock *seqlock)
{
    uint32_t new_seq = _Py_atomic_load_uint32_relaxed(&seqlock->sequence) + 1;
    assert(!SEQLOCK_IS_UPDATING(new_seq));
    _Py_atomic_store_uint32(&seqlock->sequence, new_seq);
}

uint32_t _PySeqLock_BeginRead(_PySeqLock *seqlock)
{
    uint32_t sequence = _Py_atomic_load_uint32_acquire(&seqlock->sequence);
    while (SEQLOCK_IS_UPDATING(sequence)) {
        _Py_yield();
        sequence = _Py_atomic_load_uint32_acquire(&seqlock->sequence);
    }

    return sequence;
}

int _PySeqLock_EndRead(_PySeqLock *seqlock, uint32_t previous)
{
    // gh-121368: We need an explicit acquire fence here to ensure that
    // this load of the sequence number is not reordered before any loads
    // withing the read lock.
    _Py_atomic_fence_acquire();

    if (_Py_atomic_load_uint32_relaxed(&seqlock->sequence) == previous) {
        return 1;
    }

    _Py_yield();
    return 0;
}

static PyObject *
_PyType_Lookup(PyTypeObject *type, PyObject *name)
{
    for (int i = 0; i < 2; i++) {
        uint32_t sequence = _PySeqLock_BeginRead(&entry->sequence);
        uint32_t entry_version = _Py_atomic_load_uint32_relaxed(&entry->version);
        uint32_t type_version = _Py_atomic_load_uint32_acquire(&type->tp_version_tag);
        if (entry_version == type_version &&
            _Py_atomic_load_ptr_relaxed(&entry->name) == name) {
            PyObject *value = _Py_atomic_load_ptr_relaxed(&entry->value);
            // If the sequence is still valid then we're done
            if (1 /*value == NULL || _Py_TryIncref(value)*/) {
                if (_PySeqLock_EndRead(&entry->sequence, sequence)) {
                    return value;
                }
                // Py_XDECREF(value);
            }
            else {
                // If we can't incref the object we need to fallback to locking
                break;
            }
        }
        else {
            // cache miss
            break;
        }
    }
    return NULL;
}

static void
thread1(void *arg)
{
    PyObject *value = _PyType_Lookup(&type_obj, &name1);
    MODEL_ASSERT(value == &value1 || value == NULL);
}


static void
thread2(void *arg)
{
    _Py_atomic_store_uint32_relaxed(&type_obj.tp_version_tag, 101);

    _PySeqLock_LockWrite(&entry->sequence);
    _Py_atomic_store_ptr_relaxed(&entry->value, &value2);
    _Py_atomic_store_uint32_relaxed(&entry->version, 101);
    _Py_atomic_store_ptr_relaxed(&entry->name, &name2);
    _PySeqLock_UnlockWrite(&entry->sequence);
}

int
user_main(int argc, char **argv)
{
    atomic_init(&entry->sequence.sequence, 0);
    atomic_init(&entry->name, &name1);
    atomic_init(&entry->value, &value1);
    atomic_init(&entry->version, 100);
    atomic_init(&type_obj.tp_version_tag, 100);

    thrd_t threads[2];
    thrd_create(&threads[0], thread1, NULL);
    thrd_create(&threads[1], thread2, NULL);

    thrd_join(threads[0]);
    thrd_join(threads[1]);

	return 0;
}
