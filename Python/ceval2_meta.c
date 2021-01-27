#include "Python.h"
#include "pycore_call.h"
#include "pycore_ceval.h"
#include "pycore_code.h"
#include "pycore_object.h"
#include "pycore_refcnt.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"
#include "pycore_qsbr.h"

#include "code2.h"
#include "dictobject.h"
#include "frameobject.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"
#include "opcode2.h"
#include "ceval2_meta.h"
#include "opcode_names2.h"

#include <ctype.h>

static const Register NULL_REGISTER;

static PyObject *primitives[3] = {
    Py_None,
    Py_False,
    Py_True
};

static PyObject *
vm_object(Register r) {
    if (IS_OBJ(r)) {
        return AS_OBJ(r);
    }
    else {
        __builtin_unreachable();
    }
}

static PyObject *
vm_object_steal(Register r) {
    if (IS_RC(r)) {
        return AS_OBJ(r);
    }
    else if (IS_OBJ(r)) {
        PyObject *obj  = AS_OBJ(r);
        Py_INCREF(obj);
        return obj;
    }
    else {
        __builtin_unreachable();
    }
}

static PyObject *
vm_object_autorelease(struct ThreadState *ts, Register r) {
    if (IS_OBJ(r)) {
        PyObject *obj = AS_OBJ(r);
        if (IS_RC(r)) {
            *ts->maxrefs = obj;
            ts->maxrefs++;
        }
        return obj;
    }
    else {
        __builtin_unreachable();
    }
}

static void
vm_free_autorelease(struct ThreadState *ts)
{
    while (ts->maxrefs != ts->refs) {
        ts->maxrefs--;
        PyObject *obj = *ts->maxrefs;
        Py_DECREF(obj);
    }
}

#define DECREF(reg) do { \
    if (IS_RC(reg)) { \
        _Py_DECREF_TOTAL \
        PyObject *obj = AS_OBJ(reg); \
        if (_PY_LIKELY(_Py_ThreadLocal(obj))) { \
            uint32_t refcount = obj->ob_ref_local; \
            refcount -= 4; \
            obj->ob_ref_local = refcount; \
            if (_PY_UNLIKELY(refcount == 0)) { \
                _Py_MergeZeroRefcount(obj); \
            } \
        } \
        else { \
            _Py_DecRefShared(obj); \
        } \
    } \
} while (0)

Register vm_compare(Register a, Register b)
{
    printf("vm_compare\n");
    abort();
    return a;
}

Register vm_unknown_opcode(intptr_t opcode)
{
    printf("vm_unknown_opcode: %d (%s)\n", (int)opcode, opcode_names[opcode]);
    abort();
}

__attribute__((noinline))
static const uint32_t *
vm_is_bool_slow(Register acc, const uint32_t *next_instr, intptr_t opD, int exp)
{
    int err = PyObject_IsTrue(AS_OBJ(acc));
    if (UNLIKELY(err < 0)) {
        abort();
    }
    if (err == exp) {
        return next_instr + opD - 0x8000;
    }
    else {
        return next_instr;
    }
}

const uint32_t *
vm_is_true(Register acc, const uint32_t *next_instr, intptr_t opD)
{
    PyObject *obj = AS_OBJ(acc);
    if (obj == Py_True) {
        return next_instr + opD - 0x8000;
    }
    else if (_PY_LIKELY(obj == Py_False || obj == Py_None)) {
        return next_instr;
    }
    return vm_is_bool_slow(acc, next_instr, opD, 1);
}

const uint32_t *
vm_is_false(Register acc, const uint32_t *next_instr, intptr_t opD)
{
    PyObject *obj = AS_OBJ(acc);
    if (obj == Py_True) {
        return next_instr;
    }
    else if (_PY_LIKELY(obj == Py_False || obj == Py_None)) {
        return next_instr + opD - 0x8000;
    }
    return vm_is_bool_slow(acc, next_instr, opD, 0);
}

void
vm_unpack_sequence(Register acc, Register *base, Py_ssize_t n)
{
    if (PyTuple_CheckExact(AS_OBJ(acc))) {
        PyObject *tuple = AS_OBJ(acc);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PyTuple_GET_ITEM(tuple, i);
            assert(base[i].as_int64 == 0);
            base[i] = PACK_INCREF(item);
        }
        DECREF(acc);
        acc.as_int64 = 0;
    }
    else {
        abort();
    }
}


Register vm_load_name(Register *regs, PyObject *name)
{
    PyObject *locals = AS_OBJ(regs[0]);
    assert(PyDict_Check(locals));
    PyObject *value = PyDict_GetItemWithError2(locals, name);
    if (value != NULL) {
        return PACK_OBJ(value);
    }

    PyFunc *this_func = (PyFunc *)AS_OBJ(regs[-1]);
    PyObject *globals = this_func->globals;
    value = PyDict_GetItemWithError2(globals, name);
    if (value != NULL) {
        return PACK_OBJ(value);
    }

    PyObject *builtins = this_func->builtins;
    value = PyDict_GetItemWithError2(builtins, name);
    if (value != NULL) {
        return PACK_OBJ(value);
    }

    abort();
    return NULL_REGISTER;
}

Register
vm_import_name(struct ThreadState *ts, PyFunc *this_func, PyObject *arg)
{
    assert(PyTuple_CheckExact(arg) && PyTuple_GET_SIZE(arg) == 3);
    PyObject *name = PyTuple_GET_ITEM(arg, 0);
    PyObject *fromlist = PyTuple_GET_ITEM(arg, 1);
    PyObject *level = PyTuple_GET_ITEM(arg, 2);
    PyObject *res;
    int ilevel = _PyLong_AsInt(level);
    if (ilevel == -1 && _PyErr_Occurred(ts->ts)) {
        return NULL_REGISTER;
    }
    res = PyImport_ImportModuleLevelObject(
        name,
        this_func->globals,
        Py_None,
        fromlist,
        ilevel);
    if (res == NULL) {
        return NULL_REGISTER;
    }
    return PACK_OBJ(res);
}

Register
vm_load_build_class(struct ThreadState *ts, PyObject *builtins, int opA)
{
    _Py_IDENTIFIER(__build_class__);

    PyObject *bc;
    if (PyDict_CheckExact(builtins)) {
        bc = _PyDict_GetItemIdWithError(builtins, &PyId___build_class__);
        if (bc != NULL) {
            // FIXME: might get deleted oh well
            ts->regs[opA] = PACK(bc, NO_REFCOUNT_TAG);
            return NULL_REGISTER;
        }

        if (bc == NULL) {
            if (!_PyErr_Occurred(ts->ts)) {
                _PyErr_SetString(ts->ts, PyExc_NameError,
                                    "__build_class__ not found");
            }
            goto error;
        }
    }
    else {
        PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
        if (build_class_str == NULL)
            goto error;
        bc = PyObject_GetItem(builtins, build_class_str);
        if (bc == NULL) {
            if (_PyErr_ExceptionMatches(ts->ts, PyExc_KeyError))
                _PyErr_SetString(ts->ts, PyExc_NameError,
                                    "__build_class__ not found");
            goto error;
        }
        ts->regs[opA] = PACK_OBJ(bc);
        return NULL_REGISTER;
    }

error:
    abort();
    return NULL_REGISTER;
}

int vm_store_global(PyObject *dict, PyObject *name, Register acc)
{
    PyObject *value = vm_object(acc);
    int err = PyDict_SetItem(dict, name, value);
    Register ret;
    if (err < 0) {
        abort();
    }
    DECREF(acc);
    return 0;
}

Register
vm_call_cfunction(struct ThreadState *ts, Register *args, int nargs)
{
    ts->cframe_size = nargs;
    PyObject **oargs = (PyObject **)args;
    for (int i = -1; i != nargs; i++) {
        assert(IS_OBJ(args[i]));
        oargs[i] = vm_object_autorelease(ts, args[i]);
    }
    PyCFunctionObject *func = (PyCFunctionObject *)(oargs[-1]);
    PyObject *res = func->vectorcall((PyObject *)func, (PyObject *const*)oargs, nargs, empty_tuple);
    // FIXME: args may no longer be valid
    for (int i = -1; i != nargs; i++) {
        args[i].as_int64 = 0;
    }
    vm_free_autorelease(ts);
    return PACK_OBJ(res);
}   

Register
vm_call_function(struct ThreadState *ts, int base, int nargs)
{
    ts->regs += base;
    ts->cframe_size = nargs;

    PyObject *callable = AS_OBJ(ts->regs[-1]);
    Register *args = ts->regs;
    PyObject **oargs = (PyObject **)args;
    for (int i = -1; i != nargs; i++) {
        assert(IS_OBJ(args[i]));
        oargs[i] = vm_object_autorelease(ts, args[i]);
    }
    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    PyObject *obj = _PyObject_VectorcallTstate(ts->ts, callable, (PyObject *const *)oargs, nargsf, NULL);
    // FIXME: args may no longer be valid
    for (int i = -FRAME_EXTRA; i != nargs; i++) {
        ts->regs[i].as_int64 = 0;
    }
    ts->regs -= base;
    vm_free_autorelease(ts);
    if (obj == NULL) {
        PyErr_Print();
        abort();
    }
    return PACK_OBJ(obj);
}

static PyFunc *
PyFunc_New(PyCodeObject2 *code, PyObject *globals);

Register
vm_make_function(struct ThreadState *ts, PyCodeObject2 *code)
{
    PyFunc *this_func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyObject *globals = this_func->globals;
    PyFunc *func = PyFunc_New(code, globals);
    if (func == NULL) {
        return NULL_REGISTER;
    }
    func->builtins = this_func->builtins;

    for (Py_ssize_t i = 0; i < code->co_nfreevars; i++) {
        Py_ssize_t r = code->co_free2reg[i*2];
        PyObject *cell = AS_OBJ(ts->regs[r]);
        assert(PyCell_Check(cell));

        Py_INCREF(cell);
        func->freevars[i] = cell;
    }

    return PACK_OBJ((PyObject *)func);
}

Register
vm_setup_cells(struct ThreadState *ts, PyCodeObject2 *code)
{
    Register *regs = ts->regs;
    Py_ssize_t ncells = code->co_ncells;
    for (Py_ssize_t i = 0; i < ncells; i++) {
        Py_ssize_t idx = code->co_cell2reg[i];
        PyObject *cell = PyCell_New(AS_OBJ(regs[idx]));
        if (cell == NULL) {
            abort();
            return NULL_REGISTER;
        }

        DECREF(regs[idx]);
        regs[idx] = PACK(cell, REFCOUNT_TAG);
    }
    return NULL_REGISTER;
}

Register
vm_setup_freevars(struct ThreadState *ts, PyCodeObject2 *code)
{
    Register *regs = ts->regs;
    PyFunc *this_func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    Py_ssize_t nfreevars = code->co_nfreevars;
    for (Py_ssize_t i = 0; i < nfreevars; i++) {
        Py_ssize_t r = code->co_free2reg[i*2+1];
        PyObject *cell = this_func->freevars[i];
        assert(PyCell_Check(cell));
        regs[r] = PACK(cell, NO_REFCOUNT_TAG);
    }
    return NULL_REGISTER;
}

Register vm_build_slice(Register *regs)
{
    PyObject *slice = PySlice_New(AS_OBJ(regs[0]), AS_OBJ(regs[1]), AS_OBJ(regs[2]));
    DECREF(regs[2]);
    regs[2].as_int64 = 0;
    DECREF(regs[1]);
    regs[1].as_int64 = 0;
    DECREF(regs[0]);
    regs[0].as_int64 = 0;
    return PACK(slice, REFCOUNT_TAG);
}

Register vm_build_list(Register *regs, Py_ssize_t n)
{
    PyObject *obj = PyList_New(n);
    if (obj == NULL) {
        return NULL_REGISTER;
    }
    while (n) {
        n--;
        PyList_SET_ITEM(obj, n, vm_object_steal(regs[n]));
    }
    return PACK(obj, REFCOUNT_TAG);
}


Register vm_build_tuple(Register *regs, Py_ssize_t n)
{
    PyObject *obj = PyTuple_New(n);
    if (obj == NULL) {
        return NULL_REGISTER;
    }
    while (n) {
        n--;
        PyObject *item = vm_object_steal(regs[n]);
        if (item == NULL) {
            abort();
        }
        PyTuple_SET_ITEM(obj, n, item);
        regs[n].as_int64 = 0;
    }
    return PACK(obj, REFCOUNT_TAG);
}

Register vm_list_append(Register a, Register b)
{
    PyObject *list = AS_OBJ(a);
    PyObject *item = vm_object(b);
    PyList_Append(list, item);
    Py_DECREF(item);
    return NULL_REGISTER;
}

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed)
{
    printf("vm_resize_stack\n");
    abort();
    return 0;
}

PyObject *vm_args_error(struct ThreadState *ts)
{
    printf("vm_args_error\n");
    abort();
    return NULL;
}

PyObject *vm_error_not_callable(struct ThreadState *ts)
{
    printf("vm_error_not_callable\n");
    abort();
    return NULL;
}

void vm_handle_error(struct ThreadState *ts)
{
    printf("vm_handle_error\n");
    abort();
}

static struct ThreadState *
new_threadstate(void)
{
    struct ThreadState *ts = malloc(sizeof(struct ThreadState));
    if (ts == NULL) {
        return NULL;
    }
    memset(ts, 0, sizeof(struct ThreadState));

    Py_ssize_t stack_size = 256;
    Register *stack = malloc(stack_size * sizeof(Register));
    if (stack == NULL) {
        goto err;
    }
    memset(stack, 0, stack_size * sizeof(Register));

    ts->refs_base = malloc(sizeof(PyObject*) * 256);
    if (!ts->refs_base) {
        abort();
    }
    ts->refs = ts->refs_base;
    ts->maxrefs = ts->refs_base;

    ts->stack = stack;
    ts->maxstack = &stack[stack_size];
    ts->regs = stack;
    ts->opcode_targets = malloc(sizeof(void*) * 256);
    ts->ts = PyThreadState_GET();
    if (ts->opcode_targets == NULL) {
        goto err;
    }
    memset(ts->opcode_targets, 0, sizeof(void*) * 256);
    return ts;

err:
    free(ts);
    return NULL;
}

// static PyTypeObject PyCFunc_Type;

// static PyCFunc *
// PyCFunc_New(vectorcallfunc vectorcall)
// {
//     PyCFunc *func = PyObject_Malloc(sizeof(PyCFunc));
//     if (func == NULL) {
//         return NULL;
//     }
//     memset(func, 0, sizeof(PyCFunc));
//     PyObject_Init((PyObject *)func, &PyFunc_Type);
//     printf("PyCFunc_New: first_instr=%p\n", &func_vector_call);

//     func->base.globals = NULL;
//     func->base.first_instr = &func_vector_call;
//     func->vectorcall = vectorcall;
//     return func;
// }

static PyFunc *
PyFunc_New(PyCodeObject2 *code, PyObject *globals)
{
    PyFunc *func = PyObject_NewVar(PyFunc, &PyFunc_Type, code->co_nfreevars);
    if (func == NULL) {
        return NULL;
    }
    if ((code->co_flags & CO_NESTED) == 0) {
        _PyObject_SET_DEFERRED_RC((PyObject *)func);
    }
    func->func_base.first_instr = PyCode2_GET_CODE(code);
    Py_INCREF(globals);
    func->globals = globals;
    return func;
}

static struct ThreadState *gts;
_Py_IDENTIFIER(builtins);
_Py_IDENTIFIER(__builtins__);

static PyObject *
make_globals() {
    PyObject *globals = PyDict_New();
    if (globals == NULL) {
        return NULL;
    }

    static const uint32_t func_vector_call[] = {
        CFUNC_HEADER
    };

    PyObject *builtins_name = _PyUnicode_FromId(&PyId_builtins);
    PyObject *builtins = PyImport_GetModule(builtins_name);
    PyObject *builtins_dict = PyModule_GetDict(builtins);
    Py_ssize_t i = 0;
    PyObject *key, *value;
    while (PyDict_Next(builtins_dict, &i, &key, &value)) {
        if (PyCFunction_Check(value)) {
            ((PyFuncBase *)value)->first_instr = &func_vector_call;
        }
        int err = PyDict_SetItem(globals, key, value);
        if (err < 0) {
            abort();
        }
    }
    int err = _PyDict_SetItemId(globals, &PyId___builtins__, builtins);
    if (err < 0) {
        abort();
    }

    Py_DECREF(builtins);
    return globals;
}

static PyObject *
builtins_from_globals2(PyObject *globals)
{
    PyObject *builtins = _PyDict_GetItemIdWithError(globals, &PyId___builtins__);
    if (!builtins) {
        abort();
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
        _PyObject_SET_DEFERRED_RC(builtins);
        Py_DECREF(builtins);
        return builtins;
    }
    if (PyModule_Check(builtins)) {
        builtins = PyModule_GetDict(builtins);
    }
    Py_INCREF_STACK(builtins);
    return builtins;
}

static Py_ssize_t
setup_frame(struct ThreadState *ts, PyFunc *func)
{
    Py_ssize_t frame_size;
    if (PyFunc_Check(AS_OBJ(ts->regs[-1]))) {
        PyFunc *this_func = (PyFunc *)AS_OBJ(ts->regs[-1]);
        frame_size = PyCode2_FromFunc(this_func)->co_framesize;
    }
    else {
        frame_size = ts->cframe_size;
    }

    ts->regs += frame_size + FRAME_EXTRA;

    PyCodeObject2 *code = PyCode2_FromFunc(func);
    ts->regs[-3].as_int64 = (intptr_t)code->co_constants;
    ts->regs[-2].as_int64 = FRAME_C;
    ts->regs[-1] = PACK(func, NO_REFCOUNT_TAG); // this_func
    ts->pc = PyCode2_GET_CODE(code);
    return frame_size;
}

PyObject *
_PyEval_FastCall(PyFunc *func, PyObject *locals)
{
    struct ThreadState *ts = gts;
    Py_ssize_t frame_size = setup_frame(ts, func);
    ts->regs[0] = PACK(locals, NO_REFCOUNT_TAG);
    ts->nargs = 0;
    PyObject *ret = _PyEval_Fast(ts);
    ts->regs -= frame_size + FRAME_EXTRA;
    return ret;
}

PyObject *
_PyEval_FastCallArgs(PyFunc *func, PyObject *args)
{
    struct ThreadState *ts = gts;
    Py_ssize_t frame_size = setup_frame(ts, func);
    if (args != NULL) {
        Py_ssize_t n = PyTuple_GET_SIZE(args);
        ts->nargs = n;
        for (Py_ssize_t i = 0; i != n; i++) {
            ts->regs[i] = PACK(PyTuple_GET_ITEM(args, i), NO_REFCOUNT_TAG);
        }
    }
    else {
        ts->nargs = 0;
    }
    PyObject *ret = _PyEval_Fast(ts);
    ts->regs -= frame_size + FRAME_EXTRA;
    return ret;
}

PyObject *
exec_code2(PyCodeObject2 *code, PyObject *globals)
{
    if (gts == NULL) {
        gts = new_threadstate();
        if (gts == NULL) {
            return NULL;
        }
    }
    struct ThreadState *ts = gts;

    ts->pc = PyCode2_GET_CODE(code);

    if (empty_tuple == NULL) {
        empty_tuple = PyTuple_New(0);
    }

    if (globals == NULL) {
        globals = make_globals();
        if (globals == NULL) {
            return NULL;
        }
    }

    if (PyDict_GetItemString(globals, "__builtins__") == NULL) {
        int err = PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
        assert(err == 0);
    }


    PyFunc *func = PyFunc_New(code, globals);
    if (func == NULL) {
        return NULL;
    }
    func->builtins = builtins_from_globals2(globals);

#ifdef Py_REF_DEBUG
    intptr_t oldrc = _PyThreadState_GET()->thread_ref_total;
#endif

    ts->regs += FRAME_EXTRA;
    ts->regs[-3].as_int64 = (intptr_t)PyCode2_FromFunc(func)->co_constants;
    ts->regs[-2].as_int64 = FRAME_C;
    ts->regs[-1] = PACK(func, NO_REFCOUNT_TAG); // this_func
    ts->regs[0] = PACK(globals, NO_REFCOUNT_TAG);
    ts->nargs = 0;
    PyObject *ret = _PyEval_Fast(ts);

#ifdef Py_REF_DEBUG
    intptr_t newrc = _PyThreadState_GET()->thread_ref_total;
    printf("RC %ld to %ld (%ld)\n", (long)oldrc, (long)newrc, (long)(newrc - oldrc));
#endif

    return ret;
}

PyObject *vm_new_func(void)
{
    if (PyType_Ready(&PyFunc_Type) < 0) {
        return NULL;
    }
    if (PyType_Ready(&PyMeth_Type) < 0) {
        return NULL;
    }
    PyObject *func = (PyObject *)PyObject_New(PyFunc, &PyFunc_Type);
    if (!func) {
        return NULL;
    }
    func->ob_ref_local |= _Py_REF_DEFERRED_MASK;
    return func;
}

void vm_decref_shared(PyObject *op) {
    printf("vm_decref_shared: %p\n", op);
    abort();
}
void vm_incref_shared(PyObject *op) {
    printf("vm_incref_shared\n");
    abort();
}

static void
PyFunc_dealloc(PyFunc *func)
{
    // PyObject_GC_UnTrack(func);
    Py_CLEAR(func->globals);
    Py_ssize_t nfreevars = Py_SIZE(func);
    for (Py_ssize_t i = 0; i < nfreevars; i++) {
        Py_CLEAR(func->freevars[i]);
    }
    PyObject_Del(func);
}

static PyObject *
func_call(PyObject *func, PyObject *args, PyObject *kwds)
{
    assert(kwds == NULL);
    return _PyEval_FastCallArgs((PyFunc *)func, args);
}

/* Bind a function to an object */
static PyObject *
func_descr_get(PyObject *func, PyObject *obj, PyObject *type)
{
    static uint32_t meth_instr = METHOD_HEADER;
    PyMethod *method = PyObject_New(PyMethod, &PyMeth_Type);
    if (method == NULL) {
        return NULL;
    }
    // _PyObject_SET_DEFERRED_RC((PyObject *)method);
    method->func_base.first_instr = &meth_instr;
    Py_INCREF(func);
    method->im_func = func;
    Py_INCREF(obj);
    method->im_self = obj;
    method->im_weakreflist = NULL;
    return method;
}

PyTypeObject PyFunc_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "PyFunc",
    .tp_doc = "PyFunc doc",
    .tp_basicsize = sizeof(PyFunc),
    .tp_itemsize = sizeof(PyObject*),
    .tp_call = func_call,
    .tp_descr_get = func_descr_get,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_FUNC_INTERFACE | Py_TPFLAGS_METHOD_DESCRIPTOR,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) NULL,
    .tp_dealloc = (destructor) PyFunc_dealloc,
    .tp_members = NULL,
    .tp_methods = NULL,
};

static PyObject *
method_call(PyObject *method, PyObject *args, PyObject *kwds)
{
    printf("method_call NYI\n");
    abort();
    return NULL;
}

static PyObject *
method_descr_get(PyObject *meth, PyObject *obj, PyObject *cls)
{
    Py_INCREF(meth);
    return meth;
}

static void
method_dealloc(PyMethod *im)
{
    // _PyObject_GC_UNTRACK(im);
    if (im->im_weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *)im);
    Py_DECREF(im->im_func);
    Py_XDECREF(im->im_self);
    // PyObject_GC_Del(im);
    PyObject_Del(im);
}

PyTypeObject PyMeth_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "method",
    .tp_doc = "method doc",
    .tp_basicsize = sizeof(PyMethod),
    .tp_call = method_call,
    .tp_descr_get = method_descr_get,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_FUNC_INTERFACE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) NULL,
    .tp_dealloc = (destructor)method_dealloc,
    .tp_members = NULL,
    .tp_methods = NULL,
};