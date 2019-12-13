#include "Python.h"
#include "pyatomic.h"


typedef struct {
    PyObject_HEAD
    int32_t value;
} atomicint;


static PyObject *
atomicint_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    atomicint *self;
    self = (atomicint *) type->tp_alloc(type, 0);
    return (PyObject *) self;
}

static PyObject *
atomicint_repr(atomicint *self)
{
    int value = _Py_atomic_load_int32_relaxed(&self->value);
    return PyUnicode_FromFormat("_atomic.int(%d)", value);
}

static PyObject *
atomicint_add(atomicint *self, PyObject *value)
{
    long v;
    int overflow;

    v = PyLong_AsLongAndOverflow(value, &overflow);
    if (overflow) {
        PyErr_Format(PyExc_ValueError,
                     "overflow");
        return NULL;
    }
    if (v == -1 && PyErr_Occurred()) {
        return NULL;
    }

    int32_t old = _Py_atomic_add_int32(&self->value, (int32_t)v);
    return PyLong_FromLong(old);
}

static PyObject *
atomicint_load(atomicint *self, PyObject *Py_UNUSED(ignored))
{
    int value = _Py_atomic_load_int32(&self->value);
    return PyLong_FromLong(value);
}

static PyObject *
atomicint_store(atomicint *self, PyObject *value)
{
    long v;
    int overflow;

    v = PyLong_AsLongAndOverflow(value, &overflow);
    if (overflow) {
        PyErr_Format(PyExc_ValueError,
                     "overflow");
        return NULL;
    }
    if (v == -1 && PyErr_Occurred()) {
        return NULL;
    }

    _Py_atomic_store_int32(&self->value, (int32_t)v);
    Py_RETURN_NONE;
}

static PyMethodDef atomicint_methods[] = {
    {"add", (PyCFunction)atomicint_add,
     METH_O, NULL},
    {"load", (PyCFunction)atomicint_load,
     METH_NOARGS, NULL},
    {"store", (PyCFunction)atomicint_store,
     METH_O, NULL},
    {NULL,           NULL}              /* sentinel */
};

static PyTypeObject atomicint_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_atomic.int",                      /* tp_name */
    sizeof(atomicint),                  /* tp_basicsize */
    0,                                  /* tp_itemsize */
    0,                                  /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    (reprfunc)atomicint_repr,           /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    0,                                  /* tp_call */
    0,                                  /* tp_str */
    0,                                  /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                 /* tp_flags */
    0,                                  /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    atomicint_methods,                  /* tp_methods */
    0,                                  /* tp_members */
    0,                                  /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    0,                                  /* tp_init */
    PyType_GenericAlloc,                /* tp_alloc */
    atomicint_new                       /* tp_new */
};


static PyMethodDef atomic_methods[] = {
    {NULL,                      NULL}           /* sentinel */
};

PyDoc_STRVAR(atomic_doc,
"This module provides primitive operations to write multi-threaded programs.\n\
The 'threading' module provides a more convenient interface.");


static struct PyModuleDef atomicmodule = {
    PyModuleDef_HEAD_INIT,
    "_atomic",
    atomic_doc,
    -1,
    atomic_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit__atomic(void)
{
    if (PyType_Ready(&atomicint_type) < 0) {
        return NULL;
    }

    PyObject *m = PyModule_Create(&atomicmodule);
    if (m == NULL) {
        return NULL;
    }

    Py_INCREF(&atomicint_type);
    if (PyModule_AddObject(m, "int", (PyObject *) &atomicint_type) < 0) {
        return NULL;
    }

    return m;
}
