/* Implementation of biased reference counting */

#include "Python.h"
#include "pycore_pystate.h"
#include "condvar.h"
#include "pyatomic.h"
#include "../Modules/hashtable.h"

// TODO (sgross): abstract pthread
// TODO: not efficient

static void
_Py_RefcntQueue_Push(PyThreadState *tstate, PyObject *ob)
{
    PyObject **object_queue = &tstate->object_queue;
    for (;;) {
        PyObject *head = _Py_atomic_load_ptr_relaxed(object_queue);
        _Py_atomic_store_uintptr_relaxed(&ob->ob_tid, (uintptr_t)head);
        if (_Py_atomic_compare_exchange_ptr(object_queue, head, ob)) {
            return;
        }
    }
}

void
_Py_queue_object(PyObject *ob)
{
    uint64_t ob_tid = _PyObject_ThreadId(ob);
    PyThreadState *tstate = PyThreadState_GET();
    if (!tstate) {
        // We can end up in a during runtime finalization where there's no active
        // thread state. For example, release_sentinel in _threadmodule.c calls
        // Py_DECREF without an active thread state. For now, assume that no locking
        // is needed, since all threads should be dead. (This is probably not true)
        assert(_PyRuntime.finalizing);
        _Py_ExplicitMergeRefcount(ob);
        return;
    }

    PyInterpreterState *interp = tstate->interp;
    assert(interp);

    _Py_hashtable_t *ht = interp->object_queues;
    if (!ht) {
        // We can end up in a during runtime finalization where there's no active
        // thread state. For example, release_sentinel in _threadmodule.c calls
        // Py_DECREF without an active thread state. For now, assume that no locking
        // is needed, since all threads should be dead. (This is probably not true)
        assert(_PyRuntime.finalizing);
        _Py_ExplicitMergeRefcount(ob);
        return;
    }

    int err;
    if ((err = pthread_rwlock_rdlock(&interp->object_queues_lk)) != 0) {
        Py_FatalError("_Py_queue_object: unable to lock");
        return;
    }

    PyThreadState *target_tstate = NULL;
    _Py_hashtable_entry_t *entry = _Py_HASHTABLE_GET_ENTRY(ht, ob_tid);
    if (entry) {
        _Py_HASHTABLE_ENTRY_READ_DATA(ht, entry, target_tstate);
    }

    if (target_tstate) {
        _Py_RefcntQueue_Push(target_tstate, ob);
    }

    pthread_rwlock_unlock(&interp->object_queues_lk);

    if (!target_tstate) {
        _Py_ExplicitMergeRefcount(ob);
    }

}

void
_Py_queue_process(PyThreadState *tstate)
{
    assert(tstate);

    PyObject *head = _Py_atomic_exchange_ptr(&tstate->object_queue, NULL);
    while (head) {
        PyObject *next = (PyObject *)head->ob_tid;
        _Py_ExplicitMergeRefcount(head);
        head = next;
    }
}

void
_Py_queue_create(PyThreadState *tstate)
{
    PyInterpreterState *interp = tstate->interp;
    assert(interp);

    if (pthread_rwlock_wrlock(&interp->object_queues_lk) != 0) {
        Py_FatalError("_Py_queue_create: unable to lock");
        return;
    }

    _Py_hashtable_t *ht = interp->object_queues;
    uintptr_t tid = _Py_ThreadId();
    // printf("creating queue for %ld\n", tid);
    if (_Py_HASHTABLE_SET(ht, tid, tstate) != 0) {
        Py_FatalError("unable to set thread object queue");
    }

    pthread_rwlock_unlock(&interp->object_queues_lk);
}

void
_Py_queue_destroy(PyThreadState *tstate)
{
    PyInterpreterState *interp = tstate->interp;
    assert(interp);

    _Py_hashtable_t *ht = interp->object_queues;
    uint64_t tid = tstate->fast_thread_id;

retry:
    _Py_queue_process(tstate);

    if (pthread_rwlock_wrlock(&interp->object_queues_lk) != 0) {
        Py_FatalError("_Py_queue_destroy: unable to lock");
        return;
    }

    if (tstate->object_queue) {
        pthread_rwlock_unlock(&interp->object_queues_lk);
        goto retry;
    }

    PyThreadState *value = NULL;
    if (!_Py_HASHTABLE_POP(ht, tid, value)) {
        // Py_FatalError("_Py_queue_destroy: missing thread object queue");
        // FIXME(sgross): just ignore for now. There's a race where a new thread
        // can be created and added to the interpreter's thread-list before it's
        // initialized in the threads start method. The thread only adds itself
        // to the refcnt hashtable later on. In between the two, another thread
        // may fork, which leaves the PyThreadState in the interpreter but without
        // the corresponding entry in this hashtable.
        pthread_rwlock_unlock(&interp->object_queues_lk);
        return;
    }
    assert(value == tstate);

    pthread_rwlock_unlock(&interp->object_queues_lk);
}
