#ifndef Py_LOCK_H
#define Py_LOCK_H

#include "pyatomic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t v;
} _PyMutex;

typedef struct {
    uintptr_t v;
} _PyOnce;

typedef enum {
    UNLOCKED = 0,
    LOCKED = 1
} _PyMutex_State;

void _PyMutex_lock_slow(_PyMutex *m);
void _PyMutex_unlock_slow(_PyMutex *m);

void _PyOnce_Notify(_PyOnce *o);
void _PyOnce_Wait(_PyOnce *o);
void _PyOnce_Reset(_PyOnce *o);

static inline int
_PyMutex_is_locked(_PyMutex *m)
{
    return _Py_atomic_load_uintptr(&m->v) & 1;
}

// extern int64_t lock_count;

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

#ifdef __cplusplus
}
#endif
#endif /* !Py_LOCK_H */
