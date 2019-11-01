#include "Python.h"
#include "pycore_pystate.h"
#include "condvar.h"

#include "lock.h"
#include "parking_lot.h"

#include <stdint.h>

void
_PyMutex_lock_slow(_PyMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if (!(v & LOCKED)) {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, v|LOCKED)) {
                return;
            }
            continue;
        }

        uintptr_t newv = v;
        if (!(v & HAS_PARKED)) {
            newv = v | HAS_PARKED;
            if (!_Py_atomic_compare_exchange_uintptr(&m->v, v, newv)) {
                continue;
            }
        }

        _PyTime_t now = _PyTime_GetMonotonicClock();
        int ret = _PyParkingLot_Park(&m->v, newv, now, -1);
        if (ret == PY_PARK_OK) {
            Waiter *waiter =  _PyParkingLot_ThisWaiter();
            if (waiter->handoff_elem) {
                // we own the lock now
                assert(_Py_atomic_load_uintptr_relaxed(&m->v) & LOCKED);
                return;
            }
        }
    }
}

int
_PyMutex_TryLockSlow(_PyMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if (!(v & LOCKED)) {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, v|LOCKED)) {
                return 1;
            }
            continue;
        }

        return 0;
    }
}

void
_PyMutex_unlock_slow(_PyMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & LOCKED) == UNLOCKED) {
            Py_FatalError("unlocking mutex that is not locked");
        }
        else if ((v & HAS_PARKED)) {
            int more_waiters;
            int should_be_fair;
            Waiter *waiter;

            _PyParkingLot_BeginUnpark(&m->v, &waiter, &more_waiters,
                                       &should_be_fair);
            v = 0;
            if (waiter) {
                waiter->handoff_elem = should_be_fair;
                if (should_be_fair) {
                    v |= LOCKED;
                }
                if (more_waiters) {
                    v |= HAS_PARKED;
                }
            }
            _Py_atomic_store_uintptr(&m->v, v);

            _PyParkingLot_FinishUnpark(&m->v, waiter);
            return;
        }
        else if (_Py_atomic_compare_exchange_uintptr(&m->v, v, UNLOCKED)) {
            return;
        }
    }
}

void
_PyRawMutex_lock_slow(_PyRawMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & 1) == UNLOCKED) {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, v|LOCKED)) {
                return;
            }
            continue;
        }

        Waiter *this_waiter = _PyParkingLot_ThisWaiter();
        assert(this_waiter);

        Waiter *next_waiter = (Waiter *)(v & ~1);
        this_waiter->next_waiter = next_waiter;
        if (!_Py_atomic_compare_exchange_uintptr(&m->v, v, ((uintptr_t)this_waiter)|LOCKED)) {
            continue;
        }

        _PySemaphore_WaitEx(this_waiter, -1, /*detach=*/0);
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

        Waiter *waiter = (Waiter *)(v & ~1);
        if (waiter) {
            uintptr_t next_waiter = (uintptr_t)waiter->next_waiter;
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, next_waiter)) {
                _PySemaphore_Signal(waiter, "_PyRawMutex_unlock_slow", m);
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
        Waiter *waiter = (Waiter *)v;
        _PySemaphore_Signal(waiter, "_PyRawEvent_Notify", o);
    }
}

void
_PyRawEvent_Wait(_PyRawEvent *o)
{
    int64_t ns = -1;
    _PyRawEvent_TimedWait(o, ns);
}

int
_PyRawEvent_TimedWait(_PyRawEvent *o, int64_t ns)
{
    Waiter *waiter = _PyParkingLot_ThisWaiter();
    assert(waiter);
    if (_Py_atomic_compare_exchange_uintptr(&o->v, UNLOCKED, (uintptr_t)waiter)) {
        if (_PySemaphore_WaitEx(waiter, ns, /*detach=*/0) == PY_PARK_OK) {
            assert(_Py_atomic_load_uintptr(&o->v) == LOCKED);
            return 1;
        }

        /* remove us as the waiter */
        if (_Py_atomic_compare_exchange_uintptr(&o->v, (uintptr_t)waiter, UNLOCKED)) {
            return 0;
        }

        uintptr_t v = _Py_atomic_load_uintptr(&o->v);
        if (v == LOCKED) {
            /* Grab the notification */
            for (;;) {
                if (_PySemaphore_WaitEx(waiter, -1, /*detach=*/0) == PY_PARK_OK) {
                    return 1;
                }
            }
        }
        else {
            Py_FatalError("_PyRawEvent: invalid state");
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
_PyEvent_Notify(_PyEvent *o)
{
    uintptr_t v = _Py_atomic_exchange_uintptr(&o->v, LOCKED);
    if (v == UNLOCKED) {
        return;
    }
    else if (v == LOCKED) {
        // Py_FatalError("_PyEvent: duplicate notifications");
        return;
    }
    else {
        assert(v == HAS_PARKED);
        _PyParkingLot_UnparkAll(&o->v);
    }
}

void
_PyEvent_Wait(_PyEvent *o)
{
    for (;;) {
        if (_PyEvent_TimedWait(o, -1)) {
            return;
        }
    }
}

int
_PyEvent_TimedWait(_PyEvent *o, int64_t ns)
{
    uintptr_t v = _Py_atomic_load_uintptr(&o->v);
    if (v == LOCKED) {
        return 1;
    }
    if (v == UNLOCKED) {
        _Py_atomic_compare_exchange_uintptr(&o->v, UNLOCKED, HAS_PARKED);
    }

    _PyTime_t now = _PyTime_GetMonotonicClock();
    _PyParkingLot_Park(&o->v, HAS_PARKED, now, ns);

    return _Py_atomic_load_uintptr(&o->v) == LOCKED;
}

int
_PyBeginOnce_slow(_PyOnceFlag *o)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&o->v);
        if (v == UNLOCKED) {
            if (_Py_atomic_compare_exchange_uintptr(&o->v, UNLOCKED, LOCKED)) {
                return 1;
            }
        }
        if (v == ONCE_INITIALIZED) {
            return 0;
        }

        assert((v & LOCKED) != 0);
        uintptr_t newv = LOCKED | HAS_PARKED;
        if (!_Py_atomic_compare_exchange_uintptr(&o->v, v, newv)) {
            continue;
        }

        _PyTime_t now = _PyTime_GetMonotonicClock();
        _PyParkingLot_Park(&o->v, newv, now, -1);
    }
}

void
_PyEndOnce(_PyOnceFlag *o)
{
    uintptr_t v = _Py_atomic_exchange_uintptr(&o->v, ONCE_INITIALIZED);
    assert((v & LOCKED) != 0);
    if ((v & HAS_PARKED) != 0) {
        _PyParkingLot_UnparkAll(&o->v);
    }
}

void
_PyEndOnceFailed(_PyOnceFlag *o)
{
    uintptr_t v = _Py_atomic_exchange_uintptr(&o->v, UNLOCKED);
    assert((v & LOCKED) != 0);
    if ((v & HAS_PARKED) != 0) {
        _PyParkingLot_UnparkAll(&o->v);
    }
}

void
_PyRecursiveMutex_lock_slow(_PyRecursiveMutex *m)
{
    uintptr_t v = _Py_atomic_load_uintptr_relaxed(&m->v);
    if ((v & THREAD_ID_MASK) == _Py_ThreadId()) {
        m->recursions++;
        return;
    }

    PyThreadState *finalizing = _Py_atomic_load_ptr_relaxed(&_PyRuntime._finalizing);
    if (finalizing && finalizing == _PyThreadState_GET()) {
        /* Act as-if we have ownership of the lock if the interpretr
         * shutting down. At this point all other threads have exited. */
        m->recursions++;
        return;
    }

    Waiter *waiter =  _PyParkingLot_ThisWaiter();
    assert(waiter);

    _PyTime_t now = _PyTime_GetMonotonicClock();
    int loops = 0;
    for (;;) {
        v = _Py_atomic_load_uintptr(&m->v);

        assert((v & THREAD_ID_MASK) != _Py_ThreadId());

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

        int ret = _PyParkingLot_Park(&m->v, newv, now, -1);
        if (ret == PY_PARK_OK && waiter->handoff_elem) {
            assert((_Py_atomic_load_uintptr_relaxed(&m->v) & ~HAS_PARKED) == (_Py_ThreadId() | LOCKED));
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

        if ((v & LOCKED) == UNLOCKED) {
            Py_FatalError("unlocking mutex that is not locked");
        }
        else if ((v & HAS_PARKED) == HAS_PARKED) {
            int more_waiters;
            int should_be_fair;
            Waiter *waiter;

            _PyParkingLot_BeginUnpark(&m->v, &waiter, &more_waiters,
                                       &should_be_fair);
            v = 0;
            if (waiter) {
                waiter->handoff_elem = should_be_fair;
                if (should_be_fair) {
                    v |= waiter->thread_id;
                    v |= LOCKED;
                }
                if (more_waiters) {
                    v |= HAS_PARKED;
                }
            }
            _Py_atomic_store_uintptr(&m->v, v);

            _PyParkingLot_FinishUnpark(&m->v, waiter);
            return;
        }
        else if (_Py_atomic_compare_exchange_uintptr(&m->v, v, UNLOCKED)) {
            return;
        }
    }
}
