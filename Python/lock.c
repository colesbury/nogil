#include "Python.h"
#include "pycore_lock.h"
#include "pycore_object.h"
#include "pycore_pystate.h"
#include "condvar.h"
#include "parking_lot.h"

#include <stdint.h>

#define TIME_TO_BE_FAIR_NS (1000*1000)

struct mutex_entry {
    _PyTime_t time_to_be_fair;
    int handoff;
};

void
_PyMutex_lock_slow(_PyMutex *m) {
    _PyMutex_LockSlowEx(m, _PY_LOCK_DETACH);
}

void
_PyMutex_LockSlowEx(_PyMutex *m, int detach)
{
    _PyTime_t now = _PyTime_GetMonotonicClock();

    struct mutex_entry entry;
    entry.time_to_be_fair = now + TIME_TO_BE_FAIR_NS;

    for (;;) {
        uint8_t v = _Py_atomic_load_uint8(&m->v);

        if (!(v & LOCKED)) {
            if (_Py_atomic_compare_exchange_uint8(&m->v, v, v|LOCKED)) {
                return;
            }
            continue;
        }

        uint8_t newv = v;
        if (!(v & HAS_PARKED)) {
            newv = v | HAS_PARKED;
            if (!_Py_atomic_compare_exchange_uint8(&m->v, v, newv)) {
                continue;
            }
        }

        int ret = _PyParkingLot_ParkUint8(&m->v, newv, &entry, -1, detach);
        if (ret == PY_PARK_OK) {
            if (entry.handoff) {
                // we own the lock now
                assert(_Py_atomic_load_uint8_relaxed(&m->v) & LOCKED);
                return;
            }
        }
    }
}

PyLockStatus
_PyMutex_TimedLockEx(_PyMutex *m, _PyTime_t timeout, _PyLockFlags flags)
{
    uint8_t v = _Py_atomic_load_uint8_relaxed(&m->v);
    if ((v & LOCKED) == UNLOCKED) {
        if (_Py_atomic_compare_exchange_uint8(&m->v, v, v|LOCKED)) {
            return PY_LOCK_ACQUIRED;
        }
    }
    else if (timeout == 0) {
        return PY_LOCK_FAILURE;
    }

    _PyTime_t now = _PyTime_GetMonotonicClock();
    _PyTime_t endtime = 0;
    if (timeout > 0) {
        endtime = _PyTime_Add(now, timeout);
    }

    struct mutex_entry entry;
    entry.time_to_be_fair = now + TIME_TO_BE_FAIR_NS;

    for (;;) {
        uint8_t v = _Py_atomic_load_uint8(&m->v);

        if (!(v & LOCKED)) {
            if (_Py_atomic_compare_exchange_uint8(&m->v, v, v|LOCKED)) {
                return PY_LOCK_ACQUIRED;
            }
            continue;
        }

        if (timeout == 0) {
            return PY_LOCK_FAILURE;
        }

        uint8_t newv = v;
        if (!(v & HAS_PARKED)) {
            newv = v | HAS_PARKED;
            if (!_Py_atomic_compare_exchange_uint8(&m->v, v, newv)) {
                continue;
            }
        }

        int detach = (flags & _PY_LOCK_DETACH) == _PY_LOCK_DETACH;
        int ret = _PyParkingLot_ParkUint8(&m->v, newv, &entry, timeout, detach);
        if (ret == PY_PARK_OK) {
            if (entry.handoff) {
                // we own the lock now
                assert(_Py_atomic_load_uint8_relaxed(&m->v) & LOCKED);
                return PY_LOCK_ACQUIRED;
            }
        }
        else if (ret == PY_PARK_INTR && (flags & _PY_LOCK_MAKE_PENDING_CALLS)) {
            if (Py_MakePendingCalls() < 0) {
                return PY_LOCK_INTR;
            }
        }
        else if (ret == PY_PARK_TIMEOUT) {
            assert(timeout >= 0);
            return PY_LOCK_FAILURE;
        }

        if (timeout > 0) {
            timeout = _PyDeadline_Get(endtime);
            if (timeout <= 0) {
                /* Check for negative values, since those mean block forever. */
                timeout = 0;
            }
        }
    }
}

int
_PyMutex_TryLockSlow(_PyMutex *m)
{
    for (;;) {
        uint8_t v = _Py_atomic_load_uint8(&m->v);

        if (!(v & LOCKED)) {
            if (_Py_atomic_compare_exchange_uint8(&m->v, v, v|LOCKED)) {
                return 1;
            }
            continue;
        }

        return 0;
    }
}

int
_PyMutex_TryUnlock(_PyMutex *m)
{
    for (;;) {
        uint8_t v = _Py_atomic_load_uint8(&m->v);

        if ((v & LOCKED) == UNLOCKED) {
            return -1;
        }
        else if ((v & HAS_PARKED)) {
            int more_waiters;
            struct wait_entry *wait;
            struct mutex_entry *entry;

            entry = _PyParkingLot_BeginUnpark(&m->v, &wait, &more_waiters);
            v = 0;
            if (entry) {
                int should_be_fair = _PyTime_GetMonotonicClock() > entry->time_to_be_fair;
                entry->handoff = should_be_fair;
                if (should_be_fair) {
                    v |= LOCKED;
                }
                if (more_waiters) {
                    v |= HAS_PARKED;
                }
            }
            _Py_atomic_store_uint8(&m->v, v);

            _PyParkingLot_FinishUnpark(&m->v, wait);
            return 0;
        }
        else if (_Py_atomic_compare_exchange_uint8(&m->v, v, UNLOCKED)) {
            return 0;
        }
    }
}

void
_PyMutex_unlock_slow(_PyMutex *m)
{
    if (_PyMutex_TryUnlock(m) < 0) {
        Py_FatalError("unlocking mutex that is not locked");
    }
}

struct raw_mutex_entry {
    _PyWakeup *wakeup;
    struct raw_mutex_entry *next;
};

void
_PyRawMutex_lock_slow(_PyRawMutex *m)
{
    struct raw_mutex_entry waiter;
    waiter.wakeup = _PyWakeup_Acquire();

    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & 1) == UNLOCKED) {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, v|LOCKED)) {
                break;
            }
            continue;
        }

        waiter.next = (struct raw_mutex_entry *)(v & ~1);

        if (!_Py_atomic_compare_exchange_uintptr(&m->v, v, ((uintptr_t)&waiter)|LOCKED)) {
            continue;
        }

        _PyWakeup_Wait(waiter.wakeup, -1, /*detach=*/0);
    }

    _PyWakeup_Release(waiter.wakeup);
}

void
_PyRawMutex_unlock_slow(_PyRawMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);

        if ((v & LOCKED) == UNLOCKED) {
            Py_FatalError("unlocking mutex that is not locked");
        }

        struct raw_mutex_entry *waiter = (struct raw_mutex_entry *)(v & ~LOCKED);
        if (waiter) {
            uintptr_t next_waiter = (uintptr_t)waiter->next;
            if (_Py_atomic_compare_exchange_uintptr(&m->v, v, next_waiter)) {
                _PyWakeup_Wakeup(waiter->wakeup);
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
        _PyWakeup *waiter = (_PyWakeup *)v;
        _PyWakeup_Wakeup(waiter);
    }
}

void
_PyRawEvent_Wait(_PyRawEvent *o)
{
    int64_t ns = -1;
    _PyRawEvent_TimedWait(o, ns);
}

static int
_PyRawEvent_TimedWaitEx(_PyRawEvent *o, int64_t ns, _PyWakeup *waiter)
{
    if (_Py_atomic_compare_exchange_uintptr(&o->v, UNLOCKED, (uintptr_t)waiter)) {
        if (_PyWakeup_Wait(waiter, ns, /*detach=*/0) == PY_PARK_OK) {
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
                if (_PyWakeup_Wait(waiter, -1, /*detach=*/0) == PY_PARK_OK) {
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

int
_PyRawEvent_TimedWait(_PyRawEvent *o, int64_t ns)
{
    _PyWakeup *waiter = _PyWakeup_Acquire();
    int res = _PyRawEvent_TimedWaitEx(o, ns, waiter);
    _PyWakeup_Release(waiter);
    return res;
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

    _PyParkingLot_Park(&o->v, HAS_PARKED, NULL, ns);

    return _Py_atomic_load_uintptr(&o->v) == LOCKED;
}

int
_PyBeginOnce_slow(_PyOnceFlag *o)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&o->v);
        if (v == UNLOCKED) {
            if (!_Py_atomic_compare_exchange_uintptr(&o->v, UNLOCKED, LOCKED)) {
                continue;
            }
            return 1;
        }
        if (v == ONCE_INITIALIZED) {
            return 0;
        }

        assert((v & LOCKED) != 0);
        uintptr_t newv = LOCKED | HAS_PARKED;
        if (!_Py_atomic_compare_exchange_uintptr(&o->v, v, newv)) {
            continue;
        }

        _PyParkingLot_Park(&o->v, newv, NULL, -1);
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

struct rmutex_entry {
    uintptr_t thread_id;
    _PyTime_t time_to_be_fair;
    int handoff;
};

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


    struct rmutex_entry entry;
    entry.thread_id = _Py_ThreadId();
    entry.time_to_be_fair = _PyTime_GetMonotonicClock() + TIME_TO_BE_FAIR_NS;

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

        int ret = _PyParkingLot_Park(&m->v, newv, &entry, -1);
        if (ret == PY_PARK_OK && entry.handoff) {
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
            struct wait_entry *wait;
            struct rmutex_entry *entry;

            entry = _PyParkingLot_BeginUnpark(&m->v, &wait, &more_waiters);
            v = 0;
            if (wait) {
                _PyTime_t now = _PyTime_GetMonotonicClock();
                int should_be_fair = now > entry->time_to_be_fair;
                entry->handoff = should_be_fair;
                if (should_be_fair) {
                    v |= entry->thread_id;
                    v |= LOCKED;
                }
                if (more_waiters) {
                    v |= HAS_PARKED;
                }
            }
            _Py_atomic_store_uintptr(&m->v, v);

            _PyParkingLot_FinishUnpark(&m->v, wait);
            return;
        }
        else if (_Py_atomic_compare_exchange_uintptr(&m->v, v, UNLOCKED)) {
            return;
        }
    }
}
