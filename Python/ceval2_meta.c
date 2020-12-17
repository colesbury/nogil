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

#include <ctype.h>

Register vm_compare(Register a, Register b)
{
    printf("vm_compare\n");
    abort();
    return a;
}

Register vm_unknown_opcode(intptr_t opcode)
{
    printf("vm_unknown_opcode: %d\n", (int)opcode);
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
    printf("vm_add\n");
    abort();
    Register r;
    r.obj = Py_False;
    return r;
}

Register vm_load_name(PyObject *dict, PyObject *name)
{
    PyObject *value = PyDict_GetItemWithError2(dict, name);
    if (value == NULL) {
        printf("value is null nyi\n");
    }
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

PyObject *
eval_code2(PyCodeObject2 *code)
{
    struct ThreadState *ts = new_threadstate();
    if (ts == NULL) {
        return NULL;
    }

    ts->pc = PyCode2_GET_CODE(code);
    ts->constants = code->co_constants;
//     ts->opcode_targets = malloc(sizeof(void*) * 256);
//     memset(ts->opcode_targets, 0, sizeof(void*) * 256);
//     ts->metadata = malloc(sizeof(uint16_t) * 32);
//     memset(ts->metadata, 0, sizeof(uint16_t) * 32);

//     PyObject *globals = PyDict_New();

//     PyFunc *func = (PyFunc *)vm_new_func();
//     func->code = code;
//     func->constants = constants;
//     func->nargs = 1;
//     func->framesize = 5;
//     func->globals = globals;

//     _PyDict_SetItemId(globals, &PyId_fib, (PyObject *)func);
//     PyDictKeysObject *keys = ((PyDictObject *)globals)->ma_keys;
//     PyDictKeyEntry *entry = find_unicode(((PyDictObject *)globals)->ma_keys, _PyUnicode_FromId(&PyId_fib));
//     Py_ssize_t offset = entry - keys->dk_entries;
//     ts->metadata[3] = offset;

//     stack[0].obj = (PyObject *)func; // this_func
//     stack[1].as_int64 = (intptr_t)&retc; // retc
//     stack[2] = PACK_INT32(PyLong_AsLong(args[0])); // arg

//     ts->nargs = 1;
//     return _PyEval_Fast(ts);


    return NULL;
}

static PyTypeObject PyFunc_Type;

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
    printf("vm_decref_shared\n");
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