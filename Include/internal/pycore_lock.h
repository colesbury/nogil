#ifndef Py_INTERNAL_LOCK_H
#define Py_INTERNAL_LOCK_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

typedef enum _PyLockFlags {
    _Py_LOCK_DONT_DETACH = 0,
    _PY_LOCK_DETACH = 1,
    _PY_LOCK_MAKE_PENDING_CALLS = 2,
} _PyLockFlags;

typedef struct {
    uintptr_t v;
} _PyRawMutex;

typedef struct {
    uintptr_t v;
} _PyRawEvent;

// A one-time event notification
typedef struct {
    uintptr_t v;
} _PyEvent;

// A one-time event notification with reference counting
typedef struct _PyEventRc {
    _PyEvent event;
    Py_ssize_t refcount;
} _PyEventRc;

extern void _PyMutex_LockSlowEx(_PyMutex *m, int detach);

extern void _PyRawMutex_lock_slow(_PyRawMutex *m);
extern void _PyRawMutex_unlock_slow(_PyRawMutex *m);

extern void _PyRawEvent_Notify(_PyRawEvent *o);
extern void _PyRawEvent_Wait(_PyRawEvent *o);
extern int _PyRawEvent_TimedWait(_PyRawEvent *o, int64_t ns);
extern void _PyRawEvent_Reset(_PyRawEvent *o);

extern void _PyEvent_Notify(_PyEvent *o);
extern void _PyEvent_Wait(_PyEvent *o);
extern int _PyEvent_TimedWait(_PyEvent *o, int64_t ns);

extern PyLockStatus
_PyMutex_TimedLockEx(_PyMutex *m, _PyTime_t timeout_ns, _PyLockFlags flags);

extern int _PyMutex_TryUnlock(_PyMutex *m);

static inline void
_PyMutex_LockEx(_PyMutex *m, int detach)
{
    if (_PyMutex_lock_fast(m)) {
        return;
    }
    _PyMutex_LockSlowEx(m, detach);
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

#ifdef __cplusplus
}
#endif
#endif   /* !Py_INTERNAL_LOCK_H */
