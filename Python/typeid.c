#include "Python.h"

#include "pycore_typeid.h"
#include "pycore_runtime.h"
#include "lock.h"

// This contains code for allocating unique ids to heap type objects
// and re-using those ids when the type is deallocated.
//
// The type ids are used to implement per-thread reference counts of
// heap type objects to avoid contention on the reference count fields
// of heap type objects. (Non-heap type objects are immortal, so contention
// is not an issue.)

#define POOL_MIN_SIZE 8

static int
resize_typeids(PyTypeIdPool *pool)
{
    Py_ssize_t new_size = pool->size * 2;
    if (new_size < POOL_MIN_SIZE) {
        new_size = POOL_MIN_SIZE;
    }

    PyTypeIdEntry *table;
    table = PyMem_RawCalloc(new_size, sizeof(*table));
    if (table == NULL) {
        return -1;
    }
    if (pool->table) {
        memcpy(table, pool->table, pool->size * sizeof(*table));
        PyMem_RawFree(pool->table);
    }

    Py_ssize_t start = pool->size;
    if (start == 0) {
        // don't allocate typeid '0'
        start = 1;
    }

    for (Py_ssize_t i = start; i < new_size - 1; i++) {
        table[i].next = &table[i + 1];
    }
    table[new_size - 1].next = NULL;

    pool->table = table;
    pool->next = &table[start];
    _Py_atomic_store_ssize_relaxed(&pool->size, new_size);
    return 0;
}

int
_PyTypeId_Allocate(PyTypeIdPool *pool, PyTypeObject *type)
{
    _PyMutex_lock(&pool->mutex);
    if (pool->next == NULL) {
        if (resize_typeids(pool) < 0) {
            PyErr_NoMemory();
            return -1;
        }
    }

    PyTypeIdEntry *entry = pool->next;
    pool->next = entry->next;
    entry->type = type;
    type->tp_typeid = (entry - pool->table);
    _PyMutex_unlock(&pool->mutex);
    return 0;
}

void
_PyTypeId_Release(PyTypeIdPool *pool, PyTypeObject *type)
{
    int do_lock = !_PyRuntime.stop_the_world;
    if (do_lock) {
        _PyMutex_lock(&pool->mutex);
    }
    assert(type->tp_typeid != 0);
    PyTypeIdEntry *entry = &pool->table[type->tp_typeid];
    entry->next = pool->next;
    pool->next = entry;
    type->tp_typeid = 0;
    if (do_lock) {
        _PyMutex_unlock(&pool->mutex);
    }
}

void
_PyTypeId_MergeRefcounts(PyTypeIdPool *pool, PyThreadState *tstate)
{
    if (tstate->local_refcnts == NULL) {
        return;
    }

    // We only lock the mutex if not called from garbage collection
    int do_lock = !_PyRuntime.stop_the_world;
    if (do_lock) {
        _PyMutex_lock(&pool->mutex);
    }

    for (Py_ssize_t i = 0, n = tstate->local_refcnts_size; i < n; i++) {
        Py_ssize_t refcnt = tstate->local_refcnts[i];
        if (refcnt != 0) {
            PyObject *type = (PyObject *)pool->table[i].type;
            assert(PyType_Check(type));

            uint32_t delta = (uint32_t)(refcnt << _Py_REF_SHARED_SHIFT);
            _Py_atomic_add_uint32(&type->ob_ref_shared, delta);
            tstate->local_refcnts[i] = 0;
        }
    }

    if (do_lock) {
        _PyMutex_unlock(&pool->mutex);
    }

    PyMem_RawFree(tstate->local_refcnts);
    tstate->local_refcnts = NULL;
    tstate->local_refcnts_size = 0;
}

void
_PyTypeId_IncrefSlow(PyTypeIdPool *pool, PyTypeObject *type)
{
    PyThreadState *tstate = PyThreadState_GET();
    Py_ssize_t *refcnts;
    Py_ssize_t size = _Py_atomic_load_ssize(&pool->size);

    refcnts = PyMem_RawCalloc(size, sizeof(Py_ssize_t));
    if (refcnts == NULL) {
        // on memory error, just incref the type directly.
        Py_INCREF(type);
        return;
    }
    if (tstate->local_refcnts != NULL) {
        memcpy(refcnts, tstate->local_refcnts,
               tstate->local_refcnts_size * sizeof(Py_ssize_t));
        PyMem_RawFree(tstate->local_refcnts);
    }

    tstate->local_refcnts = refcnts;
    tstate->local_refcnts_size = size;
    tstate->local_refcnts[type->tp_typeid]++;
}

void _PyTypeId_Finalize(PyTypeIdPool *pool)
{
    // First, set the free-list to NULL values
    while (pool->next) {
        PyTypeIdEntry *next = pool->next->next;
        pool->next->type = NULL;
        pool->next = next;
    }

    // Now everything non-NULL is a type. Set the type's tp_typeid
    // to zero in-case it outlives the PyRuntime.
    for (Py_ssize_t i = 0; i < pool->size; i++) {
        PyTypeObject *type = pool->table[i].type;
        if (type) {
            type->tp_typeid = 0;
            pool->table[i].type = NULL;
        }
    }
    PyMem_RawFree(pool->table);
    pool->table = NULL;
    pool->next = NULL;
    pool->size = 0;
}
