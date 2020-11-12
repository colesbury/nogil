#ifndef Py_INTERNAL_QSBR_H
#define Py_INTERNAL_QSBR_H

#include <stdbool.h>
#include "pyatomic.h"
#include "pycore_llist.h"
#include "pycore_initconfig.h"

struct qsbr_shared;
typedef struct qsbr_shared *qsbr_shared_t;

struct qsbr {
    uint64_t        t_seq;
    qsbr_shared_t   t_shared;
    struct qsbr     *t_next;
    PyThreadState   *tstate;
};

struct qsbr_pad {
    struct qsbr qsbr;
    char __padding[64 - sizeof(struct qsbr)];
};

typedef struct qsbr *qsbr_t;

struct qsbr_shared {
    /* always odd, incremented by two */
    uint64_t    s_wr;

    /* Minimum observed read sequence. */
    uint64_t    s_rd_seq;

    qsbr_t      head;
    uintptr_t   n_free;
};

static inline uint64_t
_Py_qsbr_shared_current(qsbr_shared_t shared)
{
    return _Py_atomic_load_uint64(&shared->s_wr);
}

static inline void
_Py_qsbr_quiescent_state(PyThreadState *ts)
{
    // assert(ts->status == _Py_THREAD_ATTACHED);
    qsbr_t qsbr = ts->qsbr;
    uint64_t seq = _Py_qsbr_shared_current(qsbr->t_shared); // need acquire
    _Py_atomic_store_uint64_relaxed(&qsbr->t_seq, seq); // probably release
}

PyStatus
_Py_qsbr_init(qsbr_shared_t shared);

uint64_t
_Py_qsbr_advance(qsbr_shared_t shared);

bool
_Py_qsbr_poll(qsbr_t qsbr, uint64_t goal);

void
_Py_qsbr_online(qsbr_t qsbr);

void
_Py_qsbr_offline(qsbr_t qsbr);

qsbr_t
_Py_qsbr_register(qsbr_shared_t shared, PyThreadState *tsate);

void
_Py_qsbr_unregister(qsbr_t qsbr);

void
_Py_qsbr_unregister_other(qsbr_t qsbr);

void
_Py_qsbr_after_fork(qsbr_shared_t shared, qsbr_t qsbr);

#endif /* !Py_INTERNAL_QSBR_H */
