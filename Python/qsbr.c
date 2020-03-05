/*
 * Implementation of quiescent state based reclamation.
 *
 * This is based on the "GUS" safe memory reclamation technique
 * in FreeBSD written by Jeffrey Roberson.
 *
 * TODO: explain goals, implementation, and design choices.
 *
 * Copyright (c) 2019,2020 Jeffrey Roberson <jeff@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Python.h"
#include "pycore_qsbr.h"
#include "pycore_llist.h"
#include "pycore_pystate.h"
#include "pyatomic.h"
#include "lock.h"

#include <stdint.h>

#define INITIAL_NUM_THREADS 8

#define QSBR_LT(a, b) ((int64_t)((a)-(b)) < 0)
#define QSBR_LEQ(a, b) ((int64_t)((a)-(b)) <= 0)

enum {
    QSBR_OFFLINE = 0,
    QSBR_INITIAL = 1,
    QSBR_INCR = 2,
};

PyStatus
_Py_qsbr_init(qsbr_shared_t shared)
{
    memset(shared, 0, sizeof(*shared));
    qsbr_t threads = PyMem_RawCalloc(INITIAL_NUM_THREADS, sizeof(struct qsbr));
    if (!threads) {
        return _PyStatus_NO_MEMORY();
    }
    shared->threads = threads;
    shared->n_threads = INITIAL_NUM_THREADS;
    shared->n_free = INITIAL_NUM_THREADS;
    shared->s_wr = QSBR_INITIAL;
    shared->s_rd_seq = QSBR_INITIAL;
    for (size_t i = 0; i < INITIAL_NUM_THREADS; i++) {
        shared->threads[i].t_shared = shared;
    }
    return _PyStatus_OK();
}

void
_Py_qsbr_after_fork(qsbr_shared_t shared, qsbr_t qsbr)
{
    size_t num_threads = shared->n_threads;
    qsbr_t threads = shared->threads;

    for (size_t i = 0; i != num_threads; i++) {
        if (&threads[i] != qsbr) {
            _Py_qsbr_unregister_other(&threads[i]);
        }
    }
}

uint64_t
_Py_qsbr_advance(qsbr_shared_t shared)
{
    // TODO(sgross): handle potential wrap around
    return _Py_atomic_add_uint64(&shared->s_wr, QSBR_INCR) + QSBR_INCR;
}

uint64_t
_Py_qsbr_poll_scan(qsbr_shared_t shared)
{
    size_t n_threads = shared->n_threads;
    struct qsbr *threads = shared->threads;

    uint64_t min_seq = _Py_atomic_load_uint64(&shared->s_wr);
    for (size_t i = 0; i != n_threads; i++) {
        uint64_t seq = _Py_atomic_load_uint64(&threads[i].t_seq);
        if (seq == QSBR_OFFLINE) {
            continue;
        }
        if (QSBR_LT(seq, min_seq)) {
            min_seq = seq;
        }
    }

    uint64_t rd_seq = _Py_atomic_load_uint64(&shared->s_rd_seq);
    if (QSBR_LT(rd_seq, min_seq)) {
        _Py_atomic_compare_exchange_uint64(&shared->s_rd_seq, rd_seq, min_seq);
        rd_seq = min_seq;
    }
    return rd_seq;
}

bool
_Py_qsbr_poll(qsbr_t qsbr, uint64_t goal)
{
    uint64_t rd_seq = _Py_atomic_load_uint64(&qsbr->t_shared->s_rd_seq);
    if (QSBR_LEQ(goal, rd_seq)) {
        return true;
    }

    rd_seq = _Py_qsbr_poll_scan(qsbr->t_shared);
    return QSBR_LEQ(goal, rd_seq);
}

void
_Py_qsbr_online(qsbr_t qsbr)
{
    assert(qsbr->t_seq == 0 && "thread is already online");

    uint64_t seq = _Py_qsbr_shared_current(qsbr->t_shared);
    _Py_atomic_store_uint64_relaxed(&qsbr->t_seq, seq);

    /* ensure update to local counter is visible */
    _Py_atomic_thread_fence(_Py_memory_order_seq_cst);
}

void
_Py_qsbr_offline(qsbr_t qsbr)
{
    assert(qsbr->t_seq != 0 && "thread is already offline");

    /* maybe just release fence... investigate */
    _Py_atomic_thread_fence(_Py_memory_order_release);
    _Py_atomic_store_uint64_relaxed(&qsbr->t_seq, QSBR_OFFLINE);
}

static qsbr_t
_Py_qsbr_recycle(qsbr_shared_t shared, PyThreadState *tstate)
{
    if (_Py_atomic_load_uintptr(&shared->n_free) == 0) {
        return NULL;
    }
    uintptr_t size = _Py_atomic_load_uintptr(&shared->n_threads);
    for (uintptr_t i = 0; i < size; i++) {
        qsbr_t qsbr = &shared->threads[i];
        if (_Py_atomic_load_ptr_relaxed(&qsbr->tstate) == NULL &&
            _Py_atomic_compare_exchange_ptr(&qsbr->tstate, NULL, tstate)) {

            _Py_atomic_add_uintptr(&shared->n_free, -1);
            return qsbr;
        }
    }
    return NULL;
}

qsbr_t
_Py_qsbr_register(qsbr_shared_t shared, PyThreadState *tstate)
{
    // First try to re-use a qsbr state
    qsbr_t qsbr = _Py_qsbr_recycle(shared, tstate);
    if (qsbr) {
        return qsbr;
    }

    _PyMutex_lock(&_PyRuntime.stoptheworld_mutex);
    _PyRuntimeState_StopTheWorld(&_PyRuntime);
    qsbr = _Py_qsbr_recycle(shared, tstate);
    if (qsbr) {
        goto done;
    }

    size_t n_threads = shared->n_threads * 2;
    qsbr_t threads = PyMem_RawCalloc(n_threads, sizeof(struct qsbr));
    if (!threads) {
        goto done;
    }

    memcpy(threads, shared->threads, shared->n_threads * sizeof(struct qsbr));
    PyMem_RawFree(shared->threads);
    shared->threads = threads;
    shared->n_threads = n_threads;
    shared->n_free = n_threads / 2;
    for (size_t i = 0; i < n_threads / 2; i++) {
        threads[i].tstate->qsbr = &threads[i];
    }
    for (size_t i = 0; i < n_threads; i++) {
        threads[i].t_shared = shared;
    }

    qsbr = &shared->threads[n_threads / 2];
    qsbr->tstate = tstate;
    qsbr->t_shared = shared;
    shared->n_free--;

done:
    _PyRuntimeState_StartTheWorld(&_PyRuntime);
    _PyMutex_unlock(&_PyRuntime.stoptheworld_mutex);
    return qsbr;
}

void
_Py_qsbr_unregister(qsbr_t qsbr)
{
    assert(qsbr->t_seq == 0 && "qsbr thread-state must be offline");
    assert(qsbr->tstate->status == _Py_THREAD_ATTACHED);

    _Py_atomic_store_ptr_relaxed(&qsbr->tstate, NULL);
    _Py_atomic_add_uintptr(&qsbr->t_shared->n_free, 1);
}

void
_Py_qsbr_unregister_other(qsbr_t qsbr)
{
    /* This is the same as _Py_qsbr_unregister but without the assertion
     * that ts->ctr == 0. We should merge the two and figure out the thread
     * exit mechanism re. zapthreads and daemon threads. */
    _Py_atomic_store_ptr_relaxed(&qsbr->tstate, NULL);
    _Py_atomic_add_uintptr(&qsbr->t_shared->n_free, 1);
}

