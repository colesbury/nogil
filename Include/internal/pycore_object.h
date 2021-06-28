#ifndef Py_INTERNAL_OBJECT_H
#define Py_INTERNAL_OBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_pystate.h"   /* _PyRuntime.gc */
#include "pycore_gc.h"

PyAPI_FUNC(int) _PyType_CheckConsistency(PyTypeObject *type);
PyAPI_FUNC(int) _PyDict_CheckConsistency(PyObject *mp, int check_content);
PyAPI_FUNC(void) _PyObject_Dealloc(PyObject *self);

#ifdef Py_REF_DEBUG
extern void _PyDebug_PrintTotalRefs(void);
#endif

#ifdef Py_TRACE_REFS
extern void _Py_AddToAllObjects(PyObject *op, int force);
extern void _Py_PrintReferences(FILE *);
extern void _Py_PrintReferenceAddresses(FILE *);
#endif

static inline void
_PyObject_SET_DEFERRED_RC(PyObject *op)
{
	assert(_Py_ThreadLocal(op) && "non thread-safe call to _PyObject_SET_DEFERRED_RC");
	op->ob_ref_local |= _Py_REF_DEFERRED_MASK;
}


static inline int
_Py_TryIncrefStackFast(PyObject *op) {
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    if (_PY_LIKELY((local & (_Py_REF_DEFERRED_MASK | _Py_REF_IMMORTAL_MASK)) != 0)) {
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

static inline bool
_Py_TryIncRefShared2(PyObject *op)
{
    for (;;) {
        uint32_t shared = _Py_atomic_load_uint32_relaxed(&op->ob_ref_shared);

        if (shared == _Py_REF_MERGED_MASK || shared == (_Py_REF_MERGED_MASK|_Py_REF_QUEUED_MASK)) {
            // deferred rc objects may have zero refcount, but can still be
            // incref'd.
            uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
            if ((local & _Py_REF_DEFERRED_MASK) == 0) {
                return false;
            }
        }

        if (_Py_atomic_compare_exchange_uint32(
                &op->ob_ref_shared,
                shared,
                shared + (1 << _Py_REF_SHARED_SHIFT))) {
#ifdef Py_REF_DEBUG
            _Py_IncRefTotal();
#endif
            return true;
        }
    }
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_OBJECT_H */
