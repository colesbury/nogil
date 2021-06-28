#ifndef Py_CPYTHON_OBJIMPL_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Inline functions trading binary compatibility for speed:
   PyObject_INIT() is the fast version of PyObject_Init(), and
   PyObject_INIT_VAR() is the fast version of PyObject_InitVar().

   These inline functions must not be called with op=NULL. */
static inline PyObject*
_PyObject_INIT(PyObject *op, PyTypeObject *typeobj)
{
    assert(op != NULL);
    Py_SET_TYPE(op, typeobj);
    Py_INCREF(typeobj);
    _Py_NewReference(op);
    return op;
}

#define PyObject_INIT(op, typeobj) \
    _PyObject_INIT(_PyObject_CAST(op), (typeobj))

static inline PyVarObject*
_PyObject_INIT_VAR(PyVarObject *op, PyTypeObject *typeobj, Py_ssize_t size)
{
    assert(op != NULL);
    Py_SET_SIZE(op, size);
    PyObject_INIT((PyObject *)op, typeobj);
    return op;
}

#define PyObject_INIT_VAR(op, typeobj, size) \
    _PyObject_INIT_VAR(_PyVarObject_CAST(op), (typeobj), (size))


/* This function returns the number of allocated memory blocks, regardless of size */
PyAPI_FUNC(Py_ssize_t) _Py_GetAllocatedBlocks(void);

/* Macros */
#ifdef WITH_PYMALLOC
PyAPI_FUNC(int) _PyObject_DebugMallocStats(FILE *out);
#endif


typedef struct {
    /* user context passed as the first argument to the 2 functions */
    void *ctx;

    /* allocate an arena of size bytes */
    void* (*alloc) (void *ctx, size_t size);

    /* free an arena */
    void (*free) (void *ctx, void *ptr, size_t size);
} PyObjectArenaAllocator;

/* Get the arena allocator. */
PyAPI_FUNC(void) PyObject_GetArenaAllocator(PyObjectArenaAllocator *allocator);

/* Set the arena allocator. */
PyAPI_FUNC(void) PyObject_SetArenaAllocator(PyObjectArenaAllocator *allocator);


PyAPI_FUNC(Py_ssize_t) _PyGC_CollectNoFail(void);
PyAPI_FUNC(Py_ssize_t) _PyGC_CollectIfEnabled(void);


/* Test if an object has a GC head */
#define PyObject_IS_GC(o) \
    (PyType_IS_GC(Py_TYPE(o)) \
     && (Py_TYPE(o)->tp_is_gc == NULL || Py_TYPE(o)->tp_is_gc(o)))

#ifdef __cplusplus
#define _Py_ALIGN_AS alignas
#else
#define _Py_ALIGN_AS _Alignas
#endif

/* GC information is stored BEFORE the object structure. */
typedef struct {
    // Pointer to previous object in the list.
    // Lowest two bits are used for flags documented later.
    _Py_ALIGN_AS(16) uintptr_t _gc_prev;

    // Pointer to next object in the list.
    // 0 means the object is not tracked
    uintptr_t _gc_next;
} PyGC_Head;

#define _Py_AS_GC(o) ((PyGC_Head *)(o)-1)
#define _Py_FROM_GC(g) ((PyObject *)(((PyGC_Head *)g)+1))

/* See also private _PyObject_GC_IS_TRACKED() macro. */
// TODO: should this be part of the public C-API and move to object.h?
PyAPI_FUNC(int) PyObject_GC_IsTracked(void *);
#define _PyObject_GC_IS_TRACKED(o) PyObject_GC_IsTracked(o)

/* Bit flags for _gc_prev */
/* Bit 0 is set when tp_finalize is called */
// #define _PyGC_PREV_MASK_FINALIZED  (1)
/* Bit 1 is set when the object is in generation which is GCed currently. */
// #define _PyGC_PREV_MASK_COLLECTING (2)
/* The (N-2) most significant bits contain the real address. */
#define _PyGC_PREV_SHIFT           (4)
#define _PyGC_PREV_MASK            (((uintptr_t) -1) << _PyGC_PREV_SHIFT)

// Lowest bit of _gc_next is used for flags only in GC.
// But it is always 0 for normal code.
#define _PyGCHead_NEXT(g)        ((PyGC_Head*)(g)->_gc_next)
#define _PyGCHead_SET_NEXT(g, p) ((g)->_gc_next = (uintptr_t)(p))

// Lowest two bits of _gc_prev is used for _PyGC_PREV_MASK_* flags.
#define _PyGCHead_PREV(g) ((PyGC_Head*)((g)->_gc_prev & _PyGC_PREV_MASK))
#define _PyGCHead_SET_PREV(g, p) do { \
    assert(((uintptr_t)p & ~_PyGC_PREV_MASK) == 0); \
    (g)->_gc_prev = ((g)->_gc_prev & ~_PyGC_PREV_MASK) \
        | ((uintptr_t)(p)); \
    } while (0)


// Used by Cython
#define _PyGC_FINALIZED(o) _PyObject_IsFinalized(o)

PyAPI_FUNC(int) _PyObject_IsFinalized(PyObject *op);
PyAPI_FUNC(PyObject *) _PyObject_GC_Malloc(size_t size);
PyAPI_FUNC(PyObject *) _PyObject_GC_Calloc(size_t size);


/* Test if a type supports weak references */
#define PyType_SUPPORTS_WEAKREFS(t) ((t)->tp_weaklistoffset > 0)

#define PyObject_GET_WEAKREFS_LISTPTR(o) \
    ((PyObject **) (((char *) (o)) + Py_TYPE(o)->tp_weaklistoffset))

#ifdef __cplusplus
}
#endif
