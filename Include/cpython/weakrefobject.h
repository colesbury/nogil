#ifndef Py_CPYTHON_WEAKREFOBJECT_H
#  error "this header file must not be included directly"
#endif

struct _PyWeakrefBase {
    PyObject_HEAD

    /* If wr_object is weakly referenced, wr_object has a doubly-linked NULL-
     * terminated list of weak references to it.  These are the list pointers.
     * If wr_object goes away, wr_object is set to Py_None, and these pointers
     * have no meaning then.
     */
    struct _PyWeakrefBase *wr_prev;
    struct _PyWeakrefBase *wr_next;
};

struct _PyWeakrefControl {
    struct _PyWeakrefBase base;

    /* Protectes the weakref linked-list and wr_object from
     * concurrent accesses. */
    _PyMutex mutex;

    /* The object to which this is a weak reference, or Py_None if none.
     * Note that this is a stealth reference:  wr_object's refcount is
     * not incremented to reflect this pointer.
     */
    PyObject *wr_object;
};

/* PyWeakReference is the base struct for the Python ReferenceType, ProxyType,
 * and CallableProxyType.
 */
struct _PyWeakReference {
    struct _PyWeakrefBase base;

    /* Pointer to weakref control block */
    struct _PyWeakrefControl *wr_parent;

    /* A callable to invoke when wr_object dies, or NULL if none. */
    PyObject *wr_callback;

    vectorcallfunc vectorcall;

    /* A cache for wr_object's hash code.  As usual for hashes, this is -1
     * if the hash code isn't known yet.
     */
    Py_hash_t hash;
};

typedef struct _PyWeakrefControl PyWeakrefControl;
typedef struct _PyWeakrefBase PyWeakrefBase;

PyAPI_FUNC(void) _PyWeakref_DetachRef(PyWeakReference *self);

PyAPI_FUNC(Py_ssize_t) _PyWeakref_GetWeakrefCount(struct _PyWeakrefControl *ctrl);

PyAPI_FUNC(void) _PyWeakref_ClearRef(PyWeakReference *self);

#define PyWeakref_GET_OBJECT(ref) PyWeakref_GetObject(_PyObject_CAST(ref))
