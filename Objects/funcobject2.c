
/* Function object implementation */

#include "Python.h"
#include "pycore_object.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"
#include "code.h"
#include "code2.h"
#include "structmember.h"


#include "ceval2_meta.h"
#include "opcode2.h"


PyObject *
PyFunc_New(PyObject *co, PyObject *globals)
{
    assert(PyCode2_Check(co));
    PyCodeObject2 *code = (PyCodeObject2 *)co;
    PyFunc *func = PyObject_GC_NewVar(PyFunc, &PyFunc_Type, code->co_nfreevars);
    if (func == NULL) {
        return NULL;
    }
    if ((code->co_flags & CO_NESTED) == 0) {
        _PyObject_SET_DEFERRED_RC((PyObject *)func);
    }
    Py_INCREF(code);        // TODO: deferred rc for code
    func->func_base.first_instr = PyCode2_GET_CODE(code);
    Py_INCREF(globals);
    func->globals = globals;
    func->func_doc = NULL;
    func->func_name = code->co_name;
    Py_INCREF(func->func_name);
    func->func_dict = NULL;
    func->func_weakreflist = NULL;
    func->func_module = NULL;
    func->func_annotations = NULL;
    func->func_qualname = func->func_name;
    Py_INCREF(func->func_qualname);

    _PyObject_GC_TRACK(func);
    return (PyObject *)func;
}

/*[clinic input]
class function "PyFunc *" "&PyFunc_Type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=1fb9802b206a6601]*/

#include "clinic/funcobject2.c.h"

/* function.__new__() maintains the following invariants for closures.
   The closure must correspond to the free variables of the code object.

   if len(code.co_freevars) == 0:
       closure = NULL
   else:
       len(closure) == len(code.co_freevars)
   for every elt in closure, type(elt) == cell
*/

/*[clinic input]
@classmethod
function.__new__ as func_new
    code: object(type="PyCodeObject2 *", subclass_of="&PyCode2_Type")
        a code object
    globals: object(subclass_of="&PyDict_Type")
        the globals dictionary
    name: object = None
        a string that overrides the name from the code object
    argdefs as defaults: object = None
        a tuple that specifies the default argument values
    closure: object = None
        a tuple that supplies the bindings for free variables

Create a function object.
[clinic start generated code]*/

static PyObject *
func_new_impl(PyTypeObject *type, PyCodeObject2 *code, PyObject *globals,
              PyObject *name, PyObject *defaults, PyObject *closure)
/*[clinic end generated code: output=f1039a55db32a317 input=55598410a643d7c1]*/
{
    return Py_None;
}

static int
func_clear(PyFunc *op)
{
    Py_CLEAR(op->globals);
    Py_CLEAR(op->builtins);
    Py_ssize_t nfreevars = Py_SIZE(op);
    for (Py_ssize_t i = 0; i < nfreevars; i++) {
        Py_CLEAR(op->freevars[i]);
    }
    const uint32_t *first_instr = op->func_base.first_instr;
    if (first_instr != NULL) {
        op->func_base.first_instr = NULL;
        Py_DECREF(PyCode2_FromInstr(first_instr));
    }
    // Py_CLEAR(op->func_code);
    // Py_CLEAR(op->func_globals);
    // Py_CLEAR(op->func_module);
    // Py_CLEAR(op->func_name);
    // Py_CLEAR(op->func_defaults);
    // Py_CLEAR(op->func_kwdefaults);
    // Py_CLEAR(op->func_doc);
    // Py_CLEAR(op->func_dict);
    // Py_CLEAR(op->func_closure);
    // Py_CLEAR(op->func_annotations);
    // Py_CLEAR(op->func_qualname);
    return 0;
}

static void
func_dealloc(PyFunc *op)
{
    _PyObject_GC_UNTRACK(op);
    if (op->func_weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject *) op);
    }
    (void)func_clear(op);
    PyObject_GC_Del(op);
}

static PyObject*
func_repr(PyFunc *op)
{
    PyCodeObject2 *code = PyCode2_FromFunc(op);
    return PyUnicode_FromFormat("<function %U at %p>",
                               code->co_name, op);
}

static int
func_traverse(PyFunc *f, visitproc visit, void *arg)
{
    Py_VISIT(f->globals);
    // Py_VISIT(f->builtins);
    for (Py_ssize_t i = 0, n = Py_SIZE(f); i < n; i++) {
        Py_VISIT(f->freevars[i]);
    }
    // Py_VISIT(f->func_code);
    // Py_VISIT(f->func_module);
    // Py_VISIT(f->func_defaults);
    // Py_VISIT(f->func_kwdefaults);
    // Py_VISIT(f->func_doc);
    // Py_VISIT(f->func_name);
    // Py_VISIT(f->func_dict);
    // Py_VISIT(f->func_closure);
    // Py_VISIT(f->func_annotations);
    // Py_VISIT(f->func_qualname);
    return 0;
}

/* Bind a function to an object */
static PyObject *
func_descr_get(PyObject *func, PyObject *obj, PyObject *type)
{
    if (obj == NULL) {
        Py_INCREF(func);
        return func;
    }
    return PyMethod_New(func, obj);
}

/* Methods */

#define OFF(x) offsetof(PyFunc, x)

static PyMemberDef func_memberlist[] = {
    // {"__closure__",   T_OBJECT,     OFF(func_closure),
    //  RESTRICTED|READONLY},
    {"__doc__",       T_OBJECT,     OFF(func_doc), PY_WRITE_RESTRICTED},
    {"__globals__",   T_OBJECT,     OFF(globals),
     RESTRICTED|READONLY},
    {"__module__",    T_OBJECT,     OFF(func_module), PY_WRITE_RESTRICTED},
    {NULL}  /* Sentinel */
};

static PyObject *
func_get_code(PyFunc *op, void *Py_UNUSED(ignored))
{
    if (PySys_Audit("object.__getattr__", "Os", op, "__code__") < 0) {
        return NULL;
    }
    PyObject *code = (PyObject *)PyCode2_FromFunc(op);
    Py_INCREF(code);
    return code;
}

static int
func_set_code(PyFunc *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del f.func_code or to set it to anything
     * other than a code object. */
    if (value == NULL || !PyCode2_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__code__ must be set to a code object");
        return -1;
    }

    if (PySys_Audit("object.__setattr__", "OsO",
                    op, "__code__", value) < 0) {
        return -1;
    }

    PyErr_SetString(PyExc_TypeError, "you could, but why would you?");
    return -1;

    // nfree = PyCode_GetNumFree((PyCodeObject *)value);
    // nclosure = (op->func_closure == NULL ? 0 :
    //         PyTuple_GET_SIZE(op->func_closure));
    // if (nclosure != nfree) {
    //     PyErr_Format(PyExc_ValueError,
    //                  "%U() requires a code object with %zd free vars,"
    //                  " not %zd",
    //                  op->func_name,
    //                  nclosure, nfree);
    //     return -1;
    // }
    // Py_INCREF(value);
    // Py_XSETREF(op->func_code, value);
    // return 0;
}

static PyObject *
func_get_name(PyFunc *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->func_name);
    return op->func_name;
}

static int
func_set_name(PyFunc *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del f.func_name or to set it to anything
     * other than a string object. */
    if (value == NULL || !PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__name__ must be set to a string object");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(op->func_name, value);
    return 0;
}

static PyObject *
func_get_qualname(PyFunc *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->func_qualname);
    return op->func_qualname;
}

static int
func_set_qualname(PyFunc *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del f.__qualname__ or to set it to anything
     * other than a string object. */
    if (value == NULL || !PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__qualname__ must be set to a string object");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(op->func_qualname, value);
    return 0;
}


static PyObject *
func_get_defaults(PyFunc *op, void *Py_UNUSED(ignored))
{
    if (PySys_Audit("object.__getattr__", "Os", op, "__defaults__") < 0) {
        return NULL;
    }
    PyCodeObject2 *co = PyCode2_FromFunc(op);
    Py_ssize_t required_args = co->co_totalargcount - co->co_ndefaultargs;
    Py_ssize_t n = co->co_argcount - required_args;
    if (n <= 0) {
        Py_RETURN_NONE;
    }
    PyObject *defaults = PyTuple_New(n);
    if (defaults == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *value = op->freevars[i];
        Py_INCREF(value);
        PyTuple_SET_ITEM(defaults, i, value);
    }
    return defaults;
}

static int
func_set_defaults(PyFunc *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Legal to del f.func_defaults.
     * Can only set func_defaults to NULL or a tuple. */
    if (value == Py_None)
        value = NULL;
    if (value != NULL && !PyTuple_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__defaults__ must be set to a tuple object");
        return -1;
    }
    if (value) {
        if (PySys_Audit("object.__setattr__", "OsO",
                        op, "__defaults__", value) < 0) {
            return -1;
        }
    } else if (PySys_Audit("object.__delattr__", "Os",
                           op, "__defaults__") < 0) {
        return -1;
    }

    PyErr_SetString(PyExc_TypeError, "func.__defaults__ assignment NYI");
    return -1;
    // Py_XINCREF(value);
    // Py_XSETREF(op->func_defaults, value);
    // return 0;
}

static PyObject *
func_get_kwdefaults(PyFunc *op, void *Py_UNUSED(ignored))
{
    if (PySys_Audit("object.__getattr__", "Os",
                    op, "__kwdefaults__") < 0) {
        return NULL;
    }
    PyCodeObject2 *co = PyCode2_FromFunc(op);
    Py_ssize_t kwonlyargcount = co->co_totalargcount - co->co_argcount;
    if (kwonlyargcount == 0 || co->co_ndefaultargs == 0) {
        Py_RETURN_NONE;
    }
    PyObject *kwdefaults = PyDict_New();
    if (kwdefaults == NULL) {
        return NULL;
    }
    Py_ssize_t i = co->co_ndefaultargs - kwonlyargcount;
    Py_ssize_t j = co->co_totalargcount - kwonlyargcount;
    assert(i >= 0 && j >= 0);
    for (; i < co->co_ndefaultargs; i++, j++) {
        PyObject *value = op->freevars[i];
        if (value == NULL) continue;
        PyObject *name = PyTuple_GET_ITEM(co->co_varnames, j);
        int err = PyDict_SetItem(kwdefaults, name, value);
        if (err < 0) {
            Py_DECREF(kwdefaults);
            return NULL;
        }
    }
    return kwdefaults;
}

static int
func_set_kwdefaults(PyFunc *op, PyObject *value, void *Py_UNUSED(ignored))
{
    if (value == Py_None)
        value = NULL;
    /* Legal to del f.func_kwdefaults.
     * Can only set func_kwdefaults to NULL or a dict. */
    if (value != NULL && !PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
            "__kwdefaults__ must be set to a dict object");
        return -1;
    }
    if (value) {
        if (PySys_Audit("object.__setattr__", "OsO",
                        op, "__kwdefaults__", value) < 0) {
            return -1;
        }
    } else if (PySys_Audit("object.__delattr__", "Os",
                           op, "__kwdefaults__") < 0) {
        return -1;
    }


    PyErr_SetString(PyExc_TypeError, "func.__kwdefaults__ assignment NYI");
    return -1;
    // Py_XINCREF(value);
    // Py_XSETREF(op->func_kwdefaults, value);
    // return 0;
}

static PyObject *
func_get_annotations(PyFunc *op, void *Py_UNUSED(ignored))
{
    if (op->func_annotations == NULL) {
        op->func_annotations = PyDict_New();
        if (op->func_annotations == NULL)
            return NULL;
    }
    Py_INCREF(op->func_annotations);
    return op->func_annotations;
}

static int
func_set_annotations(PyFunc *op, PyObject *value, void *Py_UNUSED(ignored))
{
    if (value == Py_None)
        value = NULL;
    /* Legal to del f.func_annotations.
     * Can only set func_annotations to NULL (through C api)
     * or a dict. */
    if (value != NULL && !PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
            "__annotations__ must be set to a dict object");
        return -1;
    }
    Py_XINCREF(value);
    Py_XSETREF(op->func_annotations, value);
    return 0;
}

static PyGetSetDef func_getsetlist[] = {
    {"__code__", (getter)func_get_code, (setter)func_set_code},
    {"__defaults__", (getter)func_get_defaults,
     (setter)func_set_defaults},
    {"__kwdefaults__", (getter)func_get_kwdefaults,
     (setter)func_set_kwdefaults},
    {"__annotations__", (getter)func_get_annotations,
     (setter)func_set_annotations},
    {"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict},
    {"__name__", (getter)func_get_name, (setter)func_set_name},
    {"__qualname__", (getter)func_get_qualname, (setter)func_set_qualname},
    {NULL} /* Sentinel */
};

// TODO: vectorcall offset?
PyTypeObject PyFunc_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "PyFunc",
    .tp_doc = func_new__doc__,
    .tp_basicsize = sizeof(PyFunc),
    .tp_itemsize = sizeof(PyObject*),
    .tp_call = (ternaryfunc)_Py_func_call,
    .tp_descr_get = func_descr_get,
    .tp_repr = (reprfunc)func_repr,
    .tp_flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                 Py_TPFLAGS_FUNC_INTERFACE | Py_TPFLAGS_METHOD_DESCRIPTOR),
    .tp_new = func_new,
    .tp_init = (initproc) NULL,
    .tp_dealloc = (destructor)func_dealloc,
    .tp_traverse = (traverseproc)func_traverse,
    .tp_clear = (inquiry)func_clear,
    .tp_weaklistoffset = offsetof(PyFunc, func_weakreflist),
    .tp_members = func_memberlist,
    .tp_getset = func_getsetlist,
    .tp_dictoffset = offsetof(PyFunc, func_dict)
};
