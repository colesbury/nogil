#include "Python.h"
#include "pycore_object.h"   // _PyObject_GET_WEAKREFS_CONTROLPTR


#define GET_WEAKREFS_CONTROL(o) \
        (_PyObject_GET_WEAKREF_CONTROL(o))


#define GET_WEAKREFS_CONTROLPTR(o) \
        (_PyObject_GET_WEAKREFS_CONTROLPTR(o))

/*[clinic input]
module _weakref
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=ffec73b85846596d]*/

#include "clinic/_weakref.c.h"

/*[clinic input]

_weakref.getweakrefcount -> Py_ssize_t

  object: object
  /

Return the number of weak references to 'object'.
[clinic start generated code]*/

static Py_ssize_t
_weakref_getweakrefcount_impl(PyObject *module, PyObject *object)
/*[clinic end generated code: output=301806d59558ff3e input=cedb69711b6a2507]*/
{
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(object)))
        return 0;

    return _PyWeakref_GetWeakrefCount(GET_WEAKREFS_CONTROL(object));
}


static int
is_dead_weakref(PyObject *value)
{
    int is_dead;
    if (!PyWeakref_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a weakref");
        return -1;
    }

    PyObject *obj = PyWeakref_FetchObject(value);
    is_dead = (obj == Py_None);
    Py_DECREF(obj);
    return is_dead;
}

/*[clinic input]

_weakref._remove_dead_weakref -> object

  dct: object(subclass_of='&PyDict_Type')
  key: object
  /

Atomically remove key from dict if it points to a dead weakref.
[clinic start generated code]*/

static PyObject *
_weakref__remove_dead_weakref_impl(PyObject *module, PyObject *dct,
                                   PyObject *key)
/*[clinic end generated code: output=d9ff53061fcb875c input=19fc91f257f96a1d]*/
{
    if (_PyDict_DelItemIf(dct, key, is_dead_weakref) < 0) {
        if (PyErr_ExceptionMatches(PyExc_KeyError))
            /* This function is meant to allow safe weak-value dicts
               with GC in another thread (see issue #28427), so it's
               ok if the key doesn't exist anymore.
               */
            PyErr_Clear();
        else
            return NULL;
    }
    Py_RETURN_NONE;
}


/*[clinic input]
_weakref.getweakrefs
    object: object
    /

Return a list of all weak reference objects pointing to 'object'.
[clinic start generated code]*/

static PyObject *
_weakref_getweakrefs(PyObject *module, PyObject *object)
/*[clinic end generated code: output=25c7731d8e011824 input=00c6d0e5d3206693]*/
{
    PyObject *result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }

    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(object))) {
        return result;
    }

    PyWeakrefControl *root = GET_WEAKREFS_CONTROL(object);
    if (root == NULL) {
        return result;
    }

    _PyMutex_lock(&root->mutex);
    PyWeakrefBase *next = root->base.wr_next;
    while (next != (PyWeakrefBase *)root) {
        if (PyList_Append(result, (PyObject *)next) < 0) {
            _PyMutex_unlock(&root->mutex);
            Py_DECREF(result);
            return NULL;
        }
        next = next->wr_next;
    }
    _PyMutex_unlock(&root->mutex);
    return result;
}


/*[clinic input]

_weakref.proxy
    object: object
    callback: object(c_default="NULL") = None
    /

Create a proxy object that weakly references 'object'.

'callback', if given, is called with a reference to the
proxy when 'object' is about to be finalized.
[clinic start generated code]*/

static PyObject *
_weakref_proxy_impl(PyObject *module, PyObject *object, PyObject *callback)
/*[clinic end generated code: output=d68fa4ad9ea40519 input=4808adf22fd137e7]*/
{
    return PyWeakref_NewProxy(object, callback);
}


static PyMethodDef
weakref_functions[] =  {
    _WEAKREF_GETWEAKREFCOUNT_METHODDEF
    _WEAKREF__REMOVE_DEAD_WEAKREF_METHODDEF
    _WEAKREF_GETWEAKREFS_METHODDEF
    _WEAKREF_PROXY_METHODDEF
    {NULL, NULL, 0, NULL}
};

static int
weakref_exec(PyObject *module)
{
    Py_INCREF(&_PyWeakref_RefType);
    if (PyModule_AddObject(module, "ref", (PyObject *) &_PyWeakref_RefType) < 0) {
        Py_DECREF(&_PyWeakref_RefType);
        return -1;
    }
    Py_INCREF(&_PyWeakref_RefType);
    if (PyModule_AddObject(module, "ReferenceType",
                           (PyObject *) &_PyWeakref_RefType) < 0) {
        Py_DECREF(&_PyWeakref_RefType);
        return -1;
    }
    Py_INCREF(&_PyWeakref_ProxyType);
    if (PyModule_AddObject(module, "ProxyType",
                           (PyObject *) &_PyWeakref_ProxyType) < 0) {
        Py_DECREF(&_PyWeakref_ProxyType);
        return -1;
    }
    Py_INCREF(&_PyWeakref_CallableProxyType);
    if (PyModule_AddObject(module, "CallableProxyType",
                           (PyObject *) &_PyWeakref_CallableProxyType) < 0) {
        Py_DECREF(&_PyWeakref_CallableProxyType);
        return -1;
    }

    return 0;
}

static struct PyModuleDef_Slot weakref_slots[] = {
    {Py_mod_exec, weakref_exec},
    {0, NULL}
};

static struct PyModuleDef weakrefmodule = {
    PyModuleDef_HEAD_INIT,
    "_weakref",
    "Weak-reference support module.",
    0,
    weakref_functions,
    weakref_slots,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit__weakref(void)
{
    return PyModuleDef_Init(&weakrefmodule);
}
