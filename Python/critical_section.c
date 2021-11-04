#include "Python.h"
#include "pycore_critical_section.h"

void
_Py_critical_section_begin_slow(struct _Py_critical_section *c, _PyMutex *m)
{
    PyThreadState *tstate = PyThreadState_GET();
    c->mutex = NULL;
    c->prev = (uintptr_t)tstate->critical_section;
    tstate->critical_section = (uintptr_t)c;

    _PyMutex_lock(m);
    c->mutex = m;
}

void
_Py_critical_section2_begin_slow(struct _Py_critical_section2 *c,
                                 _PyMutex *m1, _PyMutex *m2, int flag)
{
    PyThreadState *tstate = PyThreadState_GET();
    c->base.mutex = NULL;
    c->mutex2 = NULL;
    c->base.prev = tstate->critical_section;
    tstate->critical_section = (uintptr_t)c | _Py_CRITICAL_SECTION_TWO_MUTEXES;

    if (!flag) {
        _PyMutex_lock(m1);
    }
    _PyMutex_lock(m2);
    c->base.mutex = m1;
    c->mutex2 = m2;
}

struct _Py_critical_section *
_Py_critical_section_untag(uintptr_t tag)
{
    tag &= ~_Py_CRITICAL_SECTION_MASK;
    return (struct _Py_critical_section *)tag;
}

// Release all locks held by critical sections. This is called by
// _PyThreadState_Detach.
void
_Py_critical_section_end_all(PyThreadState *tstate)
{
    uintptr_t *tagptr;
    struct _Py_critical_section *c;
    struct _Py_critical_section2 *c2;

    tagptr = &tstate->critical_section;
    while (*tagptr && _Py_critical_section_is_active(*tagptr)) {
        c = _Py_critical_section_untag(*tagptr);

        if (c->mutex) {
            _PyMutex_unlock(c->mutex);
            if ((*tagptr & _Py_CRITICAL_SECTION_TWO_MUTEXES)) {
                c2 = (struct _Py_critical_section2 *)c;
                if (c2->mutex2) {
                    _PyMutex_unlock(c2->mutex2);
                }
            }
        }

        *tagptr |= _Py_CRITICAL_SECTION_INACTIVE;
        tagptr = &c->prev;
    }
}

void
_Py_critical_section_resume(PyThreadState *tstate)
{
    uintptr_t p;
    struct _Py_critical_section *c;
    struct _Py_critical_section2 *c2;

    p = tstate->critical_section;
    c = _Py_critical_section_untag(p);
    assert(!_Py_critical_section_is_active(p));

    _PyMutex *m1 = NULL, *m2 = NULL;

    m1 = c->mutex;
    c->mutex = NULL;
    if ((p & _Py_CRITICAL_SECTION_TWO_MUTEXES)) {
        c2 = (struct _Py_critical_section2 *)c;
        m2 = c2->mutex2;
        c2->mutex2 = NULL;
    }

    if (m1) {
        _PyMutex_lock(m1);
    }
    if (m2) {
        _PyMutex_lock(m2);
    }

    c->mutex = m1;
    if (m2) {
        c2->mutex2 = m2;
    }

    tstate->critical_section &= ~_Py_CRITICAL_SECTION_INACTIVE;
}