#ifndef Py_LIMITED_API
#ifndef Py_INTERNAL_CRITICAL_SECTION_H
#define Py_INTERNAL_CRITICAL_SECTION_H

#include "pycore_pystate.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/*
 * Implementation of Python critical sections.
 *
 * Python critical sections are helpers to replace the global interpreter lock
 * with finer grained locking. A Python critical section is a region of code
 * that can only be executed by a single thread at at time. The regions begin
 * with a call to _Py_critical_section_begin and end with either an explicit
 * call to _Py_critical_section_end or *implicitly* at any point that might
 * have  released the global interpreter lock. This is a substantial difference
 * from the traditonal notion of a "critical section", where the end of the
 * section is typically explicitly marked.
 *
 * The critical section can be resumed after a potential implicit ending by
 * the _Py_critical_section_resume function.
 *
 * The purposed of the implicitly ending critical sections is to avoid
 * potential deadlock due to holding locks on multiple objects. Any time a
 * thread would have released the GIL, it releases all locks from critical
 * sections. This includes block on a lock acquisition.
 *
 * The following are examples of calls that may implicitly end a critical
 * section:
 *
 *  Py_DECREF, PyObject_GC_New, PyObject_Call, PyObject_RichCompareBool,
 *  Py_BuildValue, _Py_critical_section_begin
 *
 * The following are examples of calls that do NOT implicitly end a critical
 * section:
 *
 *  Py_INCREF, PyMem_RawMalloc, PyMem_RawFree, memset and other C functions
 *  that do not call into the Python API.
 */

enum {
    // Tag to used with `prev` pointer to differentiate _Py_critical_section
    // vs. _Py_critical_section2.
    _Py_CRITICAL_SECTION_INACTIVE = 1,

    _Py_CRITICAL_SECTION_TWO_MUTEXES = 2,

    _Py_CRITICAL_SECTION_MASK = 3
};

#define Py_BEGIN_CRITICAL_SECTION(op) {         \
    struct _Py_critical_section _cs;            \
    _Py_critical_section_begin(&_cs, &_PyObject_CAST(op)->ob_mutex)

#define Py_BEGIN_CRITICAL_SECTION_MUTEX(m) {    \
    struct _Py_critical_section _cs;            \
    _Py_critical_section_begin(&_cs, m)

#define Py_END_CRITICAL_SECTION                 \
    _Py_critical_section_end(&_cs);             \
}

#define Py_BEGIN_CRITICAL_SECTION2(a, b) {      \
    struct _Py_critical_section2 _cs2;          \
    _Py_critical_section2_begin(&_cs2, &_PyObject_CAST(a)->ob_mutex, &_PyObject_CAST(b)->ob_mutex)

#define Py_END_CRITICAL_SECTION2                \
    _Py_critical_section2_end(&_cs2);           \
}

struct _Py_critical_section {
    // Pointer to the an outer active critical section (or
    // _Py_critical_section_sentinel). The two least-significant-bits indicate
    // whether the pointed-to critical section is inactive and whether it is
    // a _Py_critical_section2 object.
    uintptr_t prev;

    // Mutex used to protect critical section
    _PyMutex *mutex;
};

// A critical section protected by two mutexes. Use
// _Py_critical_section2_begin and _Py_critical_section2_end.
struct _Py_critical_section2 {
    struct _Py_critical_section base;

    _PyMutex *mutex2;
};

static inline int
_Py_critical_section_is_active(uintptr_t tag)
{
    return tag != 0 && (tag & _Py_CRITICAL_SECTION_INACTIVE) == 0;
}

PyAPI_FUNC(void)
_Py_critical_section_resume(PyThreadState *tstate);

PyAPI_FUNC(void)
_Py_critical_section_begin_slow(struct _Py_critical_section *c, _PyMutex *m);

PyAPI_FUNC(void)
_Py_critical_section2_begin_slow(struct _Py_critical_section2 *c,
                                 _PyMutex *m1, _PyMutex *m2, int flag);

static inline void
_Py_critical_section_begin(struct _Py_critical_section *c, _PyMutex *m)
{
    if (_PyMutex_lock_fast(m)) {
        PyThreadState *tstate = PyThreadState_GET();
        c->mutex = m;
        c->prev = tstate->critical_section;
        tstate->critical_section = (uintptr_t)c;
    }
    else {
        _Py_critical_section_begin_slow(c, m);
    }
}

static inline void
_Py_critical_section_pop(struct _Py_critical_section *c)
{
    PyThreadState *tstate = PyThreadState_GET();
    uintptr_t prev = c->prev;
    tstate->critical_section = prev;

    if (_PY_UNLIKELY((prev & _Py_CRITICAL_SECTION_INACTIVE))) {
        _Py_critical_section_resume(tstate);
    }
}

static inline void
_Py_critical_section_end(struct _Py_critical_section *c)
{
    _PyMutex_unlock(c->mutex);
    _Py_critical_section_pop(c);
}

static inline void
_Py_critical_section2_begin(struct _Py_critical_section2 *c,
                            _PyMutex *m1, _PyMutex *m2)
{
    if ((uintptr_t)m2 < (uintptr_t)m1) {
        _PyMutex *m1_ = m1;
        m1 = m2;
        m2 = m1_;
    }
    else if (m1 == m2) {
        c->mutex2 = NULL;
        _Py_critical_section_begin(&c->base, m1);
        return;
    }
    if (_PyMutex_lock_fast(m1)) {
        if (_PyMutex_lock_fast(m2)) {
            PyThreadState *tstate = PyThreadState_GET();
            c->base.mutex = m1;
            c->mutex2 = m2;
            c->base.prev = tstate->critical_section;

            uintptr_t p = (uintptr_t)c | _Py_CRITICAL_SECTION_TWO_MUTEXES;
            tstate->critical_section = p;
        }
        else {
            _Py_critical_section2_begin_slow(c, m1, m2, 1);
        }
    }
    else {
        _Py_critical_section2_begin_slow(c, m1, m2, 0);
    }
}

static inline void
_Py_critical_section2_end(struct _Py_critical_section2 *c)
{
    if (c->mutex2) {
        _PyMutex_unlock(c->mutex2);
    }
    _PyMutex_unlock(c->base.mutex);
    _Py_critical_section_pop(&c->base);
}

PyAPI_FUNC(void)
_Py_critical_section_end_all(PyThreadState *tstate);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CRITICAL_SECTION_H */
#endif /* !Py_LIMITED_API */
