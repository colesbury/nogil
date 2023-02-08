#ifndef Py_INTERNAL_OBJECT_H
#define Py_INTERNAL_OBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stdbool.h>
#include "pycore_gc.h"            // _PyObject_GC_IS_TRACKED()
#include "pycore_interp.h"        // PyInterpreterState.gc
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_runtime.h"       // _PyRuntime

/* This value provides *effective* immortality, meaning the object should never
    be deallocated (until runtime finalization).  See PEP 683 for more details about
    immortality, as well as a proposed mechanism for proper immortality. */
#define _PyObject_IMMORTAL_REFCNT 999999999

#define _PyObject_IMMORTAL_INIT(type) \
    { \
        .ob_tid = (uintptr_t)Py_REF_IMMORTAL, \
        .ob_ref_local = (uint32_t)Py_REF_IMMORTAL, \
        .ob_type = type, \
    }
#define _PyVarObject_IMMORTAL_INIT(type, size) \
    { \
        .ob_base = _PyObject_IMMORTAL_INIT(type), \
        .ob_size = size, \
    }

PyAPI_FUNC(void) _Py_NO_RETURN _Py_FatalRefcountErrorFunc(
    const char *func,
    const char *message);

#define _Py_FatalRefcountError(message) \
    _Py_FatalRefcountErrorFunc(__func__, (message))

// Increment reference count by n
static inline void _Py_RefcntAdd(PyObject* op, Py_ssize_t n)
{
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    if (_Py_REF_IS_IMMORTAL(local)) {
        return;
    }

#ifdef Py_REF_DEBUG
    _Py_IncRefTotalN(n);
#endif
    if (_PY_LIKELY(_Py_ThreadLocal(op))) {
        local += _Py_STATIC_CAST(uint32_t, (n << _Py_REF_LOCAL_SHIFT));
        _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local);
    }
    else {
        _Py_atomic_add_uint32(&op->ob_ref_shared, _Py_STATIC_CAST(uint32_t, n << _Py_REF_SHARED_SHIFT));
    }
}
#define _Py_RefcntAdd(op, n) _Py_RefcntAdd(_PyObject_CAST(op), n)

static _Py_ALWAYS_INLINE void
_Py_DECREF_SPECIALIZED(PyObject *op, const destructor destruct)
{
    Py_DECREF(op);
}

static _Py_ALWAYS_INLINE void
_Py_DECREF_NO_DEALLOC(PyObject *op)
{
    Py_DECREF(op);
}

PyAPI_FUNC(int) _PyType_CheckConsistency(PyTypeObject *type);
PyAPI_FUNC(int) _PyDict_CheckConsistency(PyObject *mp, int check_content);
PyAPI_FUNC(void) _PyObject_Dealloc(PyObject *self);

/* Update the Python traceback of an object. This function must be called
   when a memory block is reused from a free list.

   Internal function called by _Py_NewReference(). */
extern int _PyTraceMalloc_NewReference(PyObject *op);

// Fast inlined version of PyType_HasFeature()
static inline int
_PyType_HasFeature(PyTypeObject *type, unsigned long feature) {
    return PyType_HasFeature(type, feature);
}

extern void _PyType_InitCache(PyInterpreterState *interp);


/* Inline functions trading binary compatibility for speed:
   _PyObject_Init() is the fast version of PyObject_Init(), and
   _PyObject_InitVar() is the fast version of PyObject_InitVar().

   These inline functions must not be called with op=NULL. */
static inline void
_PyObject_Init(PyObject *op, PyTypeObject *typeobj)
{
    assert(op != NULL);
    Py_SET_TYPE(op, typeobj);
    if (_PyType_HasFeature(typeobj, Py_TPFLAGS_HEAPTYPE)) {
        Py_INCREF(typeobj);
    }
    _Py_NewReference(op);
}

static inline void
_PyObject_InitVar(PyVarObject *op, PyTypeObject *typeobj, Py_ssize_t size)
{
    assert(op != NULL);
    Py_SET_SIZE(op, size);
    _PyObject_Init((PyObject *)op, typeobj);
}


/* Tell the GC to track this object.
 *
 * The object must not be tracked by the GC.
 *
 * NB: While the object is tracked by the collector, it must be safe to call the
 * ob_traverse method.
 *
 * Internal note: interp->gc.generation0->_gc_prev doesn't have any bit flags
 * because it's not object header.  So we don't use _PyGCHead_PREV() and
 * _PyGCHead_SET_PREV() for it to avoid unnecessary bitwise operations.
 *
 * See also the public PyObject_GC_Track() function.
 */
static inline void _PyObject_GC_TRACK(
// The preprocessor removes _PyObject_ASSERT_FROM() calls if NDEBUG is defined
#ifndef NDEBUG
    const char *filename, int lineno,
#endif
    PyObject *op)
{
    _PyObject_ASSERT_FROM(op, !_PyObject_GC_IS_TRACKED(op),
                          "object already tracked by the garbage collector",
                          filename, lineno, __func__);

    PyGC_Head *gc = _Py_AS_GC(op);
    gc->_gc_prev |= _PyGC_PREV_MASK_TRACKED;
}

/* Tell the GC to stop tracking this object.
 *
 * Internal note: This may be called while GC. So _PyGC_PREV_MASK_COLLECTING
 * must be cleared. But _PyGC_PREV_MASK_FINALIZED bit is kept.
 *
 * The object must be tracked by the GC.
 *
 * See also the public PyObject_GC_UnTrack() which accept an object which is
 * not tracked.
 */
static inline void _PyObject_GC_UNTRACK(
// The preprocessor removes _PyObject_ASSERT_FROM() calls if NDEBUG is defined
#ifndef NDEBUG
    const char *filename, int lineno,
#endif
    PyObject *op)
{
    _PyObject_ASSERT_FROM(op, _PyObject_GC_IS_TRACKED(op),
                          "object not tracked by the garbage collector",
                          filename, lineno, __func__);

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

// Macros to accept any type for the parameter, and to automatically pass
// the filename and the filename (if NDEBUG is not defined) where the macro
// is called.
#ifdef NDEBUG
#  define _PyObject_GC_TRACK(op) \
        _PyObject_GC_TRACK(_PyObject_CAST(op))
#  define _PyObject_GC_UNTRACK(op) \
        _PyObject_GC_UNTRACK(_PyObject_CAST(op))
#else
#  define _PyObject_GC_TRACK(op) \
        _PyObject_GC_TRACK(__FILE__, __LINE__, _PyObject_CAST(op))
#  define _PyObject_GC_UNTRACK(op) \
        _PyObject_GC_UNTRACK(__FILE__, __LINE__, _PyObject_CAST(op))
#endif

/* Tries to increment an object's reference count
 *
 * This is a specialized version of _Py_TryIncref that only succeeds if the
 * object is immortal or local to this thread. It does not handle the case
 * where the  reference count modification requires an atomic operation. This
 * allows call sites to specialize for the immortal/local case.
 */
Py_ALWAYS_INLINE static inline int
_Py_TryIncrefFast(PyObject *op) {
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    local += (1 << _Py_REF_LOCAL_SHIFT);
    if (local == 0) {
        // immortal
        return 1;
    }
    if (_PY_LIKELY(_Py_ThreadLocal(op))) {
        _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local);
#ifdef Py_REF_DEBUG
        _Py_IncRefTotal();
#endif
        return 1;
    }
    return 0;
}

static _Py_ALWAYS_INLINE int
_Py_TryIncRefShared(PyObject *op)
{
    for (;;) {
        uint32_t shared = _Py_atomic_load_uint32_relaxed(&op->ob_ref_shared);

        // If the shared refcount is zero and the object is either merged
        // or may not have weak references, then we cannot incref it.
        if (shared == 0 || shared == _Py_REF_MERGED) {
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

/* Tries to incref the object op and ensures that *src still points to it. */
static inline int
_Py_TryAcquireObject(PyObject **src, PyObject *op)
{
    if (_Py_TryIncrefFast(op)) {
        return 1;
    }
    if (!_Py_TryIncRefShared(op)) {
        return 0;
    }
    if (op != _Py_atomic_load_ptr(src)) {
        Py_DECREF(op);
        return 0;
    }
    return 1;
}

/* Loads and increfs an object from ptr, which may contain a NULL value.
   Safe with concurrent (atomic) updates to ptr.
   NOTE: The writer must set maybe-weakref on the stored object! */
static _Py_ALWAYS_INLINE PyObject *
_Py_XFetchRef(PyObject **ptr)
{
#ifdef Py_NOGIL
    for (;;) {
        PyObject *value = _Py_atomic_load_ptr(ptr);
        if (value == NULL) {
            return value;
        }
        if (_Py_TryAcquireObject(ptr, value)) {
            return value;
        }
    }
#else
    return Py_XNewRef(*ptr);
#endif
}

/* Attempts to loads and increfs an object from ptr. Returns NULL
   on failure, which may be due to a NULL value or a concurrent update. */
static _Py_ALWAYS_INLINE PyObject *
_Py_TryXFetchRef(PyObject **ptr)
{
    PyObject *value = _Py_atomic_load_ptr(ptr);
    if (value == NULL) {
        return value;
    }
    if (_Py_TryAcquireObject(ptr, value)) {
        return value;
    }
    return NULL;
}

/* Like Py_NewRef but also optimistically sets _Py_REF_MAYBE_WEAKREF
   on objects owned by a different thread. */
static inline PyObject *
_Py_NewRefWithLock(PyObject *op)
{
    _Py_INCREF_STAT_INC();
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    local += (1 << _Py_REF_LOCAL_SHIFT);
    if (local == 0) {
        return op;
    }

#ifdef Py_REF_DEBUG
    _Py_IncRefTotal();
#endif
    if (_Py_ThreadLocal(op)) {
        _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local);
    }
    else {
        for (;;) {
            uint32_t shared = _Py_atomic_load_uint32_relaxed(&op->ob_ref_shared);
            uint32_t new_shared = shared + (1 << _Py_REF_SHARED_SHIFT);
            if ((shared & _Py_REF_SHARED_FLAG_MASK) == 0) {
                new_shared |= _Py_REF_MAYBE_WEAKREF;
            }
            if (_Py_atomic_compare_exchange_uint32(
                    &op->ob_ref_shared,
                    shared,
                    new_shared)) {
                return op;
            }
        }
    }
    return op;
}

static inline PyObject *
_Py_XNewRefWithLock(PyObject *obj)
{
    if (obj == NULL) {
        return NULL;
    }
    return _Py_NewRefWithLock(obj);
}

static inline void
_PyObject_SetMaybeWeakref(PyObject *op)
{
    if (_PyObject_IS_IMMORTAL(op)) {
        return;
    }
    for (;;) {
        uint32_t shared = _Py_atomic_load_uint32_relaxed(&op->ob_ref_shared);
        if ((shared & _Py_REF_SHARED_FLAG_MASK) != 0) {
            return;
        }
        if (_Py_atomic_compare_exchange_uint32(
                &op->ob_ref_shared,
                shared,
                shared | _Py_REF_MAYBE_WEAKREF)) {
            return;
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


/* Return the *address* of the object's weaklist.  The address may be
 * dereferenced to get the current head of the weaklist.  This is useful
 * for iterating over the linked list of weakrefs, especially when the
 * list is being modified externally (e.g. refs getting removed).
 *
 * The returned pointer should not be used to change the head of the list
 * nor should it be used to add, remove, or swap any refs in the list.
 * That is the sole responsibility of the code in weakrefobject.c.
 */
static inline PyWeakrefControl **
_PyObject_GET_WEAKREFS_CONTROLPTR(PyObject *op)
{
    if (PyType_Check(op) &&
            ((PyTypeObject *)op)->tp_flags & _Py_TPFLAGS_STATIC_BUILTIN) {
        static_builtin_state *state = _PyStaticType_GetState(
                                                        (PyTypeObject *)op);
        return _PyStaticType_GET_WEAKREFS_LISTPTR(state);
    }
    // Essentially _PyObject_GET_WEAKREFS_CONTROLPTR_FROM_OFFSET():
    Py_ssize_t offset = Py_TYPE(op)->tp_weaklistoffset;
    return (PyWeakrefControl **)((char *)op + offset);
}

/* This is a special case of _PyObject_GET_WEAKREFS_CONTROLPTR().
 * Only the most fundamental lookup path is used.
 * Consequently, static types should not be used.
 *
 * For static builtin types the returned pointer will always point
 * to a NULL tp_weaklist.  This is fine for any deallocation cases,
 * since static types are never deallocated and static builtin types
 * are only finalized at the end of runtime finalization.
 *
 * If the weaklist for static types is actually needed then use
 * _PyObject_GET_WEAKREFS_CONTROLPTR().
 */
static inline PyWeakrefControl **
_PyObject_GET_WEAKREFS_CONTROLPTR_FROM_OFFSET(PyObject *op)
{
    assert(!PyType_Check(op) ||
            ((PyTypeObject *)op)->tp_flags & Py_TPFLAGS_HEAPTYPE);
    Py_ssize_t offset = Py_TYPE(op)->tp_weaklistoffset;
    return (PyWeakrefControl **)((char *)op + offset);
}

static inline PyWeakrefControl *
_PyObject_GET_WEAKREF_CONTROL(PyObject *op)
{
    return _Py_atomic_load_ptr(_PyObject_GET_WEAKREFS_CONTROLPTR(op));
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
#define _PyGC_PREHEADER_SIZE (sizeof(PyGC_Head) + 2 * sizeof(PyObject *))

static inline size_t
_PyType_PreHeaderSize(PyTypeObject *tp)
{
    if (_PyType_IS_GC(tp)) {
        return _PyGC_PREHEADER_SIZE;
    }
    return 0;
}

static inline uint32_t
_Py_REF_PACK_SHARED(Py_ssize_t refcount, int flags)
{
    return _Py_STATIC_CAST(uint32_t, (refcount << _Py_REF_SHARED_SHIFT) + flags);
}

// Usage: assert(_Py_CheckSlotResult(obj, "__getitem__", result != NULL));
extern int _Py_CheckSlotResult(
    PyObject *obj,
    const char *slot_name,
    int success);

// PyType_Ready() must be called if _PyType_IsReady() is false.
// See also the Py_TPFLAGS_READY flag.
#define _PyType_IsReady(type) ((type)->tp_dict != NULL)

// Test if a type supports weak references
static inline int _PyType_SUPPORTS_WEAKREFS(PyTypeObject *type) {
    return (type->tp_weaklistoffset != 0);
}

extern PyObject* _PyType_AllocNoTrack(PyTypeObject *type, Py_ssize_t nitems);

extern int _PyObject_InitializeDict(PyObject *obj);
extern int _PyObject_StoreInstanceAttribute(PyObject *obj, PyDictValues *values,
                                          PyObject *name, PyObject *value);
PyObject * _PyObject_GetInstanceAttribute(PyObject *obj, PyDictValues *values,
                                        PyObject *name);

#define MANAGED_WEAKREF_OFFSET (((Py_ssize_t)sizeof(PyObject *))*-2)
#define MANAGED_DICT_OFFSET (((Py_ssize_t)sizeof(PyObject *))*-1)

typedef union {
    PyObject *dict;
    /* Use a char* to generate a warning if directly assigning a PyDictValues */
    char *values;
} PyDictOrValues;

static inline PyDictOrValues *
_PyObject_DictOrValuesPointer(PyObject *obj)
{
    assert(Py_TYPE(obj)->tp_flags & Py_TPFLAGS_MANAGED_DICT);
    return (PyDictOrValues *)((char *)obj + MANAGED_DICT_OFFSET);
}

static inline PyDictOrValues
_PyObject_DictOrValues(PyObject *obj)
{
    PyDictOrValues dorv;
    dorv.values = _Py_atomic_load_ptr_relaxed(_PyObject_DictOrValuesPointer(obj));
    return dorv;
}

static inline int
_PyDictOrValues_IsValues(PyDictOrValues dorv)
{
    return ((uintptr_t)dorv.values & 4) != 0;
}

static inline PyDictValues *
_PyDictOrValues_GetValues(PyDictOrValues dorv)
{
    assert(_PyDictOrValues_IsValues(dorv));
    return (PyDictValues *)((uintptr_t)dorv.values & ~7);
}

static inline PyObject *
_PyDictOrValues_GetDict(PyDictOrValues dorv)
{
    assert(!_PyDictOrValues_IsValues(dorv));
    return dorv.dict;
}

static inline void
_PyDictOrValues_SetValues(PyDictOrValues *ptr, PyDictValues *values)
{
    ptr->values = ((char *)values) + 4;
}

extern PyDictValues*
_PyDictValues_LockSlow(PyDictOrValues *dorv_ptr);

extern void
_PyDictValues_UnlockSlow(PyDictOrValues *dorv_ptr);

extern void
_PyDictValues_UnlockDict(PyDictOrValues *dorv_ptr, PyObject *dict);

static inline PyDictValues *
_PyDictValues_Lock(PyDictOrValues *dorv_ptr)
{
    PyDictOrValues dorv;
    dorv.values = _Py_atomic_load_ptr_relaxed(dorv_ptr);
    if (!_PyDictOrValues_IsValues(dorv)) {
        return NULL;
    }
    uintptr_t v = (uintptr_t)dorv.values;
    if ((v & LOCKED) == UNLOCKED) {
        if (_Py_atomic_compare_exchange_ptr(dorv_ptr, dorv.values, dorv.values + LOCKED)) {
            return _PyDictOrValues_GetValues(dorv);
        }
    }
    return _PyDictValues_LockSlow(dorv_ptr);
}

static inline void
_PyDictValues_Unlock(PyDictOrValues *dorv_ptr)
{
    char *values = _Py_atomic_load_ptr_relaxed(&dorv_ptr->values);
    uintptr_t v = (uintptr_t)values;
    assert((v & LOCKED));
    if ((v & HAS_PARKED) == 0) {
        if (_Py_atomic_compare_exchange_ptr(dorv_ptr, values, values - LOCKED)) {
            return;
        }
    }
    _PyDictValues_UnlockSlow(dorv_ptr);
}

extern PyObject ** _PyObject_ComputedDictPointer(PyObject *);
extern void _PyObject_FreeInstanceAttributes(PyObject *obj);
extern int _PyObject_IsInstanceDictEmpty(PyObject *);
extern int _PyType_HasSubclasses(PyTypeObject *);
extern PyObject* _PyType_GetSubclasses(PyTypeObject *);

// Access macro to the members which are floating "behind" the object
static inline PyMemberDef* _PyHeapType_GET_MEMBERS(PyHeapTypeObject *etype) {
    return (PyMemberDef*)((char*)etype + Py_TYPE(etype)->tp_basicsize);
}

PyAPI_FUNC(PyObject *) _PyObject_LookupSpecial(PyObject *, PyObject *);

/* C function call trampolines to mitigate bad function pointer casts.
 *
 * Typical native ABIs ignore additional arguments or fill in missing
 * values with 0/NULL in function pointer cast. Compilers do not show
 * warnings when a function pointer is explicitly casted to an
 * incompatible type.
 *
 * Bad fpcasts are an issue in WebAssembly. WASM's indirect_call has strict
 * function signature checks. Argument count, types, and return type must
 * match.
 *
 * Third party code unintentionally rely on problematic fpcasts. The call
 * trampoline mitigates common occurrences of bad fpcasts on Emscripten.
 */
#if defined(__EMSCRIPTEN__) && defined(PY_CALL_TRAMPOLINE)
#define _PyCFunction_TrampolineCall(meth, self, args) \
    _PyCFunctionWithKeywords_TrampolineCall( \
        (*(PyCFunctionWithKeywords)(void(*)(void))(meth)), (self), (args), NULL)
extern PyObject* _PyCFunctionWithKeywords_TrampolineCall(
    PyCFunctionWithKeywords meth, PyObject *, PyObject *, PyObject *);
#else
#define _PyCFunction_TrampolineCall(meth, self, args) \
    (meth)((self), (args))
#define _PyCFunctionWithKeywords_TrampolineCall(meth, self, args, kw) \
    (meth)((self), (args), (kw))
#endif // __EMSCRIPTEN__ && PY_CALL_TRAMPOLINE

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_OBJECT_H */
