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


Register vm_load_name(PyObject *dict, PyObject *name)
{
    PyObject *value = PyDict_GetItemWithError2(dict, name);
    if (value == NULL) {
        abort();
    }
    return PACK_OBJ(value);
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
    PyObject **oargs = (PyObject **)args;
    for (int i = -1; i != nargs; i++) {
        assert(IS_OBJ(args[i]));
        oargs[i] = vm_object_autorelease(ts, args[i]);
    }
    PyCFunctionObject *func = (PyCFunctionObject *)(oargs[-1]);
    PyObject *res = func->vectorcall((PyObject *)func, (PyObject *const*)oargs, nargs, empty_tuple);
    for (int i = -1; i != nargs; i++) {
        args[i].as_int64 = 0;
    }
    vm_free_autorelease(ts);
    return PACK_OBJ(res);
}   

Register
vm_call_function(struct ThreadState *ts, int base, int nargs)
{
    PyObject *callable = AS_OBJ(ts->regs[base-1]);
    Register *args = &ts->regs[base];
    PyObject **oargs = (PyObject **)args;
    for (int i = -1; i != nargs; i++) {
        assert(IS_OBJ(args[i]));
        oargs[i] = vm_object_autorelease(ts, args[i]);
    }
    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    PyObject *obj = _PyObject_VectorcallTstate(ts->ts, callable, (PyObject *const *)oargs, nargsf, NULL);
    for (int i = -FRAME_EXTRA; i != nargs; i++) {
        args[i].as_int64 = 0;
    }

    vm_free_autorelease(ts);
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

static PyTypeObject PyFunc_Type;
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

static struct ThreadState *ts;
_Py_IDENTIFIER(builtins);

static PyObject *
make_globals() {
    PyObject *globals = PyDict_New();
    if (globals == NULL) {
        return NULL;
    }

    static const uint32_t func_vector_call[] = {
        CFUNC_HEADER
    };

    PyObject *name = _PyUnicode_FromId(&PyId_builtins);
    PyObject *builtins = PyImport_GetModule(name);
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

    Py_DECREF(builtins);
    return globals;
}

PyObject *
exec_code2(PyCodeObject2 *code, PyObject *globals)
{
    if (ts == NULL) {
        ts = new_threadstate();
        if (ts == NULL) {
            return NULL;
        }
    }

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

    PyFunc *func = PyFunc_New(code, globals);
    if (func == NULL) {
        return NULL;
    }

#ifdef Py_REF_DEBUG
    intptr_t oldrc = _PyThreadState_GET()->thread_ref_total;
#endif

    ts->regs += FRAME_EXTRA;
    ts->regs[-3].as_int64 = (intptr_t)PyCode2_FromInstr(func->func_base.first_instr)->co_constants;
    ts->regs[-2].as_int64 = FRAME_C;
    ts->regs[-1] = PACK(func, NO_REFCOUNT_TAG); // this_func
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

static PyTypeObject PyFunc_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "PyFunc",
    .tp_doc = "PyFunc",
    .tp_basicsize = sizeof(PyFunc),
    .tp_itemsize = sizeof(PyObject*),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_FUNC_INTERFACE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) NULL,
    .tp_dealloc = (destructor) PyFunc_dealloc,
    .tp_members = NULL,
    .tp_methods = NULL,
};