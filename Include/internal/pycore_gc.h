#ifndef Py_INTERNAL_GC_H
#define Py_INTERNAL_GC_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_lock.h"

/* GC information is stored BEFORE the object structure. */
typedef struct {
    // Pointer to previous object in the list.
    // Lowest three bits are used for flags documented later.
    uintptr_t _gc_prev;

    // Pointer to next object in the list.
    // 0 means the object is not tracked
    uintptr_t _gc_next;
} PyGC_Head;

#define PyGC_Head_OFFSET (((Py_ssize_t)sizeof(PyObject *))*-4)

/* Bit 0 is set if the object is tracked by the GC */
#define _PyGC_PREV_MASK_TRACKED     (1)
/* Bit 1 is set when tp_finalize is called */
#define _PyGC_PREV_MASK_FINALIZED   (2)
/* Bit 2 is set when the object is not currently reachable */
#define _PyGC_PREV_MASK_UNREACHABLE (4)
/* The (N-3) most significant bits contain the real address. */
#define _PyGC_PREV_SHIFT            (3)
#define _PyGC_PREV_MASK             (((uintptr_t) -1) << _PyGC_PREV_SHIFT)

/* Bit 0 is set if the object is tracked by the GC */
#define _PyGC_MASK_TRACKED     (1)
/* Bit 1 is set when tp_finalize is called */
#define _PyGC_MASK_FINALIZED   (2)
/* Bit 2 is set when the object is not currently reachable */
#define _PyGC_UNREACHABLE      (4)
/* Bit 3 is used by list and dict */
#define _PyGC_MASK_SHARED      (8)

static inline PyGC_Head* _Py_AS_GC(PyObject *op) {
    char *mem = _Py_STATIC_CAST(char*, op);
    return _Py_STATIC_CAST(PyGC_Head*, mem + PyGC_Head_OFFSET);
}

/* True if the object is currently tracked by the GC. */
static inline int _PyObject_GC_IS_TRACKED(PyObject *op) {
    return (op->ob_gc_bits & _PyGC_MASK_TRACKED) != 0;
}
#define _PyObject_GC_IS_TRACKED(op) _PyObject_GC_IS_TRACKED(_Py_CAST(PyObject*, op))

/* True if the object may be tracked by the GC in the future, or already is.
   This can be useful to implement some optimizations. */
static inline int _PyObject_GC_MAY_BE_TRACKED(PyObject *obj) {
    if (!PyObject_IS_GC(obj)) {
        return 0;
    }
    if (PyTuple_CheckExact(obj)) {
        return _PyObject_GC_IS_TRACKED(obj);
    }
    return 1;
}

static inline int _PyGC_FINALIZED(PyObject *op) {
    return ((op->ob_gc_bits & _PyGC_MASK_FINALIZED) != 0);
}
static inline void _PyGC_SET_FINALIZED(PyObject *op) {
    op->ob_gc_bits |= _PyGC_MASK_FINALIZED;
}

static inline int _PyObject_GC_IS_SHARED(PyObject *op) {
    return (op->ob_gc_bits & _PyGC_MASK_SHARED) != 0;
}
#define _PyObject_GC_IS_SHARED(op) _PyObject_GC_IS_SHARED(_Py_CAST(PyObject*, op))

static inline int _PyObject_GC_SET_SHARED(PyObject *op) {
    return op->ob_gc_bits |= _PyGC_MASK_SHARED;
}
#define _PyObject_GC_SET_SHARED(op) _PyObject_GC_SET_SHARED(_Py_CAST(PyObject*, op))


/* GC runtime state */

/* If we change this, we need to change the default value in the
   signature of gc.collect. */
#define NUM_GENERATIONS 1
/*
   NOTE: about untracking of mutable objects.

   Certain types of container cannot participate in a reference cycle, and
   so do not need to be tracked by the garbage collector. Untracking these
   objects reduces the cost of garbage collections. However, determining
   which objects may be untracked is not free, and the costs must be
   weighed against the benefits for garbage collection.

   There are two possible strategies for when to untrack a container:

   i) When the container is created.
   ii) When the container is examined by the garbage collector.

   Tuples containing only immutable objects (integers, strings etc, and
   recursively, tuples of immutable objects) do not need to be tracked.
   The interpreter creates a large number of tuples, many of which will
   not survive until garbage collection. It is therefore not worthwhile
   to untrack eligible tuples at creation time.

   Instead, all tuples except the empty tuple are tracked when created.
   During garbage collection it is determined whether any surviving tuples
   can be untracked. A tuple can be untracked if all of its contents are
   already not tracked. Tuples are examined for untracking in all garbage
   collection cycles. It may take more than one cycle to untrack a tuple.

   Dictionaries containing only immutable objects also do not need to be
   tracked. Dictionaries are untracked when created. If a tracked item is
   inserted into a dictionary (either as a key or value), the dictionary
   becomes tracked. During a full garbage collection (all generations),
   the collector will untrack any dictionaries whose contents are not
   tracked.

   The module provides the python function is_tracked(obj), which returns
   the CURRENT tracking status of the object. Subsequent garbage
   collections may change the tracking status of the object.

   Untracking of certain containers was introduced in issue #4688, and
   the algorithm was refined in response to issue #14775.
*/

struct gc_generation {
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};

/* Running stats per generation */
struct gc_generation_stats {
    /* total number of collections */
    Py_ssize_t collections;
    /* total number of collected objects */
    Py_ssize_t collected;
    /* total number of uncollectable objects (put into gc.garbage) */
    Py_ssize_t uncollectable;
};

typedef struct _PyObjectQueue _PyObjectQueue;

struct _gc_runtime_state {
    /* List of objects that still need to be cleaned up, singly linked
     * via their gc headers' gc_prev pointers.  */
    PyObject *trash_delete_later;
    /* Current call-stack depth of tp_dealloc calls. */
    int trash_delete_nesting;

    /* Is automatic collection enabled? */
    int enabled;
    int debug;
    /* a permanent generation which won't be collected */
    struct gc_generation_stats stats;
    /* true if we are currently running the collector */
    int collecting;
    /* list of uncollectable objects */
    PyObject *garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject *callbacks;
    /* the number of live GC objects */
    Py_ssize_t gc_live;
    /* the threshold at which to trigger a collection */
    Py_ssize_t gc_threshold;
    /* The ratio used to compute gc_threshold:
            gc_threshold = (1 + gc_scale/100) * gc_live
       A value of 100 means to collect every time the number of live
       objects doubles. */
    int gc_scale;
    /* This is the number of objects that survived the last full
       collection. It approximates the number of long lived objects
       tracked by the GC.

       (by "full collection", we mean a collection of the oldest
       generation). */
    Py_ssize_t long_lived_total;
    /* This is the number of objects that survived all "non-full"
       collections, and are awaiting to undergo a full collection for
       the first time. */
    Py_ssize_t long_lived_pending;

    Py_ssize_t gc_collected;
    Py_ssize_t gc_uncollectable;

    _PyObjectQueue *gc_work;
    _PyObjectQueue *gc_unreachable;
    _PyObjectQueue *gc_finalizers;
    _PyObjectQueue *gc_wrcb_to_call;
};


extern void _PyGC_InitState(struct _gc_runtime_state *);

extern Py_ssize_t _PyGC_CollectNoFail(PyThreadState *tstate);
extern void _PyGC_ResetHeap(void);
extern void _PyGC_DeferredToImmortal(void);

static inline int
_PyGC_ShouldCollect(struct _gc_runtime_state *gcstate)
{
    Py_ssize_t live = _Py_atomic_load_ssize_relaxed(&gcstate->gc_live);
    Py_ssize_t threshold = _Py_atomic_load_ssize_relaxed(&gcstate->gc_threshold);
    return (live >= threshold &&
            gcstate->enabled &&
            threshold);
}

// Functions to clear types free lists
extern void _PyTuple_ClearFreeList(PyThreadState *tstate);
extern void _PyFloat_ClearFreeList(PyThreadState *tstate);
extern void _PyList_ClearFreeList(PyThreadState *tstate);
extern void _PyDict_ClearFreeList(PyThreadState *tstate);
extern void _PyAsyncGen_ClearFreeLists(PyThreadState *tstate);
extern void _PyContext_ClearFreeList(PyThreadState *tstate);
extern void _Py_RunGC(PyThreadState *tstate);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_GC_H */
