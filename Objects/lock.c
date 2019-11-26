#include "Python.h"
#include "pycore_pystate.h"
#include "Python/condvar.h"

#include "lock.h"
#include "parking_lot.h"

#include <stdint.h>

void
_PyMutex_lock_slow(_PyMutex *m)
{
    PyThreadState *tstate = _PyThreadState_GET();
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & 1) == UNLOCKED) {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, v|LOCKED)) {
                return;
            }
            continue;
        }

        PyThreadState *next_waiter = (PyThreadState *)(v & ~1);
        tstate->os->next_waiter = next_waiter;
        if (!_Py_atomic_compare_exchange_uintptr(&m->v, v, ((uintptr_t)tstate)|LOCKED)) {
            continue;
        }

        int64_t ns = -1;
        _PySemaphore_Wait(tstate->os, DETACH, ns);
    }
}

void
_PyMutex_unlock_slow(_PyMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & 1) == UNLOCKED) {
            Py_FatalError("unlocking mutex that is not locked");
        }

        PyThreadState *waiter = (PyThreadState *)(v & ~1);
        if (waiter) {
            uintptr_t next_waiter = (uintptr_t)waiter->os->next_waiter;
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, next_waiter)) {
                _PySemaphore_Signal(waiter->os, "_PyMutex_unlock_slow", m);
                return;
            }
        }
        else {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, UNLOCKED)) {
                return;
            }
        }
    }

}

void
_PyRawMutex_lock_slow(_PyRawMutex *m)
{
    PyThreadState *tstate = _PyThreadState_GET();
    assert(tstate);

    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & 1) == UNLOCKED) {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, v|LOCKED)) {
                return;
            }
            continue;
        }

        PyThreadState *next_waiter = (PyThreadState *)(v & ~1);
        tstate->os->next_waiter = next_waiter;
        if (!_Py_atomic_compare_exchange_uintptr(&m->v, v, ((uintptr_t)tstate)|LOCKED)) {
            continue;
        }

        int64_t ns = -1;
        _PySemaphore_Wait(tstate->os, DONT_DETACH, ns);
    }
}

void
_PyRawMutex_unlock_slow(_PyRawMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & 1) == UNLOCKED) {
            Py_FatalError("unlocking mutex that is not locked");
        }

        PyThreadState *waiter = (PyThreadState *)(v & ~1);
        if (waiter) {
            uintptr_t next_waiter = (uintptr_t)waiter->os->next_waiter;
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, next_waiter)) {
                _PySemaphore_Signal(waiter->os, "_PyRawMutex_unlock_slow", m);
                return;
            }
        }
        else {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, UNLOCKED)) {
                return;
            }
        }
    }
}

void
_PyRawEvent_Notify(_PyRawEvent *o)
{
    uintptr_t v = _Py_atomic_exchange_uintptr(&o->v, LOCKED);
    if (v == UNLOCKED) {
        return;
    }
    else if (v == LOCKED) {
        Py_FatalError("_PyRawEvent: duplicate notifications");
    }
    else {
        PyThreadState *waiter = (PyThreadState *)v;
        _PySemaphore_Signal(waiter->os, "_PyRawEvent_Notify", o);
    }
}

void
_PyRawEvent_Wait(_PyRawEvent *o, PyThreadState *tstate)
{
    int64_t ns = -1;
    _PyRawEvent_TimedWait(o, tstate, ns);
}

int
_PyRawEvent_TimedWait(_PyRawEvent *o, PyThreadState *tstate, int64_t ns)
{
    assert(tstate);

    if (_Py_atomic_compare_exchange_uintptr(&o->v, UNLOCKED, (uintptr_t)tstate)) {
        if (_PySemaphore_Wait(tstate->os, DONT_DETACH, ns)) {
            assert(_Py_atomic_load_uintptr(&o->v) == LOCKED);
            return 1;
        }

        for (;;) {
                if (_Py_atomic_compare_exchange_uintptr(&o->v, (uintptr_t)tstate, UNLOCKED)) {
                    return 0;
                }
                uintptr_t v = _Py_atomic_load_uintptr(&o->v);
                if (v == LOCKED) {
                    /* Grab the notification */
                    int ret = _PySemaphore_Wait(tstate->os, DONT_DETACH, -1);
                    assert(ret == 1);
                    return 1;
                }
                else {
                    Py_FatalError("_PyRawEvent: invalid state");
                }
        }
    }

    uintptr_t v = _Py_atomic_load_uintptr(&o->v);
    if (v == LOCKED) {
        return 1;
    }
    else {
        Py_FatalError("_PyRawEvent: duplicate waiter");
    }
}

void
_PyRawEvent_Reset(_PyRawEvent *o)
{
    _Py_atomic_store_uintptr(&o->v, UNLOCKED);
}

void
_PyRecursiveMutex_lock_slow(_PyRecursiveMutex *m)
{
    uintptr_t v = _Py_atomic_load_uintptr_relaxed(&m->v);
    if ((v & ~3) == _Py_ThreadId()) {
        m->recursions++;
        return;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    assert(tstate);

    if (_Py_atomic_load_ptr_relaxed(&_PyRuntime.finalizing) == tstate) {
        /* Act as-if we have ownership of the lock if the interpretr
         * shutting down. At this point all other threads have exited. */
        m->recursions++;
        return;
    }

    _PyTime_t now = _PyTime_GetMonotonicClock();
    int loops = 0;
    for (;;) {
        v = _Py_atomic_load_uintptr(&m->v);

        assert((v & ~3) != (uintptr_t)tstate);

        if ((v & 1) == UNLOCKED) {
            uintptr_t newv = _Py_ThreadId() | (v & HAS_PARKED) | LOCKED;
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, newv)) {
                return;
            }
            loops++;
            continue;
        }

        uintptr_t newv = v;
        if (!(v & HAS_PARKED)) {
            newv = v | HAS_PARKED;
            if (!_Py_atomic_compare_exchange_uintptr(&m->v, v, newv)) {
                continue;
            }
        }

        int ret = _PyParkingLot_Park(&m->v, newv, now);
        if (ret == -1) {
            continue;
        }

        if (tstate->os->token) {
            assert((_Py_atomic_load_uintptr_relaxed(&m->v) & ~2) == (_Py_ThreadId() | LOCKED));
            return;
        }
    }
}

void
_PyRecursiveMutex_unlock_slow(_PyRecursiveMutex *m)
{
    if (m->recursions > 0) {
        m->recursions--;
        return;
    }

    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & 1) == UNLOCKED) {
            Py_FatalError("unlocking mutex that is not locked");
        }
        else if ((v & 2) == HAS_PARKED) {
            int more_waiters;
            int should_be_fair;
            PyThreadStateOS *os;

            _PyParkingLot_BeginUnpark(&m->v, &os, &more_waiters,
                                      &should_be_fair);
            v = 0;
            if (os) {
                os->token = should_be_fair;
                if (should_be_fair) {
                    v |= os->tstate->fast_thread_id;
                    v |= LOCKED;
                }
                if (more_waiters) {
                    v |= HAS_PARKED;
                }
            }
            _Py_atomic_store_uintptr(&m->v, v);

            _PyParkingLot_FinishUnpark(&m->v, os);
            return;
        }
        else if (_Py_atomic_compare_exchange_uintptr(&m->v, v, UNLOCKED)) {
            return;
        }
    }
}
