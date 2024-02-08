// Model for QSBR
#include <stdio.h>
#include <threads.h>
#include <stdatomic.h>
#include <model-assert.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "librace.h"

#define QSBR_LT(a, b) ((int)((a)-(b)) < 0)
#define QSBR_LEQ(a, b) ((int)((a)-(b)) <= 0)

#define QSBR_OFFLINE 0
#define QSBR_INITIAL 1
#define QSBR_INCR    2

// Number of threads to test (two or three)
#define NUM_THREADS 2

typedef atomic_ulong atomic_uint64;

struct _qsbr_shared;

struct _qsbr_thread_state {
    atomic_uint64 seq;
    struct _qsbr_shared *shared;
    int tid;
};

struct _qsbr_shared {
    atomic_uint64 wr_seq;
    atomic_uint64 rd_seq;

    struct _qsbr_thread_state array[NUM_THREADS];
    thrd_t threads[NUM_THREADS];
};

struct object {
    int is_dead;
};

atomic_address shared_data;

struct object old_data = { .is_dead = 0 };
struct object new_data = { .is_dead = 1 };


static uint64_t
_Py_qsbr_shared_current(struct _qsbr_shared *shared)
{
    return atomic_load_explicit(&shared->wr_seq, memory_order_acquire);
}

static void
_Py_qsbr_quiescent_state(struct _qsbr_thread_state *qsbr)
{
    uint64_t seq = _Py_qsbr_shared_current(qsbr->shared);
    atomic_store_explicit(&qsbr->seq, seq, memory_order_release);
}

static uint64_t
_Py_qsbr_advance(struct _qsbr_shared *shared)
{
    return atomic_fetch_add(&shared->wr_seq, QSBR_INCR) + QSBR_INCR;
}

static int
qsbr_poll_scan(struct _qsbr_shared *shared)
{
    atomic_thread_fence(memory_order_seq_cst);

    // Compute the minimum sequence number of all attached threads
    uint64_t min_seq = _Py_qsbr_shared_current(shared);
    for (int i = 0; i < NUM_THREADS; i++) {
        struct _qsbr_thread_state *qsbr = &shared->array[i];
        uint64_t seq = atomic_load_explicit(&qsbr->seq, memory_order_acquire);
        if (seq != QSBR_OFFLINE && QSBR_LT(seq, min_seq)) {
            min_seq = seq;
        }
    }

    // Update the shared read sequence
    uint64_t rd_seq = atomic_load(&shared->rd_seq);
    if (QSBR_LT(rd_seq, min_seq)) {
        // It's okay if the compare-exchange failed: another thread updated it
        atomic_compare_exchange_strong(&shared->rd_seq, &rd_seq, min_seq);
        rd_seq = min_seq;
    }

    return rd_seq;
}

bool
_Py_qsbr_poll(struct _qsbr_thread_state *qsbr, uint64_t goal)
{
    uint64_t rd_seq = atomic_load(&qsbr->shared->rd_seq);
    if (QSBR_LEQ(goal, rd_seq)) {
        return true;
    }

    rd_seq = qsbr_poll_scan(qsbr->shared);
    return QSBR_LEQ(goal, rd_seq);
}

void
_Py_qsbr_attach(struct _qsbr_thread_state *qsbr)
{
    // assert(atomic_load_explicit(&qsbr->seq, memory_order_relaxed) == 0 && "already attached");

    uint64_t seq = _Py_qsbr_shared_current(qsbr->shared);
    atomic_store_explicit(&qsbr->seq, seq, memory_order_seq_cst);
}

void
_Py_qsbr_detach(struct _qsbr_thread_state *qsbr)
{
    // assert(atomic_load_explicit(&qsbr->seq, memory_order_relaxed) != 0 && "already detached");

    atomic_store_explicit(&qsbr->seq, 0, memory_order_release);
}


static void
run_thread(void *arg)
{
    struct _qsbr_thread_state *qsbr = arg;
    int tid = qsbr->tid;
    _Py_qsbr_attach(qsbr);

    if (tid == 0) {
        struct object *old = atomic_load_explicit(&shared_data, memory_order_relaxed);

        store_32(&new_data.is_dead, 0);
        atomic_store_explicit(&shared_data, &new_data, memory_order_release);

        uint64_t goal = _Py_qsbr_advance(qsbr->shared);
        _Py_qsbr_quiescent_state(qsbr);

        if (_Py_qsbr_poll(qsbr, goal)) {
            // Simulate freeing the data
            store_32(&old->is_dead, 1);
        }
    }
    else if (tid == 1) {
        _Py_qsbr_quiescent_state(qsbr);

        struct object *data = atomic_load_explicit(&shared_data, memory_order_acquire);
        MODEL_ASSERT(!load_32(&data->is_dead));

        _Py_qsbr_quiescent_state(qsbr);
    }
    else if (tid == 2) {
        // Optional: test another thread advancing the read sequence
        _Py_qsbr_quiescent_state(qsbr);
        _Py_qsbr_poll(qsbr, 10000);
    }

    _Py_qsbr_detach(qsbr);
}

int
user_main(int argc, char **argv)
{
    struct _qsbr_shared shared = {0};
    atomic_init(&shared.wr_seq, QSBR_INITIAL);
    atomic_init(&shared.rd_seq, QSBR_INITIAL);
    for (int i = 0; i < NUM_THREADS; i++) {
        struct _qsbr_thread_state *qsbr = &shared.array[i];
        atomic_init(&qsbr->seq, QSBR_OFFLINE);
        qsbr->shared = &shared;
        qsbr->tid = i;
    }

    printf("old = %p new = %p\n", &old_data, &new_data);
    atomic_init(&shared_data, &old_data);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_create(&shared.threads[i], run_thread, &shared.array[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(shared.threads[i]);
    }

	return 0;
}
