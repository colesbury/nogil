#include "Python.h"
#include "pycore_object.h"        // _PyObject_GET_WEAKREFS_CONTROLPTR()
#include "structmember.h"         // PyMemberDef
#include "pycore_refcnt.h"
#include "pyatomic.h"


typedef struct _PyWeakrefBase PyWeakrefBase;

extern PyTypeObject _PyWeakref_ControlType;

Py_ssize_t
_PyWeakref_GetWeakrefCount(PyWeakrefControl *ctrl)
{
    if (ctrl == NULL) {
        return 0;
    }

    PyWeakrefBase *head = &ctrl->base;
    PyWeakrefBase *ref;
    Py_ssize_t count = 0;

    ref = head->wr_next;
    while (ref != head) {
        ++count;
        ref = ref->wr_next;
    }

    return count;
}

static PyObject *weakref_vectorcall(PyWeakReference *self, PyObject *const *args, size_t nargsf, PyObject *kwnames);

static PyWeakReference *
new_weakref(PyTypeObject *type, PyWeakrefControl *root, PyObject *callback)
{
    PyWeakReference *self = (PyWeakReference *)type->tp_alloc(type, 0);
    if (!self) {
        return NULL;
    }
    _PyObject_SetMaybeWeakref((PyObject *)self);
    self->hash = -1;
    self->base.wr_prev = NULL;
    self->base.wr_next = NULL;
    self->vectorcall = (vectorcallfunc)weakref_vectorcall;
    self->wr_parent = (PyWeakrefControl *)Py_NewRef(root);
    self->wr_callback = Py_XNewRef(callback);
    return self;
}

/* Removes 'ref' from the list of weak references. */
static void
remove_weakref(PyWeakrefBase *ref)
{
    PyWeakrefBase *prev = ref->wr_prev;
    if (prev != NULL) {
        PyWeakrefBase *next = ref->wr_next;
        prev->wr_next = next;
        next->wr_prev = prev;
    }
    ref->wr_prev = ref->wr_next = NULL;
}

/* The _PyWeakref_DetachRef function clears the passed-in reference and
 * removes it from the list of weak references for the referent.
 *
 * Cyclic gc uses this to *just* detach the passed-in reference, leaving
 * the callback intact and uncalled.  It must be possible to call self's
 * tp_dealloc() after calling this, so self has to be left in a sane enough
 * state for that to work.  We expect tp_dealloc to decref the callback
 * then.  The reason for not letting this function decref the callback
 * right now is that if the callback goes away, that may in turn trigger
 * another callback (if a weak reference to the callback exists) -- running
 * arbitrary Python code in the middle of gc is a disaster.  The convolution
 * here allows gc to delay triggering such callbacks until the world is in
 * a sane state again.
 */
void
_PyWeakref_DetachRef(PyWeakReference *ref)
{
    PyWeakrefControl *ctrl = ref->wr_parent;
    if (ctrl != NULL) {
        remove_weakref(&ref->base);
        ref->wr_parent = NULL;
        Py_DECREF(ctrl);
    }
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
    Py_CLEAR(self->wr_callback);
    return 0;
}

static void
weakref_lock(PyWeakrefControl *ctrl)
{
    _PyMutex_lock(&_PyObject_CAST(ctrl)->ob_mutex);
}

static void
weakref_unlock(PyWeakrefControl *ctrl)
{
    _PyMutex_unlock(&_PyObject_CAST(ctrl)->ob_mutex);
}

static void
weakref_dealloc(PyWeakReference *self)
{
    PyObject_GC_UnTrack(self);
    gc_clear(self);
    if (self->wr_parent) {
        PyWeakrefControl *ctrl = self->wr_parent;

        weakref_lock(ctrl);
        remove_weakref(&self->base);
        weakref_unlock(ctrl);

        Py_CLEAR(self->wr_parent);
    }
    Py_TYPE(self)->tp_free(self);
}


static PyObject *
weakref_vectorcall(PyWeakReference *self, PyObject *const *args,
                   size_t nargsf, PyObject *kwnames)
{
    if (!_PyArg_NoKwnames("weakref", kwnames)) {
        return NULL;
    }
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (!_PyArg_CheckPositional("weakref", nargs, 0, 0)) {
        return NULL;
    }
    return PyWeakref_FetchObject((PyObject *)self);
}

static Py_hash_t
weakref_hash(PyWeakReference *self)
{
    if (self->hash != -1)
        return self->hash;

    PyObject *obj = PyWeakref_FetchObject((PyObject *)self);
    if (obj == Py_None) {
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
    PyObject* obj = PyWeakref_FetchObject((PyObject *)self);

    if (obj == Py_None) {
        return PyUnicode_FromFormat("<weakref at %p; dead>", self);
    }

    if (_PyObject_LookupAttr(obj, &_Py_ID(__name__), &name) < 0) {
        Py_DECREF(obj);
        return NULL;
    }
    if (name == NULL || !PyUnicode_Check(name)) {
        repr = PyUnicode_FromFormat(
            "<weakref at %p; to '%s' at %p>",
            self,
            Py_TYPE(obj)->tp_name,
            obj);
    }
    else {
        repr = PyUnicode_FromFormat(
            "<weakref at %p; to '%s' at %p (%U)>",
            self,
            Py_TYPE(obj)->tp_name,
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
    PyObject* obj = PyWeakref_FetchObject((PyObject *)self);
    PyObject* other_obj = PyWeakref_FetchObject((PyObject *)other);
    if (obj == Py_None || other_obj == Py_None) {
        int res = (self == other);
        if (op == Py_NE)
            res = !res;
        Py_DECREF(obj);
        Py_DECREF(other_obj);
        return res ? Py_True : Py_False;
    }
    PyObject* res = PyObject_RichCompare(obj, other_obj, op);
    Py_DECREF(obj);
    Py_DECREF(other_obj);
    return res;
}

/* Insert 'newref' in the list before 'next'.  Both must be non-NULL. */
static void
insert_before(PyWeakrefBase *newref, PyWeakrefBase *next)
{
    newref->wr_next = next;
    newref->wr_prev = next->wr_prev;
    next->wr_prev->wr_next = newref;
    next->wr_prev = newref;
}

/* Insert 'newref' in the list after 'prev'.  Both must be non-NULL. */
static void
insert_after(PyWeakrefBase *newref, PyWeakrefBase *prev)
{
    newref->wr_prev = prev;
    newref->wr_next = prev->wr_next;
    prev->wr_next->wr_prev = newref;
    prev->wr_next = newref;
}

static PyWeakrefControl *
PyWeakref_Control(PyObject *ob)
{
    PyWeakrefControl **wrptr = _PyObject_GET_WEAKREFS_CONTROLPTR(ob);

    PyWeakrefControl *ctrl = _Py_atomic_load_ptr(wrptr);
    if (ctrl != NULL) {
        return ctrl;
    }

    ctrl = PyObject_New(PyWeakrefControl, &_PyWeakref_ControlType);
    if (ctrl == NULL) {
        return NULL;
    }
    ctrl->wr_object = ob;
    _PyObject_SetMaybeWeakref(ob);

    PyWeakrefBase *base = &ctrl->base;
    base->wr_prev = base->wr_next = base;

    if (!_Py_atomic_compare_exchange_ptr(wrptr, NULL, ctrl)) {
        /* Another thread already set the ctrl weakref; use it */
        Py_DECREF(ctrl);
        ctrl = _Py_atomic_load_ptr(wrptr);
        assert(ctrl != NULL);
    }

    return ctrl;
}

static int
try_incref(PyObject *op)
{
    return _Py_TryIncrefFast(op) || _Py_TryIncRefShared(op);
}

static PyWeakReference *
weakref_matching(PyWeakrefControl *ctrl, PyTypeObject *type)
{
    assert(_PyMutex_is_locked(&_PyObject_CAST(ctrl)->ob_mutex));
    PyWeakrefBase *wr = ctrl->base.wr_prev;
    int i = 0;
    while (wr != &ctrl->base && i < 2) {
        PyWeakReference *ref = (PyWeakReference *)wr;
        if (Py_TYPE(ref) == type && ref->wr_callback == NULL) {
            if (try_incref((PyObject *)ref)) {
                return ref;
            }
        }
        wr = wr->wr_prev;
        i++;
    }
    return NULL;
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

    PyWeakrefControl *root = PyWeakref_Control(ob);
    if (root == NULL) {
        return NULL;
    }

    int can_reuse = (callback == NULL &&
                     (type == &_PyWeakref_RefType ||
                      type == &_PyWeakref_ProxyType ||
                      type == &_PyWeakref_CallableProxyType));

    if (can_reuse) {
        /* We can re-use an existing reference. */
        PyWeakReference *wr;
        weakref_lock(root);
        wr = weakref_matching(root, type);
        weakref_unlock(root);

        if (wr != NULL) {
            return (PyObject *)wr;
        }
    }

    /* We have to create a new reference. */
    PyWeakReference *self = new_weakref(type, root, callback);
    if (self == NULL) {
        return NULL;
    }

    weakref_lock(root);
    if (can_reuse) {
        insert_before(&self->base, &root->base);
    }
    else {
        insert_after(&self->base, &root->base);
    }
    weakref_unlock(root);
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

PyTypeObject
_PyWeakref_ControlType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "weakref_control",
    .tp_dealloc = (destructor)PyObject_Del,
    .tp_basicsize = sizeof(PyWeakrefControl),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_alloc = PyType_GenericAlloc,
    .tp_free = PyObject_Del
};


static PyMemberDef weakref_members[] = {
    {"__callback__", T_OBJECT, offsetof(PyWeakReference, wr_callback), READONLY},
    {NULL} /* Sentinel */
};

static PyMethodDef weakref_methods[] = {
    {"__class_getitem__",    Py_GenericAlias,
    METH_O|METH_CLASS,       PyDoc_STR("See PEP 585")},
    {NULL} /* Sentinel */
};

PyTypeObject
_PyWeakref_RefType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "weakref.ReferenceType",
    .tp_basicsize = sizeof(PyWeakReference),
    .tp_dealloc = (destructor)weakref_dealloc,
    .tp_vectorcall_offset = offsetof(PyWeakReference, vectorcall),
    .tp_call = PyVectorcall_Call,
    .tp_repr = (reprfunc)weakref_repr,
    .tp_hash = (hashfunc)weakref_hash,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_BASETYPE,
    .tp_traverse = (traverseproc)gc_traverse,
    .tp_clear = (inquiry)gc_clear,
    .tp_richcompare = (richcmpfunc)weakref_richcompare,
    .tp_methods = weakref_methods,
    .tp_members = weakref_members,
    .tp_init = weakref___init__,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = weakref___new__,
    .tp_free = PyObject_GC_Del,
};


static PyObject *
dead_proxy_error(void)
{
    PyErr_SetString(PyExc_ReferenceError,
                    "weakly-referenced object no longer exists");
    return NULL;
}

#define UNWRAP(o, ...) \
    do { \
        if (!PyWeakref_Check(o)) { \
            Py_INCREF(o); \
            break; \
        } \
        o = PyWeakref_FetchObject(o); \
        if (o == Py_None) { \
            __VA_ARGS__; \
            return dead_proxy_error(); \
        } \
    } while (0)

#define WRAP_UNARY(method, generic) \
    static PyObject * \
    method(PyObject *proxy) { \
        UNWRAP(proxy); \
        PyObject* res = generic(proxy); \
        Py_DECREF(proxy); \
        return res; \
    }

#define WRAP_BINARY(method, generic) \
    static PyObject * \
    method(PyObject *x, PyObject *y) { \
        UNWRAP(x); \
        UNWRAP(y, Py_DECREF(x)); \
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
        UNWRAP(v, Py_DECREF(proxy)); \
        if (w != NULL) { \
            UNWRAP(w, Py_DECREF(proxy), Py_DECREF(v)); \
        } \
        PyObject* res = generic(proxy, v, w); \
        Py_DECREF(proxy); \
        Py_DECREF(v); \
        Py_XDECREF(w); \
        return res; \
    }

#define WRAP_METHOD(method, SPECIAL) \
    static PyObject * \
    method(PyObject *proxy, PyObject *Py_UNUSED(ignored)) { \
            assert(PyWeakref_Check(proxy)); \
            proxy = PyWeakref_FetchObject(proxy); \
            if (proxy == Py_None) return dead_proxy_error(); \
            PyObject* res = PyObject_CallMethodNoArgs(proxy, &_Py_ID(SPECIAL)); \
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
    PyObject *obj, *repr;

    obj = PyWeakref_FetchObject((PyObject *)proxy);
    repr = PyUnicode_FromFormat(
        "<weakproxy at %p to %s at %p>",
        proxy,
        Py_TYPE(obj)->tp_name,
        obj);

    Py_DECREF(obj);
    return repr;
}

static int
proxy_setattr(PyObject *proxy, PyObject *name, PyObject *value)
{
    PyObject *obj = PyWeakref_FetchObject(proxy);
    if (obj == Py_None) {
        return dead_proxy_error(), -1;
    }
    int res = PyObject_SetAttr(obj, name, value);
    Py_DECREF(obj);
    return res;
}

static PyObject *
proxy_richcompare(PyObject *proxy, PyObject *v, int op)
{
    proxy = PyWeakref_FetchObject(proxy);
    if (proxy == Py_None) {
        return dead_proxy_error();
    }
    UNWRAP(v, Py_DECREF(proxy));
    PyObject *ret = PyObject_RichCompare(proxy, v, op);
    Py_DECREF(proxy);
    Py_DECREF(v);
    return ret;
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
proxy_bool(PyObject *proxy)
{
    PyObject *obj = PyWeakref_FetchObject(proxy);
    if (obj == Py_None) {
        return dead_proxy_error(), -1;
    }
    int res = PyObject_IsTrue(obj);
    Py_DECREF(obj);
    return res;
}

/* sequence slots */

static int
proxy_contains(PyObject *proxy, PyObject *value)
{
    PyObject *obj = PyWeakref_FetchObject(proxy);
    if (obj == Py_None) {
        return dead_proxy_error(), -1;
    }
    int res = PySequence_Contains(obj, value);
    Py_DECREF(obj);
    return res;
}

/* mapping slots */

static Py_ssize_t
proxy_length(PyObject *proxy)
{
    PyObject *obj = PyWeakref_FetchObject(proxy);
    if (obj == Py_None) {
        return dead_proxy_error(), -1;
    }
    Py_ssize_t res = PyObject_Length(obj);
    Py_DECREF(obj);
    return res;
}

WRAP_BINARY(proxy_getitem, PyObject_GetItem)

static int
proxy_setitem(PyObject *proxy, PyObject *key, PyObject *value)
{
    PyObject *obj = PyWeakref_FetchObject(proxy);
    if (obj == Py_None) {
        return dead_proxy_error(), -1;
    }
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
proxy_iter(PyObject *proxy)
{
    PyObject *obj = PyWeakref_FetchObject(proxy);
    if (obj == Py_None) {
        return dead_proxy_error();
    }
    PyObject* res = PyObject_GetIter(obj);
    Py_DECREF(obj);
    return res;
}

static PyObject *
proxy_iternext(PyObject *proxy)
{
    PyObject *obj = PyWeakref_FetchObject(proxy);
    if (obj == Py_None) {
        return dead_proxy_error();
    }
    if (!PyIter_Check(obj)) {
        PyErr_Format(PyExc_TypeError,
            "Weakref proxy referenced a non-iterator '%.200s' object",
            Py_TYPE(obj)->tp_name);
        Py_DECREF(obj);
        return NULL;
    }
    PyObject* res = PyIter_Next(obj);
    Py_DECREF(obj);
    return res;
}


WRAP_METHOD(proxy_bytes, __bytes__)
WRAP_METHOD(proxy_reversed, __reversed__)


static PyMethodDef proxy_methods[] = {
        {"__bytes__", proxy_bytes, METH_NOARGS},
        {"__reversed__", proxy_reversed, METH_NOARGS},
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
    "weakref.ProxyType",
    sizeof(PyWeakReference),
    0,
    /* methods */
    (destructor)weakref_dealloc,        /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    (reprfunc)proxy_repr,               /* tp_repr */
    &proxy_as_number,                   /* tp_as_number */
    &proxy_as_sequence,                 /* tp_as_sequence */
    &proxy_as_mapping,                  /* tp_as_mapping */
// Notice that tp_hash is intentionally omitted as proxies are "mutable" (when the reference dies).
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
    "weakref.CallableProxyType",
    sizeof(PyWeakReference),
    0,
    /* methods */
    (destructor)weakref_dealloc,        /* tp_dealloc */
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
    PyObject *obj = PyWeakref_FetchObject(ref);
    Py_XDECREF(obj);
    return obj;
}


PyObject *
PyWeakref_FetchObject(PyObject *ref)
{
    if (ref == NULL || !PyWeakref_Check(ref)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    PyWeakReference *wr = (PyWeakReference *)ref;
    if (wr->wr_parent == NULL) {
        return Py_None;
    }

    PyWeakrefControl *ctrl = wr->wr_parent;
    weakref_lock(ctrl);
    PyObject *obj = ctrl->wr_object;
    assert((_PyObject_IS_IMMORTAL(obj) || obj->ob_ref_shared & _Py_REF_SHARED_FLAG_MASK) != 0);
    if (!try_incref(obj)) {
        obj = Py_None;
    }
    weakref_unlock(ctrl);

    return obj;
}

/* Note that there's an inlined copy-paste of handle_callback() in gcmodule.c's
 * handle_weakrefs().
 */
static void
handle_callback(PyWeakReference *ref, PyObject *callback)
{
    PyObject *cbresult = PyObject_CallOneArg(callback, (PyObject *)ref);

    if (cbresult == NULL)
        PyErr_WriteUnraisable(callback);
    else
        Py_DECREF(cbresult);
}

static Py_ssize_t
_PyWeakref_DetachRefs(PyWeakrefControl *ctrl, PyWeakReference *list[], Py_ssize_t max)
{
    Py_ssize_t count = 0;

    PyWeakrefBase *head = &ctrl->base;
    PyWeakrefBase *current = head->wr_next;
    while (current != head && count < max) {
        PyWeakrefBase *next = current->wr_next;

        if (try_incref((PyObject *)current)) {
            list[count] = (PyWeakReference *)current;
            ++count;
        }

        current->wr_next = NULL;
        current->wr_prev = NULL;

        current = next;
    }

    head->wr_next = current;
    current->wr_prev = head;

    return count;
}

// Clears weakrefs without calling callabacks. Called from subtype_dealloc.
void
_PyObject_ClearWeakRefsFromDealloc(PyObject *object)
{
    PyWeakrefControl **wrptr = _PyObject_GET_WEAKREFS_CONTROLPTR(object);
    PyWeakrefControl *root = *wrptr;
    if (!root) {
        return;
    }

    *wrptr = NULL;
    weakref_lock(root);
    root->wr_object = Py_None;
    weakref_unlock(root);
    Py_DECREF(root);
}

// Clears weakrefs without calling callabacks or acquiring locks. Called
// during stop-the-world garbage collection.
void
_PyObject_ClearWeakRefsFromGC(PyObject *object)
{
    PyWeakrefControl **wrptr = _PyObject_GET_WEAKREFS_CONTROLPTR(object);
    PyWeakrefControl *root = *wrptr;
    if (!root) {
        return;
    }

    assert(_PyRuntime.stop_the_world && "should only be called during GC");
    *wrptr = NULL;
    root->wr_object = Py_None;
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
    if (object == NULL) {
        fprintf(stderr, "PyObject_ClearWeakRefs called with NULL\n");
        PyErr_BadInternalCall();
        return;
    }
    uint32_t ob_ref_local = object->ob_ref_local;
    Py_ssize_t ob_ref_shared = object->ob_ref_shared;
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(object))) {
        fprintf(stderr, "PyObject_ClearWeakRefs called on object without weakrefs\n");
        PyErr_BadInternalCall();
        return;
    }
    if (Py_REFCNT(object) != 0) {
        Py_ssize_t refcnt = Py_REFCNT(object);
        fprintf(stderr, "PyObject_ClearWeakRefs called on object with refcnt != 0 (ob_ref_local=%u ob_ref_shared=%zd refcnt=%zd)\n", ob_ref_local, ob_ref_shared, refcnt);
        PyErr_BadInternalCall();
        return;
    }
    if (object == NULL
        || !_PyType_SUPPORTS_WEAKREFS(Py_TYPE(object))
        || Py_REFCNT(object) != 0)
    {
        PyErr_BadInternalCall();
        return;
    }

    PyWeakrefControl **wrptr = _PyObject_GET_WEAKREFS_CONTROLPTR(object);

    PyWeakrefControl *root = *wrptr;
    if (!root)
        return;

    *wrptr = NULL;

    assert(root->wr_object == object);

    int make_callbacks;

    weakref_lock(root);
    make_callbacks = (root->wr_object != Py_None);
    root->wr_object = Py_None;
    int has_refs = root->base.wr_next != &root->base;
    weakref_unlock(root);

    if (!has_refs) {
        Py_DECREF(root);
        return;
    }

    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);

    PyWeakReference *list[16];
    while (has_refs) {
        weakref_lock(root);
        Py_ssize_t count = _PyWeakref_DetachRefs(root, list, 16);
        has_refs = root->base.wr_next != &root->base;
        weakref_unlock(root);

        for (Py_ssize_t i = 0; i < count; i++) {
            PyWeakReference *ref = list[i];
            if (ref->wr_callback && make_callbacks) {
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

/* This function is called by _PyStaticType_Dealloc() to clear weak references.
 *
 * This is called at the end of runtime finalization, so we can just
 * wipe out the type's weaklist.  We don't bother with callbacks
 * or anything else.
 */
void
_PyStaticType_ClearWeakRefs(PyTypeObject *type)
{
    PyWeakrefControl **ptr = _PyObject_GET_WEAKREFS_CONTROLPTR((PyObject *)type);
    PyWeakrefBase *head = (PyWeakrefBase *)(*ptr);
    if (head == NULL) {
        return;
    }
    while (head->wr_next != head) {
        PyWeakReference *ref = (PyWeakReference *)head->wr_next;
        _PyWeakref_DetachRef(ref);
    }
    Py_CLEAR(*ptr);
}
