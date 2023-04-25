#ifndef Py_LIMITED_API
#ifndef Py_LOCK_H
#define Py_LOCK_H

#include "pyatomic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t v;
} _PyOnceFlag;

typedef _PyMutex PyMutex;

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

PyAPI_FUNC(void) _PyRecursiveMutex_lock_slow(_PyRecursiveMutex *m);
PyAPI_FUNC(void) _PyRecursiveMutex_unlock_slow(_PyRecursiveMutex *m);

PyAPI_FUNC(int) _PyBeginOnce_slow(_PyOnceFlag *o);
PyAPI_FUNC(void) _PyEndOnce(_PyOnceFlag *o);
PyAPI_FUNC(void) _PyEndOnceFailed(_PyOnceFlag *o);

static inline int
_PyMutex_is_locked(_PyMutex *m)
{
    return _Py_atomic_load_uint8(&m->v) & LOCKED;
}

static inline int
_PyMutex_lock_fast(_PyMutex *m)
{
    return _Py_atomic_compare_exchange_uint8(&m->v, UNLOCKED, LOCKED);
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
    return _Py_atomic_compare_exchange_uint8(&m->v, LOCKED, UNLOCKED);
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
