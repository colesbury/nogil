#ifndef Py_LOCK_H
#define Py_LOCK_H

#include "pyatomic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t v;
} _PyRawMutex;

typedef struct {
    uintptr_t v;
} _PyRawEvent;

typedef struct {
    uintptr_t v;
} _PyOnceFlag;

typedef struct {
    uintptr_t v;
} _PyMutex;

typedef struct {
    uintptr_t v;
} _PyEvent;

typedef struct {
    uintptr_t v;
    size_t recursions;
} _PyRecursiveMutex;

typedef enum {
    UNLOCKED = 0,
    LOCKED = 1,
    HAS_PARKED = 2,
    ONCE_INITIALIZED = 4,
} _PyMutex_State;

void _PyMutex_lock_slow(_PyMutex *m);
void _PyMutex_unlock_slow(_PyMutex *m);

void _PyRawMutex_lock_slow(_PyRawMutex *m);
void _PyRawMutex_unlock_slow(_PyRawMutex *m);

PyAPI_FUNC(void) _PyRecursiveMutex_lock_slow(_PyRecursiveMutex *m);
PyAPI_FUNC(void) _PyRecursiveMutex_unlock_slow(_PyRecursiveMutex *m);

void _PyRawEvent_Notify(_PyRawEvent *o);
void _PyRawEvent_Wait(_PyRawEvent *o);
int _PyRawEvent_TimedWait(_PyRawEvent *o, int64_t ns);
void _PyRawEvent_Reset(_PyRawEvent *o);

void _PyEvent_Notify(_PyEvent *o);
void _PyEvent_Wait(_PyEvent *o);
int _PyEvent_TimedWait(_PyEvent *o, int64_t ns);

int _PyBeginOnce_slow(_PyOnceFlag *o);
void _PyEndOnce(_PyOnceFlag *o);
void _PyEndOnceFailed(_PyOnceFlag *o);

static inline int
_PyMutex_is_locked(_PyMutex *m)
{
    return _Py_atomic_load_uintptr(&m->v) & 1;
}

static inline int
_PyRawMutex_is_locked(_PyRawMutex *m)
{
    return _Py_atomic_load_uintptr(&m->v) & 1;
}

static inline void
_PyRawMutex_lock(_PyRawMutex *m)
{
    if (_Py_atomic_compare_exchange_uintptr(&m->v, UNLOCKED, LOCKED)) {
        return;
    }
    _PyRawMutex_lock_slow(m);
}

static inline int
_PyRawMutex_trylock(_PyRawMutex *m)
{
    if (_Py_atomic_compare_exchange_uintptr(&m->v, UNLOCKED, LOCKED)) {
        return 1;
    }
    return 0;
}

static inline void
_PyRawMutex_unlock(_PyRawMutex *m)
{
    if (_Py_atomic_compare_exchange_uintptr(&m->v, LOCKED, UNLOCKED)) {
        return;
    }
    _PyRawMutex_unlock_slow(m);
}

static inline void
_PyMutex_lock(_PyMutex *m)
{
    // lock_count++;
    if (_Py_atomic_compare_exchange_uintptr(&m->v, UNLOCKED, LOCKED)) {
        return;
    }
    _PyMutex_lock_slow(m);
}

static inline void
_PyMutex_unlock(_PyMutex *m)
{
    if (_Py_atomic_compare_exchange_uintptr(&m->v, LOCKED, UNLOCKED)) {
        return;
    }
    _PyMutex_unlock_slow(m);
}

static inline void
_PyRecursiveMutex_lock(_PyRecursiveMutex *m)
{
    if (_Py_atomic_compare_exchange_uintptr(&m->v, UNLOCKED, _Py_ThreadId() | LOCKED)) {
        return;
    }
    _PyRecursiveMutex_lock_slow(m);
}

static inline void
_PyRecursiveMutex_unlock(_PyRecursiveMutex *m)
{
    uintptr_t v = _Py_atomic_load_uintptr_relaxed(&m->v);
    if (m->recursions == 0 && (v & 3) == LOCKED) {
        if (_Py_atomic_compare_exchange_uintptr(&m->v, v, UNLOCKED)) {
            return;
        }
    }
    _PyRecursiveMutex_unlock_slow(m);
}

static inline int
_PyEvent_IsSet(_PyEvent *e)
{
    return _Py_atomic_load_uintptr(&e->v) == LOCKED;
}

static inline int
_PyOnce_Initialized(_PyOnceFlag *o)
{
    return (_Py_atomic_load_uintptr(&o->v) & ONCE_INITIALIZED) != 0;
}

static inline int
_PyBeginOnce(_PyOnceFlag *o)
{
    if ((_Py_atomic_load_uintptr(&o->v) & ONCE_INITIALIZED) != 0) {
        return 0;
    }
    return _PyBeginOnce_slow(o);
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_LOCK_H */
