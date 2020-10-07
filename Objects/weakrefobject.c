#include "Python.h"
#include "structmember.h"
#include "internal/pycore_refcnt.h"
#include "pyatomic.h"


#define GET_WEAKREFS_LISTPTR(o) \
        ((PyWeakReference **) PyObject_GET_WEAKREFS_LISTPTR(o))


Py_ssize_t
_PyWeakref_GetWeakrefCount(PyWeakReference *head)
{
    if (!head) {
        return 0;
    }

    PyWeakReference *ref;
    Py_ssize_t count = 1;

    ref = head->wr_prev;
    while (ref != NULL) {
        ++count;
        ref = ref->wr_prev;
    }

    ref = head->wr_next;
    while (ref != NULL) {
        ++count;
        ref = ref->wr_next;
    }

    return count;
}

static PyWeakReference *
new_weakref(PyTypeObject *type, PyObject *ob, PyObject *callback)
{
    PyWeakReference *self = (PyWeakReference *)type->tp_alloc(type, 0);
    if (!self) {
        return NULL;
    }
    self->mutex.v = 0;
    self->hash = -1;
    self->wr_object = ob;
    Py_XINCREF(callback);
    self->wr_callback = callback;
    return self;
}

/* This function clears the passed-in reference and removes it from the
 * list of weak references for the referent.  This is the only code that
 * removes an item from the doubly-linked list of weak references for an
 * object; it is also responsible for clearing the callback slot.
 */
static void
clear_weakref(PyWeakReference *self)
{
    if (!self->wr_parent) {
        assert(!self->wr_prev && !self->wr_next);
        return;
    }

    PyWeakReference *root = self->wr_parent;
    assert(root);

    _PyMutex_lock(&root->mutex);

    if (self->wr_prev != NULL)
        self->wr_prev->wr_next = self->wr_next;
    if (self->wr_next != NULL)
        self->wr_next->wr_prev = self->wr_prev;
    self->wr_prev = NULL;
    self->wr_next = NULL;

    _PyMutex_unlock(&root->mutex);

    Py_CLEAR(self->wr_callback);
    Py_CLEAR(self->wr_parent);
}

/* Cyclic gc uses this to *just* clear the passed-in reference, leaving
 * the callback intact and uncalled.  It must be possible to call self's
 * tp_dealloc() after calling this, so self has to be left in a sane enough
 * state for that to work.  We expect tp_dealloc to decref the callback
 * then.  The reason for not letting clear_weakref() decref the callback
 * right now is that if the callback goes away, that may in turn trigger
 * another callback (if a weak reference to the callback exists) -- running
 * arbitrary Python code in the middle of gc is a disaster.  The convolution
 * here allows gc to delay triggering such callbacks until the world is in
 * a sane state again.
 */
void
_PyWeakref_ClearRef(PyWeakReference *self)
{
    self->wr_object = Py_None;
}

static void
weakref_dealloc(PyWeakReference *self)
{
    PyObject_GC_UnTrack(self);
    clear_weakref(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}


static int
gc_traverse(PyWeakReference *self, visitproc visit, void *arg)
{
    Py_VISIT(self->wr_callback);
    return 0;
}


static int
gc_clear(PyWeakReference *self)
{
    clear_weakref(self);
    return 0;
}


static PyObject *
weakref_call(PyWeakReference *self, PyObject *args, PyObject *kw)
{
    static char *kwlist[] = {NULL};

    if (PyArg_ParseTupleAndKeywords(args, kw, ":__call__", kwlist)) {
        return PyWeakref_LockObject((PyObject*)self);
    }
    return NULL;
}


static Py_hash_t
weakref_hash(PyWeakReference *self)
{
    if (self->hash != -1)
        return self->hash;

    PyObject *obj = PyWeakref_LockObject((PyObject*)self);
    if (obj == Py_None) {
        Py_DECREF(obj);
        PyErr_SetString(PyExc_TypeError, "weak object has gone away");
        return -1;
    }

    self->hash = PyObject_Hash(obj);
    Py_DECREF(obj);
    return self->hash;
}


static PyObject *
weakref_repr(PyWeakReference *self)
{
    PyObject *name, *repr;
    _Py_IDENTIFIER(__name__);
    PyObject* obj = PyWeakref_GET_OBJECT(self);

    if (obj == Py_None) {
        return PyUnicode_FromFormat("<weakref at %p; dead>", self);
    }

    Py_INCREF(obj);
    if (_PyObject_LookupAttrId(obj, &PyId___name__, &name) < 0) {
        Py_DECREF(obj);
        return NULL;
    }
    if (name == NULL || !PyUnicode_Check(name)) {
        repr = PyUnicode_FromFormat(
            "<weakref at %p; to '%s' at %p>",
            self,
            Py_TYPE(PyWeakref_GET_OBJECT(self))->tp_name,
            obj);
    }
    else {
        repr = PyUnicode_FromFormat(
            "<weakref at %p; to '%s' at %p (%U)>",
            self,
            Py_TYPE(PyWeakref_GET_OBJECT(self))->tp_name,
            obj,
            name);
    }
    Py_DECREF(obj);
    Py_XDECREF(name);
    return repr;
}

/* Weak references only support equality, not ordering. Two weak references
   are equal if the underlying objects are equal. If the underlying object has
   gone away, they are equal if they are identical. */

static PyObject *
weakref_richcompare(PyWeakReference* self, PyWeakReference* other, int op)
{
    if ((op != Py_EQ && op != Py_NE) ||
        !PyWeakref_Check(self) ||
        !PyWeakref_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (PyWeakref_GET_OBJECT(self) == Py_None
        || PyWeakref_GET_OBJECT(other) == Py_None) {
        int res = (self == other);
        if (op == Py_NE)
            res = !res;
        if (res)
            Py_RETURN_TRUE;
        else
            Py_RETURN_FALSE;
    }
    PyObject* obj = PyWeakref_GET_OBJECT(self);
    PyObject* other_obj = PyWeakref_GET_OBJECT(other);
    Py_INCREF(obj);
    Py_INCREF(other_obj);
    PyObject* res = PyObject_RichCompare(obj, other_obj, op);
    Py_DECREF(obj);
    Py_DECREF(other_obj);
    return res;
}

/* Insert 'newref' in the list before 'next'.  Both must be non-NULL. */
static void
insert_before(PyWeakReference *newref, PyWeakReference *next)
{
    newref->wr_next = next;
    newref->wr_prev = next->wr_prev;
    if (next->wr_prev != NULL)
        next->wr_prev->wr_next = newref;
    next->wr_prev = newref;
}

/* Insert 'newref' in the list after 'prev'.  Both must be non-NULL. */
static void
insert_after(PyWeakReference *newref, PyWeakReference *prev)
{
    newref->wr_prev = prev;
    newref->wr_next = prev->wr_next;
    if (prev->wr_next != NULL)
        prev->wr_next->wr_prev = newref;
    prev->wr_next = newref;
}

static PyWeakReference *
PyWeakref_Root(PyObject *ob)
{
    PyWeakReference **wrptr = GET_WEAKREFS_LISTPTR(ob);

    PyWeakReference *root = _Py_atomic_load_ptr(wrptr);
    if (root) {
        return root;
    }

    root = new_weakref(&_PyWeakref_RefType, ob, NULL);
    if (!root) {
        return NULL;
    }

    if (!_Py_atomic_compare_exchange_ptr(wrptr, NULL, root)) {
        /* Another thread already set the root weakref; use it */
        Py_DECREF(root);
        root = _Py_atomic_load_ptr(wrptr);
        assert(root);
    }

    return root;
}

static PyWeakReference *
PyWeakref_SharedProxy(PyTypeObject *type, PyObject *ob)
{
    PyWeakReference *root;

    root = PyWeakref_Root(ob);
    if (!root) {
        return NULL;
    }

    _PyMutex_lock(&root->mutex);
    PyWeakReference *prev = root->wr_prev;
    if (prev && _Py_TRY_INCREF(prev)) {
        _PyMutex_unlock(&root->mutex);
        return prev;
    }
    _PyMutex_unlock(&root->mutex);

    PyWeakReference *self = new_weakref(type, ob, NULL);
    if (!self) {
        return NULL;
    }

    Py_INCREF(root);
    self->wr_parent = root;

    _PyMutex_lock(&root->mutex);
    insert_before(self, root);
    _PyMutex_unlock(&root->mutex);

    return self;
}

static PyObject *
PyWeakref_NewWithType(PyTypeObject *type, PyObject *ob, PyObject *callback)
{
    if (!PyType_SUPPORTS_WEAKREFS(Py_TYPE(ob))) {
        PyErr_Format(PyExc_TypeError,
                     "cannot create weak reference to '%s' object",
                     Py_TYPE(ob)->tp_name);
        return NULL;
    }

    if (callback == Py_None)
        callback = NULL;

    if (callback == NULL) {
        /* We can re-use an existing reference. */
        if (type == &_PyWeakref_RefType) {
            PyWeakReference *root = PyWeakref_Root(ob);
            Py_XINCREF(root);
            return (PyObject *)root;
        }
        else if (type == &_PyWeakref_ProxyType ||
                 type == &_PyWeakref_CallableProxyType) {
            return (PyObject *)PyWeakref_SharedProxy(type, ob);
        }
    }

    /* We have to create a new reference. */
    PyWeakReference *self = new_weakref(type, ob, callback);
    if (self != NULL) {
        PyWeakReference *root = PyWeakref_Root(ob);
        if (!root) {
            Py_DECREF(self);
            return NULL;
        }

        Py_INCREF(root);
        self->wr_parent = root;

        _PyMutex_lock(&root->mutex);
        insert_after(self, root);
        _PyMutex_unlock(&root->mutex);
    }
    return (PyObject *)self;
}

static int
parse_weakref_init_args(const char *funcname, PyObject *args, PyObject *kwargs,
                        PyObject **obp, PyObject **callbackp)
{
    return PyArg_UnpackTuple(args, funcname, 1, 2, obp, callbackp);
}

static PyObject *
weakref___new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *ob, *callback = NULL;

    if (!parse_weakref_init_args("__new__", args, kwargs, &ob, &callback)) {
        return NULL;
    }

    return PyWeakref_NewWithType(type, ob, callback);
}

static int
weakref___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *tmp;

    if (!_PyArg_NoKeywords("ref", kwargs))
        return -1;

    if (parse_weakref_init_args("__init__", args, kwargs, &tmp, &tmp))
        return 0;
    else
        return -1;
}


static PyMemberDef weakref_members[] = {
    {"__callback__", T_OBJECT, offsetof(PyWeakReference, wr_callback), READONLY},
    {NULL} /* Sentinel */
};

PyTypeObject
_PyWeakref_RefType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "weakref",
    sizeof(PyWeakReference),
    0,
    (destructor)weakref_dealloc,/*tp_dealloc*/
    0,                          /*tp_vectorcall_offset*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_as_async*/
    (reprfunc)weakref_repr,     /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    (hashfunc)weakref_hash,     /*tp_hash*/
    (ternaryfunc)weakref_call,  /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC
        | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    0,                          /*tp_doc*/
    (traverseproc)gc_traverse,  /*tp_traverse*/
    (inquiry)gc_clear,          /*tp_clear*/
    (richcmpfunc)weakref_richcompare,   /*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    0,                          /*tp_methods*/
    weakref_members,            /*tp_members*/
    0,                          /*tp_getset*/
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    weakref___init__,           /*tp_init*/
    PyType_GenericAlloc,        /*tp_alloc*/
    weakref___new__,            /*tp_new*/
    PyObject_GC_Del,            /*tp_free*/
};


static int
proxy_checkref(PyWeakReference *proxy)
{
    if (PyWeakref_GET_OBJECT(proxy) == Py_None) {
        PyErr_SetString(PyExc_ReferenceError,
                        "weakly-referenced object no longer exists");
        return 0;
    }
    return 1;
}


/* If a parameter is a proxy, check that it is still "live" and wrap it,
 * replacing the original value with the raw object.  Raises ReferenceError
 * if the param is a dead proxy.
 */
#define UNWRAP(o) \
        if (PyWeakref_CheckProxy(o)) { \
            if (!proxy_checkref((PyWeakReference *)o)) \
                return NULL; \
            o = PyWeakref_GET_OBJECT(o); \
        }

#define WRAP_UNARY(method, generic) \
    static PyObject * \
    method(PyObject *proxy) { \
        UNWRAP(proxy); \
        Py_INCREF(proxy); \
        PyObject* res = generic(proxy); \
        Py_DECREF(proxy); \
        return res; \
    }

#define WRAP_BINARY(method, generic) \
    static PyObject * \
    method(PyObject *x, PyObject *y) { \
        UNWRAP(x); \
        UNWRAP(y); \
        Py_INCREF(x); \
        Py_INCREF(y); \
        PyObject* res = generic(x, y); \
        Py_DECREF(x); \
        Py_DECREF(y); \
        return res; \
    }

/* Note that the third arg needs to be checked for NULL since the tp_call
 * slot can receive NULL for this arg.
 */
#define WRAP_TERNARY(method, generic) \
    static PyObject * \
    method(PyObject *proxy, PyObject *v, PyObject *w) { \
        UNWRAP(proxy); \
        UNWRAP(v); \
        if (w != NULL) \
            UNWRAP(w); \
        Py_INCREF(proxy); \
        Py_INCREF(v); \
        Py_XINCREF(w); \
        PyObject* res = generic(proxy, v, w); \
        Py_DECREF(proxy); \
        Py_DECREF(v); \
        Py_XDECREF(w); \
        return res; \
    }

#define WRAP_METHOD(method, special) \
    static PyObject * \
    method(PyObject *proxy, PyObject *Py_UNUSED(ignored)) { \
            _Py_IDENTIFIER(special); \
            UNWRAP(proxy); \
            Py_INCREF(proxy); \
            PyObject* res = _PyObject_CallMethodIdNoArgs(proxy, &PyId_##special); \
            Py_DECREF(proxy); \
            return res; \
        }


/* direct slots */

WRAP_BINARY(proxy_getattr, PyObject_GetAttr)
WRAP_UNARY(proxy_str, PyObject_Str)
WRAP_TERNARY(proxy_call, PyObject_Call)

static PyObject *
proxy_repr(PyWeakReference *proxy)
{
    return PyUnicode_FromFormat(
        "<weakproxy at %p to %s at %p>",
        proxy,
        Py_TYPE(PyWeakref_GET_OBJECT(proxy))->tp_name,
        PyWeakref_GET_OBJECT(proxy));
}


static int
proxy_setattr(PyWeakReference *proxy, PyObject *name, PyObject *value)
{
    if (!proxy_checkref(proxy))
        return -1;
    PyObject *obj = PyWeakref_GET_OBJECT(proxy);
    Py_INCREF(obj);
    int res = PyObject_SetAttr(obj, name, value);
    Py_DECREF(obj);
    return res;
}

static PyObject *
proxy_richcompare(PyObject *proxy, PyObject *v, int op)
{
    UNWRAP(proxy);
    UNWRAP(v);
    return PyObject_RichCompare(proxy, v, op);
}

/* number slots */
WRAP_BINARY(proxy_add, PyNumber_Add)
WRAP_BINARY(proxy_sub, PyNumber_Subtract)
WRAP_BINARY(proxy_mul, PyNumber_Multiply)
WRAP_BINARY(proxy_floor_div, PyNumber_FloorDivide)
WRAP_BINARY(proxy_true_div, PyNumber_TrueDivide)
WRAP_BINARY(proxy_mod, PyNumber_Remainder)
WRAP_BINARY(proxy_divmod, PyNumber_Divmod)
WRAP_TERNARY(proxy_pow, PyNumber_Power)
WRAP_UNARY(proxy_neg, PyNumber_Negative)
WRAP_UNARY(proxy_pos, PyNumber_Positive)
WRAP_UNARY(proxy_abs, PyNumber_Absolute)
WRAP_UNARY(proxy_invert, PyNumber_Invert)
WRAP_BINARY(proxy_lshift, PyNumber_Lshift)
WRAP_BINARY(proxy_rshift, PyNumber_Rshift)
WRAP_BINARY(proxy_and, PyNumber_And)
WRAP_BINARY(proxy_xor, PyNumber_Xor)
WRAP_BINARY(proxy_or, PyNumber_Or)
WRAP_UNARY(proxy_int, PyNumber_Long)
WRAP_UNARY(proxy_float, PyNumber_Float)
WRAP_BINARY(proxy_iadd, PyNumber_InPlaceAdd)
WRAP_BINARY(proxy_isub, PyNumber_InPlaceSubtract)
WRAP_BINARY(proxy_imul, PyNumber_InPlaceMultiply)
WRAP_BINARY(proxy_ifloor_div, PyNumber_InPlaceFloorDivide)
WRAP_BINARY(proxy_itrue_div, PyNumber_InPlaceTrueDivide)
WRAP_BINARY(proxy_imod, PyNumber_InPlaceRemainder)
WRAP_TERNARY(proxy_ipow, PyNumber_InPlacePower)
WRAP_BINARY(proxy_ilshift, PyNumber_InPlaceLshift)
WRAP_BINARY(proxy_irshift, PyNumber_InPlaceRshift)
WRAP_BINARY(proxy_iand, PyNumber_InPlaceAnd)
WRAP_BINARY(proxy_ixor, PyNumber_InPlaceXor)
WRAP_BINARY(proxy_ior, PyNumber_InPlaceOr)
WRAP_UNARY(proxy_index, PyNumber_Index)
WRAP_BINARY(proxy_matmul, PyNumber_MatrixMultiply)
WRAP_BINARY(proxy_imatmul, PyNumber_InPlaceMatrixMultiply)

static int
proxy_bool(PyWeakReference *proxy)
{
    PyObject *o = PyWeakref_GET_OBJECT(proxy);
    if (!proxy_checkref(proxy)) {
        return -1;
    }
    Py_INCREF(o);
    int res = PyObject_IsTrue(o);
    Py_DECREF(o);
    return res;
}

static void
proxy_dealloc(PyWeakReference *self)
{
    if (self->wr_callback != NULL)
        PyObject_GC_UnTrack((PyObject *)self);
    clear_weakref(self);
    PyObject_GC_Del(self);
}

/* sequence slots */

static int
proxy_contains(PyWeakReference *proxy, PyObject *value)
{
    if (!proxy_checkref(proxy))
        return -1;

    PyObject *obj = PyWeakref_GET_OBJECT(proxy);
    Py_INCREF(obj);
    int res = PySequence_Contains(obj, value);
    Py_DECREF(obj);
    return res;
}

/* mapping slots */

static Py_ssize_t
proxy_length(PyWeakReference *proxy)
{
    if (!proxy_checkref(proxy))
        return -1;

    PyObject *obj = PyWeakref_GET_OBJECT(proxy);
    Py_INCREF(obj);
    Py_ssize_t res = PyObject_Length(obj);
    Py_DECREF(obj);
    return res;
}

WRAP_BINARY(proxy_getitem, PyObject_GetItem)

static int
proxy_setitem(PyWeakReference *proxy, PyObject *key, PyObject *value)
{
    if (!proxy_checkref(proxy))
        return -1;

    PyObject *obj = PyWeakref_GET_OBJECT(proxy);
    Py_INCREF(obj);
    int res;
    if (value == NULL) {
        res = PyObject_DelItem(obj, key);
    } else {
        res = PyObject_SetItem(obj, key, value);
    }
    Py_DECREF(obj);
    return res;
}

/* iterator slots */

static PyObject *
proxy_iter(PyWeakReference *proxy)
{
    if (!proxy_checkref(proxy))
        return NULL;
    PyObject *obj = PyWeakref_GET_OBJECT(proxy);
    Py_INCREF(obj);
    PyObject* res = PyObject_GetIter(obj);
    Py_DECREF(obj);
    return res;
}

static PyObject *
proxy_iternext(PyWeakReference *proxy)
{
    if (!proxy_checkref(proxy))
        return NULL;

    PyObject *obj = PyWeakref_GET_OBJECT(proxy);
    Py_INCREF(obj);
    PyObject* res = PyIter_Next(obj);
    Py_DECREF(obj);
    return res;
}


WRAP_METHOD(proxy_bytes, __bytes__)


static PyMethodDef proxy_methods[] = {
        {"__bytes__", proxy_bytes, METH_NOARGS},
        {NULL, NULL}
};


static PyNumberMethods proxy_as_number = {
    proxy_add,              /*nb_add*/
    proxy_sub,              /*nb_subtract*/
    proxy_mul,              /*nb_multiply*/
    proxy_mod,              /*nb_remainder*/
    proxy_divmod,           /*nb_divmod*/
    proxy_pow,              /*nb_power*/
    proxy_neg,              /*nb_negative*/
    proxy_pos,              /*nb_positive*/
    proxy_abs,              /*nb_absolute*/
    (inquiry)proxy_bool,    /*nb_bool*/
    proxy_invert,           /*nb_invert*/
    proxy_lshift,           /*nb_lshift*/
    proxy_rshift,           /*nb_rshift*/
    proxy_and,              /*nb_and*/
    proxy_xor,              /*nb_xor*/
    proxy_or,               /*nb_or*/
    proxy_int,              /*nb_int*/
    0,                      /*nb_reserved*/
    proxy_float,            /*nb_float*/
    proxy_iadd,             /*nb_inplace_add*/
    proxy_isub,             /*nb_inplace_subtract*/
    proxy_imul,             /*nb_inplace_multiply*/
    proxy_imod,             /*nb_inplace_remainder*/
    proxy_ipow,             /*nb_inplace_power*/
    proxy_ilshift,          /*nb_inplace_lshift*/
    proxy_irshift,          /*nb_inplace_rshift*/
    proxy_iand,             /*nb_inplace_and*/
    proxy_ixor,             /*nb_inplace_xor*/
    proxy_ior,              /*nb_inplace_or*/
    proxy_floor_div,        /*nb_floor_divide*/
    proxy_true_div,         /*nb_true_divide*/
    proxy_ifloor_div,       /*nb_inplace_floor_divide*/
    proxy_itrue_div,        /*nb_inplace_true_divide*/
    proxy_index,            /*nb_index*/
    proxy_matmul,           /*nb_matrix_multiply*/
    proxy_imatmul,          /*nb_inplace_matrix_multiply*/
};

static PySequenceMethods proxy_as_sequence = {
    (lenfunc)proxy_length,      /*sq_length*/
    0,                          /*sq_concat*/
    0,                          /*sq_repeat*/
    0,                          /*sq_item*/
    0,                          /*sq_slice*/
    0,                          /*sq_ass_item*/
    0,                           /*sq_ass_slice*/
    (objobjproc)proxy_contains, /* sq_contains */
};

static PyMappingMethods proxy_as_mapping = {
    (lenfunc)proxy_length,        /*mp_length*/
    proxy_getitem,                /*mp_subscript*/
    (objobjargproc)proxy_setitem, /*mp_ass_subscript*/
};


PyTypeObject
_PyWeakref_ProxyType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "weakproxy",
    sizeof(PyWeakReference),
    0,
    /* methods */
    (destructor)proxy_dealloc,          /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    (reprfunc)proxy_repr,               /* tp_repr */
    &proxy_as_number,                   /* tp_as_number */
    &proxy_as_sequence,                 /* tp_as_sequence */
    &proxy_as_mapping,                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    0,                                  /* tp_call */
    proxy_str,                          /* tp_str */
    proxy_getattr,                      /* tp_getattro */
    (setattrofunc)proxy_setattr,        /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                  /* tp_doc */
    (traverseproc)gc_traverse,          /* tp_traverse */
    (inquiry)gc_clear,                  /* tp_clear */
    proxy_richcompare,                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    (getiterfunc)proxy_iter,            /* tp_iter */
    (iternextfunc)proxy_iternext,       /* tp_iternext */
        proxy_methods,                      /* tp_methods */
};


PyTypeObject
_PyWeakref_CallableProxyType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "weakcallableproxy",
    sizeof(PyWeakReference),
    0,
    /* methods */
    (destructor)proxy_dealloc,          /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    (unaryfunc)proxy_repr,              /* tp_repr */
    &proxy_as_number,                   /* tp_as_number */
    &proxy_as_sequence,                 /* tp_as_sequence */
    &proxy_as_mapping,                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    proxy_call,                         /* tp_call */
    proxy_str,                          /* tp_str */
    proxy_getattr,                      /* tp_getattro */
    (setattrofunc)proxy_setattr,        /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                  /* tp_doc */
    (traverseproc)gc_traverse,          /* tp_traverse */
    (inquiry)gc_clear,                  /* tp_clear */
    proxy_richcompare,                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    (getiterfunc)proxy_iter,            /* tp_iter */
    (iternextfunc)proxy_iternext,       /* tp_iternext */
};



PyObject *
PyWeakref_NewRef(PyObject *ob, PyObject *callback)
{
    return PyWeakref_NewWithType(&_PyWeakref_RefType, ob, callback);
}


PyObject *
PyWeakref_NewProxy(PyObject *ob, PyObject *callback)
{
    PyTypeObject *type;
    if (PyCallable_Check(ob))
        type = &_PyWeakref_CallableProxyType;
    else
        type = &_PyWeakref_ProxyType;


    return PyWeakref_NewWithType(type, ob, callback);
}


PyObject *
PyWeakref_GetObject(PyObject *ref)
{
    PyObject *obj = PyWeakref_LockObject(ref);
    Py_XDECREF(obj);
    return obj;
}


PyObject *
PyWeakref_LockObject(PyObject *ref)
{
    if (ref == NULL || !PyWeakref_Check(ref)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    PyWeakReference *wr = (PyWeakReference *)ref;
    PyWeakReference *root = wr->wr_parent ? wr->wr_parent : wr;

    _PyMutex_lock(&root->mutex);
    PyObject *obj = root->wr_object;
    if (!_Py_TRY_INCREF(obj)) {
        obj = Py_None;
    }
    _PyMutex_unlock(&root->mutex);

    return obj;
}

/* Note that there's an inlined copy-paste of handle_callback() in gcmodule.c's
 * handle_weakrefs().
 */
static void
handle_callback(PyWeakReference *ref, PyObject *callback)
{
    PyObject *cbresult = _PyObject_CallOneArg(callback, (PyObject *)ref);

    if (cbresult == NULL)
        PyErr_WriteUnraisable(callback);
    else
        Py_DECREF(cbresult);
}

static Py_ssize_t
_PyWeakref_DetachRefs(PyWeakReference *head, PyWeakReference *list[], Py_ssize_t max)
{
    Py_ssize_t count = 0;

    PyWeakReference *current = head->wr_next;
    while (current != NULL && count < max) {
        // TODO: race on is referenced????
        PyWeakReference *next = current->wr_next;

        if (_Py_TRY_INCREF(current)) {
            list[count] = current;
            ++count;
        }

        // TODO(sgross): should this be in hte if block above?
        current->wr_next = NULL;
        current->wr_prev = NULL;

        current = next;
    }

    head->wr_next = current;
    if (current) {
        current->wr_prev = head;
    }

    return count;
}

void
_PyObject_ClearWeakRefsFromGC(PyObject *object)
{
    PyWeakReference **wrptr = GET_WEAKREFS_LISTPTR(object);

    PyWeakReference *root = *wrptr;
    if (!root)
        return;

    *wrptr = NULL;

    _PyMutex_lock(&root->mutex);
    root->wr_object = Py_None;
    _PyMutex_unlock(&root->mutex);

    Py_DECREF(root);
}

/* This function is called by the tp_dealloc handler to clear weak references.
 *
 * This iterates through the weak references for 'object' and calls callbacks
 * for those references which have one.  It returns when all callbacks have
 * been attempted.
 *
 * Thread safety note: no other threads may create weak references to this
 * object concurrent with this function. However, they may destroy weak
 * references concurrently.
 */
void
PyObject_ClearWeakRefs(PyObject *object)
{
    if (object == NULL
        || !PyType_SUPPORTS_WEAKREFS(Py_TYPE(object))
        || Py_REFCNT(object) != 0)
    {
        PyErr_BadInternalCall();
        return;
    }

    PyWeakReference **wrptr = GET_WEAKREFS_LISTPTR(object);

    PyWeakReference *root = *wrptr;
    if (!root)
        return;

    *wrptr = NULL;

    assert(root->wr_object == object);
    assert(root->wr_parent == NULL);

    _PyMutex_lock(&root->mutex);
    root->wr_object = Py_None;
    int has_refs = root->wr_next != NULL;
    _PyMutex_unlock(&root->mutex);

    if (!has_refs) {
        Py_DECREF(root);
        return;
    }

    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);

    PyWeakReference *list[16];
    while (has_refs) {
        _PyMutex_lock(&root->mutex);
        Py_ssize_t count = _PyWeakref_DetachRefs(root, list, 16);
        has_refs = root->wr_next != NULL;
        _PyMutex_unlock(&root->mutex);

        for (Py_ssize_t i = 0; i < count; i++) {
            PyWeakReference *ref = list[i];
            if (ref->wr_callback && ref->wr_object != Py_None) {
                handle_callback(ref, ref->wr_callback);
            }
            Py_CLEAR(ref->wr_callback);
            Py_DECREF(ref);
        }
    }

    Py_DECREF(root);

    assert(!PyErr_Occurred());
    PyErr_Restore(type, value, traceback);
}
