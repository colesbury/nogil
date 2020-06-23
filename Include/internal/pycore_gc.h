#ifndef Py_INTERNAL_GC_H
#define Py_INTERNAL_GC_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_pystate.h"
#include "mimalloc.h"
#include "mimalloc-internal.h"

//   0   1   2   3   4   5   6   7
// *-------------------------------*
// |  GEN  | U | F | RSRVD         |
// *-------------------------------*


#define _PyObject_GC_TRACK(op) \
    _PyObject_GC_TRACK_impl(__FILE__, __LINE__, _PyObject_CAST(op))

#define _PyObject_GC_UNTRACK(op) \
    _PyObject_GC_UNTRACK_impl(__FILE__, __LINE__, _PyObject_CAST(op))

#undef _PyObject_GC_IS_TRACKED
#define _PyObject_GC_IS_TRACKED(o) \
    (_PyObject_GC_IS_TRACKED_impl(_Py_AS_GC(o)))

// _PyGC_FINALIZED(o) defined in objimpl.h (used by Cython)

#define _PyGC_SET_FINALIZED(o) \
    (_PyObject_GC_SET_FINALIZED_impl(_Py_AS_GC(o)))

/* True if the object may be tracked by the GC in the future, or already is.
   This can be useful to implement some optimizations. */
#define _PyObject_GC_MAY_BE_TRACKED(obj) \
    (PyObject_IS_GC(obj) && \
        (!PyTuple_CheckExact(obj) || _PyObject_GC_IS_TRACKED(obj)))


#define GC_TRACKED_SHIFT     (0)
#define GC_UNREACHABLE_SHIFT (2)
#define GC_FINALIZED_SHIFT   (3)

#define GC_TRACKED_MASK      (1<<GC_TRACKED_SHIFT)   // 3
#define GC_UNREACHABLE_MASK  (1<<GC_UNREACHABLE_SHIFT)  // 4
#define GC_FINALIZED_MASK    (1<<GC_FINALIZED_SHIFT)    // 8

static inline int
GC_BITS_IS_TRACKED(PyGC_Head *gc)
{
    return (gc->_gc_prev & GC_TRACKED_MASK) >> GC_TRACKED_SHIFT;
}

static inline int
GC_BITS_IS_UNREACHABLE(PyGC_Head *gc)
{
    return (gc->_gc_prev & GC_UNREACHABLE_MASK) >> GC_UNREACHABLE_SHIFT;
}

static inline int
GC_BITS_IS_FINALIZED(PyGC_Head *gc)
{
    return (gc->_gc_prev & GC_FINALIZED_MASK) >> GC_FINALIZED_SHIFT;
}

static inline void
GC_BITS_CLEAR(PyGC_Head *gc, uintptr_t mask)
{
    gc->_gc_prev &= ~mask;
}

static inline void
GC_BITS_SET(PyGC_Head *gc, uintptr_t mask)
{
    gc->_gc_prev |= mask;
}

static inline int
_PyObject_GC_IS_TRACKED_impl(PyGC_Head *gc)
{
    return GC_BITS_IS_TRACKED(gc) != 0;
}

static inline int
_PyObject_GC_FINALIZED_impl(PyGC_Head *gc)
{
    return GC_BITS_IS_FINALIZED(gc) != 0;
}

static inline void
_PyObject_GC_SET_FINALIZED_impl(PyGC_Head *gc)
{
    GC_BITS_SET(gc, GC_FINALIZED_MASK);
}

/* Tell the GC to track this object.
 *
 * NB: While the object is tracked by the collector, it must be safe to call the
 * ob_traverse method.
 *
 * Internal note: _PyRuntime.gc.generation0->_gc_prev doesn't have any bit flags
 * because it's not object header.  So we don't use _PyGCHead_PREV() and
 * _PyGCHead_SET_PREV() for it to avoid unnecessary bitwise operations.
 *
 * The PyObject_GC_Track() function is the public version of this macro.
 */
static inline void
_PyObject_GC_TRACK_impl(const char *filename, int lineno, PyObject *op)
{
    _PyObject_ASSERT_FROM(op, !_PyObject_GC_IS_TRACKED(op),
                          "object already tracked by the garbage collector",
                          filename, lineno, "_PyObject_GC_TRACK");

    PyGC_Head *gc = _Py_AS_GC(op);

    gc->_gc_prev |= (1 << GC_TRACKED_SHIFT);
    assert(GC_BITS_IS_TRACKED(gc) == 1);
}

static inline int
_PyGC_ShouldCollect(struct _gc_runtime_state *gcstate)
{
    int64_t live = _Py_atomic_load_int64_relaxed(&gcstate->gc_live);
    return !gcstate->collecting && gcstate->enabled && live >= gcstate->gc_threshold;
}

void gc_list_remove(PyGC_Head *node);

/* Tell the GC to stop tracking this object.
 *
 * Internal note: This may be called while GC. So _PyGC_PREV_MASK_COLLECTING
 * must be cleared. But _PyGC_PREV_MASK_FINALIZED bit is kept.
 *
 * The object must be tracked by the GC.
 *
 * The PyObject_GC_UnTrack() function is the public version of this macro.
 */
static inline void
_PyObject_GC_UNTRACK_impl(const char *filename, int lineno, PyObject *op)
{
    _PyObject_ASSERT_FROM(op, _PyObject_GC_IS_TRACKED(op),
                          "object not tracked by the garbage collector",
                          filename, lineno, "_PyObject_GC_UNTRACK");

    PyGC_Head *gc = _Py_AS_GC(op);
    if (gc->_gc_next != 0) {
        assert(gc->_gc_next != 0);
        assert(gc->_gc_prev != 0);
        gc_list_remove(gc);
    }
    assert(_PyGCHead_PREV(gc) == NULL);
    gc->_gc_prev &= GC_FINALIZED_MASK;
    assert(GC_BITS_IS_TRACKED(gc) == 0);
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_GC_H */
