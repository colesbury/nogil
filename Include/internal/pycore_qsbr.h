#ifndef Py_INTERNAL_QSBR_H
#define Py_INTERNAL_QSBR_H

#include <stdbool.h>
#include "pyatomic.h"
#include "pycore_llist.h"
#include "pycore_initconfig.h"
#include "pycore_pystate.h"
#include "pycore_runtime.h"

/* Per-thread state */
struct qsbr {
    uint64_t            t_seq;
    struct qsbr_shared  *t_shared;
    struct qsbr         *t_next;
    int                 t_deferred;
    int                 t_limit;
    PyThreadState       *tstate;
};

struct qsbr_pad {
    struct qsbr qsbr;
    char __padding[64 - sizeof(struct qsbr)];
};

struct _Py_qsbr_head {
    struct _Py_qsbr_head *next;
    uint64_t seq;
};

static inline uint64_t
_Py_qsbr_shared_current(struct qsbr_shared *shared)
{
    return _Py_atomic_load_uint64(&shared->s_wr);
}

static inline void
_Py_qsbr_quiescent_state(PyThreadState *ts)
{
    struct qsbr *qsbr = ((struct PyThreadStateImpl *)ts)->qsbr;
    uint64_t seq = _Py_qsbr_shared_current(qsbr->t_shared); // need acquire
    _Py_atomic_store_uint64_relaxed(&qsbr->t_seq, seq); // probably release
}

PyStatus
_Py_qsbr_init(struct qsbr_shared *shared);

uint64_t
_Py_qsbr_advance(struct qsbr_shared *shared);

uint64_t
_Py_qsbr_deferred_advance(struct qsbr *qsbr);

bool
_Py_qsbr_poll(struct qsbr *qsbr, uint64_t goal);

void
_Py_qsbr_online(struct qsbr *qsbr);

void
_Py_qsbr_offline(struct qsbr *qsbr);

struct qsbr *
_Py_qsbr_recycle(struct qsbr_shared *shared, PyThreadState *tsate);

struct qsbr *
_Py_qsbr_register(struct qsbr_shared *shared, PyThreadState *tsate, struct qsbr *qsbr);

void
_Py_qsbr_unregister(struct qsbr *qsbr);

void
_Py_qsbr_after_fork(struct qsbr_shared *shared, struct qsbr *qsbr);

#endif /* !Py_INTERNAL_QSBR_H */
