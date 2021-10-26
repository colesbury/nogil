#include "Python.h"
#include "lock.h"
#include "parking_lot.h"
#include <stddef.h>               // offsetof()

/*[clinic input]
module _queue
class _queue.SimpleQueue "simplequeueobject *" "&PySimpleQueueType"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=cf49af81bcbbbea6]*/

static PyTypeObject PySimpleQueueType;  /* forward decl */

static PyObject *EmptyError;


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


static void
simplequeue_dealloc(simplequeueobject *self)
{
    PyObject_GC_UnTrack(self);
    if (_PyMutex_is_locked(&self->mutex)) {
        Py_FatalError("SimpleQueue: dealloc with locked queue");
    }
    if (self->data) {
        PyObject **data = self->data;
        Py_ssize_t n = self->count;
        Py_ssize_t idx = self->get_index;
        for (; n != 0 ; n--) {
            Py_DECREF(data[idx]);
            idx++;
            if (idx == self->buffer_size) {
                idx = 0;
            }
        }
        PyMem_Free(self->data);
    }
    Py_TYPE(self)->tp_free(self);
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
    Py_ssize_t new_buffer_size = self->buffer_size * 2;
    PyObject **new_data = PyMem_Malloc(new_buffer_size * sizeof(PyObject*));
    if (!new_data) {
        return -1;
    }

    /* Copy the contiguous "tail" of the old buffer to the beginning
     * of the new buffer. */
    Py_ssize_t tail_size = self->buffer_size - self->get_index;
    memcpy(new_data, self->data + self->get_index, tail_size * sizeof(PyObject*));

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

/*[clinic input]
_queue.SimpleQueue.get
    block: bool = True
    timeout: object = None

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
_queue_SimpleQueue_get_impl(simplequeueobject *self, int block,
                            PyObject *timeout)
/*[clinic end generated code: output=ec82a7157dcccd1a input=4bf691f9f01fa297]*/
{
    _PyTime_t endtime = 0;
    if (block && timeout != Py_None) {
        _PyTime_t timeout_val;
        /* With timeout */
        if (_PyTime_FromSecondsObject(&timeout_val,
                                      timeout, _PyTime_ROUND_CEILING) < 0)
            return NULL;
        if (timeout_val < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "'timeout' must be a non-negative number");
            return NULL;
        }
        endtime = _PyTime_GetMonotonicClock() + timeout_val;
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
            self->waiting = 1;
        }
        _PyMutex_unlock(&self->mutex);

        if (item) {
            return item;
        }

        if (!block) {
            PyErr_SetNone(EmptyError);
            return NULL;
        }

        int64_t timeout_ns = -1;
        if (endtime != 0) {
            timeout_ns = endtime - _PyTime_GetMonotonicClock();
            if (timeout_ns < 0) {
                PyErr_SetNone(EmptyError);
                return NULL;
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
            PyErr_SetNone(EmptyError);
            return NULL;
        }
    }
}

/*[clinic input]
_queue.SimpleQueue.get_nowait

Remove and return an item from the queue without blocking.

Only get an item if one is immediately available. Otherwise
raise the Empty exception.
[clinic start generated code]*/

static PyObject *
_queue_SimpleQueue_get_nowait_impl(simplequeueobject *self)
/*[clinic end generated code: output=a89731a75dbe4937 input=6fe5102db540a1b9]*/
{
    return _queue_SimpleQueue_get_impl(self, 0, Py_None);
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


#include "clinic/_queuemodule.c.h"


static PyMethodDef simplequeue_methods[] = {
    _QUEUE_SIMPLEQUEUE_EMPTY_METHODDEF
    _QUEUE_SIMPLEQUEUE_GET_METHODDEF
    _QUEUE_SIMPLEQUEUE_GET_NOWAIT_METHODDEF
    _QUEUE_SIMPLEQUEUE_PUT_METHODDEF
    _QUEUE_SIMPLEQUEUE_PUT_NOWAIT_METHODDEF
    _QUEUE_SIMPLEQUEUE_QSIZE_METHODDEF
    {"__class_getitem__",    (PyCFunction)Py_GenericAlias,
    METH_O|METH_CLASS,       PyDoc_STR("See PEP 585")},
    {NULL,           NULL}              /* sentinel */
};


static PyTypeObject PySimpleQueueType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_queue.SimpleQueue",               /*tp_name*/
    sizeof(simplequeueobject),          /*tp_basicsize*/
    0,                                  /*tp_itemsize*/
    /* methods */
    (destructor)simplequeue_dealloc,    /*tp_dealloc*/
    0,                                  /*tp_vectorcall_offset*/
    0,                                  /*tp_getattr*/
    0,                                  /*tp_setattr*/
    0,                                  /*tp_as_async*/
    0,                                  /*tp_repr*/
    0,                                  /*tp_as_number*/
    0,                                  /*tp_as_sequence*/
    0,                                  /*tp_as_mapping*/
    0,                                  /*tp_hash*/
    0,                                  /*tp_call*/
    0,                                  /*tp_str*/
    0,                                  /*tp_getattro*/
    0,                                  /*tp_setattro*/
    0,                                  /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE
        | Py_TPFLAGS_HAVE_GC,           /* tp_flags */
    simplequeue_new__doc__,             /*tp_doc*/
    (traverseproc)simplequeue_traverse, /*tp_traverse*/
    0,                                  /*tp_clear*/
    0,                                  /*tp_richcompare*/
    offsetof(simplequeueobject, weakreflist), /*tp_weaklistoffset*/
    0,                                  /*tp_iter*/
    0,                                  /*tp_iternext*/
    simplequeue_methods,                /*tp_methods*/
    0,                                  /* tp_members */
    0,                                  /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    0,                                  /* tp_init */
    0,                                  /* tp_alloc */
    simplequeue_new                     /* tp_new */
};


/* Initialization function */

PyDoc_STRVAR(queue_module_doc,
"C implementation of the Python queue module.\n\
This module is an implementation detail, please do not use it directly.");

static struct PyModuleDef queuemodule = {
    PyModuleDef_HEAD_INIT,
    "_queue",
    queue_module_doc,
    -1,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};


PyMODINIT_FUNC
PyInit__queue(void)
{
    PyObject *m;

    /* Create the module */
    m = PyModule_Create(&queuemodule);
    if (m == NULL)
        return NULL;

    EmptyError = PyErr_NewExceptionWithDoc(
        "_queue.Empty",
        "Exception raised by Queue.get(block=0)/get_nowait().",
        NULL, NULL);
    if (EmptyError == NULL)
        return NULL;

    Py_INCREF(EmptyError);
    if (PyModule_AddObject(m, "Empty", EmptyError) < 0)
        return NULL;

    if (PyModule_AddType(m, &PySimpleQueueType) < 0) {
        return NULL;
    }

    return m;
}
