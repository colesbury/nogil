
/* Function object implementation */

#include "Python.h"
#include "pycore_object.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"
#include "structmember.h"

#include "ceval_meta.h"
#include "opcode.h"

PyObject *
PyFunction_NewWithBuiltins(PyObject *co, PyObject *globals, PyObject *builtins)
{
    _Py_IDENTIFIER(__name__);

    assert(PyCode_Check(co));
    PyCodeObject *code = (PyCodeObject *)co;
    PyFunctionObject *func = PyObject_GC_New(PyFunctionObject, &PyFunction_Type);
    if (func == NULL) {
        return NULL;
    }
    func->func_base.first_instr = PyCode_FirstInstr(code);
    if (!_PyObject_IS_DEFERRED_RC(code)) {
        // code almost always uses deferred rc, but it might be
        // disabled if the code object was resurrected by a finalizer.
        func->retains_code = 1;
        Py_INCREF(code);
    }
    else {
        func->retains_code = 0;
    }

    func->globals = globals;
    if (!_PyObject_IS_DEFERRED_RC(globals)) {
        func->retains_globals = 1;
        Py_INCREF(globals);
    }
    else {
        func->retains_globals = 0;
    }

    func->builtins = builtins;
    if (!_PyObject_IS_DEFERRED_RC(builtins)) {
        func->retains_builtins = 1;
        Py_INCREF(builtins);
    }
    else {
        func->retains_builtins = 0;
    }

    if (code->co_nconsts > 0) {
        func->func_doc = code->co_constants[0];
        Py_INCREF(func->func_doc);
    }
    else {
        func->func_doc = NULL;
    }
    if (code->co_nconsts > 1) {
        func->func_qualname = code->co_constants[1];
    }
    else {
        func->func_qualname = code->co_name;
    }
    Py_INCREF(func->func_qualname);
    func->func_name = code->co_name;
    Py_INCREF(func->func_name);
    func->func_dict = NULL;
    func->func_weakreflist = NULL;
    func->func_annotations = NULL;
    func->vectorcall = _PyFunction_Vectorcall;
    func->num_defaults = 0;
    func->freevars = NULL;
    _PyObject_GC_TRACK(func);

    func->func_module = _PyDict_GetItemIdWithError(globals, &PyId___name__);
    if (UNLIKELY(func->func_module == NULL && PyErr_Occurred())) {
        Py_DECREF(func);
        return NULL;
    }
    else if (func->func_module != NULL) {
        Py_INCREF(func->func_module);
    }

    if (code->co_nfreevars > 0) {
        func->freevars = PyObject_Malloc(code->co_nfreevars * sizeof(PyObject *));
        if (func->freevars == NULL) {
            Py_DECREF(func);
            return NULL;
        }
        func->num_defaults = code->co_ndefaultargs;
    }

    if ((code->co_flags & CO_NESTED) == 0) {
        _PyObject_SET_DEFERRED_RC(func);
    }
    return (PyObject *)func;
}

static PyObject *
builtins_from_globals(PyObject *globals)
{
    _Py_IDENTIFIER(__builtins__);
    PyObject *builtins = _PyDict_GetItemIdWithError(globals, &PyId___builtins__);
    if (!builtins) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        /* No builtins! Make up a minimal one
           Give them 'None', at least. */
        builtins = PyDict_New();
        if (!builtins) {
            return NULL;
        }
        if (PyDict_SetItemString(builtins, "None", Py_None) < 0) {
            Py_DECREF(builtins);
            return NULL;
        }
        return builtins;
    }
    if (PyModule_Check(builtins)) {
        builtins = PyModule_GetDict(builtins);
    }
    if (!PyDict_Check(builtins)) {
        PyErr_Format(PyExc_TypeError, "__builtins__ must be a dict, not '%.200s'",
            Py_TYPE(builtins)->tp_name);
        return NULL;
    }

    Py_INCREF(builtins);
    return builtins;
}

PyObject *
PyFunction_New(PyObject *co, PyObject *globals)
{
    PyObject *builtins = builtins_from_globals(globals);
    if (builtins == NULL) {
        return NULL;
    }
    PyObject *func = PyFunction_NewWithBuiltins(co, globals, builtins);
    Py_DECREF(builtins);
    return func;
}

PyObject *
PyFunction_NewWithQualName(PyObject *code, PyObject *globals, PyObject *qualname)
{
    PyFunctionObject *func = (PyFunctionObject *)PyFunction_New(code, globals);
    if (func == NULL) {
        return NULL;
    }
    if (qualname != NULL) {
        Py_INCREF(qualname);
        Py_SETREF(func->func_qualname, qualname);
    }
    return (PyObject *)func;
}

PyObject *
PyFunction_GetCode(PyObject *op)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyFunction_GET_CODE(op);
}

PyObject *
PyFunction_GetGlobals(PyObject *op)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyFunction_GET_GLOBALS(op);
}

PyObject *
PyFunction_GetModule(PyObject *op)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyFunction_GET_MODULE(op);
}

static PyObject *
_PyFunction_GetDefaults(PyFunctionObject *op)
{
    PyCodeObject *co = _PyFunction_GET_CODE(op);
    Py_ssize_t required_args = co->co_totalargcount - op->num_defaults;
    Py_ssize_t n = co->co_argcount - required_args;
    if (n <= 0) {
        return NULL;
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

PyObject *
PyFunction_GetDefaults(PyObject *op)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyFunction_GetDefaults((PyFunctionObject *)op);
}

int
_PyFunction_SetDefaults(PyObject *op, PyObject *const *defs, int defcount)
{
    PyFunctionObject *func = (PyFunctionObject *)op;
    PyCodeObject *co = _PyFunction_GET_CODE(op);
    Py_ssize_t num_defaults = defcount + PyCode_NumKwargs(co);

    if (num_defaults != func->num_defaults) {
        Py_ssize_t num_kwargs = PyCode_NumKwargs(co);
        Py_ssize_t num_freevars = PyCode_NumFreevars(co);
        Py_ssize_t new_size = num_defaults + num_freevars;

        PyObject **freevars = PyObject_Calloc(new_size, sizeof(PyObject *));
        if (freevars == NULL) {
            return -1;
        }

        for (Py_ssize_t i = 0; i < defcount; i++) {
            Py_INCREF(defs[i]);
            freevars[i] = defs[i];
        }

        Py_ssize_t prev_defcount = func->num_defaults - num_kwargs;
        Py_ssize_t n = num_kwargs + num_freevars;
        memcpy(&freevars[defcount], &func->freevars[prev_defcount], n * sizeof(PyObject *));

        PyObject **prev = func->freevars;
        func->freevars = freevars;
        func->num_defaults = num_defaults;

        for (Py_ssize_t i = 0; i < prev_defcount; i++) {
            Py_DECREF(prev[i]);
        }
        PyObject_Free(prev);
    }
    else {
        for (Py_ssize_t i = 0; i < defcount; i++) {
            Py_INCREF(defs[i]);
            Py_XSETREF(func->freevars[i], defs[i]);
        }
    }
    return 0;
}

int
PyFunction_SetDefaults(PyObject *op, PyObject *value)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (value == Py_None)
        value = NULL;
    if (value && !PyTuple_Check(value)) {
        PyErr_SetString(PyExc_SystemError, "non-tuple default args");
        return -1;
    }
    PyObject **defs = value ? _PyTuple_ITEMS(value) : NULL;
    Py_ssize_t size = value ? PyTuple_GET_SIZE(value) : 0;
    return _PyFunction_SetDefaults(op, defs, size);
}

static PyObject *
_PyFunction_GetKwDefaults(PyFunctionObject *op)
{
    PyCodeObject *co = _PyFunction_GET_CODE(op);
    Py_ssize_t num_kwargs = PyCode_NumKwargs(co);
    if (num_kwargs == 0 || op->num_defaults == 0) {
        return NULL;
    }
    PyObject *kwdefaults = PyDict_New();
    if (kwdefaults == NULL) {
        return NULL;
    }
    Py_ssize_t i = op->num_defaults - num_kwargs;
    Py_ssize_t j = co->co_totalargcount - num_kwargs;
    assert(i >= 0 && j >= 0);
    for (; i < op->num_defaults; i++, j++) {
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

PyObject *
PyFunction_GetKwDefaults(PyObject *op)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyFunction_GetKwDefaults((PyFunctionObject *)op);
}

static int
_PyFunction_SetKwDefaults(PyFunctionObject *op, PyObject *defaults)
{
    PyCodeObject *co = _PyFunction_GET_CODE(op);
    Py_ssize_t co_argcount = co->co_argcount;
    Py_ssize_t co_totalargcount = co->co_totalargcount;
    Py_ssize_t co_kwonlyargcount = co->co_kwonlyargcount;

    Py_ssize_t j = op->num_defaults - co_kwonlyargcount;
    for (Py_ssize_t i = co_argcount; i < co_totalargcount; i++, j++) {
        PyObject *kwname = PyTuple_GET_ITEM(co->co_varnames, i);
        PyObject *dflt = defaults ? PyDict_GetItemWithError(defaults, kwname) : NULL;
        if (dflt == NULL && PyErr_Occurred()) {
            return -1;
        }
        Py_XINCREF(dflt);
        Py_XSETREF(op->freevars[j], dflt);
    }
    return 0;
}

int
PyFunction_SetKwDefaults(PyObject *op, PyObject *defaults)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (defaults == Py_None)
        defaults = NULL;
    if (defaults && !PyDict_Check(defaults)) {
        PyErr_SetString(PyExc_SystemError,
                        "non-dict keyword only default args");
        return -1;
    }
    return _PyFunction_SetKwDefaults((PyFunctionObject *)op, defaults);
}

static PyObject *
_PyFunction_GetClosure(PyFunctionObject *op)
{
    PyCodeObject *co = _PyFunction_GET_CODE(op);
    Py_ssize_t n = co->co_nfreevars - op->num_defaults;
    if (n <= 0) {
        return NULL;
    }
    PyObject *closure = PyTuple_New(n);
    if (closure == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *value = op->freevars[i + op->num_defaults];
        Py_INCREF(value);
        PyTuple_SET_ITEM(closure, i, value);
    }
    return closure;
}

PyObject *
PyFunction_GetClosure(PyObject *op)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyFunction_GetClosure((PyFunctionObject *)op);
}

int
PyFunction_SetClosure(PyObject *op, PyObject *closure)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (closure && !PyTuple_Check(closure)) {
        PyErr_Format(PyExc_SystemError,
                     "expected tuple for closure, got '%.100s'",
                     Py_TYPE(closure)->tp_name);
        return -1;
    }

    PyCodeObject *co = _PyFunction_GET_CODE(op);
    Py_ssize_t size = closure ? PyTuple_GET_SIZE(closure) : 0;
    if (size != co->co_nfreevars) {
        PyErr_Format(PyExc_ValueError,
            "%U requires closure of length %zd, not %zd",
            co->co_name, co->co_nfreevars, size);
        return -1;
    }

    PyFunctionObject *func = (PyFunctionObject *)op;
    Py_ssize_t num_defaults = func->num_defaults;
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PyTuple_GET_ITEM(closure, i);
        Py_XSETREF(func->freevars[i + num_defaults], item);
    }

    return 0;
}

PyObject *
PyFunction_GetAnnotations(PyObject *op)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyFunction_GET_ANNOTATIONS(op);
}

int
PyFunction_SetAnnotations(PyObject *op, PyObject *annotations)
{
    if (!PyFunction_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (annotations == Py_None)
        annotations = NULL;
    else if (annotations && PyDict_Check(annotations)) {
        Py_INCREF(annotations);
    }
    else {
        PyErr_SetString(PyExc_SystemError,
                        "non-dict annotations");
        return -1;
    }
    Py_XSETREF(((PyFunctionObject *)op)->func_annotations, annotations);
    return 0;
}

/*[clinic input]
class function "PyFunctionObject *" "&PyFunction_Type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=70af9c90aa2e71b0]*/

#include "clinic/funcobject.c.h"

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
    code: object(type="PyCodeObject *", subclass_of="&PyCode_Type")
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
func_new_impl(PyTypeObject *type, PyCodeObject *code, PyObject *globals,
              PyObject *name, PyObject *defaults, PyObject *closure)
/*[clinic end generated code: output=99c6d9da3a24e3be input=93611752fc2daf11]*/
{
    PyFunctionObject *newfunc;
    Py_ssize_t nfree, nclosure;

    if (name != Py_None && !PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError,
                        "arg 3 (name) must be None or string");
        return NULL;
    }
    if (defaults != Py_None && !PyTuple_Check(defaults)) {
        PyErr_SetString(PyExc_TypeError,
                        "arg 4 (defaults) must be None or tuple");
        return NULL;
    }
    nfree = code->co_nfreevars;
    if (!PyTuple_Check(closure)) {
        if (nfree && closure == Py_None) {
            PyErr_SetString(PyExc_TypeError,
                            "arg 5 (closure) must be tuple");
            return NULL;
        }
        else if (closure != Py_None) {
            PyErr_SetString(PyExc_TypeError,
                "arg 5 (closure) must be None or tuple");
            return NULL;
        }
    }

    /* check that the closure is well-formed */
    nclosure = closure == Py_None ? 0 : PyTuple_GET_SIZE(closure);
    if (nfree != nclosure)
        return PyErr_Format(PyExc_ValueError,
                            "%U requires closure of length %zd, not %zd",
                            code->co_name, nfree, nclosure);
    if (nclosure) {
        Py_ssize_t i;
        for (i = 0; i < nclosure; i++) {
            PyObject *o = PyTuple_GET_ITEM(closure, i);
            if (!PyCell_Check(o)) {
                return PyErr_Format(PyExc_TypeError,
                    "arg 5 (closure) expected cell, found %s",
                                    Py_TYPE(o)->tp_name);
            }
        }
    }
    if (PySys_Audit("function.__new__", "O", code) < 0) {
        return NULL;
    }

    newfunc = (PyFunctionObject *)PyFunction_New((PyObject *)code, globals);
    if (newfunc == NULL) {
        return NULL;
    }

    if (name != Py_None) {
        Py_INCREF(name);
        Py_SETREF(newfunc->func_name, name);
    }
    if (defaults != Py_None) {
        PyErr_Format(PyExc_SystemError, "NYI: function() with defaults");
        return NULL;
        // Py_INCREF(defaults);
        // newfunc->func_defaults  = defaults;
    }
    if (closure != Py_None) {
        PyErr_Format(PyExc_SystemError, "NYI: function() with closure");
        return NULL;
        // Py_INCREF(closure);
        // newfunc->func_closure = closure;
    }

    return (PyObject *)newfunc;
}

static int
func_clear(PyFunctionObject *op)
{
    if (op->freevars != NULL) {
        PyCodeObject *co = _PyFunction_GET_CODE(op);
        PyObject **freevars = op->freevars;
        op->freevars = NULL;
        Py_ssize_t n = op->num_defaults + PyCode_NumFreevars(co);
        for (Py_ssize_t i = 0; i < n; i++) {
            Py_CLEAR(freevars[i]);
        }
        PyObject_Free(freevars);
    }
    PyObject *globals = op->globals;
    op->globals = NULL;
    if (op->retains_globals) {
        Py_XDECREF(globals);
    }
    PyObject *builtins = op->builtins;
    op->builtins = NULL;
    if (op->retains_builtins) {
        Py_XDECREF(builtins);
    }
    Py_CLEAR(op->func_doc);
    Py_CLEAR(op->func_name);
    Py_CLEAR(op->func_dict);
    Py_CLEAR(op->func_module);
    Py_CLEAR(op->func_annotations);
    Py_CLEAR(op->func_qualname);
    return 0;
}

static void
func_dealloc(PyFunctionObject *op)
{
    _PyObject_GC_UNTRACK(op);
    if (op->func_weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject *) op);
    }
    (void)func_clear(op);
    PyCodeObject *co = _PyFunction_GET_CODE(op);
    ((PyFuncBase *)op)->first_instr = NULL;
    if (op->retains_code) {
        Py_DECREF(co);
    }
    PyObject_GC_Del(op);
}

static PyObject*
func_repr(PyFunctionObject *op)
{
    return PyUnicode_FromFormat("<function %U at %p>",
                               op->func_qualname, op);
}

static int
func_traverse(PyFunctionObject *op, visitproc visit, void *arg)
{
    int visitor_type = _PyGC_VisitorType(visit);
    PyCodeObject *co = _PyFunction_GET_CODE(op);
    assert(!_PyMem_IsPtrFreed(co->co_name));
    if (op->retains_code || visitor_type != _Py_GC_VISIT_DECREF) {
        Py_VISIT(co);
    }
    if (op->retains_globals || visitor_type != _Py_GC_VISIT_DECREF) {
        Py_VISIT(op->globals);
    }
    if (op->retains_builtins || visitor_type != _Py_GC_VISIT_DECREF) {
        Py_VISIT(op->builtins);
    }
    Py_VISIT(op->func_doc);
    Py_VISIT(op->func_name);
    Py_VISIT(op->func_dict);
    Py_VISIT(op->func_module);
    Py_VISIT(op->func_annotations);
    Py_VISIT(op->func_qualname);
    if (op->freevars != NULL) {
        Py_ssize_t n = op->num_defaults + PyCode_NumFreevars(co);
        for (Py_ssize_t i = 0; i < n; i++) {
            Py_VISIT(op->freevars[i]);
        }
    }
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

#define OFF(x) offsetof(PyFunctionObject, x)

static PyMemberDef func_memberlist[] = {
    {"__doc__",       T_OBJECT,     OFF(func_doc), PY_WRITE_RESTRICTED},
    {"__globals__",   T_OBJECT,     OFF(globals),
     RESTRICTED|READONLY},
    {"__module__",    T_OBJECT,     OFF(func_module), PY_WRITE_RESTRICTED},
    {NULL}  /* Sentinel */
};

static PyObject *
func_get_code(PyFunctionObject *op, void *Py_UNUSED(ignored))
{
    if (PySys_Audit("object.__getattr__", "Os", op, "__code__") < 0) {
        return NULL;
    }
    PyObject *code = PyFunction_GET_CODE(op);
    Py_INCREF(code);
    return code;
}

static int
func_set_code(PyFunctionObject *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del f.func_code or to set it to anything
     * other than a code object. */
    if (value == NULL || !PyCode_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__code__ must be set to a code object");
        return -1;
    }

    if (PySys_Audit("object.__setattr__", "OsO",
                    op, "__code__", value) < 0) {
        return -1;
    }

    // TODO: check other attributes
    PyCodeObject *co = ((PyCodeObject *)value);
    PyCodeObject *prev = _PyFunction_GET_CODE(op);
    if (PyCode_NumFreevars(prev) != PyCode_NumFreevars(co)) {
        PyErr_Format(PyExc_ValueError,
                     "%U() requires a code object with %zd free vars,"
                     " not %zd",
                     op->func_name,
                     PyCode_NumFreevars(prev),
                     PyCode_NumFreevars(co));
        return -1;
    }

    int decref = op->retains_code;
    Py_INCREF(value);
    op->retains_code = 1;
    op->func_base.first_instr = PyCode_FirstInstr(co);
    if (decref) {
        Py_DECREF(prev);
    }
    return 0;
}

static PyObject *
func_get_name(PyFunctionObject *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->func_name);
    return op->func_name;
}

static int
func_set_name(PyFunctionObject *op, PyObject *value, void *Py_UNUSED(ignored))
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
func_get_qualname(PyFunctionObject *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->func_qualname);
    return op->func_qualname;
}

static int
func_set_qualname(PyFunctionObject *op, PyObject *value, void *Py_UNUSED(ignored))
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
func_get_defaults(PyFunctionObject *op, void *Py_UNUSED(ignored))
{
    if (PySys_Audit("object.__getattr__", "Os", op, "__defaults__") < 0) {
        return NULL;
    }
    PyObject *defaults = _PyFunction_GetDefaults(op);
    if (!defaults && !PyErr_Occurred()) {
        Py_RETURN_NONE;
    }
    return defaults;
}

static int
func_set_defaults(PyObject *op, PyObject *value, void *Py_UNUSED(ignored))
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
    PyObject **defs = value ? _PyTuple_ITEMS(value) : NULL;
    Py_ssize_t size = value ? PyTuple_GET_SIZE(value) : 0;
    return _PyFunction_SetDefaults(op, defs, size);
}

static PyObject *
func_get_kwdefaults(PyFunctionObject *op, void *Py_UNUSED(ignored))
{
    if (PySys_Audit("object.__getattr__", "Os",
                    op, "__kwdefaults__") < 0) {
        return NULL;
    }
    PyObject *kwdefaults = _PyFunction_GetKwDefaults(op);
    if (!kwdefaults && !PyErr_Occurred()) {
        Py_RETURN_NONE;
    }
    return kwdefaults;
}

static int
func_set_kwdefaults(PyFunctionObject *op, PyObject *value, void *Py_UNUSED(ignored))
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
    return _PyFunction_SetKwDefaults(op, value);
}

static PyObject *
func_get_closure(PyFunctionObject *op, void *Py_UNUSED(ignored))
{
    PyObject *closure = _PyFunction_GetClosure(op);
    if (!closure && !PyErr_Occurred()) {
        Py_RETURN_NONE;
    }
    return closure;
}

static PyObject *
func_get_annotations(PyFunctionObject *op, void *Py_UNUSED(ignored))
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
func_set_annotations(PyFunctionObject *op, PyObject *value, void *Py_UNUSED(ignored))
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
    {"__closure__", (getter)func_get_closure, NULL},
    {"__annotations__", (getter)func_get_annotations,
     (setter)func_set_annotations},
    {"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict},
    {"__name__", (getter)func_get_name, (setter)func_set_name},
    {"__qualname__", (getter)func_get_qualname, (setter)func_set_qualname},
    {NULL} /* Sentinel */
};

PyTypeObject PyFunction_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "function",
    .tp_doc = func_new__doc__,
    .tp_basicsize = sizeof(PyFunctionObject),
    .tp_call = (ternaryfunc)_PyFunc_Call,
    .tp_vectorcall_offset = offsetof(PyFunctionObject, vectorcall),
    .tp_descr_get = func_descr_get,
    .tp_repr = (reprfunc)func_repr,
    .tp_flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                 Py_TPFLAGS_FUNC_INTERFACE | Py_TPFLAGS_METHOD_DESCRIPTOR |
                 Py_TPFLAGS_HAVE_VECTORCALL),
    .tp_new = func_new,
    .tp_init = (initproc) NULL,
    .tp_dealloc = (destructor)func_dealloc,
    .tp_traverse = (traverseproc)func_traverse,
    .tp_clear = (inquiry)func_clear,
    .tp_weaklistoffset = offsetof(PyFunctionObject, func_weakreflist),
    .tp_members = func_memberlist,
    .tp_getset = func_getsetlist,
    .tp_dictoffset = offsetof(PyFunctionObject, func_dict)
};


/* Class method object */

/* A class method receives the class as implicit first argument,
   just like an instance method receives the instance.
   To declare a class method, use this idiom:

     class C:
         @classmethod
         def f(cls, arg1, arg2, ...):
             ...

   It can be called either on the class (e.g. C.f()) or on an instance
   (e.g. C().f()); the instance is ignored except for its class.
   If a class method is called for a derived class, the derived class
   object is passed as the implied first argument.

   Class methods are different than C++ or Java static methods.
   If you want those, see static methods below.
*/

typedef struct {
    PyObject_HEAD
    PyObject *cm_callable;
    PyObject *cm_dict;
} classmethod;

static void
cm_dealloc(classmethod *cm)
{
    _PyObject_GC_UNTRACK((PyObject *)cm);
    Py_XDECREF(cm->cm_callable);
    Py_XDECREF(cm->cm_dict);
    Py_TYPE(cm)->tp_free((PyObject *)cm);
}

static int
cm_traverse(classmethod *cm, visitproc visit, void *arg)
{
    Py_VISIT(cm->cm_callable);
    Py_VISIT(cm->cm_dict);
    return 0;
}

static int
cm_clear(classmethod *cm)
{
    Py_CLEAR(cm->cm_callable);
    Py_CLEAR(cm->cm_dict);
    return 0;
}


static PyObject *
cm_descr_get(PyObject *self, PyObject *obj, PyObject *type)
{
    classmethod *cm = (classmethod *)self;

    if (cm->cm_callable == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "uninitialized classmethod object");
        return NULL;
    }
    if (type == NULL)
        type = (PyObject *)(Py_TYPE(obj));
    if (Py_TYPE(cm->cm_callable)->tp_descr_get != NULL) {
        return Py_TYPE(cm->cm_callable)->tp_descr_get(cm->cm_callable, type,
                                                      NULL);
    }
    return PyMethod_New(cm->cm_callable, type);
}

static int
cm_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    classmethod *cm = (classmethod *)self;
    PyObject *callable;

    if (!_PyArg_NoKeywords("classmethod", kwds))
        return -1;
    if (!PyArg_UnpackTuple(args, "classmethod", 1, 1, &callable))
        return -1;
    Py_INCREF(callable);
    Py_XSETREF(cm->cm_callable, callable);
    return 0;
}

static PyMemberDef cm_memberlist[] = {
    {"__func__", T_OBJECT, offsetof(classmethod, cm_callable), READONLY},
    {NULL}  /* Sentinel */
};

static PyObject *
cm_get___isabstractmethod__(classmethod *cm, void *closure)
{
    int res = _PyObject_IsAbstract(cm->cm_callable);
    if (res == -1) {
        return NULL;
    }
    else if (res) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyGetSetDef cm_getsetlist[] = {
    {"__isabstractmethod__",
     (getter)cm_get___isabstractmethod__, NULL,
     NULL,
     NULL},
    {"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, NULL, NULL},
    {NULL} /* Sentinel */
};

PyDoc_STRVAR(classmethod_doc,
"classmethod(function) -> method\n\
\n\
Convert a function to be a class method.\n\
\n\
A class method receives the class as implicit first argument,\n\
just like an instance method receives the instance.\n\
To declare a class method, use this idiom:\n\
\n\
  class C:\n\
      @classmethod\n\
      def f(cls, arg1, arg2, ...):\n\
          ...\n\
\n\
It can be called either on the class (e.g. C.f()) or on an instance\n\
(e.g. C().f()).  The instance is ignored except for its class.\n\
If a class method is called for a derived class, the derived class\n\
object is passed as the implied first argument.\n\
\n\
Class methods are different than C++ or Java static methods.\n\
If you want those, see the staticmethod builtin.");

PyTypeObject PyClassMethod_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "classmethod",
    sizeof(classmethod),
    0,
    (destructor)cm_dealloc,                     /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    classmethod_doc,                            /* tp_doc */
    (traverseproc)cm_traverse,                  /* tp_traverse */
    (inquiry)cm_clear,                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    cm_memberlist,              /* tp_members */
    cm_getsetlist,                              /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    cm_descr_get,                               /* tp_descr_get */
    0,                                          /* tp_descr_set */
    offsetof(classmethod, cm_dict),             /* tp_dictoffset */
    cm_init,                                    /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    PyType_GenericNew,                          /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
};

PyObject *
PyClassMethod_New(PyObject *callable)
{
    classmethod *cm = (classmethod *)
        PyType_GenericAlloc(&PyClassMethod_Type, 0);
    if (cm != NULL) {
        Py_INCREF(callable);
        cm->cm_callable = callable;
    }
    return (PyObject *)cm;
}


/* Static method object */

/* A static method does not receive an implicit first argument.
   To declare a static method, use this idiom:

     class C:
         @staticmethod
         def f(arg1, arg2, ...):
             ...

   It can be called either on the class (e.g. C.f()) or on an instance
   (e.g. C().f()). Both the class and the instance are ignored, and
   neither is passed implicitly as the first argument to the method.

   Static methods in Python are similar to those found in Java or C++.
   For a more advanced concept, see class methods above.
*/

typedef struct {
    PyObject_HEAD
    PyObject *sm_callable;
    PyObject *sm_dict;
} staticmethod;

static void
sm_dealloc(staticmethod *sm)
{
    _PyObject_GC_UNTRACK((PyObject *)sm);
    Py_XDECREF(sm->sm_callable);
    Py_XDECREF(sm->sm_dict);
    Py_TYPE(sm)->tp_free((PyObject *)sm);
}

static int
sm_traverse(staticmethod *sm, visitproc visit, void *arg)
{
    Py_VISIT(sm->sm_callable);
    Py_VISIT(sm->sm_dict);
    return 0;
}

static int
sm_clear(staticmethod *sm)
{
    Py_CLEAR(sm->sm_callable);
    Py_CLEAR(sm->sm_dict);
    return 0;
}

static PyObject *
sm_descr_get(PyObject *self, PyObject *obj, PyObject *type)
{
    staticmethod *sm = (staticmethod *)self;

    if (sm->sm_callable == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "uninitialized staticmethod object");
        return NULL;
    }
    Py_INCREF(sm->sm_callable);
    return sm->sm_callable;
}

static int
sm_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    staticmethod *sm = (staticmethod *)self;
    PyObject *callable;

    if (!_PyArg_NoKeywords("staticmethod", kwds))
        return -1;
    if (!PyArg_UnpackTuple(args, "staticmethod", 1, 1, &callable))
        return -1;
    Py_INCREF(callable);
    Py_XSETREF(sm->sm_callable, callable);
    return 0;
}

static PyMemberDef sm_memberlist[] = {
    {"__func__", T_OBJECT, offsetof(staticmethod, sm_callable), READONLY},
    {NULL}  /* Sentinel */
};

static PyObject *
sm_get___isabstractmethod__(staticmethod *sm, void *closure)
{
    int res = _PyObject_IsAbstract(sm->sm_callable);
    if (res == -1) {
        return NULL;
    }
    else if (res) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyGetSetDef sm_getsetlist[] = {
    {"__isabstractmethod__",
     (getter)sm_get___isabstractmethod__, NULL,
     NULL,
     NULL},
    {"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, NULL, NULL},
    {NULL} /* Sentinel */
};

PyDoc_STRVAR(staticmethod_doc,
"staticmethod(function) -> method\n\
\n\
Convert a function to be a static method.\n\
\n\
A static method does not receive an implicit first argument.\n\
To declare a static method, use this idiom:\n\
\n\
     class C:\n\
         @staticmethod\n\
         def f(arg1, arg2, ...):\n\
             ...\n\
\n\
It can be called either on the class (e.g. C.f()) or on an instance\n\
(e.g. C().f()). Both the class and the instance are ignored, and\n\
neither is passed implicitly as the first argument to the method.\n\
\n\
Static methods in Python are similar to those found in Java or C++.\n\
For a more advanced concept, see the classmethod builtin.");

PyTypeObject PyStaticMethod_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "staticmethod",
    sizeof(staticmethod),
    0,
    (destructor)sm_dealloc,                     /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    staticmethod_doc,                           /* tp_doc */
    (traverseproc)sm_traverse,                  /* tp_traverse */
    (inquiry)sm_clear,                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    sm_memberlist,              /* tp_members */
    sm_getsetlist,                              /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    sm_descr_get,                               /* tp_descr_get */
    0,                                          /* tp_descr_set */
    offsetof(staticmethod, sm_dict),            /* tp_dictoffset */
    sm_init,                                    /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    PyType_GenericNew,                          /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
};

PyObject *
PyStaticMethod_New(PyObject *callable)
{
    staticmethod *sm = (staticmethod *)
        PyType_GenericAlloc(&PyStaticMethod_Type, 0);
    if (sm != NULL) {
        Py_INCREF(callable);
        sm->sm_callable = callable;
    }
    return (PyObject *)sm;
}
