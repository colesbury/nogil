#include "Python.h"
#include "pycore_pystate.h"
#include "Python/condvar.h"

#include "lock.h"

#ifndef _POSIX_THREADS
#error oops
#endif

#include <stdint.h>

// int64_t lock_count;

// static void __attribute__ ((destructor))
// print_lock_count(void) {
//     printf("locks acquired: %lld\n", lock_count);
// }

static void
_PySemaphore_Wait(PyThreadStateOS *os)
{
    PyMUTEX_LOCK(&os->waiter_mutex);
    while (os->waiter_counter == 0) {
        // TODO: release GIL
        PyCOND_WAIT(&os->waiter_cond, &os->waiter_mutex);
    }
    os->waiter_counter--;
    PyMUTEX_UNLOCK(&os->waiter_mutex);
}

static void
_PySemaphore_Signal(PyThreadStateOS *os)
{
    PyMUTEX_LOCK(&os->waiter_mutex);
    os->waiter_counter++;
    PyCOND_SIGNAL(&os->waiter_cond);
    PyMUTEX_UNLOCK(&os->waiter_mutex);
}


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

        _PySemaphore_Wait(tstate->os);
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
                _PySemaphore_Signal(waiter->os);
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
_PyOnce_Notify(_PyOnce *o)
{
    uintptr_t v = _Py_atomic_exchange_uintptr(&o->v, LOCKED);
    if (v == UNLOCKED) {
        return;
    }
    else if (v == LOCKED) {
        Py_FatalError("_PyOnce: duplicate notifications");
    }
    else {
        PyThreadState *waiter = (PyThreadState *)v;
        _PySemaphore_Signal(waiter->os);
    }
}

void
_PyOnce_Wait(_PyOnce *o)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_Py_atomic_compare_exchange_uintptr(&o->v, UNLOCKED, (uintptr_t)tstate)) {
        _PySemaphore_Wait(tstate->os);
        return;
    }

    uintptr_t v = _Py_atomic_load_uintptr(&o->v);
    if (v == LOCKED) {
        return;
    }
    else {
        Py_FatalError("_PyOnce: duplicate waiter");
    }
}

void
_PyOnce_Reset(_PyOnce *o)
{
    _Py_atomic_store_uintptr(&o->v, UNLOCKED);
}
