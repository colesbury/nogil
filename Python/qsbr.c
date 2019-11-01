/*
 * Implementation of safe memory reclamation scheme using
 * quiescent states.
 *
 * This is dervied from the "GUS" safe memory reclamation technique
 * in FreeBSD written by Jeffrey Roberson. The original copyright
 * is preserved below.
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
#include "pycore_atomic.h"
#include "pycore_qsbr.h"
#include "pycore_pystate.h"
#include "pyatomic.h"

#include <stdint.h>

#define QSBR_LT(a, b) ((int64_t)((a)-(b)) < 0)
#define QSBR_LEQ(a, b) ((int64_t)((a)-(b)) <= 0)

enum {
    QSBR_OFFLINE = 0,
    QSBR_INITIAL = 1,
    QSBR_INCR = 2,
};

static struct qsbr *
_Py_qsbr_alloc(struct qsbr_shared *shared)
{
    struct qsbr *qsbr;

    qsbr = (struct qsbr *)PyMem_RawMalloc(sizeof(struct qsbr_pad));
    if (qsbr == NULL) {
        return NULL;
    }
    memset(qsbr, 0, sizeof(*qsbr));
    qsbr->t_shared = shared;
    return qsbr;
}

PyStatus
_Py_qsbr_init(struct qsbr_shared *shared)
{
    memset(shared, 0, sizeof(*shared));
    struct qsbr *head = _Py_qsbr_alloc(shared);
    if (head == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    shared->head = head;
    shared->n_free = 1;
    shared->s_wr = QSBR_INITIAL;
    shared->s_rd_seq = QSBR_INITIAL;
    return _PyStatus_OK();
}

void
_Py_qsbr_after_fork(struct qsbr_shared *shared, struct qsbr *this_qsbr)
{
    struct qsbr *qsbr = _Py_atomic_load_ptr(&shared->head);
    while (qsbr != NULL) {
        if (qsbr != this_qsbr) {
            _Py_qsbr_unregister(qsbr);
        }
        qsbr = qsbr->t_next;
    }
}

uint64_t
_Py_qsbr_advance(struct qsbr_shared *shared)
{
    // TODO(sgross): handle potential wrap around
    return _Py_atomic_add_uint64(&shared->s_wr, QSBR_INCR) + QSBR_INCR;
}

uint64_t
_Py_qsbr_poll_scan(struct qsbr_shared *shared)
{
    uint64_t min_seq = _Py_atomic_load_uint64(&shared->s_wr);
    struct qsbr *qsbr = _Py_atomic_load_ptr(&shared->head);
    while (qsbr != NULL) {
        uint64_t seq = _Py_atomic_load_uint64(&qsbr->t_seq);
        if (seq != QSBR_OFFLINE && QSBR_LT(seq, min_seq)) {
            min_seq = seq;
        }
        qsbr = qsbr->t_next;
    }

    uint64_t rd_seq = _Py_atomic_load_uint64(&shared->s_rd_seq);
    if (QSBR_LT(rd_seq, min_seq)) {
        _Py_atomic_compare_exchange_uint64(&shared->s_rd_seq, rd_seq, min_seq);
        rd_seq = min_seq;
    }
    return rd_seq;
}

bool
_Py_qsbr_poll(struct qsbr *qsbr, uint64_t goal)
{
    uint64_t rd_seq = _Py_atomic_load_uint64(&qsbr->t_shared->s_rd_seq);
    if (QSBR_LEQ(goal, rd_seq)) {
        return true;
    }

    rd_seq = _Py_qsbr_poll_scan(qsbr->t_shared);
    return QSBR_LEQ(goal, rd_seq);
}

void
_Py_qsbr_online(struct qsbr *qsbr)
{
    assert(qsbr->t_seq == 0 && "thread is already online");

    uint64_t seq = _Py_qsbr_shared_current(qsbr->t_shared);
    _Py_atomic_store_uint64_relaxed(&qsbr->t_seq, seq);

    /* ensure update to local counter is visible */
    _Py_atomic_fence_seq_cst();
}

void
_Py_qsbr_offline(struct qsbr *qsbr)
{
    assert(qsbr->t_seq != 0 && "thread is already offline");

    /* maybe just release fence... investigate */
    _Py_atomic_fence_release();
    _Py_atomic_store_uint64_relaxed(&qsbr->t_seq, QSBR_OFFLINE);
}

struct qsbr *
_Py_qsbr_recycle(struct qsbr_shared *shared, PyThreadState *tstate)
{
    if (_Py_atomic_load_uintptr(&shared->n_free) == 0) {
        return NULL;
    }
    struct qsbr *qsbr = _Py_atomic_load_ptr(&shared->head);
    while (qsbr != NULL) {
        if (_Py_atomic_load_ptr_relaxed(&qsbr->tstate) == NULL &&
            _Py_atomic_compare_exchange_ptr(&qsbr->tstate, NULL, tstate)) {

            _Py_atomic_add_uintptr(&shared->n_free, -1);
            return qsbr;
        }
        qsbr = qsbr->t_next;
    }
    return NULL;
}

struct qsbr *
_Py_qsbr_register(struct qsbr_shared *shared, PyThreadState *tstate, struct qsbr *qsbr)
{
    qsbr->tstate = tstate;
    qsbr->t_shared = shared;
    struct qsbr *next;
    do {
        next = _Py_atomic_load_ptr(&shared->head);
        qsbr->t_next = next;
    } while (!_Py_atomic_compare_exchange_ptr(&shared->head, next, qsbr));
    return qsbr;
}

void
_Py_qsbr_unregister(struct qsbr *qsbr)
{
    assert(qsbr->t_seq == 0 && "qsbr thread-state must be offline");

    _Py_atomic_store_ptr_relaxed(&qsbr->tstate, NULL);
    _Py_atomic_add_uintptr(&qsbr->t_shared->n_free, 1);
}
