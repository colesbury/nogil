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
    else if (IS_INT32(r)) {
        return PyLong_FromLong(AS_INT32(r));
    }
    else if (IS_PRI(r)) {
        return primitives[AS_PRI(r)];
    }
    else {
        __builtin_unreachable();
    }
}

static Register
FROM_OBJ(PyObject *obj)
{
    Register r;
    r.obj = obj;
    if (obj && _PyObject_IS_DEFERRED_RC(obj)) {
        r.as_int64 |= REFCOUNT_TAG;
    }
    return r;
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
}

Register vm_add(Register a, Register acc)
{
    printf("vm_add %p %p\n", a.as_int64, acc.as_int64);
    PyObject *o1 = vm_object(a);
    PyObject *o2 = vm_object(acc);
    PyObject *res = PyNumber_Add(o1, o2);
    if (res == NULL) {
        PyErr_Print();
        assert(res != NULL);
    }
    DECREF(acc);
    return FROM_OBJ(res);
}

Register vm_inplace_add(Register a, Register acc)
{
    printf("vm_inplace_add %p %p\n", a.as_int64, acc.as_int64);
    PyObject *o1 = vm_object(a);
    PyObject *o2 = vm_object(acc);
    PyObject *res = PyNumber_InPlaceAdd(o1, o2);
    if (res == NULL) {
        PyErr_Print();
        assert(res != NULL);
    }
    DECREF(acc);
    return FROM_OBJ(res);
}

Register vm_sub(Register a, Register b)
{
    printf("vm_sub: %p %p\n", a.as_int64, b.as_int64);
    abort();
}

Register vm_mul(Register a, Register acc)
{
    PyObject *o1 = vm_object(a);
    PyObject *o2 = vm_object(acc);
    PyObject *res = PyNumber_Multiply(o1, o2);
    DECREF(acc);
    return FROM_OBJ(res);
}

Register vm_true_div(Register a, Register acc)
{
    PyObject *o1 = vm_object(a);
    PyObject *o2 = vm_object(acc);
    PyObject *res = PyNumber_TrueDivide(o1, o2);
    DECREF(acc);
    return FROM_OBJ(res);
}

Register vm_floor_div(Register a, Register acc)
{
    PyObject *o1 = vm_object(a);
    PyObject *o2 = vm_object(acc);
    PyObject *res = PyNumber_FloorDivide(o1, o2);
    DECREF(acc);
    return FROM_OBJ(res);
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


Register
vm_call_function(struct ThreadState *ts, int base, int nargs)
{
    PyObject *callable = AS_OBJ(ts->regs[base+1]);
    Register *args = &ts->regs[base+2];
    Register res;

    // FIXME: leaks galore
    for (int i = 0; i != nargs; i++) {
        args[i].obj = AS_OBJ(args[i]);
    }

    printf("vm_call_function: %s %d %d\n", Py_TYPE(callable)->tp_name, base, nargs);
    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    res.obj = _PyObject_VectorcallTstate(ts->ts, callable, (PyObject *const *)args, nargsf, NULL);
    res.as_int64 |= REFCOUNT_TAG;

    for (int i = -2; i != nargs; i++) {
        args[i].as_int64 = 0;
    }

    printf("res = %p %s\n", res.as_int64, Py_TYPE(AS_OBJ(res))->tp_name);
    return res;
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

    assert(func == NULL || _PyObject_IS_DEFERRED_RC((PyObject *)func));
    ret.obj = func;
    ret.as_int64 |= REFCOUNT_TAG;
    return ret;
}

Register
vm_setup_cells(struct ThreadState *ts, PyCodeObject2 *code)
{
    Register *regs = ts->regs;
    Py_ssize_t ncells = code->co_ncells;
    for (Py_ssize_t i = 0; i < ncells; i++) {
        Py_ssize_t idx = code->co_cell2reg[i];
        PyObject *cell = PyCell_New(vm_object(regs[idx]));
        if (cell == NULL) {
            abort();
            return NULL_REGISTER;
        }

        regs[idx].as_int64 = (intptr_t)cell | REFCOUNT_TAG;
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

        regs[r].obj = cell;
    }
    return NULL_REGISTER;
}


Register vm_build_list(Register *regs, Py_ssize_t n)
{
    PyObject *obj = PyList_New(n);
    if (obj == NULL) {
        return NULL_REGISTER;
    }
    while (n) {
        n--;
        PyList_SET_ITEM(obj, n, vm_object(regs[n]));
    }
    Register r;
    r.as_int64 = (intptr_t)obj | REFCOUNT_TAG;
    return r;
}


Register vm_build_tuple(Register *regs, Py_ssize_t n)
{
    PyObject *obj = PyTuple_New(n);
    if (obj == NULL) {
        return NULL_REGISTER;
    }
    while (n) {
        n--;
        PyObject *item = vm_object(regs[n]);
        printf("item at %zd = %p regs[n]=%p\n", n, item, (void*)regs[n].obj);
        if (item == NULL) {
            abort();
        }
        PyTuple_SET_ITEM(obj, n, item);
    }
    printf("build tuple: %p size=%zd\n", obj, PyTuple_GET_SIZE(obj));
    Register r;
    r.as_int64 = (intptr_t)obj | REFCOUNT_TAG;
    return r;
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
    ((PyObject *)func)->ob_ref_local |= _Py_REF_DEFERRED_MASK;
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
    ts->regs[-2].as_int64 = FRAME_C;
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
    .tp_itemsize = sizeof(PyObject*),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_FUNC_INTERFACE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) NULL,
    .tp_dealloc = (destructor) NULL,
    .tp_members = NULL,
    .tp_methods = NULL,
};