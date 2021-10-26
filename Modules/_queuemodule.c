#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "pycore_moduleobject.h"  // _PyModule_GetState()
#include "structmember.h"         // PyMemberDef
#include "parking_lot.h"
#include <stddef.h>               // offsetof()

typedef struct {
    PyTypeObject *SimpleQueueType;
    PyObject *EmptyError;
} simplequeue_state;

static simplequeue_state *
simplequeue_get_state(PyObject *module)
{
    simplequeue_state *state = _PyModule_GetState(module);
    assert(state);
    return state;
}
static struct PyModuleDef queuemodule;
#define simplequeue_get_state_by_type(type) \
    (simplequeue_get_state(PyType_GetModuleByDef(type, &queuemodule)))

typedef struct {
    PyObject_HEAD
    /* protects all operations on queue */
    _PyMutex mutex;
    /* number of items in queue */
    Py_ssize_t count;
    /* offset of where to put next item */
    Py_ssize_t put_index;
    /* offset of where to take next item */
    Py_ssize_t get_index;
    /* size of data buffer */
    Py_ssize_t buffer_size;
    /* array of items with length buffer_size */
    PyObject **data;
    uintptr_t waiting;
    PyObject *weakreflist;
} simplequeueobject;

/*[clinic input]
module _queue
class _queue.SimpleQueue "simplequeueobject *" "simplequeue_get_state_by_type(type)->SimpleQueueType"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=0a4023fe4d198c8d]*/

static int
simplequeue_clear(simplequeueobject *self)
{
    if (self->data) {
        PyObject **data = self->data;
        Py_ssize_t n = self->count;
        Py_ssize_t idx = self->get_index;
        Py_ssize_t buffer_size = self->buffer_size;

        self->data = NULL;
        self->count = 0;
        self->put_index = 0;
        self->get_index = 0;
        self->buffer_size = 0;

        for (; n != 0 ; n--) {
            Py_DECREF(data[idx]);
            idx++;
            if (idx == buffer_size) {
                idx = 0;
            }
        }
        PyMem_Free(self->data);
    }
    return 0;
}

static void
simplequeue_dealloc(simplequeueobject *self)
{
    PyTypeObject *tp = Py_TYPE(self);

    PyObject_GC_UnTrack(self);
    if (_PyMutex_is_locked(&self->mutex)) {
        Py_FatalError("SimpleQueue: dealloc with locked queue");
    }
    (void)simplequeue_clear(self);
    if (self->weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *) self);
    Py_TYPE(self)->tp_free(self);
    Py_DECREF(tp);
}

static int
simplequeue_traverse(simplequeueobject *self, visitproc visit, void *arg)
{
    PyObject **data = self->data;
    Py_ssize_t n = self->count;
    Py_ssize_t idx = self->get_index;
    for (; n != 0 ; n--) {
        Py_VISIT(data[idx]);
        idx++;
        if (idx == self->buffer_size) {
            idx = 0;
        }
    }
    Py_VISIT(Py_TYPE(self));
    return 0;
}

/*[clinic input]
@classmethod
_queue.SimpleQueue.__new__ as simplequeue_new

Simple, unbounded, reentrant FIFO queue.
[clinic start generated code]*/

static PyObject *
simplequeue_new_impl(PyTypeObject *type)
/*[clinic end generated code: output=ba97740608ba31cd input=a0674a1643e3e2fb]*/
{
    simplequeueobject *self;

    self = (simplequeueobject *) type->tp_alloc(type, 0);
    if (!self) {
        return NULL;
    }

    self->weakreflist = NULL;
    self->buffer_size = 8;
    self->data = PyMem_Malloc(self->buffer_size * sizeof(PyObject*));
    if (self->data == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    memset(self->data, 0, self->buffer_size * sizeof(PyObject*));
    return (PyObject *) self;
}

static int
simplequeue_grow(simplequeueobject *self)
{
    Py_ssize_t new_buffer_size = Py_MAX(8, self->buffer_size * 2);
    PyObject **new_data = PyMem_Malloc(new_buffer_size * sizeof(PyObject*));
    if (!new_data) {
        return -1;
    }

    /* Copy the contiguous "tail" of the old buffer to the beginning
     * of the new buffer. */
    Py_ssize_t tail_size = self->buffer_size - self->get_index;
    if (tail_size > 0) {
        memcpy(new_data, self->data + self->get_index, tail_size * sizeof(PyObject*));
    }

    /* Next copy any elements that wrapped around the old buffer */
    Py_ssize_t remaining = self->count - tail_size;
    if (remaining > 0) {
        memcpy(new_data + tail_size, self->data, remaining * sizeof(PyObject*));
    }

    PyMem_Free(self->data);
    self->data = new_data;
    self->buffer_size = new_buffer_size;
    self->get_index = 0;
    self->put_index = self->count;
    return 0;
}

/*[clinic input]
_queue.SimpleQueue.put
    item: object
    block: bool = True
    timeout: object = None

Put the item on the queue.

The optional 'block' and 'timeout' arguments are ignored, as this method
never blocks.  They are provided for compatibility with the Queue class.

[clinic start generated code]*/

static PyObject *
_queue_SimpleQueue_put_impl(simplequeueobject *self, PyObject *item,
                            int block, PyObject *timeout)
/*[clinic end generated code: output=4333136e88f90d8b input=6e601fa707a782d5]*/
{
    _PyMutex_lock(&self->mutex);

    int handoff = 0;
    if (self->waiting) {
        int more_waiters;
        struct wait_entry *waiter;
        PyObject **objptr;

        /* If there is a waiter, handoff the item directly */
        objptr = _PyParkingLot_BeginUnpark(&self->waiting, &waiter, &more_waiters);
        if (objptr) {
            Py_INCREF(item);
            *objptr = item;
            handoff = 1;
        }
        self->waiting = more_waiters;
        _PyParkingLot_FinishUnpark(&self->waiting, waiter);
    }

    if (!handoff) {
        /* If we didn't handoff the item, add it to the queue */
        if (self->count == self->buffer_size && simplequeue_grow(self) < 0) {
            _PyMutex_unlock(&self->mutex);
            return NULL;
        }
        Py_INCREF(item);
        self->data[self->put_index] = item;
        self->put_index++;
        self->count++;
        if (self->put_index == self->buffer_size) {
            self->put_index = 0;
        }
    }

    _PyMutex_unlock(&self->mutex);
    Py_RETURN_NONE;
}

/*[clinic input]
_queue.SimpleQueue.put_nowait
    item: object

Put an item into the queue without blocking.

This is exactly equivalent to `put(item)` and is only provided
for compatibility with the Queue class.

[clinic start generated code]*/

static PyObject *
_queue_SimpleQueue_put_nowait_impl(simplequeueobject *self, PyObject *item)
/*[clinic end generated code: output=0990536715efb1f1 input=36b1ea96756b2ece]*/
{
    return _queue_SimpleQueue_put_impl(self, item, 0, Py_None);
}

static PyObject *
empty_error(PyTypeObject *cls)
{
    PyObject *module = PyType_GetModule(cls);
    simplequeue_state *state = simplequeue_get_state(module);
    PyErr_SetNone(state->EmptyError);
    return NULL;
}

/*[clinic input]
_queue.SimpleQueue.get

    cls: defining_class
    /
    block: bool = True
    timeout as timeout_obj: object = None

Remove and return an item from the queue.

If optional args 'block' is true and 'timeout' is None (the default),
block if necessary until an item is available. If 'timeout' is
a non-negative number, it blocks at most 'timeout' seconds and raises
the Empty exception if no item was available within that time.
Otherwise ('block' is false), return an item if one is immediately
available, else raise the Empty exception ('timeout' is ignored
in that case).

[clinic start generated code]*/

static PyObject *
_queue_SimpleQueue_get_impl(simplequeueobject *self, PyTypeObject *cls,
                            int block, PyObject *timeout_obj)
/*[clinic end generated code: output=5c2cca914cd1e55b input=5b4047bfbc645ec1]*/
{
    _PyTime_t endtime = 0;
    if (block != 0 && timeout_obj != Py_None) {
        _PyTime_t timeout;
        /* With timeout */
        if (_PyTime_FromSecondsObject(&timeout,
                                      timeout_obj, _PyTime_ROUND_CEILING) < 0) {
            return NULL;
        }
        if (timeout < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "'timeout' must be a non-negative number");
            return NULL;
        }
        PY_TIMEOUT_T microseconds = _PyTime_AsMicroseconds(timeout,
                                              _PyTime_ROUND_CEILING);
        if (microseconds > PY_TIMEOUT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "timeout value is too large");
            return NULL;
        }
        endtime = _PyDeadline_Init(timeout);
    }

    for (;;) {
        PyObject *item = NULL;

        _PyMutex_lock(&self->mutex);
        if (self->count > 0) {
            item = self->data[self->get_index];
            self->data[self->get_index] = NULL;

            self->count--;
            self->get_index++;
            if (self->get_index == self->buffer_size) {
                self->get_index = 0;
            }
        }
        else {
            _Py_atomic_store_uintptr_relaxed(&self->waiting, 1);
        }
        _PyMutex_unlock(&self->mutex);

        if (item) {
            return item;
        }

        if (!block) {
            return empty_error(cls);
        }

        int64_t timeout_ns = -1;
        if (endtime != 0) {
            timeout_ns = _PyDeadline_Get(endtime);
            if (timeout_ns < 0) {
                return empty_error(cls);
            }
        }

        int ret = _PyParkingLot_Park(&self->waiting, 1, &item, timeout_ns);
        if (ret == PY_PARK_OK) {
            assert(item);
            return item;
        }
        else if (ret == PY_PARK_INTR && Py_MakePendingCalls() < 0) {
            /* interrupted */
            return NULL;
        }
        else if (ret == PY_PARK_TIMEOUT) {
            return empty_error(cls);
        }
    }
}

/*[clinic input]
_queue.SimpleQueue.get_nowait

    cls: defining_class
    /

Remove and return an item from the queue without blocking.

Only get an item if one is immediately available. Otherwise
raise the Empty exception.
[clinic start generated code]*/

static PyObject *
_queue_SimpleQueue_get_nowait_impl(simplequeueobject *self,
                                   PyTypeObject *cls)
/*[clinic end generated code: output=620c58e2750f8b8a input=842f732bf04216d3]*/
{
    return _queue_SimpleQueue_get_impl(self, cls, 0, Py_None);
}

/*[clinic input]
_queue.SimpleQueue.empty -> bool

Return True if the queue is empty, False otherwise (not reliable!).
[clinic start generated code]*/

static int
_queue_SimpleQueue_empty_impl(simplequeueobject *self)
/*[clinic end generated code: output=1a02a1b87c0ef838 input=1a98431c45fd66f9]*/
{
    _PyMutex_lock(&self->mutex);
    int empty = self->count == 0;
    _PyMutex_unlock(&self->mutex);
    return empty;
}

/*[clinic input]
_queue.SimpleQueue.qsize -> Py_ssize_t

Return the approximate size of the queue (not reliable!).
[clinic start generated code]*/

static Py_ssize_t
_queue_SimpleQueue_qsize_impl(simplequeueobject *self)
/*[clinic end generated code: output=f9dcd9d0a90e121e input=7a74852b407868a1]*/
{
    _PyMutex_lock(&self->mutex);
    Py_ssize_t qsize = self->count;
    _PyMutex_unlock(&self->mutex);
    return qsize;
}

static int
queue_traverse(PyObject *m, visitproc visit, void *arg)
{
    simplequeue_state *state = simplequeue_get_state(m);
    Py_VISIT(state->SimpleQueueType);
    Py_VISIT(state->EmptyError);
    return 0;
}

static int
queue_clear(PyObject *m)
{
    simplequeue_state *state = simplequeue_get_state(m);
    Py_CLEAR(state->SimpleQueueType);
    Py_CLEAR(state->EmptyError);
    return 0;
}

static void
queue_free(void *m)
{
    queue_clear((PyObject *)m);
}

#include "clinic/_queuemodule.c.h"


static PyMethodDef simplequeue_methods[] = {
    _QUEUE_SIMPLEQUEUE_EMPTY_METHODDEF
    _QUEUE_SIMPLEQUEUE_GET_METHODDEF
    _QUEUE_SIMPLEQUEUE_GET_NOWAIT_METHODDEF
    _QUEUE_SIMPLEQUEUE_PUT_METHODDEF
    _QUEUE_SIMPLEQUEUE_PUT_NOWAIT_METHODDEF
    _QUEUE_SIMPLEQUEUE_QSIZE_METHODDEF
    {"__class_getitem__",    Py_GenericAlias,
    METH_O|METH_CLASS,       PyDoc_STR("See PEP 585")},
    {NULL,           NULL}              /* sentinel */
};

static struct PyMemberDef simplequeue_members[] = {
    {"__weaklistoffset__", T_PYSSIZET, offsetof(simplequeueobject, weakreflist), READONLY},
    {NULL},
};

static PyType_Slot simplequeue_slots[] = {
    {Py_tp_dealloc, simplequeue_dealloc},
    {Py_tp_doc, (void *)simplequeue_new__doc__},
    {Py_tp_traverse, simplequeue_traverse},
    {Py_tp_clear, simplequeue_clear},
    {Py_tp_members, simplequeue_members},
    {Py_tp_methods, simplequeue_methods},
    {Py_tp_new, simplequeue_new},
    {0, NULL},
};

static PyType_Spec simplequeue_spec = {
    .name = "_queue.SimpleQueue",
    .basicsize = sizeof(simplequeueobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = simplequeue_slots,
};


/* Initialization function */

PyDoc_STRVAR(queue_module_doc,
"C implementation of the Python queue module.\n\
This module is an implementation detail, please do not use it directly.");

static int
queuemodule_exec(PyObject *module)
{
    simplequeue_state *state = simplequeue_get_state(module);

    state->EmptyError = PyErr_NewExceptionWithDoc(
        "_queue.Empty",
        "Exception raised by Queue.get(block=0)/get_nowait().",
        NULL, NULL);
    if (state->EmptyError == NULL) {
        return -1;
    }
    if (PyModule_AddObjectRef(module, "Empty", state->EmptyError) < 0) {
        return -1;
    }

    state->SimpleQueueType = (PyTypeObject *)PyType_FromModuleAndSpec(
        module, &simplequeue_spec, NULL);
    if (state->SimpleQueueType == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, state->SimpleQueueType) < 0) {
        return -1;
    }

    return 0;
}

static PyModuleDef_Slot queuemodule_slots[] = {
    {Py_mod_exec, queuemodule_exec},
    {0, NULL}
};


static struct PyModuleDef queuemodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_queue",
    .m_doc = queue_module_doc,
    .m_size = sizeof(simplequeue_state),
    .m_slots = queuemodule_slots,
    .m_traverse = queue_traverse,
    .m_clear = queue_clear,
    .m_free = queue_free,
};


PyMODINIT_FUNC
PyInit__queue(void)
{
   return PyModuleDef_Init(&queuemodule);
}
