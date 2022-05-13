#ifndef Py_LIMITED_API
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

typedef _PyMutex PyMutex;

// A one-time event notification
typedef struct {
    uintptr_t v;
} _PyEvent;

// A one-time event notification with reference counting
typedef struct _PyEventRc {
    _PyEvent event;
    Py_ssize_t refcount;
} _PyEventRc;

typedef struct {
    uintptr_t v;
    size_t recursions;
} _PyRecursiveMutex;

typedef enum {
    UNLOCKED = 0,
    LOCKED = 1,
    HAS_PARKED = 2,
    ONCE_INITIALIZED = 4,
    THREAD_ID_MASK = ~(LOCKED | HAS_PARKED)
} _PyMutex_State;

PyAPI_FUNC(void) _PyMutex_lock_slow(_PyMutex *m);
PyAPI_FUNC(void) _PyMutex_unlock_slow(_PyMutex *m);
PyAPI_FUNC(int) _PyMutex_TryLockSlow(_PyMutex *m);


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

PyAPI_FUNC(int) _PyBeginOnce_slow(_PyOnceFlag *o);
PyAPI_FUNC(void) _PyEndOnce(_PyOnceFlag *o);
PyAPI_FUNC(void) _PyEndOnceFailed(_PyOnceFlag *o);

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

static inline int
_PyMutex_lock_fast(_PyMutex *m)
{
    return _Py_atomic_compare_exchange_uintptr(&m->v, UNLOCKED, LOCKED);
}

static inline void
_PyMutex_lock(_PyMutex *m)
{
    if (_PyMutex_lock_fast(m)) {
        return;
    }
    _PyMutex_lock_slow(m);
}

static inline int
_PyMutex_TryLock(_PyMutex *m)
{
    if (_PyMutex_lock_fast(m)) {
        return 1;
    }
    return _PyMutex_TryLockSlow(m);
}

static inline int
_PyMutex_unlock_fast(_PyMutex *m)
{
    return _Py_atomic_compare_exchange_uintptr(&m->v, LOCKED, UNLOCKED);
}

static inline void
_PyMutex_unlock(_PyMutex *m)
{
    if (_PyMutex_unlock_fast(m)) {
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

static inline int
_PyRecursiveMutex_owns_lock(_PyRecursiveMutex *m)
{
    uintptr_t v = _Py_atomic_load_uintptr(&m->v);
    return (v & THREAD_ID_MASK) == _Py_ThreadId();
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

static inline _PyEventRc *
_PyEventRc_New(void)
{
    _PyEventRc *erc = (_PyEventRc *)PyMem_RawCalloc(1, sizeof(_PyEventRc));
    if (erc != NULL) {
        erc->refcount = 1;
    }
    return erc;
}

static inline void
_PyEventRc_Incref(_PyEventRc *erc)
{
    _Py_atomic_add_ssize(&erc->refcount, 1);
}

static inline void
_PyEventRc_Decref(_PyEventRc *erc)
{
    if (_Py_atomic_add_ssize(&erc->refcount, -1) == 1) {
        PyMem_RawFree(erc);
    }
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
#endif /* Py_LIMITED_API */
