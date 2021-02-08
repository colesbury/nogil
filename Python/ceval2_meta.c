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
#include <alloca.h>

static PyObject *
vm_object_steal(Register r) {
    PyObject *obj = AS_OBJ(r);
    if (!IS_RC(r)) {
        Py_INCREF(obj);
    }
    return obj;
}

static Py_ssize_t
vm_frame_size(struct ThreadState *ts)
{
    PyObject *this_func = AS_OBJ(ts->regs[-1]);
    if (!PyFunc_Check(this_func)) {
        return ts->regs[-3].as_int64;
    }
    return PyCode2_FromFunc((PyFunc *)this_func)->co_framesize;
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

#define CLEAR(reg) do {     \
    Register _tmp = (reg);  \
    (reg).as_int64 = 0;    \
    DECREF(_tmp);           \
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

 static const uint32_t * _Py_NO_INLINE
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

static Register _Py_NO_INLINE
attribute_error(struct ThreadState *ts, _Py_Identifier *id)
{
    Register error = {0};
    PyThreadState *tstate = ts->ts;
    if (!_PyErr_Occurred(tstate)) {
        _PyErr_SetObject(tstate, PyExc_AttributeError, id->object);
    }
    return error;
}

Register
vm_setup_with(struct ThreadState *ts, Py_ssize_t opA)
{
    _Py_IDENTIFIER(__enter__);
    _Py_IDENTIFIER(__exit__);
    Register error = {0};

    PyObject *mgr = AS_OBJ(ts->regs[opA]);
    PyObject *exit = _PyObject_LookupSpecial(mgr, &PyId___exit__);
    if (UNLIKELY(exit == NULL)) {
        return attribute_error(ts, &PyId___exit__);
    }
    ts->regs[opA + 1] = PACK_OBJ(exit);
    PyObject *enter = _PyObject_LookupSpecial(mgr, &PyId___enter__);
    if (UNLIKELY(enter == NULL)) {
        return attribute_error(ts, &PyId___exit__);
    }
    PyObject *res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    if (UNLIKELY(res == NULL)) {
        return error;
    }
    return PACK_OBJ(res);
}

static void
vm_clear_regs(struct ThreadState *ts, Py_ssize_t lo, Py_ssize_t hi);

static int
vm_exit_with_exc(struct ThreadState *ts, Py_ssize_t opA)
{
    PyObject *exit = AS_OBJ(ts->regs[opA + 1]);
    PyObject *res;

    PyObject *exc = ts->handled_exc;
    PyObject *type = (PyObject *)Py_TYPE(exc);
    PyObject *tb = ((PyBaseExceptionObject *)exc)->traceback;
    PyObject *stack[4] = {NULL, type, exc, tb};
    Py_ssize_t nargsf = 3 | PY_VECTORCALL_ARGUMENTS_OFFSET;

    res = _PyObject_Vectorcall(exit, stack + 1, nargsf, NULL);
    if (UNLIKELY(res == NULL)) {
        return -1;
    }

    int is_true = PyObject_IsTrue(res);
    Py_DECREF(res);
    if (UNLIKELY(is_true < 0)) {
        return -1;
    }
    if (UNLIKELY(is_true == 1)) {
        // ignore the exception and continue
        vm_clear_regs(ts, opA, opA + 4);
        return 0;
    }

    // re-raise the exception
    ts->handled_exc = AS_OBJ(ts->regs[opA + 3]);
    ts->regs[opA + 3].as_int64 = 0;
    vm_reraise(ts, exc);
    return -1;
}

int
vm_exit_with(struct ThreadState *ts, Py_ssize_t opA)
{
    int64_t link = ts->regs[opA + 2].as_int64;
    if (UNLIKELY(link == -1)) {
        return vm_exit_with_exc(ts, opA);
    }

    PyObject *res;
    // PyObject *mgr = AS_OBJ(ts->regs[opA]);
    PyObject *exit = AS_OBJ(ts->regs[opA + 1]);

    PyObject *stack[4] = {NULL, Py_None, Py_None, Py_None};
    Py_ssize_t nargsf = 3 | PY_VECTORCALL_ARGUMENTS_OFFSET;
    res = _PyObject_VectorcallTstate(ts->ts, exit, stack + 1, nargsf, NULL);
    if (UNLIKELY(res == NULL)) {
        return -1;
    }
    Py_DECREF(res);
    assert(ts->regs[opA + 3].as_int64 == 0);
    assert(ts->regs[opA + 2].as_int64 == 0);
    CLEAR(ts->regs[opA + 1]);
    CLEAR(ts->regs[opA]);
    return 0;
}

static void
vm_clear_regs(struct ThreadState *ts, Py_ssize_t lo, Py_ssize_t hi)
{
    // clear regs in range [lo, hi)
    assert(lo <= hi);
    Py_ssize_t n = hi;
    while (n != lo) {
        n--;
        Register tmp = ts->regs[n];
        if (tmp.as_int64 != 0) {
            ts->regs[n].as_int64 = 0;
            DECREF(tmp);
        }
    }
}

/* Finds the inner most exception handler for the current instruction.
   Exception handlers are stored in inner-most to outer-most order.
*/
static ExceptionHandler *
vm_exception_handler(PyCodeObject2 *code, const uint32_t *next_instr)
{
    const uint32_t *first_instr = PyCode2_GET_CODE(code);
    Py_ssize_t instr_offset = (next_instr - 1 - first_instr);

    struct _PyHandlerTable *table = code->co_exc_handlers;
    for (Py_ssize_t i = 0, n = table->size; i < n; i++) {
        ExceptionHandler eh = table->entries[i];
        printf("instr_offset = %zd start = %zd handler = %zd\n", instr_offset, eh.start, eh.handler);
        if (eh.start <= instr_offset && instr_offset < eh.handler) {
            return &table->entries[i];
        }
    }
    return NULL;
}

const uint32_t *
vm_exception_unwind(struct ThreadState *ts, const uint32_t *next_instr)
{
    assert(PyErr_Occurred());
    for (;;) {
        PyObject *callable = AS_OBJ(ts->regs[-1]);
        if (PyFunc_Check(callable)) {
            PyFunc *func = (PyFunc *)callable;
            PyCodeObject2 *code = PyCode2_FromFunc(func);
            ExceptionHandler *handler = vm_exception_handler(code, next_instr);

            if (handler != NULL) {
                printf("found handler at %zd\n", handler->handler);
                vm_clear_regs(ts, handler->reg, code->co_framesize);

                PyObject *exc, *val, *tb;
                _PyErr_Fetch(ts->ts, &exc, &val, &tb);
                /* Make the raw exception data
                    available to the handler,
                    so a program can emulate the
                    Python main loop. */
                _PyErr_NormalizeException(ts->ts, &exc, &val, &tb);
                if (tb != NULL)
                    PyException_SetTraceback(val, tb);
                else
                    PyException_SetTraceback(val, Py_None);
                Py_ssize_t link_reg = handler->reg;
                ts->regs[link_reg].as_int64 = -1;
                if (ts->handled_exc != NULL) {
                    ts->regs[link_reg + 1] = PACK_OBJ(ts->handled_exc);
                }
                else {
                    ts->regs[link_reg + 1].as_int64 = 0;
                }
                ts->handled_exc = val;
                Py_DECREF(exc);
                Py_XDECREF(tb);
                return PyCode2_GET_CODE(code) + handler->handler;
            }
        }

        // No handler found in this call frame. Clears the entire frame and
        // unwinds the call stack.
        Py_ssize_t frame_size = vm_frame_size(ts);
        vm_clear_regs(ts, -1, frame_size);
        uintptr_t frame_link = ts->regs[-2].as_int64;
        ts->regs[-2].as_int64 = 0;
        ts->regs[-3].as_int64 = 0;
        if ((frame_link & FRAME_TAG_MASK) != FRAME_PYTHON) {
            ts->next_instr = (const uint32_t *)(frame_link & ~FRAME_TAG_MASK);
            Py_ssize_t frame_delta = ts->regs[-4].as_int64;
            ts->regs[-4].as_int64 = 0;
            ts->regs -= frame_delta;
            return NULL;
        }
        next_instr = (const uint32_t *)frame_link;
        // this is the call that dispatched to us
        uint32_t call = next_instr[-1];
        intptr_t offset = (call >> 8) & 0xFF;
        ts->regs -= offset;
    }
}

void
vm_reraise(struct ThreadState *ts, PyObject *exc)
{
    PyObject *tb = PyException_GetTraceback(exc);
    PyObject *type = (PyObject *)Py_TYPE(exc);
    Py_INCREF(type);
    _PyErr_Restore(ts->ts, type, exc, tb);
}

int
vm_raise(struct ThreadState *ts, PyObject *exc)
{
    PyThreadState *tstate = ts->ts;

    if (exc == NULL) {
        /* Reraise */
        PyObject *exc = ts->handled_exc;
        if (exc == NULL) {
            _PyErr_SetString(tstate, PyExc_RuntimeError,
                             "No active exception to reraise");
            return 0;
        }
        ts->handled_exc = NULL;
        PyObject *type = (PyObject *)Py_TYPE(exc);
        Py_INCREF(type);
        PyObject *tb = PyException_GetTraceback(exc);
        _PyErr_Restore(tstate, type, exc, tb);
        return 1;
    }

    PyObject *type = NULL, *value = NULL, *cause = NULL;
    /* We support the following forms of raise:
       raise
       raise <instance>
       raise <type> */

    if (PyExceptionClass_Check(exc)) {
        type = exc;
        value = _PyObject_CallNoArg(exc);
        if (value == NULL)
            goto raise_error;
        if (!PyExceptionInstance_Check(value)) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "calling %R should have returned an instance of "
                          "BaseException, not %R",
                          type, Py_TYPE(value));
             goto raise_error;
        }
    }
    else if (PyExceptionInstance_Check(exc)) {
        value = exc;
        type = PyExceptionInstance_Class(exc);
        Py_INCREF(type);
    }
    else {
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        Py_DECREF(exc);
        _PyErr_SetString(tstate, PyExc_TypeError,
                         "exceptions must derive from BaseException");
        goto raise_error;
    }

    assert(type != NULL);
    assert(value != NULL);

    // if (cause) {
    //     PyObject *fixed_cause;
    //     if (PyExceptionClass_Check(cause)) {
    //         fixed_cause = _PyObject_CallNoArg(cause);
    //         if (fixed_cause == NULL)
    //             goto raise_error;
    //         Py_DECREF(cause);
    //     }
    //     else if (PyExceptionInstance_Check(cause)) {
    //         fixed_cause = cause;
    //     }
    //     else if (cause == Py_None) {
    //         Py_DECREF(cause);
    //         fixed_cause = NULL;
    //     }
    //     else {
    //         _PyErr_SetString(tstate, PyExc_TypeError,
    //                          "exception causes must derive from "
    //                          "BaseException");
    //         goto raise_error;
    //     }
    //     PyException_SetCause(value, fixed_cause);
    // }

    _PyErr_SetObject(tstate, type, value);
    /* _PyErr_SetObject incref's its arguments */
    Py_DECREF(value);
    Py_DECREF(type);
    return 0;

raise_error:
    Py_XDECREF(value);
    Py_XDECREF(type);
    Py_XDECREF(cause);
    return 0;
}

const uint32_t *
vm_exc_match(struct ThreadState *ts, PyObject *tp, const uint32_t *next_instr, int opD)
{
    static const char *CANNOT_CATCH_MSG = (
        "catching classes that do not inherit from "
        "BaseException is not allowed");

    if (PyTuple_Check(tp)) {
        Py_ssize_t i, length;
        length = PyTuple_GET_SIZE(tp);
        for (i = 0; i < length; i++) {
            PyObject *exc = PyTuple_GET_ITEM(tp, i);
            if (!PyExceptionClass_Check(exc)) {
                printf("err\n");
                _PyErr_SetString(ts->ts, PyExc_TypeError,
                                 CANNOT_CATCH_MSG);
                return NULL;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(tp)) {
            printf("err\n");
            _PyErr_SetString(ts->ts, PyExc_TypeError,
                             CANNOT_CATCH_MSG);
            return NULL;
        }
    }
    PyObject *exc = ts->handled_exc;
    int res = PyErr_GivenExceptionMatches(exc, tp);
    if (res > 0) {
        /* Exception matches -- Do nothing */;
        printf("exception matches\n");
        return next_instr;
    }
    else if (res == 0) {
        return next_instr + opD - 0x8000;
    }
    else {
        return NULL;
    }
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
    return (Register){0};
}

static int
format_name_error(struct ThreadState *ts, PyObject *name)
{
    const char *obj_str = PyUnicode_AsUTF8(name);
    if (obj_str == NULL) {
        return -1;
    }
    _PyErr_Format(ts->ts, PyExc_NameError, "name '%.200s' is not defined", obj_str);
    return -1;
}

int
vm_delete_name(struct ThreadState *ts, PyObject *name)
{
    PyObject *locals = AS_OBJ(ts->regs[0]);
    assert(PyDict_Check(locals));
    int err = PyObject_DelItem(locals, name);
    if (UNLIKELY(err != 0)) {
        return format_name_error(ts, name);
    }
    return 0;
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
        return (Register){0};
    }
    res = PyImport_ImportModuleLevelObject(
        name,
        this_func->globals,
        Py_None,
        fromlist,
        ilevel);
    if (res == NULL) {
        return (Register){0};
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
            return (Register){0};
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
        return (Register){0};
    }

error:
    abort();
    return (Register){0};
}

int vm_store_global(PyObject *dict, PyObject *name, Register acc)
{
    PyObject *value = AS_OBJ(acc);
    int err = PyDict_SetItem(dict, name, value);
    if (err < 0) {
        abort();
    }
    DECREF(acc);
    return 0;
}

int
vm_load_method(struct ThreadState *ts, PyObject *obj, PyObject *name, int opA)
{
    assert(ts->regs[opA].as_int64 == 0);
    assert(ts->regs[opA+1].as_int64 == 0);
    PyObject *descr;
    if (Py_TYPE(obj)->tp_getattro != PyObject_GenericGetAttr) {
        PyObject *value = PyObject_GetAttr(obj, name);
        if (value == NULL) {
            return -1;
        }
        ts->regs[opA] = PACK_OBJ(value);
        return 0;
    }

    PyObject **dictptr = _PyObject_GetDictPtr(obj);
    if (dictptr == NULL) {
        goto lookup_type;
    }

    PyObject *dict = *dictptr;
    if (dict == NULL) {
        goto lookup_type;
    }

    Py_INCREF(dict);
    PyObject *attr = PyDict_GetItemWithError2(dict, name);
    if (attr != NULL) {
        ts->regs[opA] = PACK_OBJ(attr);
        Py_DECREF(dict);
        return 0;
    }
    else if (UNLIKELY(_PyErr_Occurred(ts->ts) != NULL)) {
        Py_DECREF(dict);
        return -1;
    }
    Py_DECREF(dict);

lookup_type:
    descr = _PyType_Lookup(Py_TYPE(obj), name);
    if (descr == NULL) {
        goto err;
    }

    if (PyType_HasFeature(Py_TYPE(descr), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
        ts->regs[opA] = PACK_INCREF(descr);
        ts->regs[opA+1] = PACK_INCREF(obj);
        return 0;
    }

    descrgetfunc f = Py_TYPE(descr)->tp_descr_get;
    if (f != NULL) {
        PyObject *value = f(descr, obj, (PyObject *)Py_TYPE(obj));
        ts->regs[opA] = PACK_OBJ(value);
        return 0;
    }
    else {
        ts->regs[opA] = PACK_INCREF(descr);
        return 0;
    }

err:
    PyErr_Format(PyExc_AttributeError,
                 "'%.50s' object has no attribute '%U'",
                 Py_TYPE(obj)->tp_name, name);
    return -1;
}

static PyObject * _Py_NO_INLINE
vm_call_cfunction_argggg(struct ThreadState *ts, Py_ssize_t nargs)
{
    if (UNLIKELY(nargs > 255)) {
        abort();
    }
    PyObject **args = alloca((nargs + 1) * sizeof(PyObject *));
    for (int i = 0; i != nargs + 1; i++) {
        args[i] = AS_OBJ(ts->regs[i - 1]);
    }
    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    return _PyObject_VectorcallTstate(ts->ts, args[0], args + 1, nargsf, NULL);
}

PyObject *
vm_call_cfunction(struct ThreadState *ts, Py_ssize_t nargs)
{
    if (UNLIKELY(nargs > 6)) {
        return vm_call_cfunction_argggg(ts, nargs);
    }

    PyObject *args[7];
    for (int i = 0; i != nargs + 1; i++) {
        args[i] = AS_OBJ(ts->regs[i - 1]);
    }

    PyCFunctionObject *func = (PyCFunctionObject *)args[0];
    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    return func->vectorcall(args[0], args + 1, nargsf, NULL);
}

PyObject *
vm_call_function(struct ThreadState *ts, Py_ssize_t nargs)
{
    if (UNLIKELY(nargs > 6)) {
        return vm_call_cfunction_argggg(ts, nargs);
    }

    PyObject *args[7];
    for (int i = 0; i != nargs + 1; i++) {
        args[i] = AS_OBJ(ts->regs[i - 1]);
    }

    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    return _PyObject_VectorcallTstate(ts->ts, args[0], args + 1, nargsf, NULL);
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
        return (Register){0};
    }
    func->builtins = this_func->builtins;

    Py_ssize_t ncaptured = code->co_ndefaultargs + code->co_nfreevars;
    for (Py_ssize_t i = 0; i < ncaptured; i++) {
        Py_ssize_t r = code->co_free2reg[i*2];
        PyObject *var = AS_OBJ(ts->regs[r]);
        assert(i < code->co_ndefaultargs || PyCell_Check(var));

        Py_INCREF(var);
        func->freevars[i] = var;
    }

    return PACK_OBJ((PyObject *)func);
}

static int
vm_setup_cells(struct ThreadState *ts, PyCodeObject2 *code)
{
    Py_ssize_t ncells = code->co_ncells;
    for (Py_ssize_t i = 0; i < ncells; i++) {
        Py_ssize_t idx = code->co_cell2reg[i];
        PyObject *cell = PyCell_New(AS_OBJ(ts->regs[idx]));
        if (UNLIKELY(cell == NULL)) {
            return -1;
        }

        Register prev = ts->regs[idx];
        ts->regs[idx] = PACK(cell, REFCOUNT_TAG);
        DECREF(prev);
    }
    return 0;
}

static void
vm_setup_freevars(struct ThreadState *ts, PyCodeObject2 *code)
{
    PyFunc *this_func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    Py_ssize_t nfreevars = code->co_nfreevars;
    for (Py_ssize_t i = 0; i < nfreevars; i++) {
        Py_ssize_t r = code->co_free2reg[i*2+1];
        PyObject *cell = this_func->freevars[i];
        assert(PyCell_Check(cell));
        ts->regs[r] = PACK(cell, NO_REFCOUNT_TAG);
    }
}

int
vm_setup_cells_freevars(struct ThreadState *ts, PyCodeObject2 *code)
{
    int err;
    if (code->co_ncells != 0) {
        err = vm_setup_cells(ts, code);
        if (UNLIKELY(err != 0)) {
            return err;
        }
    }
    if (code->co_nfreevars != 0) {
        vm_setup_freevars(ts, code);
    }
    return 0;
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
        return (Register){0};
    }
    while (n) {
        n--;
        PyList_SET_ITEM(obj, n, vm_object_steal(regs[n]));
        regs[n].as_int64 = 0;
    }
    return PACK(obj, REFCOUNT_TAG);
}

Register
vm_build_set(struct ThreadState *ts, Py_ssize_t base, Py_ssize_t n)
{
    PyObject *set = PySet_New(NULL);
    if (UNLIKELY(set == NULL)) {
        return (Register){0};
    }

    for (Py_ssize_t i = 0; i != n; i++) {
        PyObject *item = AS_OBJ(ts->regs[base + i]);
        int err = PySet_Add(set, item);
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        Register r = ts->regs[base + i];
        ts->regs[base + i].as_int64 = 0;
        DECREF(r);
    }
    return PACK(set, REFCOUNT_TAG);

error:
    Py_DECREF(set);
    return (Register){0};
}

Register
vm_build_tuple(Register *regs, Py_ssize_t n)
{
    PyObject *obj = PyTuple_New(n);
    if (obj == NULL) {
        return (Register){0};
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

static PyObject *
vm_unimplemented(/*intentionally empty*/)
{
    printf("calling unimplemented intrinsic!\n");
    abort();
}

static PyObject *
vm_format_value(PyObject *value)
{
    if (PyUnicode_CheckExact(value)) {
        Py_INCREF(value);
        return value;
    }
    return PyObject_Format(value, NULL);
}

static PyObject *
vm_format_value_spec(PyObject * const *args, Py_ssize_t nargs)
{
    assert(nargs == 2);
    return PyObject_Format(args[0], args[1]);
}

static PyObject *
vm_build_string(PyObject *const*args, Py_ssize_t nargs)
{
    PyObject *empty = PyUnicode_New(0, 0);
    assert(empty != NULL && _PyObject_IS_IMMORTAL(empty));
    return _PyUnicode_JoinArray(empty, args, nargs);
}

PyObject *
vm_call_intrinsic(struct ThreadState *ts, Py_ssize_t id, Py_ssize_t opA, Py_ssize_t nargs)
{
    intrinsicN fn = intrinsics_table[id].intrinsicN;
    PyObject **args = alloca(nargs * sizeof(PyObject *));
    for (Py_ssize_t i = 0; i < nargs; i++) {
        args[i] = AS_OBJ(ts->regs[opA + i]);
    }
    PyObject *res = fn(args, nargs);
    if (UNLIKELY(res == NULL)) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        Register prev = ts->regs[opA + i];
        ts->regs[opA + i].as_int64 = 0;
        DECREF(prev);
    }
    return res;
}

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed)
{
    printf("vm_resize_stack\n");
    abort();
    return 0;
}

PyObject *
vm_args_error(struct ThreadState *ts)
{
    PyErr_Format(PyExc_TypeError, "wrong number of arguments");
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
    // TODO: fill in exception stack trace
    printf("vm_handle_error\n");
    abort();
}

int
vm_init_stack(struct ThreadState *ts, Py_ssize_t stack_size)
{
    Register *stack = mi_malloc(stack_size * sizeof(Register));
    if (UNLIKELY(stack == NULL)) {
        PyErr_SetNone(PyExc_MemoryError);
        return -1;
    }

    memset(stack, 0, stack_size * sizeof(Register));
    ts->stack = stack;
    ts->regs = stack;
    ts->maxstack = stack + stack_size;
    return 0;
}

struct ThreadState *
new_threadstate(void)
{
    struct ThreadState *ts = malloc(sizeof(struct ThreadState));
    if (ts == NULL) {
        PyErr_SetNone(PyExc_MemoryError);
        return NULL;
    }
    memset(ts, 0, sizeof(struct ThreadState));

    Py_ssize_t stack_size = 256;
    if (UNLIKELY(vm_init_stack(ts, stack_size) != 0)) {
        free(ts);
        return NULL;
    }
    ts->ts = PyThreadState_GET();
    return ts;
}

void
vm_free_stack(struct ThreadState *ts)
{
    // printf("vm_free_stack: %p %zd\n", ts);
    assert(ts->regs > ts->stack);
    for (;;) {
        PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[-1]);
        Py_ssize_t frame_size = vm_frame_size(ts);
        for (Py_ssize_t i = frame_size - 1; i >= -1; --i) {
            Register value = ts->regs[i];
            if (value.as_int64 != 0) {
                ts->regs[i].as_int64 = 0;
                DECREF(value);
            }
        }
        uintptr_t frame_link = ts->regs[-2].as_int64;
        ts->regs[-2].as_int64 = 0;
        ts->regs[-3].as_int64 = 0;
        if ((frame_link & FRAME_TAG_MASK) != FRAME_PYTHON) {
            Py_ssize_t frame_delta = ts->regs[-4].as_int64;
            ts->regs[-4].as_int64 = 0;
            ts->regs -= frame_delta;
            break;
        }
        const uint32_t *next_instr = (const uint32_t *)frame_link;
        // this is the call that dispatched to us
        uint32_t call = next_instr[-1];
        intptr_t offset = (call >> 8) & 0xFF;
        printf("offset = %zd\n", offset);
        ts->regs -= offset;
    }
    assert(ts->regs == ts->stack);
}


void vm_free_threadstate(struct ThreadState *ts)
{
    // printf("vm_free_threadstate: %p\n", ts);
    if (ts->regs != ts->stack) {
        vm_free_stack(ts);
    }
    mi_free(ts->stack);
    ts->stack = ts->regs = ts->maxstack = NULL;
}

int
vm_for_iter_exc(struct ThreadState *ts)
{
    assert(PyErr_Occurred());
    PyThreadState *tstate = ts->ts;
    if (!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
        return -1;
    }
    // else if (tstate->c_tracefunc != NULL) {
    //     call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, f);
    // }
    _PyErr_Clear(tstate);
    return 0;
}

int
vm_init_thread_state(struct ThreadState *old, struct ThreadState *ts)
{
    memset(ts, 0, sizeof(*ts));

    Py_ssize_t generator_stack_size = 256;
    if (UNLIKELY(vm_init_stack(ts, generator_stack_size) != 0)) {
        return -1;
    }

    ts->thread_type = THREAD_GENERATOR;

    PyFunc *func = (PyFunc *)AS_OBJ(old->regs[-1]);
    PyCodeObject2 *code = PyCode2_FromFunc(func);

    // copy over func and arguments
    Py_ssize_t frame_delta = CFRAME_EXTRA;
    ts->regs += frame_delta;
    ts->regs[-4].as_int64 = frame_delta;
    ts->regs[-3] = old->regs[-3];   // copy constants
    ts->regs[-2].as_int64 = FRAME_GENERATOR;
    ts->regs[-1] = old->regs[-1];   // copy func

    // The new thread-state takes ownership of the "func" and constants.
    // We can't clear the old thread states values because they will be
    // referenced (and cleared) by RETURN_VALUE momentarily. Instead, just
    // mark them as non-refcounted references -- the generator owns them now.
    old->regs[-3].as_int64 |= NO_REFCOUNT_TAG;
    old->regs[-1].as_int64 |= NO_REFCOUNT_TAG;

    Py_ssize_t nargs = code->co_argcount;
    for (Py_ssize_t i = 0; i < nargs; i++) {
        ts->regs[i] = old->regs[i];
        old->regs[i].as_int64 = 0;
    }
    ts->ts = PyThreadState_GET();
    return 0;
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

    static uint32_t func_vector_call[] = {
        CFUNC_HEADER
    };

    PyObject *builtins_name = _PyUnicode_FromId(&PyId_builtins);
    PyObject *builtins = PyImport_GetModule(builtins_name);
    PyObject *builtins_dict = PyModule_GetDict(builtins);
    Py_ssize_t i = 0;
    PyObject *key, *value;
    while (PyDict_Next(builtins_dict, &i, &key, &value)) {
        if (PyCFunction_Check(value)) {
            ((PyFuncBase *)value)->first_instr = &func_vector_call[0];
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

static inline Register
PACK_FRAME_LINK(const uint32_t *next_instr, int tag)
{
    Register r;
    r.as_int64 = (intptr_t)next_instr + tag;
    return r;
}

static void
setup_frame(struct ThreadState *ts, PyFunc *func)
{
    Py_ssize_t frame_delta = vm_frame_size(ts);
    frame_delta += CFRAME_EXTRA;

    ts->regs += frame_delta;

    PyCodeObject2 *code = PyCode2_FromFunc(func);
    ts->regs[-4].as_int64 = frame_delta;
    ts->regs[-3].as_int64 = (intptr_t)code->co_constants;
    ts->regs[-2] = PACK_FRAME_LINK(ts->next_instr, FRAME_C);
    ts->regs[-1] = PACK(func, NO_REFCOUNT_TAG); // this_func
}

PyObject *
_PyEval_FastCall(PyFunc *func, PyObject *locals)
{
    struct ThreadState *ts = gts;
    Py_ssize_t nargs = 0;
    const uint32_t *pc;

    setup_frame(ts, func);
    ts->regs[0] = PACK(locals, NO_REFCOUNT_TAG);

    pc = PyCode2_GET_CODE(PyCode2_FromFunc(func));
    return _PyEval_Fast(ts, nargs, pc);
}

PyObject *
_PyEval_FastCallArgs(PyFunc *func, PyObject *args)
{
    struct ThreadState *ts = gts;
    Py_ssize_t nargs = 0;
    setup_frame(ts, func);
    const uint32_t *pc = PyCode2_GET_CODE(PyCode2_FromFunc(func));
    if (args != NULL) {
        Py_ssize_t n = PyTuple_GET_SIZE(args);
        nargs = n;
        for (Py_ssize_t i = 0; i != n; i++) {
            ts->regs[i] = PACK(PyTuple_GET_ITEM(args, i), NO_REFCOUNT_TAG);
        }
    }
    return _PyEval_Fast(ts, nargs, pc);
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

    if (globals == NULL) {
        globals = make_globals();
        if (globals == NULL) {
            return NULL;
        }
    }

    if (PyDict_GetItemString(globals, "__builtins__") == NULL) {
        int err = PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
        (void)err;
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
    Py_ssize_t frame_delta = CFRAME_EXTRA;

    ts->regs += frame_delta;
    ts->regs[-4].as_int64 = frame_delta;
    ts->regs[-3].as_int64 = (intptr_t)PyCode2_FromFunc(func)->co_constants;
    ts->regs[-2] = PACK_FRAME_LINK(ts->next_instr, FRAME_C);
    ts->regs[-1] = PACK(func, NO_REFCOUNT_TAG); // this_func
    ts->regs[0] = PACK(globals, NO_REFCOUNT_TAG);

    Py_ssize_t nargs = 0;
    const uint32_t *pc = PyCode2_GET_CODE(code);

    PyObject *ret = _PyEval_Fast(ts, nargs, pc);

#ifdef Py_REF_DEBUG
    intptr_t newrc = _PyThreadState_GET()->thread_ref_total;
    printf("RC %ld to %ld (%ld)\n", (long)oldrc, (long)newrc, (long)(newrc - oldrc));
    assert(ts->regs == ts->stack);
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


PyObject *
vm_import_from(struct ThreadState *ts, PyObject *v, PyObject *name)
{
    _Py_IDENTIFIER(__name__);
    PyObject *x;
    PyObject *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;

    if (_PyObject_LookupAttr(v, name, &x) != 0) {
        return x;
    }
    /* Issue #17636: in case this failed because of a circular relative
       import, try to fallback on reading the module directly from
       sys.modules. */
    pkgname = _PyObject_GetAttrId(v, &PyId___name__);
    if (pkgname == NULL) {
        goto error;
    }
    if (!PyUnicode_Check(pkgname)) {
        Py_CLEAR(pkgname);
        goto error;
    }
    fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
    if (fullmodname == NULL) {
        Py_DECREF(pkgname);
        return NULL;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == NULL && !_PyErr_Occurred(ts->ts)) {
        goto error;
    }
    Py_DECREF(pkgname);
    return x;
 error:
    pkgpath = PyModule_GetFilenameObject(v);
    if (pkgname == NULL) {
        pkgname_or_unknown = PyUnicode_FromString("<unknown module name>");
        if (pkgname_or_unknown == NULL) {
            Py_XDECREF(pkgpath);
            return NULL;
        }
    } else {
        pkgname_or_unknown = pkgname;
    }

    if (pkgpath == NULL || !PyUnicode_Check(pkgpath)) {
        _PyErr_Clear(ts->ts);
        errmsg = PyUnicode_FromFormat(
            "cannot import name %R from %R (unknown location)",
            name, pkgname_or_unknown
        );
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, NULL);
    }
    else {
        _Py_IDENTIFIER(__spec__);
        PyObject *spec = _PyObject_GetAttrId(v, &PyId___spec__);
        const char *fmt =
            _PyModuleSpec_IsInitializing(spec) ?
            "cannot import name %R from partially initialized module %R "
            "(most likely due to a circular import) (%S)" :
            "cannot import name %R from %R (%S)";
        Py_XDECREF(spec);

        errmsg = PyUnicode_FromFormat(fmt, name, pkgname_or_unknown, pkgpath);
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, pkgpath);
    }

    Py_XDECREF(errmsg);
    Py_XDECREF(pkgname_or_unknown);
    Py_XDECREF(pkgpath);
    return NULL;
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

static PyObject*
func_repr(PyFunc *op)
{
    PyCodeObject2 *code = PyCode2_FromFunc(op);
    return PyUnicode_FromFormat("<function %U at %p>",
                               code->co_name, op);
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
    return (PyObject *)method;
}

PyTypeObject PyFunc_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "PyFunc",
    .tp_doc = "PyFunc doc",
    .tp_basicsize = sizeof(PyFunc),
    .tp_itemsize = sizeof(PyObject*),
    .tp_call = func_call,
    .tp_descr_get = func_descr_get,
    .tp_repr = (reprfunc)func_repr,
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

#include "ceval_intrinsics.h"
