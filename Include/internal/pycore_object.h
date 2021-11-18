#ifndef Py_INTERNAL_OBJECT_H
#define Py_INTERNAL_OBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_gc.h"         // _PyObject_GC_IS_TRACKED()
#include "pycore_interp.h"     // PyInterpreterState.gc
#include "pycore_pystate.h"    // _PyThreadState_GET()

struct _PyWeakrefBase;

struct _PyWeakrefControl {
    struct _PyWeakrefBase base;

    _PyMutex mutex;

    /* The object to which this is a weak reference, or Py_None if none.
     * Note that this is a stealth reference:  wr_object's refcount is
     * not incremented to reflect this pointer.
     */
    PyObject *wr_object;
};

typedef struct _PyWeakrefControl PyWeakrefControl;
typedef struct _PyWeakrefBase PyWeakrefBase;

PyAPI_FUNC(int) _PyType_CheckConsistency(PyTypeObject *type);
PyAPI_FUNC(int) _PyDict_CheckConsistency(PyObject *mp, int check_content);
PyAPI_FUNC(void) _PyObject_Dealloc(PyObject *self);

/* Only private in Python 3.10 and 3.9.8+; public in 3.11 */
extern PyObject *_PyType_GetQualName(PyTypeObject *type);

/* Tell the GC to track this object.
 *
 * NB: While the object is tracked by the collector, it must be safe to call the
 * ob_traverse method.
 *
 * Internal note: interp->gc.generation0->_gc_prev doesn't have any bit flags
 * because it's not object header.  So we don't use _PyGCHead_PREV() and
 * _PyGCHead_SET_PREV() for it to avoid unnecessary bitwise operations.
 *
 * The PyObject_GC_Track() function is the public version of this macro.
 */
static inline void _PyObject_GC_TRACK_impl(const char *filename, int lineno,
                                           PyObject *op)
{
    _PyObject_ASSERT_FROM(op, !_PyObject_GC_IS_TRACKED(op),
                          "object already tracked by the garbage collector",
                          filename, lineno, "_PyObject_GC_TRACK");

    PyGC_Head *gc = _Py_AS_GC(op);
    gc->_gc_prev |= _PyGC_PREV_MASK_TRACKED;
}

#define _PyObject_GC_TRACK(op) \
    _PyObject_GC_TRACK_impl(__FILE__, __LINE__, _PyObject_CAST(op))

/* Tell the GC to stop tracking this object.
 *
 * Internal note: This may be called while GC. So _PyGC_PREV_MASK_COLLECTING
 * must be cleared. But _PyGC_PREV_MASK_FINALIZED bit is kept.
 *
 * The object must be tracked by the GC.
 *
 * The PyObject_GC_UnTrack() function is the public version of this macro.
 */
static inline void _PyObject_GC_UNTRACK_impl(const char *filename, int lineno,
                                             PyObject *op)
{
    _PyObject_ASSERT_FROM(op, _PyObject_GC_IS_TRACKED(op),
                          "object not tracked by the garbage collector",
                          filename, lineno, "_PyObject_GC_UNTRACK");

    PyGC_Head *gc = _Py_AS_GC(op);
    if (gc->_gc_next != 0) {
        PyGC_Head *prev = _PyGCHead_PREV(gc);
        PyGC_Head *next = _PyGCHead_NEXT(gc);

        _PyGCHead_SET_NEXT(prev, next);
        _PyGCHead_SET_PREV(next, prev);

        gc->_gc_next = 0;
    }

    gc->_gc_prev &= _PyGC_PREV_MASK_FINALIZED;
}

#define _PyObject_GC_UNTRACK(op) \
    _PyObject_GC_UNTRACK_impl(__FILE__, __LINE__, _PyObject_CAST(op))

#define _PyObject_FROM_GC(g) ((PyObject *)(((PyGC_Head *)g)+1))

/* Marks the object as support deferred reference counting.
 *
 * The object's type must be GC-enabled. This function is not thread-safe with
 * respect to concurrent modifications; it must be called before the object
 * becomes visible to other threads.
 *
 * Deferred refcounted objects are marked as "queued" to prevent merging
 * reference count fields outside the garbage collector.
 */ 
static _Py_ALWAYS_INLINE void
_PyObject_SET_DEFERRED_RC_impl(PyObject *op)
{
    assert(_Py_ThreadLocal(op) && "non thread-safe call to _PyObject_SET_DEFERRED_RC");
    assert(PyType_IS_GC(Py_TYPE(op)));
    op->ob_ref_local |= _Py_REF_DEFERRED_MASK;
    op->ob_ref_shared |= _Py_REF_QUEUED_MASK;
}

#define _PyObject_SET_DEFERRED_RC(op) \
    _PyObject_SET_DEFERRED_RC_impl(_PyObject_CAST(op))

static inline int
_PyObject_IS_DEFERRED_RC_impl(PyObject *op)
{
    return (op->ob_ref_local & _Py_REF_DEFERRED_MASK) != 0;
}

#define _PyObject_IS_DEFERRED_RC(op) \
    _PyObject_IS_DEFERRED_RC_impl(_PyObject_CAST(op))

static _Py_ALWAYS_INLINE PyObject **
_PyObject_GET_DICT_PTR(PyObject *obj)
{
    Py_ssize_t dictoffset;
    PyTypeObject *tp = Py_TYPE(obj);

    dictoffset = tp->tp_dictoffset;
    if (dictoffset == 0) {
        return NULL;
    }
    if (_PY_UNLIKELY(dictoffset < 0)) {
        Py_ssize_t tsize = Py_SIZE(obj);
        if (tsize < 0) {
            tsize = -tsize;
        }
        size_t size = _PyObject_VAR_SIZE(tp, tsize);

        dictoffset += (long)size;
        _PyObject_ASSERT(obj, dictoffset > 0);
        _PyObject_ASSERT(obj, dictoffset % SIZEOF_VOID_P == 0);
    }
    return (PyObject **) ((char *)obj + dictoffset);
}

static _Py_ALWAYS_INLINE PyObject *
_PyObject_GET_DICT(PyObject *obj)
{
    PyObject **dictptr = _PyObject_GET_DICT_PTR(obj);
    if (dictptr == NULL) {
        return NULL;
    }
    return *dictptr;
}

/* Tries to increment an object's reference count
 *
 * This is a specialized version of _Py_TryIncref that only succeeds if the
 * object is immortal or local to this thread. It does not handle the case
 * where the  reference count modification requires an atomic operation. This
 * allows call sites to specialize for the immortal/local case.
 */
static _Py_ALWAYS_INLINE int
_Py_TryIncrefFast(PyObject *op) {
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    if (_Py_REF_IS_IMMORTAL(local)) {
        return 1;
    }
    if (_PY_LIKELY(_Py_ThreadLocal(op))) {
        local += (1 << _Py_REF_LOCAL_SHIFT);
        _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local);
#ifdef Py_REF_DEBUG
        _Py_IncRefTotal();
#endif
        return 1;
    }
    return 0;
}

static _Py_ALWAYS_INLINE int
_Py_TryIncRefShared_impl(PyObject *op)
{
    for (;;) {
        uint32_t shared = _Py_atomic_load_uint32_relaxed(&op->ob_ref_shared);

        // Check the refcount and merged flag (ignoring queued flag)
        if ((shared & ~_Py_REF_QUEUED_MASK) == _Py_REF_MERGED_MASK) {
            // Can't incref merged objects with zero refcount
            return 0;
        }

        if (_Py_atomic_compare_exchange_uint32(
                &op->ob_ref_shared,
                shared,
                shared + (1 << _Py_REF_SHARED_SHIFT))) {
#ifdef Py_REF_DEBUG
            _Py_IncRefTotal();
#endif
            return 1;
        }
    }
}

#ifdef Py_REF_DEBUG
extern void _PyDebug_PrintTotalRefs(void);
#endif

#ifdef Py_TRACE_REFS
extern void _Py_AddToAllObjects(PyObject *op, int force);
extern void _Py_PrintReferences(FILE *);
extern void _Py_PrintReferenceAddresses(FILE *);
#endif

static inline PyObject **
_PyObject_GET_WEAKREFS_LISTPTR(PyObject *op)
{
    Py_ssize_t offset = Py_TYPE(op)->tp_weaklistoffset;
    return (PyObject **)((char *)op + offset);
}

static inline PyWeakrefControl **
_PyObject_GET_WEAKREFS_CONTROLPTR(PyObject *op)
{
    Py_ssize_t offset = Py_TYPE(op)->tp_weaklistoffset;
    return (PyWeakrefControl **)((char *)op + offset);
}

static inline PyWeakrefControl *
_PyObject_GET_WEAKREF_CONTROL(PyObject *op)
{
    return _Py_atomic_load_ptr(_PyObject_GET_WEAKREFS_CONTROLPTR(op));
}

// Fast inlined version of PyType_HasFeature()
static inline int
_PyType_HasFeature(PyTypeObject *type, unsigned long feature) {
    return ((type->tp_flags & feature) != 0);
}

// Fast inlined version of PyObject_IS_GC()
static inline int
_PyObject_IS_GC(PyObject *obj)
{
    return (PyType_IS_GC(Py_TYPE(obj))
            && (Py_TYPE(obj)->tp_is_gc == NULL
                || Py_TYPE(obj)->tp_is_gc(obj)));
}

// Fast inlined version of PyType_IS_GC()
#define _PyType_IS_GC(t) _PyType_HasFeature((t), Py_TPFLAGS_HAVE_GC)

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_OBJECT_H */
