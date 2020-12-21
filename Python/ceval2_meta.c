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

static PyObject *primitives[3] = {
    Py_None,
    Py_False,
    Py_True
};

static PyObject *
vm_object(Register acc) {
    if (IS_OBJ(acc)) {
        acc.as_int64 &= ~REFCOUNT_TAG;
        return acc.obj;
    }
    else if (IS_INT32(acc)) {
        return PyLong_FromLong(AS_INT32(acc));
    }
    else if (IS_PRI(acc)) {
        return primitives[AS_PRI(acc)];
    }
    else {
        __builtin_unreachable();
    }
}

#define DECREF(reg) do { \
    if (IS_RC(reg)) { \
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


Register vm_to_bool(Register x)
{
    printf("vm_to_bool\n");
    abort();
    Register r;
    r.obj = Py_False;
    return r;
}

Register vm_add(Register a, Register b)
{
    printf("vm_add: %p %p\n", a.as_int64, b.as_int64);
    if (IS_OBJ(a)) {
        printf("a = %s\n", Py_TYPE(AS_OBJ(a))->tp_name);
    }
    if (IS_OBJ(b)) {
        printf("b = %s\n", Py_TYPE(AS_OBJ(b))->tp_name);
    }

    abort();
    Register r;
    r.obj = Py_False;
    return r;
}

Register vm_sub(Register a, Register b)
{
    printf("vm_sub: %p %p\n", a.as_int64, b.as_int64);
    abort();
}


Register vm_load_name(PyObject *dict, PyObject *name)
{
    printf("loading %p[%p]\n", dict, name);
    printf("loading %s (%s) from %p\n", PyUnicode_AsUTF8(name), Py_TYPE(name)->tp_name, dict);
    PyObject *value = PyDict_GetItemWithError2(dict, name);
    if (value == NULL) {
        printf("value is null nyi\n");
    }
    printf("value = %p\n", value);
    Register r;
    r.obj = value;
    uint32_t local = value->ob_ref_local;
    if ((local & (_Py_REF_IMMORTAL_MASK|_Py_REF_DEFERRED_MASK)) != 0) {
    }
    else {
        Py_INCREF(value);
        r.as_int64 |= 1;
    }
    return r;
}

Register vm_store_global(PyObject *dict, PyObject *name, Register acc)
{
    PyObject *value = vm_object(acc);
    printf("storing %p[%p] = %p\n", dict, name, value);
    int err = PyDict_SetItem(dict, name, value);
    Register ret;
    if (err < 0) {
        ret.as_int64 = 0;
        abort();
        return ret;
    }
    DECREF(acc);
    ret.obj = Py_None;
    return ret;
}

static PyFunc *
PyFunc_New(PyCodeObject2 *code, PyObject *globals);

Register
vm_make_function(struct ThreadState *ts, PyCodeObject2 *code)
{
    Register ret;
    PyFunc *this_func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyObject *globals = this_func->globals;
    PyFunc *func = PyFunc_New(code, globals);
    assert(func == NULL || _PyObject_IS_DEFERRED_RC((PyObject *)func));
    ret.obj = func;
    ret.as_int64 |= REFCOUNT_TAG;
    return ret;
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

    ts->stack = stack;
    ts->maxstack = &stack[stack_size];
    ts->regs = stack;
    ts->opcode_targets = malloc(sizeof(void*) * 256);
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
    PyFunc *func = PyObject_New(PyFunc, &PyFunc_Type);
    if (func == NULL) {
        return NULL;
    }
    ((PyObject *)func)->ob_ref_local |= _Py_REF_DEFERRED_MASK;
    func->first_instr = PyCode2_GET_CODE(code);
    Py_INCREF(globals);
    func->globals = globals;
    return func;
}

static const uint32_t retc[] = {
    RETURN_TO_C
};

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
            ((PyFunc *)value)->first_instr = &func_vector_call;
        }
        int err = PyDict_SetItem(globals, key, value);
        assert(err == 0);
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
    printf("exec with func: %p\n", func);
    printf("calling with regs = %p\n", ts->regs);

    ts->regs += 2;
    ts->regs[-2].as_int64 = (intptr_t)&retc;
    ts->regs[-1].obj = (PyObject *)func; // this_func
    ts->nargs = 0;
    return _PyEval_Fast(ts);
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

void vm_zero_refcount(PyObject *op) {}
void vm_decref_shared(PyObject *op) {
    printf("vm_decref_shared: %p\n", op);
    abort();
}
void vm_incref_shared(PyObject *op) {
    printf("vm_incref_shared\n");
    abort();
}

static PyTypeObject PyFunc_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "PyFunc",
    .tp_doc = "PyFunc",
    .tp_basicsize = sizeof(PyFunc),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) NULL,
    .tp_dealloc = (destructor) NULL,
    .tp_members = NULL,
    .tp_methods = NULL,
};