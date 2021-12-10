#ifndef Py_INTERNAL_TYPEID_H
#define Py_INTERNAL_TYPEID_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// This contains code for allocating unique ids to heap type objects
// and re-using those ids when the type is deallocated.
//
// The type ids are used to implement per-thread reference counts of
// heap type objects to avoid contention on the reference count fields
// of heap type objects. (Non-heap type objects are immortal, so contention
// is not an issue.)
//
// Note that type id of 0 is never allocated and indicates the type does
// not have an assigned id, such as for non-heap types.

// Each entry implicitly represents a type id based on it's offset in the
// table. Non-allocated entries form a free-list via the 'next' pointer.
// Allocated entries store the corresponding PyTypeObject.
typedef union PyTypeIdEntry {
    PyTypeObject *type;
    union PyTypeIdEntry *next;
} PyTypeIdEntry;

typedef struct PyTypeIdPool {
    _PyMutex mutex;

    // combined table of types with allocated type ids and unallocated
    // type ids.
    PyTypeIdEntry *table;

    // Next entry to allocate inside 'table' or NULL
    PyTypeIdEntry *next;

    // size of 'table'
    Py_ssize_t size;
} PyTypeIdPool;

// Allocates the next id from the pool of type ids and returns it.
// On error, returns -1.
extern int _PyTypeId_Allocate(PyTypeIdPool *pool, PyTypeObject *type);

// Releases the allocated type id back to the pool.
extern void _PyTypeId_Release(PyTypeIdPool *pool, PyTypeObject *type);

// Merges the thread-local reference counts into the corresponding types. 
extern void _PyTypeId_MergeRefcounts(PyTypeIdPool *pool, PyThreadState *tstate);

// Resizes
extern void _PyTypeId_IncrefSlow(PyTypeIdPool *pool, PyTypeObject *type);

extern void _PyTypeId_Finalize(PyTypeIdPool *pool);

#ifdef __cplusplus
}
#endif
#endif   /* !Py_INTERNAL_TYPEID_H */
