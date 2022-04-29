#include "Python.h"

#include "ceval_meta.h"

#include "frameobject.h"
#include "opcode.h"
#include "opcode_names.h"

#include "pycore_abstract.h"
#include "pycore_call.h"
#include "pycore_ceval.h"
#include "pycore_code.h"
#include "pycore_generator.h"
#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pymem.h"         // _PyMem_IsPtrFreed()
#include "pycore_pystate.h"
#include "pycore_qsbr.h"
#include "pycore_refcnt.h"
#include "pycore_stackwalk.h"
#include "pycore_sysmodule.h"
#include "pycore_traceback.h"
#include "pycore_tupleobject.h"

#include "mimalloc.h"

#include <ctype.h>

struct ThreadState *
vm_active(PyThreadState *tstate) {
    struct ThreadState *active = tstate->active;
    if (active) {
        active->regs = tstate->regs;
        active->pc = tstate->pc;
    }
    return active;
}

static PyObject *
vm_object_steal(Register* addr) {
    Register reg = *addr;
    addr->as_int64 = 0;
    PyObject *obj = AS_OBJ(reg);
    if (!IS_RC(reg)) {
        Py_INCREF(obj);
    }
    return obj;
}

Py_ssize_t
vm_regs_frame_size(Register *regs)
{
    PyObject *this_func = AS_OBJ(regs[-1]);
    if (this_func == NULL) {
        return 0;
    }
    if (!PyFunction_Check(this_func)) {
        return regs[-2].as_int64;
    }
    return _PyFunction_GET_CODE((PyFunctionObject *)this_func)->co_framesize;
}

static Py_ssize_t
vm_frame_size(PyThreadState *ts)
{
    if (ts->regs == ts->stack) {
        return 0;
    }
    return vm_regs_frame_size(ts->regs);
}

#define DECREF(reg) do {                                                    \
    if (IS_RC(reg)) {                                                       \
        _Py_DEC_REFTOTAL;                                                   \
        PyObject *obj = AS_OBJ(reg);                                        \
        if (_PY_LIKELY(_Py_ThreadLocal(obj))) {                             \
            uint32_t refcount = obj->ob_ref_local;                          \
            refcount -= (1 << _Py_REF_LOCAL_SHIFT);                         \
            obj->ob_ref_local = refcount;                                   \
            if (_PY_UNLIKELY(refcount == 0)) {                              \
                _Py_MergeZeroRefcount(obj);                                 \
            }                                                               \
        }                                                                   \
        else {                                                              \
            _Py_DecRefShared(obj);                                          \
        }                                                                   \
    }                                                                       \
} while (0)

Register vm_unknown_opcode(intptr_t opcode)
{
    printf("vm_unknown_opcode: %d (%s)\n", (int)opcode, opcode_names[opcode]);
    abort();
}

static int
vm_opcode(const uint8_t *pc)
{
    int opcode = pc[0];
    if (opcode == WIDE) {
        opcode = pc[1];
    }
    return opcode;
}

static int
vm_oparg(const uint8_t *pc, int idx)
{
    if (pc[0] == WIDE) {
        uint32_t arg;
        memcpy(&arg, &pc[(idx * 4) + 2], sizeof(uint32_t));
        return (int)arg;
    }
    return (int)pc[idx + 1];
}

static PyObject *
vm_constant(PyThreadState *ts, int idx)
{
    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyCodeObject *code = _PyFunction_GET_CODE(func);

    int oparg = vm_oparg(ts->pc, idx);
    return code->co_constants[oparg];
}

static Register _Py_NO_INLINE
attribute_error(PyThreadState *tstate, _Py_Identifier *id)
{
    Register error = {0};
    if (!_PyErr_Occurred(tstate)) {
        _PyErr_SetObject(tstate, PyExc_AttributeError, id->object);
    }
    return error;
}

Register
vm_setup_with(PyThreadState *ts, Py_ssize_t opA)
{
    _Py_IDENTIFIER(__enter__);
    _Py_IDENTIFIER(__exit__);

    PyObject *mgr = AS_OBJ(ts->regs[opA]);
    PyObject *exit = _PyObject_LookupSpecial(mgr, &PyId___exit__);
    if (UNLIKELY(exit == NULL)) {
        return attribute_error(ts, &PyId___exit__);
    }
    ts->regs[opA + 1] = PACK_OBJ(exit);
    PyObject *enter = _PyObject_LookupSpecial(mgr, &PyId___enter__);
    if (UNLIKELY(enter == NULL)) {
        return attribute_error(ts, &PyId___enter__);
    }
    PyObject *res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    if (UNLIKELY(res == NULL)) {
        return (Register){0};
    }
    return PACK_OBJ(res);
}


Register
vm_setup_async_with(PyThreadState *ts, Py_ssize_t opA)
{
    _Py_IDENTIFIER(__aenter__);
    _Py_IDENTIFIER(__aexit__);

    PyObject *mgr = AS_OBJ(ts->regs[opA]);
    PyObject *exit = _PyObject_LookupSpecial(mgr, &PyId___aexit__);
    if (UNLIKELY(exit == NULL)) {
        return attribute_error(ts, &PyId___aexit__);
    }
    ts->regs[opA + 1] = PACK_OBJ(exit);
    PyObject *enter = _PyObject_LookupSpecial(mgr, &PyId___aenter__);
    if (UNLIKELY(enter == NULL)) {
        return attribute_error(ts, &PyId___aenter__);
    }
    PyObject *res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    if (UNLIKELY(res == NULL)) {
        return (Register){0};
    }
    return PACK_OBJ(res);}

static void
vm_clear_regs(PyThreadState *ts, Py_ssize_t lo, Py_ssize_t hi);

int
vm_stack_walk_lineno(struct stack_walk *w)
{
    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w->regs[-1]);
    PyCodeObject *co = _PyFunction_GET_CODE(func);
    int addrq = (int)(w->pc - PyCode_FirstInstr(co));
    return PyCode_Addr2Line(co, addrq);
}

void
vm_dump_stack(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = PyThread_tss_get(&runtime->gilstate.autoTSSkey);
    if (tstate == NULL) {
        fprintf(stderr, "no thread state\n");
        return;
    }

    struct ThreadState *ts = vm_active(tstate);
    if (ts == NULL) {
        fprintf(stderr, "no vm thread state\n");
        return;
    }

    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w.regs[-1]);
        PyCodeObject *co = _PyFunction_GET_CODE(func);
        int line = vm_stack_walk_lineno(&w);

        fprintf(stderr, "File \"%s\", line %d, in %s\n",
            PyUnicode_AsUTF8(co->co_filename),
            line,
            PyUnicode_AsUTF8(func->func_name));
    }
}

Py_ssize_t
vm_stack_depth(PyThreadState *tstate)
{
    struct ThreadState *ts = vm_active(tstate);
    if (!ts) {
        return 0;
    }
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    Py_ssize_t n = 0;
    while (vm_stack_walk_all(&w)) {
        n++;
    }
    return n;
}

/* returns the currently handled exception or NULL */
PyObject *
vm_handled_exc(PyThreadState *ts)
{
    struct stack_walk w;
    vm_stack_walk_init(&w, vm_active(ts));
    while (vm_stack_walk(&w)) {
        PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w.regs[-1]);
        PyCodeObject *code = _PyFunction_GET_CODE(func);

        const uint8_t *first_instr = PyCode_FirstInstr(code);
        Py_ssize_t instr_offset = (w.pc - first_instr);  // FIXME!

        // Find the inner-most active except/finally block. Note that because
        // try-blocks are stored inner-most to outer-most, the except/finally
        // blocks have the opposite nesting order: outer-most to inner-most.
        struct _PyHandlerTable *table = code->co_exc_handlers;
        for (Py_ssize_t i = table->size - 1; i >= 0; i--) {
            ExceptionHandler *eh = &table->entries[i];
            Py_ssize_t start = eh->handler;
            Py_ssize_t end = eh->handler_end;
            if (start <= instr_offset && instr_offset < end) {
                Py_ssize_t link_reg = eh->reg;
                if (w.regs[link_reg].as_int64 != -1) {
                    // not handling an exception
                    continue;
                }
                return AS_OBJ(w.regs[link_reg+1]);
            }
        }
    }
    return NULL;
}

/* returns the currently handled exception or NULL */
PyObject *
vm_handled_exc2(struct ThreadState *ts)
{
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w.regs[-1]);
        PyCodeObject *code = _PyFunction_GET_CODE(func);

        const uint8_t *first_instr = PyCode_FirstInstr(code);
        Py_ssize_t instr_offset = (w.pc - first_instr);  // FIXME!

        // Find the inner-most active except/finally block. Note that because
        // try-blocks are stored inner-most to outer-most, the except/finally
        // blocks have the opposite nesting order: outer-most to inner-most.
        struct _PyHandlerTable *table = code->co_exc_handlers;
        for (Py_ssize_t i = table->size - 1; i >= 0; i--) {
            ExceptionHandler *eh = &table->entries[i];
            Py_ssize_t start = eh->handler;
            Py_ssize_t end = eh->handler_end;
            if (start <= instr_offset && instr_offset < end) {
                Py_ssize_t link_reg = eh->reg;
                if (w.regs[link_reg].as_int64 != -1) {
                    // not handling an exception
                    continue;
                }
                return AS_OBJ(w.regs[link_reg+1]);
            }
        }
    }
    return NULL;
}

int
vm_set_handled_exc(PyThreadState *ts, PyObject *exc)
{
    if (exc == NULL) {
        exc = Py_None;
    }

    struct stack_walk w;
    vm_stack_walk_init(&w, vm_active(ts));
    while (vm_stack_walk(&w)) {
        PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w.regs[-1]);
        PyCodeObject *code = _PyFunction_GET_CODE(func);

        const uint8_t *first_instr = PyCode_FirstInstr(code);
        Py_ssize_t instr_offset = (w.pc - first_instr);  // FIXME!

        // Find the inner-most active except/finally block. Note that because
        // try-blocks are stored inner-most to outer-most, the except/finally
        // blocks have the opposite nesting order: outer-most to inner-most.
        struct _PyHandlerTable *table = code->co_exc_handlers;
        for (Py_ssize_t i = table->size - 1; i >= 0; i--) {
            ExceptionHandler *eh = &table->entries[i];
            Py_ssize_t start = eh->handler;
            Py_ssize_t end = eh->handler_end;
            if (start <= instr_offset && instr_offset < end) {
                Py_ssize_t link_reg = eh->reg;
                if (w.regs[link_reg].as_int64 != -1) {
                    // not handling an exception
                    continue;
                }

                CLEAR(w.regs[link_reg+1]);
                w.regs[link_reg+1] = PACK_INCREF(exc);
                return 0;
            }
        }
    }

    return -1;
}

#define IS_OBJ(r) (((r).as_int64 & NON_OBJECT_TAG) != NON_OBJECT_TAG)

PyObject *
vm_compute_cr_origin(PyThreadState *ts)
{
    int origin_depth = ts->coroutine_origin_tracking_depth;
    assert(origin_depth > 0);

    // First count how many frames we have
    int frame_count = 0;

    struct stack_walk w;
    vm_stack_walk_init(&w, vm_active(ts));
    vm_stack_walk(&w);  // skip the first frame
    while (vm_stack_walk(&w) && frame_count < origin_depth) {
        ++frame_count;
    }

    // Now collect them
    PyObject *cr_origin = PyTuple_New(frame_count);
    if (cr_origin == NULL) {
        return NULL;
    }

    int i = 0;
    vm_stack_walk_init(&w, vm_active(ts));
    vm_stack_walk(&w);  // skip the first frame
    while (vm_stack_walk(&w) && i < frame_count) {
        PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w.regs[-1]);
        PyCodeObject *code = _PyFunction_GET_CODE(func);
        int lineno = vm_stack_walk_lineno(&w);

        PyObject *frameinfo = Py_BuildValue(
            "OiO",
            code->co_filename,
            lineno,
            code->co_name);
        if (!frameinfo) {
            Py_DECREF(cr_origin);
            return NULL;
        }

        PyTuple_SET_ITEM(cr_origin, i, frameinfo);
        i++;
    }

    return cr_origin;
}

static int
vm_exit_with_exc(PyThreadState *ts, Py_ssize_t opA)
{
    if (ts->regs[opA].as_int64 == 0) {
        // immediately re-raise
        Register reg = ts->regs[opA + 3];
        ts->regs[opA + 3].as_int64 = 0;
        return vm_reraise(ts, reg);
    }

    PyObject *exit = AS_OBJ(ts->regs[opA + 1]);
    PyObject *res;

    PyObject *exc = AS_OBJ(ts->regs[opA + 3]);
    assert(exc != NULL && exc == vm_handled_exc(ts));
    PyObject *type = (PyObject *)Py_TYPE(exc);
    PyObject *tb = ((PyBaseExceptionObject *)exc)->traceback;
    Py_INCREF(tb);  // keep traceback alive for duration of call
    PyObject *stack[4] = {NULL, type, exc, tb};
    Py_ssize_t nargsf = 3 | PY_VECTORCALL_ARGUMENTS_OFFSET;
    res = _PyObject_Vectorcall(exit, stack + 1, nargsf, NULL);
    Py_DECREF(tb);
    if (UNLIKELY(res == NULL)) {
        return -1;
    }
    return vm_exit_with_res(ts, opA, res);
}

int
vm_exit_with_res(PyThreadState *ts, Py_ssize_t opA, PyObject *exit_res)
{
    assert(ts->regs[opA + 2].as_int64 == -1);
    int is_true = PyObject_IsTrue(exit_res);
    Py_DECREF(exit_res);
    if (UNLIKELY(is_true < 0)) {
        return -1;
    }
    if (UNLIKELY(is_true == 1)) {
        // ignore the exception and continue
        vm_clear_regs(ts, opA, opA + 4);
        return 0;
    }

    // re-raise the exception
    Register reg = ts->regs[opA + 3];
    ts->regs[opA + 3].as_int64 = 0;
    return vm_reraise(ts, reg);
}

/* returns 0 on success, -1 on error, and -2 on re-raise */
int
vm_exit_with(PyThreadState *ts, Py_ssize_t opA)
{
    int64_t link = ts->regs[opA + 2].as_int64;
    if (UNLIKELY(link == -1)) {
        return vm_exit_with_exc(ts, opA);
    }

    assert(ts->regs[opA].as_int64 != 0);
    assert(ts->regs[opA + 2].as_int64 == 0);
    assert(ts->regs[opA + 3].as_int64 == 0);

    PyObject *res;
    // PyObject *mgr = AS_OBJ(ts->regs[opA]);
    PyObject *exit = AS_OBJ(ts->regs[opA + 1]);

    PyObject *stack[4] = {NULL, Py_None, Py_None, Py_None};
    Py_ssize_t nargsf = 3 | PY_VECTORCALL_ARGUMENTS_OFFSET;
    res = _PyObject_VectorcallTstate(ts, exit, stack + 1, nargsf, NULL);
    CLEAR(ts->regs[opA]);
    CLEAR(ts->regs[opA + 1]);
    if (UNLIKELY(res == NULL)) {
        return -1;
    }
    Py_DECREF(res);
    return 0;
}

int
vm_exit_async_with(PyThreadState *ts, Py_ssize_t opA)
{
    PyObject *exit = AS_OBJ(ts->regs[opA + 1]);
    int64_t link = ts->regs[opA + 2].as_int64;

    PyObject *stack[4];
    stack[0] = NULL;
    if (link == -1) {
        PyObject *exc = AS_OBJ(ts->regs[opA + 3]);
        assert(exc != NULL && exc == vm_handled_exc(ts));
        stack[1] = (PyObject *)Py_TYPE(exc);
        stack[2] = exc;
        stack[3] = ((PyBaseExceptionObject *)exc)->traceback;
    }
    else {
        stack[1] = Py_None;
        stack[2] = Py_None;
        stack[3] = Py_None;
    }
    // Ensure the traceback is kept alive for duration of call, even if it is
    // replaced on the exception object.
    Py_INCREF(stack[3]);
    Py_ssize_t nargsf = 3 | PY_VECTORCALL_ARGUMENTS_OFFSET;
    PyObject *obj = _PyObject_VectorcallTstate(ts, exit, stack + 1, nargsf, NULL);
    Py_DECREF(stack[3]);
    if (obj == NULL) {
        return -1;
    }
    CLEAR(ts->regs[opA]);
    CLEAR(ts->regs[opA + 1]);
    ts->regs[opA] = PACK_OBJ(obj);

    // convert obj to awaitable (effectively GET_AWAITABLE)
    if (PyCoro_CheckExact(obj)) {
        PyObject *yf = ((PyCoroObject *)obj)->base.yield_from;
        if (UNLIKELY(yf != NULL)) {
            vm_err_coroutine_awaited(ts);
            return -1;
        }
    }
    else {
        PyObject *iter = _PyCoro_GetAwaitableIter(obj);
        if (iter == NULL) {
            _PyErr_Format(ts, PyExc_TypeError,
                          "'async with' received an object from __aexit__ "
                          "that does not implement __await__: %.100s",
                          Py_TYPE(obj)->tp_name);
            return -1;
        }
        CLEAR(ts->regs[opA]);
        ts->regs[opA] = PACK_OBJ(iter);
    }
    return 0;
}

static void
vm_clear_regs(PyThreadState *ts, Py_ssize_t lo, Py_ssize_t hi)
{
    // clear regs in range [lo, hi)
    assert(lo <= hi);
    Py_ssize_t n = hi;
    Py_ssize_t depth = ts->regs - ts->stack;
    while (n != lo) {
        n--;
        Register tmp = ts->regs[n];
        if (tmp.as_int64 != 0) {
            ts->regs[n].as_int64 = 0;
            DECREF(tmp);
        }
    }

    // Asserts that the DECREF() calls did not re-entrantly pop this frame
    // from underneath us.
    assert((ts->regs - ts->stack) == depth && "frame moved underneath");
    (void)depth;
}

static intptr_t
vm_pop_frame(PyThreadState *ts)
{
    assert(ts->regs > ts->stack);
    Py_ssize_t frame_size = vm_frame_size(ts);
    if (ts->regs + frame_size > ts->maxstack) {
        // Ensure we don't exceed maxstack in case we're popping a partially
        // setup frame (e.g. CALL_FUNCTION_EX).
        frame_size = ts->maxstack - ts->regs;
    }
    int is_pyfunc = PyFunction_Check(AS_OBJ(ts->regs[-1]));
    if (is_pyfunc && ts->regs[-2].as_int64 != 0) {
        vm_clear_frame(ts);
    }
    vm_clear_regs(ts, -1, frame_size);
    intptr_t frame_delta = ts->regs[-4].as_int64;
    intptr_t frame_link = ts->regs[-3].as_int64;
    ts->regs[-2].as_int64 = 0;
    ts->regs[-3].as_int64 = 0;
    ts->regs[-4].as_int64 = 0;
    ts->regs -= frame_delta;
    return frame_link;
}

// Finds the inner most exception handler for the current instruction.
// Exception handlers are stored in inner-most to outer-most order.
static ExceptionHandler *
vm_exception_handler(PyCodeObject *code, const uint8_t *pc)
{
    const uint8_t *first_instr = PyCode_FirstInstr(code);
    Py_ssize_t instr_offset = (pc - first_instr);

    struct _PyHandlerTable *table = code->co_exc_handlers;
    for (Py_ssize_t i = 0, n = table->size; i < n; i++) {
        ExceptionHandler *eh = &table->entries[i];
        Py_ssize_t start = eh->start;
        Py_ssize_t end = eh->handler;
        if (start <= instr_offset && instr_offset < end) {
            return eh;
        }
    }
    return NULL;
}

static void
vm_trace_err(PyThreadState *ts, PyObject **type, PyObject **value, PyObject **traceback);

static void
vm_trace_active_exc(PyThreadState *ts);

static int
vm_trace_return(PyThreadState *ts);

// Clears the arguments to a failed function call. This is necessary
// when the function is the outermost call into the interpreter, because
// the calling code assumes the interpreter will clean-up the frame.
// For simplicity, we clean-up here for all Python functions, not just
// the outermost calls.
static void
vm_func_header_clear_frame(PyThreadState *ts, Register acc)
{
    if ((acc.as_int64 & (ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS)) != 0) {
        XCLEAR(ts->regs[-FRAME_EXTRA - 2]);
        XCLEAR(ts->regs[-FRAME_EXTRA - 1]);
        return;
    }
    if ((acc.as_int64 & ACC_MASK_KWARGS) != 0) {
        XCLEAR(ts->regs[-FRAME_EXTRA - 1]);
    }
    while ((acc.as_int64 & ACC_MASK_KWARGS) != 0) {
        Py_ssize_t kwdpos = -FRAME_EXTRA - ACC_KWCOUNT(acc) - 1;
        XCLEAR(ts->regs[kwdpos]);
        acc.as_int64 -= (1 << ACC_SHIFT_KWARGS);
    }
    assert(acc.as_int64 <= 255);
    while ((acc.as_int64 & ACC_MASK_ARGS) != 0) {
        Py_ssize_t pos = acc.as_int64 - 1;
        XCLEAR(ts->regs[pos]);
        acc.as_int64 -= 1;
    }
}

// Unwinds the stack looking for the nearest exception handler. Returns
// the program counter (PC) of the exception handler block, or NULL if
// there are no handlers before the next C frame.
const uint8_t *
vm_exception_unwind(PyThreadState *ts, Register acc, bool skip_first_frame)
{
    if (!PyErr_Occurred()) {
        PyObject *callable = AS_OBJ(ts->regs[-1]);
        if (callable)
            PyErr_Format(PyExc_SystemError,
                         "%R returned NULL without setting an error",
                         callable);
        else
            PyErr_Format(PyExc_SystemError,
                         "a function returned NULL without setting an error");
#ifdef Py_DEBUG
        /* Ensure that the bug is caught in debug mode.
            Py_FatalError() logs the SystemError exception raised above. */
        Py_FatalError("a function returned NULL without setting an error");
#endif
    }

    assert(PyErr_Occurred());
    assert(ts->regs > ts->stack);
    assert(ts == PyThreadState_GET());

    // Clear the accumulator, unless the exception happened during FUNC_HEADER,
    // in which case the accumulator stores a representation of the number of
    // arguments.
    if (vm_opcode(ts->pc) == FUNC_HEADER) {
        vm_func_header_clear_frame(ts, acc);
    }
    else if (acc.as_int64 != 0) {
        DECREF(acc);
    }

    PyObject *exc = NULL, *val = NULL, *tb = NULL;
    _PyErr_Fetch(ts, &exc, &val, &tb);

    bool skip_frame = skip_first_frame;
    const uint8_t *pc = ts->pc;
    for (;;) {
        PyObject *callable = AS_OBJ(ts->regs[-1]);
        if (!PyFunction_Check(callable)) {
            goto next;
        }

        PyFunctionObject *func = (PyFunctionObject *)callable;
        PyCodeObject *code = _PyFunction_GET_CODE(func);
        if (pc == func->func_base.first_instr) {
            goto next;
        }

        if (!skip_frame) {
            PyFrameObject *frame = vm_frame(ts);
            PyObject *newtb = NULL;
            if (frame != NULL) {
                newtb = _PyTraceBack_FromFrame(tb, frame);
            }
            if (newtb != NULL) {
                Py_XSETREF(tb, newtb);
            }
            else {
                _PyErr_ChainExceptions(exc, val, tb);
                PyErr_Fetch(&exc, &val, &tb);
            }
        }

        if (ts->use_tracing && !skip_frame) {
            vm_trace_err(ts, &exc, &val, &tb);
        }

        skip_frame = false;

        ExceptionHandler *handler = vm_exception_handler(code, pc);
        if (handler != NULL) {
            /* Make the raw exception data
                available to the handler,
                so a program can emulate the
                Python main loop. */
            _PyErr_NormalizeException(ts, &exc, &val, &tb);
            PyException_SetTraceback(val, tb ? tb : Py_None);

            vm_clear_regs(ts, handler->reg, code->co_framesize);

            Py_ssize_t link_reg = handler->reg;
            ts->regs[link_reg].as_int64 = -1;
            assert(!_PyObject_IS_IMMORTAL(val));
            ts->regs[link_reg + 1] = PACK(val, REFCOUNT_TAG);
            Py_DECREF(exc);
            Py_XDECREF(tb);
            return PyCode_FirstInstr(code) + handler->handler;
        }

        if (ts->use_tracing) {
            if (vm_trace_return(ts) != 0) {
                Py_CLEAR(exc);
                Py_CLEAR(val);
                Py_CLEAR(tb);
                PyErr_Fetch(&exc, &val, &tb);
            }
        }

      next: ;
        // No handler found in this call frame. Clears the entire frame and
        // unwinds the call stack.

        intptr_t frame_link = vm_pop_frame(ts);
        if (frame_link <= 0) {
            _PyErr_Restore(ts, exc, val, tb);
            if (frame_link == FRAME_GENERATOR) {
                PyGenObject *gen = PyGen_FromThread(ts->active);
                assert(PyGen_CheckExact(gen) || PyCoro_CheckExact(gen) || PyAsyncGen_CheckExact(gen));
                gen->status = GEN_CLOSED;
            }
            else {
                ts->pc = (const uint8_t *)(-frame_link);
            }
            return NULL;
        }
        ts->pc = pc = (const uint8_t *)frame_link;
    }
}

void
vm_error_with_result(PyThreadState *tstate, Register acc)
{
    if (acc.as_int64 != 0) {
        DECREF(acc);
    }

    PyObject *callable = AS_OBJ(tstate->regs[-1]);
    if (callable) {
        _PyErr_FormatFromCauseTstate(
            tstate, PyExc_SystemError,
            "%R returned a result with an error set", callable);
    }
    else {
        _PyErr_FormatFromCauseTstate(
            tstate, PyExc_SystemError,
            "a function returned a result with an error set");
    }
#ifdef Py_DEBUG
    /* Ensure that the bug is caught in debug mode.
        Py_FatalError() logs the SystemError exception raised above. */
    Py_FatalError("a function returned a result with an error set");
#endif
}

static int
is_importlib_frame(PyFunctionObject *func)
{
    _Py_IDENTIFIER(importlib);
    _Py_IDENTIFIER(_bootstrap);

    PyObject *filename, *importlib_string, *bootstrap_string;
    int contains;

    filename = _PyFunction_GET_CODE(func)->co_filename;
    if (!PyUnicode_Check(filename)) {
        return 0;
    }

    importlib_string = _PyUnicode_FromId(&PyId_importlib);
    if (importlib_string == NULL) {
        return -1;
    }

    bootstrap_string = _PyUnicode_FromId(&PyId__bootstrap);
    if (bootstrap_string == NULL) {
        return -1;
    }

    contains = PyUnicode_Contains(filename, importlib_string);
    if (contains > 0) {
        contains = PyUnicode_Contains(filename, bootstrap_string);
        if (contains > 0) {
            return 1;
        }
    }
    if (contains < 0) {
        return -1;
    }
    return 0;
}

int
vm_frame_info(PyFunctionObject **out_func, int *out_lineno, int depth,
              int skip_importlib_frames)
{
    struct ThreadState *ts = vm_active(_PyThreadState_GET());

    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w.regs[-1]);

        if (skip_importlib_frames) {
            int skip = is_importlib_frame(func);
            if (skip == 1) {
                --depth;
                continue;
            }
            else if (skip < 0) {
                return -1;
            }
        }

        --depth;
        if (depth <= 0) {
            *out_func = func;
            *out_lineno = vm_stack_walk_lineno(&w);
            return 1;
        }
    }

    *out_func = NULL;
    *out_lineno = 1;
    return 0;
}

static PyObject *
normalize_exception(PyObject *exc)
{
    if (PyExceptionClass_Check(exc)) {
        PyObject *value = _PyObject_CallNoArg(exc);
        if (value == NULL) {
            return NULL;
        }
        if (!PyExceptionInstance_Check(value)) {
            PyErr_Format(PyExc_TypeError,
                         "calling %R should have returned an instance of "
                         "BaseException, not %R",
                         exc, Py_TYPE(value));
            Py_DECREF(value);
            return NULL;
        }
        return value;
    }
    if (!PyExceptionInstance_Check(exc)) {
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        PyErr_SetString(PyExc_TypeError,
                        "exceptions must derive from BaseException");
        return NULL;
    }
    Py_INCREF(exc);
    return exc;
}

static PyObject *
vm_exc_set_cause(PyObject * const *args, Py_ssize_t nargs)
{
    assert(nargs == 2);
    PyObject *exc = normalize_exception(args[0]);
    if (exc == NULL) {
        return NULL;
    }

    if (PyExceptionClass_Check(args[1])) {
        PyObject *cause = _PyObject_CallNoArg(args[1]);
        if (cause == NULL) {
            Py_DECREF(exc);
            return NULL;
        }
        PyException_SetCause(exc, cause);
    }
    else if (PyExceptionInstance_Check(args[1])) {
        PyObject *cause = args[1];
        Py_INCREF(cause);
        PyException_SetCause(exc, cause);
    }
    else if (args[1] == Py_None) {
        PyException_SetCause(exc, NULL);
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "exception causes must derive from "
                        "BaseException");
        Py_DECREF(exc);
        return NULL;
    }
    return exc;
}

int
vm_reraise(PyThreadState *ts, Register reg)
{
    assert(IS_RC(reg) || _PyObject_IS_IMMORTAL(AS_OBJ(reg)));
    PyObject *exc = AS_OBJ(reg);
    PyObject *type = (PyObject *)Py_TYPE(exc);
    Py_INCREF(type);
    PyObject *tb = PyException_GetTraceback(exc);
    _PyErr_Restore(ts, type, exc, tb);
    return -2;
}

int
vm_raise(PyThreadState *ts, PyObject *exc)
{
    if (exc == NULL) {
        exc = vm_handled_exc(ts);
        if (exc == NULL) {
            _PyErr_SetString(ts, PyExc_RuntimeError,
                            "No active exception to reraise");
            return -1;
        }
        return vm_reraise(ts, PACK_INCREF(exc));
    }
    PyObject *fixed_exc = normalize_exception(exc);
    if (fixed_exc == NULL) {
        return -1;
    }
    PyErr_SetObject((PyObject *)Py_TYPE(fixed_exc), fixed_exc);
    Py_DECREF(fixed_exc);
    return -1;
}

// Search the jump side table for the jump target of the current
// program counter.
Py_ssize_t
vm_jump_side_table(PyThreadState *ts, const uint8_t *pc)
{
    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyCodeObject *code = _PyFunction_GET_CODE(func);

    // The current address
    uint32_t addr = (uint32_t)(pc - func->func_base.first_instr);

    // Based on the binary search described in:
    // http://pvk.ca/Blog/2015/11/29/retrospective-on-binary-search-and-on-compression-slash-compilation/
    JumpEntry* low = &code->co_jump_table->entries[0];
    for (Py_ssize_t n = code->co_jump_table->size; n > 1; n -= n / 2) {
        JumpEntry *e = &low[n / 2];
        if (e->from <= addr) {
            low = e;
        }
    }

    assert(low->from == addr);
    return low->delta;
}

int
vm_exc_match(PyThreadState *ts, PyObject *tp, PyObject *exc)
{
    static const char *CANNOT_CATCH_MSG = (
        "catching classes that do not inherit from "
        "BaseException is not allowed");

    if (PyTuple_Check(tp)) {
        Py_ssize_t i, length;
        length = PyTuple_GET_SIZE(tp);
        for (i = 0; i < length; i++) {
            PyObject *item = PyTuple_GET_ITEM(tp, i);
            if (!PyExceptionClass_Check(item)) {
                _PyErr_SetString(ts, PyExc_TypeError,
                                 CANNOT_CATCH_MSG);
                return -1;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(tp)) {
            _PyErr_SetString(ts, PyExc_TypeError,
                             CANNOT_CATCH_MSG);
            return -1;
        }
    }

    return PyErr_GivenExceptionMatches(exc, tp);
}

PyObject *
vm_get_iter(PyObject *o)
{
    assert(Py_TYPE(o)->tp_iter == NULL &&
           "GET_ITER should have use fast-path");
    if (PySequence_Check(o)) {
        return PySeqIter_New(o);
    }
    PyErr_Format(PyExc_TypeError, "'%.200s' object is not iterable",
                 Py_TYPE(o)->tp_name);
    return NULL;
}

int
vm_unpack(PyThreadState *ts, PyObject *v, Py_ssize_t base,
          Py_ssize_t argcnt, Py_ssize_t argcntafter)
{
    Py_ssize_t ll = 0;
    PyObject *it;  /* iter(v) */
    PyObject *w;
    PyObject *l = NULL; /* variable list */

    assert(v != NULL);

    if (UNLIKELY(Py_TYPE(v)->tp_iter == NULL && !PySequence_Check(v))) {
        _PyErr_Format(ts, PyExc_TypeError,
                      "cannot unpack non-iterable %.200s object",
                      Py_TYPE(v)->tp_name);
        return -1;
    }

    it = PyObject_GetIter(v);
    if (UNLIKELY(it == NULL)) {
        return -1;
    }

    Py_ssize_t top = base + argcnt + argcntafter;
    for (Py_ssize_t i = 0; i < argcnt; i++) {
        w = PyIter_Next(it);
        if (UNLIKELY(w == NULL)) {
            /* Iterator done, via error or exhaustion. */
            if (!_PyErr_Occurred(ts)) {
                if (argcntafter == 0) {
                    _PyErr_Format(ts, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected %d, got %d)",
                                  argcnt, i);
                }
                else {
                    _PyErr_Format(ts, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected at least %d, got %d)",
                                  argcnt + argcntafter - 1, i);
                }
            }
            goto Error;
        }
        ts->regs[--top] = PACK_OBJ(w);
    }

    if (argcntafter == 0) {
        /* We better have exhausted the iterator now. */
        w = PyIter_Next(it);
        if (w == NULL) {
            if (_PyErr_Occurred(ts))
                goto Error;
            Py_DECREF(it);
            return 0;
        }
        Py_DECREF(w);
        _PyErr_Format(ts, PyExc_ValueError,
                      "too many values to unpack (expected %d)",
                      argcnt);
        goto Error;
    }

    l = PySequence_List(it);
    if (l == NULL)
        goto Error;
    ts->regs[--top] = PACK_OBJ(l);

    ll = PyList_GET_SIZE(l);
    Py_ssize_t remaining = argcntafter - 1;
    if (remaining > ll) {
        _PyErr_Format(ts, PyExc_ValueError,
            "not enough values to unpack (expected at least %d, got %zd)",
            argcnt + remaining, argcnt + ll);
        goto Error;
    }

    /* Pop the "after-variable" args off the list. */
    for (Py_ssize_t j = remaining; j > 0; j--) {
        ts->regs[--top] = PACK_INCREF(PyList_GET_ITEM(l, ll - j));
    }
    assert(top == base);
    /* Resize the list. */
    Py_SET_SIZE(l, ll - remaining);
    Py_DECREF(it);
    return 0;

Error:
    Py_XDECREF(it);
    return -1;
}

PyObject *
vm_load_name(PyThreadState *ts, PyObject *locals, PyObject *name)
{
    if (UNLIKELY(!PyDict_CheckExact(locals))) {
        PyObject *value = PyObject_GetItem(locals, name);
        if (value == NULL && _PyErr_ExceptionMatches(ts, PyExc_KeyError)) {
            _PyErr_Clear(ts);
        }
        return value;
    }
    return PyDict_GetItemWithError2(locals, name);
}

Register
vm_load_class_deref(PyThreadState *ts, Py_ssize_t opA, PyObject *name)
{
    PyObject *locals = AS_OBJ(ts->regs[0]);
    if (PyDict_CheckExact(locals)) {
        PyObject *value = PyDict_GetItemWithError2(locals, name);
        if (value != NULL) {
            return PACK_OBJ(value);
        }
        else if (_PyErr_Occurred(ts)) {
            return (Register){0};
        }
    }
    else {
        PyObject *value = PyObject_GetItem(locals, name);
        if (value != NULL) {
            return PACK_OBJ(value);
        }
        else if (!_PyErr_ExceptionMatches(ts, PyExc_KeyError)) {
            return (Register){0};
        }
        else {
            _PyErr_Clear(ts);
        }
    }
    PyObject *cell = AS_OBJ(ts->regs[opA]);
    assert(cell != NULL && PyCell_Check(cell));
    PyObject *value = PyCell_GET(cell);
    if (value == NULL) {
        PyErr_Format(PyExc_NameError,
            "free variable '%U' referenced before assignment in enclosing scope", name);
        return (Register){0};
    }
    return PACK_INCREF(value);
}

static PyObject *
vm_import_name_custom(PyThreadState *ts, PyFunctionObject *this_func, PyObject *arg,
                      PyObject *import_func)
{
    PyObject *res;
    PyObject *stack[5];

    Py_INCREF(import_func);  // FIXME: thread-safety if builtins.__import__ changes
    stack[0] = PyTuple_GET_ITEM(arg, 0);  // name
    stack[1] = this_func->globals;
    stack[2] = Py_None;
    stack[3] = PyTuple_GET_ITEM(arg, 1);  // fromlist
    stack[4] = PyTuple_GET_ITEM(arg, 2);  // level
    res = _PyObject_FastCall(import_func, stack, 5);
    Py_DECREF(import_func);
    return res;
}

PyObject *
vm_import_name(PyThreadState *ts, PyFunctionObject *this_func, PyObject *arg)
{
    _Py_IDENTIFIER(__import__);
    PyObject *import_func, *builtins;

    builtins = this_func->builtins;
    import_func = _PyDict_GetItemIdWithError(builtins, &PyId___import__);
    if (import_func == NULL) {
        if (!_PyErr_Occurred(ts)) {
            _PyErr_SetString(ts, PyExc_ImportError, "__import__ not found");
        }
        return NULL;
    }

    if (UNLIKELY(import_func != ts->interp->import_func)) {
        return vm_import_name_custom(ts, this_func, arg, import_func);
    }

    assert(PyTuple_CheckExact(arg) && PyTuple_GET_SIZE(arg) == 3);
    PyObject *name = PyTuple_GET_ITEM(arg, 0);
    PyObject *fromlist = PyTuple_GET_ITEM(arg, 1);
    PyObject *level = PyTuple_GET_ITEM(arg, 2);
    int ilevel = _PyLong_AsInt(level);
    if (ilevel == -1 && _PyErr_Occurred(ts)) {
        return NULL;
    }
    return PyImport_ImportModuleLevelObject(
        name,
        this_func->globals,
        Py_None,
        fromlist,
        ilevel);
}

Register
vm_load_build_class(PyThreadState *ts, PyObject *builtins)
{
    _Py_IDENTIFIER(__build_class__);

    PyObject *bc;
    if (PyDict_CheckExact(builtins)) {
        bc = _PyDict_GetItemIdWithError(builtins, &PyId___build_class__);
        if (bc == NULL) {
            if (!_PyErr_Occurred(ts)) {
                _PyErr_SetString(ts, PyExc_NameError,
                                    "__build_class__ not found");
            }
            return (Register){0};
        }

        // FIXME: might get deleted oh well
        // should use deferred rc when available
        return PACK(bc, NO_REFCOUNT_TAG);
    }
    else {
        PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
        if (build_class_str == NULL) {
            return (Register){0};
        }
        bc = PyObject_GetItem(builtins, build_class_str);
        if (bc == NULL) {
            if (_PyErr_ExceptionMatches(ts, PyExc_KeyError))
                _PyErr_SetString(ts, PyExc_NameError,
                                    "__build_class__ not found");
            return (Register){0};
        }
        return PACK(bc, REFCOUNT_TAG);
    }
}

_Py_NO_INLINE static PyObject *
vm_call_function_ex(PyThreadState *ts)
{
    PyObject *callable = AS_OBJ(ts->regs[-1]);
    PyObject *args = AS_OBJ(ts->regs[-FRAME_EXTRA - 2]);
    PyObject *kwargs = AS_OBJ(ts->regs[-FRAME_EXTRA - 1]);
    PyObject *res = PyObject_Call(callable, args, kwargs);
    XCLEAR(ts->regs[-FRAME_EXTRA - 1]);
    CLEAR(ts->regs[-FRAME_EXTRA - 2]);
    return res;
}

_Py_NO_INLINE PyObject *
vm_call_cfunction_slow(PyThreadState *ts, Register acc)
{
    const int flags_ex = ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS;
    if (UNLIKELY((acc.as_int64 & flags_ex) != 0)) {
        return vm_call_function_ex(ts);
    }

    Py_ssize_t total_args = 1 + ACC_ARGCOUNT(acc) + ACC_KWCOUNT(acc);
    PyObject **args = PyMem_RawMalloc(total_args * sizeof(PyObject*));
    if (UNLIKELY(args == NULL)) {
        return NULL;
    }
    args[0] = AS_OBJ(ts->regs[-1]);
    for (Py_ssize_t i = 0; i != ACC_ARGCOUNT(acc); i++) {
        args[i + 1] = AS_OBJ(ts->regs[i]);
    }
    PyObject *kwnames = NULL;
    if (ACC_KWCOUNT(acc) > 0) {
        kwnames = AS_OBJ(ts->regs[-FRAME_EXTRA - 1]);
        assert(PyTuple_CheckExact(kwnames));
        for (Py_ssize_t i = 0; i != ACC_KWCOUNT(acc); i++) {
            Py_ssize_t k = -FRAME_EXTRA - ACC_KWCOUNT(acc) - 1 + i;
            args[i + ACC_ARGCOUNT(acc) + 1] = AS_OBJ(ts->regs[k]);
        }
    }

    Py_ssize_t nargsf = ACC_ARGCOUNT(acc) | PY_VECTORCALL_ARGUMENTS_OFFSET;
    PyObject *res = _PyObject_VectorcallTstate(ts, args[0], args + 1, nargsf, kwnames);
    if (ACC_KWCOUNT(acc) > 0) {
        for (Py_ssize_t i =  -FRAME_EXTRA - ACC_KWCOUNT(acc) - 1; i != -FRAME_EXTRA; i++) {
            CLEAR(ts->regs[i]);
        }
    }
    PyMem_RawFree(args);
    return res;
}

PyObject *
vm_call_cfunction(PyThreadState *ts, Register acc)
{
    // PyObject *f = AS_OBJ(ts->regs[-1]);
    // assert(PyCFunction_Check(f) || Py_TYPE(f) == &PyMethodDescr_Type);

    if (UNLIKELY(acc.as_int64 >= 6)) {
        return vm_call_cfunction_slow(ts, acc);
    }

    Py_ssize_t nargs = acc.as_int64;
    PyObject *args[7];
    for (int i = 0; i != nargs + 1; i++) {
        args[i] = AS_OBJ(ts->regs[i - 1]);
    }

    PyCFunctionObject *func = (PyCFunctionObject *)args[0];
    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    return func->vectorcall(args[0], args + 1, nargsf, NULL);
}

PyObject *
vm_call_function(PyThreadState *ts, Register acc)
{
    if (UNLIKELY(acc.as_int64 > 6)) {
        return vm_call_cfunction_slow(ts, acc);
    }

    Py_ssize_t nargs = acc.as_int64;
    PyObject *args[7];
    for (int i = 0; i != nargs + 1; i++) {
        args[i] = AS_OBJ(ts->regs[i - 1]);
    }

    Py_ssize_t nargsf = nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    return _PyObject_VectorcallTstate(ts, args[0], args + 1, nargsf, NULL);
}

static PyObject *
build_tuple(PyThreadState *ts, Py_ssize_t base, Py_ssize_t n);

static PyObject *
build_kwargs(PyThreadState *ts, Py_ssize_t n);

PyObject *
vm_tpcall_function(PyThreadState *ts, Register acc)
{
    PyCFunctionObject *func = (PyCFunctionObject *)AS_OBJ(ts->regs[-1]);
    const int flags_ex = ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS;
    if (UNLIKELY((acc.as_int64 & flags_ex) != 0)) {
        return vm_call_function_ex(ts);
    }

    int flags = PyCFunction_GET_FLAGS(func);
    assert((flags & METH_VARARGS) != 0 && "vp_tpcall without METH_VARARGS");

    PyCFunction meth = PyCFunction_GET_FUNCTION(func);
    PyObject *self = PyCFunction_GET_SELF(func);

    PyObject *args = build_tuple(ts, 0, ACC_ARGCOUNT(acc));
    if (UNLIKELY(args == NULL)) {
        return NULL;
    }

    PyObject *result;
    if ((flags & METH_KEYWORDS) != 0) {
        PyObject *kwargs = NULL;
        if (ACC_KWCOUNT(acc) != 0) {
            kwargs = build_kwargs(ts, ACC_KWCOUNT(acc));
            if (UNLIKELY(kwargs == NULL)) {
                goto error;
            }
        }
        result = (*(PyCFunctionWithKeywords)(void(*)(void))meth)(self, args, kwargs);
        Py_XDECREF(kwargs);
    }
    else if (UNLIKELY(ACC_KWCOUNT(acc) != 0)) {
        _PyErr_Format(ts, PyExc_TypeError,
                "%.200s() takes no keyword arguments",
                ((PyCFunctionObject*)func)->m_ml->ml_name);
        goto error;
    }
    else {
        result = meth(self, args);
    }

    Py_DECREF(args);
    return result;

error:
    Py_DECREF(args);
    return NULL;
}

static PyObject *
build_kwargs(PyThreadState *ts, Py_ssize_t kwcount)
{
    PyObject *kwargs = _PyDict_NewPresized(kwcount);
    if (kwargs == NULL) {
        return NULL;
    }

    PyObject **kwnames = _PyTuple_ITEMS(AS_OBJ(ts->regs[-FRAME_EXTRA - 1]));
    ts->regs[-FRAME_EXTRA - 1].as_int64 = 0;

    while (kwcount != 0) {
        Py_ssize_t k = -FRAME_EXTRA - kwcount - 1;
        PyObject *keyword = *kwnames;
        PyObject *value = AS_OBJ(ts->regs[k]);
        if (PyDict_SetItem(kwargs, keyword, value) < 0) {
            Py_DECREF(kwargs);
            return NULL;
        }
        CLEAR(ts->regs[k]);
        kwnames++;
        kwcount--;
    }
    return kwargs;
}

Register
vm_make_function(PyThreadState *ts, PyCodeObject *code)
{
    PyFunctionObject *this_func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyFunctionObject *func = (PyFunctionObject *)PyFunction_NewWithBuiltins(
        (PyObject *)code, this_func->globals, this_func->builtins);
    if (func == NULL) {
        return (Register){0};
    }

    for (Py_ssize_t i = 0, n = code->co_nfreevars; i < n; i++) {
        Py_ssize_t r = code->co_free2reg[i*2];
        PyObject *var = AS_OBJ(ts->regs[r]);
        assert(i < code->co_ndefaultargs || PyCell_Check(var));

        Py_XINCREF(var);    // default args might be NULL (yuck)
        func->freevars[i] = var;
    }

    return PACK_OBJ((PyObject *)func);
}

static int
positional_only_passed_as_keyword(PyThreadState *ts, PyCodeObject *co,
                                  Py_ssize_t kwcount, PyObject** kwnames)
{
    int posonly_conflicts = 0;
    PyObject* posonly_names = PyList_New(0);

    for(int k=0; k < co->co_posonlyargcount; k++){
        PyObject* posonly_name = PyTuple_GET_ITEM(co->co_varnames, k);

        for (int k2=0; k2<kwcount; k2++){
            PyObject* kwname = kwnames[k2];
            int cmp = PyObject_RichCompareBool(posonly_name, kwname, Py_EQ);
            if (cmp == 1) {
                if(PyList_Append(posonly_names, kwname) != 0) {
                    goto fail;
                }
                posonly_conflicts++;
            } else if (cmp < 0) {
                goto fail;
            }

        }
    }
    if (posonly_conflicts) {
        PyObject* comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            goto fail;
        }
        PyObject* error_names = PyUnicode_Join(comma, posonly_names);
        Py_DECREF(comma);
        if (error_names == NULL) {
            goto fail;
        }
        _PyErr_Format(ts, PyExc_TypeError,
                      "%U() got some positional-only arguments passed"
                      " as keyword arguments: '%U'",
                      co->co_name, error_names);
        Py_DECREF(error_names);
        goto fail;
    }

    Py_DECREF(posonly_names);
    return 0;

fail:
    Py_XDECREF(posonly_names);
    return 1;
}

static int _Py_NO_INLINE
unexpected_keyword_argument(PyThreadState *ts, PyCodeObject *co,
                            PyObject *keyword, Py_ssize_t kwcount,
                            PyObject **kwnames)
{
    if (co->co_posonlyargcount == 0 ||
        !positional_only_passed_as_keyword(ts, co, kwcount, kwnames))
    {
        _PyErr_Format(ts, PyExc_TypeError,
                    "%U() got an unexpected keyword argument '%S'",
                    co->co_name, keyword);
    }
    return -1;
}

static int _Py_NO_INLINE
unexpected_keyword_argument_dict(PyThreadState *ts, PyCodeObject *co,
                                 PyObject *keyword, PyObject *kwargs)
{
    Py_ssize_t kwcount = PyDict_Size(kwargs);
    PyObject *keys = PyTuple_New(kwcount);
    if (keys == NULL) {
        return -1;
    }

    Py_ssize_t i = 0, j = 0;
    PyObject *key, *value;
    while (PyDict_Next(kwargs, &i, &key, &value)) {
        Py_INCREF(key);
        PyTuple_SET_ITEM(keys, j, key);
        j++;
    }

    PyObject **kwnames = _PyTuple_ITEMS(keys);
    unexpected_keyword_argument(ts, co, keyword, kwcount, kwnames);
    Py_DECREF(keys);
    return -1;
}

int _Py_NO_INLINE
duplicate_keyword_argument(PyThreadState *ts, PyCodeObject *co, PyObject *keyword)
{
    _PyErr_Format(ts, PyExc_TypeError,
                    "%U() got multiple values for argument '%S'",
                    co->co_name, keyword);
    return -1;
}

static void
format_missing(PyThreadState *ts, const char *kind,
               PyCodeObject *co, PyObject *names)
{
    int err;
    Py_ssize_t len = PyList_GET_SIZE(names);
    PyObject *name_str, *comma, *tail, *tmp;

    assert(PyList_CheckExact(names));
    assert(len >= 1);
    /* Deal with the joys of natural language. */
    switch (len) {
    case 1:
        name_str = PyList_GET_ITEM(names, 0);
        Py_INCREF(name_str);
        break;
    case 2:
        name_str = PyUnicode_FromFormat("%U and %U",
                                        PyList_GET_ITEM(names, len - 2),
                                        PyList_GET_ITEM(names, len - 1));
        break;
    default:
        tail = PyUnicode_FromFormat(", %U, and %U",
                                    PyList_GET_ITEM(names, len - 2),
                                    PyList_GET_ITEM(names, len - 1));
        if (tail == NULL)
            return;
        /* Chop off the last two objects in the list. This shouldn't actually
           fail, but we can't be too careful. */
        err = PyList_SetSlice(names, len - 2, len, NULL);
        if (err == -1) {
            Py_DECREF(tail);
            return;
        }
        /* Stitch everything up into a nice comma-separated list. */
        comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            Py_DECREF(tail);
            return;
        }
        tmp = PyUnicode_Join(comma, names);
        Py_DECREF(comma);
        if (tmp == NULL) {
            Py_DECREF(tail);
            return;
        }
        name_str = PyUnicode_Concat(tmp, tail);
        Py_DECREF(tmp);
        Py_DECREF(tail);
        break;
    }
    if (name_str == NULL)
        return;
    _PyErr_Format(ts, PyExc_TypeError,
                  "%U() missing %i required %s argument%s: %U",
                  co->co_name,
                  len,
                  kind,
                  len == 1 ? "" : "s",
                  name_str);
    Py_DECREF(name_str);
}

int _Py_NO_INLINE
missing_arguments(PyThreadState *ts)
{
    PyObject *positional = NULL;
    PyObject *kwdonly = NULL;
    PyObject *name = NULL;

    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyCodeObject *co = _PyFunction_GET_CODE(func);
    Py_ssize_t required_args = co->co_totalargcount - func->num_defaults;

    // names of missing positional arguments
    positional = PyList_New(0);
    if (positional == NULL) {
        goto cleanup;
    }

    // names of missing keyword-only arguments
    kwdonly = PyList_New(0);
    if (kwdonly == NULL) {
        goto cleanup;
    }

    for (Py_ssize_t i = 0; i < co->co_totalargcount; i++) {
        if (ts->regs[i].as_int64 != 0) {
            // argument has value
            continue;
        }
        if (i >= required_args && func->freevars[i - required_args] != NULL) {
            // argument has default value
            continue;
        }
        PyObject *raw = PyTuple_GET_ITEM(co->co_varnames, i);
        if (i >= co->co_argcount && func->func_kwdefaults != NULL) {
            int rv = PyDict_Contains(func->func_kwdefaults, raw);
            if (rv < 0) {
                goto cleanup;
            }
            else if (rv == 1) {
                // argument has default value
                continue;
            }
        }
        name = PyObject_Repr(raw);  // quote the 'name' string
        if (name == NULL) {
            goto cleanup;
        }
        PyObject *list = (i < co->co_argcount) ? positional : kwdonly;
        int err = PyList_Append(list, name);
        if (err < 0) {
            goto cleanup;
        }
        Py_CLEAR(name);
    }
    if (PyList_GET_SIZE(positional) > 0) {
        format_missing(ts, "positional", co, positional);
    }
    else {
        format_missing(ts, "keyword-only", co, kwdonly);
    }
    goto cleanup;

cleanup:
    Py_XDECREF(positional);
    Py_XDECREF(kwdonly);
    Py_XDECREF(name);
    return -1;
}

static int _Py_NO_INLINE
too_many_positional_ex(PyThreadState *ts,
                       Py_ssize_t given,
                       Py_ssize_t kwcount)
{
    int plural;
    PyObject *sig, *kwonly_sig;
    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyCodeObject *co = _PyFunction_GET_CODE(func);
    Py_ssize_t co_argcount = co->co_argcount;
    Py_ssize_t co_totalargcount = co->co_totalargcount;

    assert((co->co_flags & CO_VARARGS) == 0);
    if ((co->co_flags & CO_VARKEYWORDS) != 0) {
        kwcount = 0;
    }


    Py_ssize_t defcount = co_argcount + func->num_defaults - co_totalargcount;
    if (defcount > 0) {
        Py_ssize_t atleast = co_argcount - defcount;
        plural = 1;
        sig = PyUnicode_FromFormat("from %zd to %zd", atleast, co_argcount);
    }
    else {
        plural = (co_argcount != 1);
        sig = PyUnicode_FromFormat("%zd", co_argcount);
    }
    if (sig == NULL)
        return -1;
    if (kwcount) {
        const char *format = " positional argument%s (and %zd keyword-only argument%s)";
        kwonly_sig = PyUnicode_FromFormat(format,
                                          given != 1 ? "s" : "",
                                          kwcount,
                                          kwcount != 1 ? "s" : "");
        if (kwonly_sig == NULL) {
            Py_DECREF(sig);
            return -1;
        }
    }
    else {
        /* This will not fail. */
        kwonly_sig = PyUnicode_FromString("");
        assert(kwonly_sig != NULL);
    }
    _PyErr_Format(ts, PyExc_TypeError,
                  "%U() takes %U positional argument%s but %zd%U %s given",
                  co->co_name,
                  sig,
                  plural ? "s" : "",
                  given,
                  kwonly_sig,
                  given == 1 && !kwcount ? "was" : "were");
    Py_DECREF(sig);
    Py_DECREF(kwonly_sig);
    return -1;
}

void
too_many_positional(PyThreadState *ts, Register acc)
{
    // We have too many positional arguments, but we might also have invalid
    // keyword arguments -- those error messages take precedence.

    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyCodeObject *co = _PyFunction_GET_CODE(func);

    assert((co->co_packed_flags & CODE_FLAG_VARARGS) == 0);

    Py_ssize_t argcount = ACC_ARGCOUNT(acc);
    Py_ssize_t kwcount = ACC_KWCOUNT(acc);

    if (kwcount > 0) {
        // First, clear extra positional arguments
        for (Py_ssize_t i = co->co_argcount; i < argcount; i++) {
            CLEAR(ts->regs[i]);
        }

        if ((co->co_packed_flags & CODE_FLAG_VARKEYWORDS) != 0) {
            // if the function uses **kwargs, create and store the dict
            PyObject *kwdict = PyDict_New();
            if (kwdict == NULL) {
                return;
            }
            Py_ssize_t pos = co->co_totalargcount;
            assert(ts->regs[pos].as_int64 == 0);
            ts->regs[pos] = PACK(kwdict, REFCOUNT_TAG);
        }

        PyObject **kwnames = _PyTuple_ITEMS(AS_OBJ(ts->regs[-FRAME_EXTRA - 1]));
        int err = vm_setup_kwargs(ts, co, acc, kwnames);
        if (err != 0) {
            return;
        }
    }

    too_many_positional_ex(ts, argcount, kwcount);
}

// Setup up a function frame when invoked like `func(*args, **kwargs)`.
int
vm_setup_ex(PyThreadState *ts, PyCodeObject *co, Register acc)
{
    assert(ACC_ARGCOUNT(acc) == 0 && ACC_KWCOUNT(acc) == 0);
    PyObject *varargs = AS_OBJ(ts->regs[-FRAME_EXTRA - 2]);
    PyObject *kwargs = AS_OBJ(ts->regs[-FRAME_EXTRA - 1]);
    assert(PyTuple_Check(varargs));
    if (kwargs) {
        assert(PyDict_Check(kwargs));
    }
    PyObject *kwdict = NULL;

    Py_ssize_t argcount = PyTuple_GET_SIZE(varargs);
    Py_ssize_t total_args = co->co_totalargcount;
    Py_ssize_t n = argcount;
    if (n > co->co_argcount) {
        n = co->co_argcount;
    }

    for (Py_ssize_t j = 0; j < n; j++) {
        PyObject *x = PyTuple_GET_ITEM(varargs, j);
        ts->regs[j] = PACK_INCREF(x);
    }
    if (co->co_packed_flags & CODE_FLAG_VARARGS) {
        PyObject *u = PyTuple_GetSlice(varargs, n, argcount);
        if (UNLIKELY(u == NULL)) {
            return -1;
        }
        ts->regs[total_args] = PACK_OBJ(u);
    }
    if (co->co_packed_flags & CODE_FLAG_VARKEYWORDS) {
        kwdict = PyDict_New();
        if (UNLIKELY(kwdict == NULL)) {
            return -1;
        }
        Py_ssize_t j = total_args;
        if (co->co_packed_flags & CODE_FLAG_VARARGS) {
            j++;
        }
        ts->regs[j] = PACK(kwdict, REFCOUNT_TAG);
    }

    Py_ssize_t i = 0;
    PyObject *keyword, *value;
    // FIXME: PyDict_Next isn't safe if the rich comparison modifies kwargs
    while (kwargs && PyDict_Next(kwargs, &i, &keyword, &value)) {
        if (keyword == NULL || !PyUnicode_Check(keyword)) {
            _PyErr_Format(ts, PyExc_TypeError,
                          "%U() keywords must be strings",
                          co->co_name);
            return -1;
        }

        Py_INCREF(value);

        // Speed hack: do raw pointer compares. As names are
        // normally interned this should almost always hit.
        PyObject **co_varnames = _PyTuple_ITEMS(co->co_varnames);
        Py_ssize_t j;
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = co_varnames[j];
            if (name == keyword) {
                goto kw_found;
            }
        }

        // Slow fallback, just in case.
        // We need to ensure that keyword and value are kept alive across the
        // rich comparison call. The call might modify "kwargs"!
        Py_INCREF(keyword);
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = co_varnames[j];
            int cmp = PyObject_RichCompareBool(keyword, name, Py_EQ);
            if (cmp > 0) {
                Py_DECREF(keyword);
                goto kw_found;
            }
            else if (cmp < 0) {
                goto error;
            }
        }

        assert(j >= total_args);
        if (kwdict == NULL) {
            unexpected_keyword_argument_dict(ts, co, keyword, kwargs);
            goto error;
        }

        if (PyDict_SetItem(kwdict, keyword, value) == -1) {
            goto error;
        }
        Py_DECREF(keyword);
        Py_DECREF(value);
        continue;

      kw_found:
        if (ts->regs[j].as_int64 != 0) {
            return duplicate_keyword_argument(ts, co, keyword);
        }
        ts->regs[j] = PACK_OBJ(value);
    }

    /* Check the number of positional arguments */
    if ((argcount > co->co_argcount) && !(co->co_flags & CO_VARARGS)) {
        Py_ssize_t kwcount = kwargs ? PyDict_Size(kwargs) : 0;
        return too_many_positional_ex(ts, argcount, kwcount);
    }

    CLEAR(ts->regs[-FRAME_EXTRA - 2]);
    if (kwargs) {
        CLEAR(ts->regs[-FRAME_EXTRA - 1]);
    }
    return 0;

error:
    Py_DECREF(keyword);
    Py_DECREF(value);
    return -1;
}

int
vm_setup_varargs(PyThreadState *ts, PyCodeObject *co, Register acc)
{
    Py_ssize_t argcount = (acc.as_int64 & ACC_MASK_ARGS);
    Py_ssize_t n = argcount - co->co_argcount;
    Py_ssize_t total_args = co->co_totalargcount;
    if (n <= 0) {
        PyObject *varargs = PyTuple_New(0); // TODO: get empty tuple directly?
        assert(varargs != NULL && _PyObject_IS_IMMORTAL(varargs));
        ts->regs[total_args] = PACK(varargs, NO_REFCOUNT_TAG);
    }
    else {
        PyObject *varargs = PyTuple_New(n);
        if (UNLIKELY(varargs == NULL)) {
            return -1;
        }
        for (Py_ssize_t j = 0; j < n; j++) {
            PyObject *item = vm_object_steal(&ts->regs[co->co_argcount + j]);
            PyTuple_SET_ITEM(varargs, j, item);
        }
        ts->regs[total_args] = PACK(varargs, REFCOUNT_TAG);
    }
    return 0;
}

int
vm_setup_kwargs(PyThreadState *ts, PyCodeObject *co, Register acc, PyObject **kwnames)
{
    Py_ssize_t total_args = co->co_totalargcount;
    Py_ssize_t kwcount = ACC_KWCOUNT(acc);
    for (; kwcount != 0; kwnames++, kwcount--) {
        PyObject *keyword = *kwnames;
        Py_ssize_t kwdpos = -FRAME_EXTRA - kwcount - 1;

        /* Speed hack: do raw pointer compares. As names are
           normally interned this should almost always hit. */
        Py_ssize_t j;
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = PyTuple_GET_ITEM(co->co_varnames, j);
            if (name == keyword) {
                goto kw_found;
            }
        }

        /* Slow fallback, just in case */
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = PyTuple_GET_ITEM(co->co_varnames, j);
            int cmp = PyObject_RichCompareBool(keyword, name, Py_EQ);
            if (cmp > 0) {
                goto kw_found;
            }
            else if (cmp < 0) {
                return -1;
            }
        }

        if (co->co_packed_flags & CODE_FLAG_VARKEYWORDS) {
            Py_ssize_t kwdict_pos = total_args;
            if ((co->co_packed_flags & CODE_FLAG_VARARGS) != 0) {
                kwdict_pos += 1;
            }
            PyObject *kwdict = AS_OBJ(ts->regs[kwdict_pos]);
            PyObject *value = AS_OBJ(ts->regs[kwdpos]);
            if (PyDict_SetItem(kwdict, keyword, value) < 0) {
                return -1;
            }
            DECREF(ts->regs[kwdpos]);
            ts->regs[kwdpos].as_int64 = 0;
            continue;
        }

        return unexpected_keyword_argument(ts, co, keyword, kwcount, kwnames);

      kw_found:
        if (UNLIKELY(ts->regs[j].as_int64 != 0)) {
            return duplicate_keyword_argument(ts, co, keyword);
        }
        ts->regs[j] = ts->regs[kwdpos];
        ts->regs[kwdpos].as_int64 = 0;
    }

    return 0;
}

int
vm_setup_kwdefaults(PyThreadState *ts, Py_ssize_t i)
{
    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyCodeObject *code = _PyFunction_GET_CODE(func);
    PyObject *kwdefs = func->func_kwdefaults;

    if (kwdefs == NULL) {
        // no keyword defaults dict; missing a required keyword argument
        return missing_arguments(ts);
    }

    Py_ssize_t total_args = code->co_totalargcount;
    for (; i < total_args; i++) {
        if (ts->regs[i].as_int64 != 0) {
            continue;
        }
        PyObject *name = PyTuple_GET_ITEM(code->co_varnames, i);
        PyObject *def = PyDict_GetItemWithError2(kwdefs, name);
        if (def) {
            ts->regs[i] = PACK_OBJ(def);
        }
        else if (_PyErr_Occurred(ts)) {
            return -1;
        }
        else {
            return missing_arguments(ts);
        }
    }

    return 0;
}

int
vm_setup_cells(PyThreadState *ts, PyCodeObject *code)
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
        if (prev.as_int64 != 0) {
            DECREF(prev);
        }
    }
    return 0;
}

Register
vm_build_set(PyThreadState *ts, Py_ssize_t base, Py_ssize_t n)
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

PyObject *
vm_build_slice(PyThreadState *ts, Py_ssize_t base)
{
    PySliceObject *obj = PyObject_GC_New(PySliceObject, &PySlice_Type);
    if (obj == NULL) {
        return NULL;
    }

    PyObject *start, *stop, *step;

    start = AS_OBJ(ts->regs[base]);
    if (!IS_RC(ts->regs[base])) {
        Py_INCREF(start);
    }

    stop = AS_OBJ(ts->regs[base + 1]);
    if (!IS_RC(ts->regs[base + 1])) {
        Py_INCREF(stop);
    }
    step = AS_OBJ(ts->regs[base + 2]);
    if (!IS_RC(ts->regs[base + 2])) {
        Py_INCREF(step);
    }

    ts->regs[base + 0].as_int64 = 0;
    ts->regs[base + 1].as_int64 = 0;
    ts->regs[base + 2].as_int64 = 0;

    obj->step = step;
    obj->start = start;
    obj->stop = stop;

    _PyObject_GC_TRACK(obj);
    return (PyObject *) obj;
}

static PyObject *
build_tuple(PyThreadState *ts, Py_ssize_t base, Py_ssize_t n)
{
    PyObject *obj = PyTuple_New(n);
    if (UNLIKELY(obj == NULL)) {
        return NULL;
    }
    Register *regs = &ts->regs[base];
    while (n) {
        n--;
        PyObject *item = vm_object_steal(&regs[n]);
        assert(item != NULL);
        PyTuple_SET_ITEM(obj, n, item);
    }
    return obj;
}

Register
vm_tuple_prepend(PyObject *tuple, PyObject *obj)
{
    PyObject *res = PyTuple_New(PyTuple_GET_SIZE(tuple) + 1);
    if (res == NULL) {
        return (Register){0};
    }
    Py_INCREF(obj);
    PyTuple_SET_ITEM(res, 0, obj);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(tuple); i++) {
        PyObject *item = PyTuple_GET_ITEM(tuple, i);
        Py_INCREF(item);
        PyTuple_SET_ITEM(res, i + 1, item);
    }
    return PACK(res, REFCOUNT_TAG);
}

int
vm_callargs_to_tuple(PyThreadState *ts, Py_ssize_t base)
{
    PyObject *args = AS_OBJ(ts->regs[base + CALLARGS_IDX]);
    PyObject *res = PySequence_Tuple(args);
    if (UNLIKELY(res == NULL)) {
        if (Py_TYPE(args)->tp_iter == NULL && !PySequence_Check(args)) {
            PyErr_Clear();
            PyObject *funcstr = _PyObject_FunctionStr(AS_OBJ(ts->regs[base-1]));
            if (funcstr != NULL) {
                _PyErr_Format(ts, PyExc_TypeError,
                            "%U argument after * must be an iterable, not %.200s",
                            funcstr, Py_TYPE(args)->tp_name);
                Py_DECREF(funcstr);
            }
        }
        return -1;
    }
    Register prev = ts->regs[base + CALLARGS_IDX];
    ts->regs[base + CALLARGS_IDX] = PACK_OBJ(res);
    DECREF(prev);
    return 0;
}

static void
format_kwargs_error(PyThreadState *tstate, PyObject *func, PyObject *kwargs)
{
    /* _PyDict_MergeEx raises attribute
     * error (percolated from an attempt
     * to get 'keys' attribute) instead of
     * a type error if its second argument
     * is not a mapping.
     */
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Clear(tstate);
        PyObject *funcstr = _PyObject_FunctionStr(func);
        if (funcstr != NULL) {
            _PyErr_Format(
                tstate, PyExc_TypeError,
                "%U argument after ** must be a mapping, not %.200s",
                funcstr, Py_TYPE(kwargs)->tp_name);
            Py_DECREF(funcstr);
        }
    }
    else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            _PyErr_Clear(tstate);
            PyObject *funcstr = _PyObject_FunctionStr(func);
            if (funcstr != NULL) {
                PyObject *key = PyTuple_GET_ITEM(val, 0);
                _PyErr_Format(
                    tstate, PyExc_TypeError,
                    "%U got multiple values for keyword argument '%S'",
                    funcstr, key);
                Py_DECREF(funcstr);
            }
            Py_XDECREF(exc);
            Py_XDECREF(val);
            Py_XDECREF(tb);
        }
        else {
            _PyErr_Restore(tstate, exc, val, tb);
        }
    }
}

int
vm_kwargs_to_dict(PyThreadState *ts, Py_ssize_t base)
{
    PyObject *d = PyDict_New();
    if (d == NULL) {
        return -1;
    }
    PyObject *kwargs = AS_OBJ(ts->regs[base + KWARGS_IDX]);
    if (_PyDict_MergeEx(d, kwargs, 2) < 0) {
        Py_DECREF(d);
        format_kwargs_error(ts, AS_OBJ(ts->regs[base-1]), kwargs);
        return -1;
    }
    Register prev = ts->regs[base + KWARGS_IDX];
    ts->regs[base + KWARGS_IDX] = PACK_OBJ(d);
    DECREF(prev);
    return 0;
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
vm_print(PyObject *value)
{
    _Py_IDENTIFIER(displayhook);
    PyObject *hook = _PySys_GetObjectId(&PyId_displayhook);
    if (hook == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "lost sys.displayhook");
        return NULL;
    }
    return _PyObject_CallOneArg(hook, value);
}

static PyObject *
vm_build_string(PyObject *const*args, Py_ssize_t nargs)
{
    PyObject *empty = PyUnicode_New(0, 0);
    assert(empty != NULL && _PyObject_IS_IMMORTAL(empty));
    return _PyUnicode_JoinArray(empty, args, nargs);
}

int
vm_setup_annotations(PyThreadState *ts, PyObject *locals)
{
    _Py_IDENTIFIER(__annotations__);
    PyObject *ann_dict;
    int err;
    if (PyDict_CheckExact(locals)) {
        ann_dict = _PyDict_GetItemIdWithError(locals, &PyId___annotations__);
        if (ann_dict != NULL) {
            return 0;
        }
        if (_PyErr_Occurred(ts)) {
            return -1;
        }
        ann_dict = PyDict_New();
        if (UNLIKELY(ann_dict == NULL)) {
            return -1;
        }
        err = _PyDict_SetItemId(locals, &PyId___annotations__, ann_dict);
        Py_DECREF(ann_dict);
        return err;
    }
    else {
        /* do the same if locals() is not a dict */
        PyObject *ann_str = _PyUnicode_FromId(&PyId___annotations__);
        if (UNLIKELY(ann_str == NULL)) {
            return -1;
        }
        ann_dict = PyObject_GetItem(locals, ann_str);
        if (ann_dict != NULL) {
            Py_DECREF(ann_dict);
            return 0;
        }
        if (!_PyErr_ExceptionMatches(ts, PyExc_KeyError)) {
            return -1;
        }
        _PyErr_Clear(ts);
        ann_dict = PyDict_New();
        if (ann_dict == NULL) {
            return -1;
        }
        err = PyObject_SetItem(locals, ann_str, ann_dict);
        Py_DECREF(ann_dict);
        return err;
    }
}

PyObject *
vm_call_intrinsic(PyThreadState *ts, Py_ssize_t id, Py_ssize_t opA, Py_ssize_t nargs)
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

#define MAX_STACK_SIZE (1073741824/sizeof(Register))
#define PY_STACK_EXTRA 1

int
vm_resize_stack(PyThreadState *tstate, Py_ssize_t needed)
{
    Py_ssize_t oldsize = tstate->maxstack - tstate->stack + PY_STACK_EXTRA;
    Py_ssize_t newsize = oldsize * 2;
    while (newsize < oldsize + needed) {
        if (newsize > (Py_ssize_t)MAX_STACK_SIZE) {
            PyErr_SetString(PyExc_MemoryError, "stack overflow");
            return -1;
        }
        newsize *= 2;
    }

    if (UNLIKELY(newsize > 4 * _Py_CheckRecursionLimit)) {
        if (vm_stack_depth(tstate) > _Py_CheckRecursionLimit) {
            PyErr_SetString(PyExc_RecursionError,
                            "maximum recursion depth exceeded");
            return -1;
        }
    }

    Py_ssize_t offset = tstate->regs - tstate->stack;
    Register *newstack = (Register *)mi_realloc(tstate->stack, newsize * sizeof(Register));
    if (newstack == NULL) {
        PyErr_SetString(PyExc_MemoryError, "unable to allocate stack");
        return -1;
    }
    tstate->stack = newstack;
    tstate->regs = newstack + offset;
    tstate->maxstack = newstack + newsize - PY_STACK_EXTRA;

    struct ThreadState *ts = tstate->active;
    ts->stack = tstate->stack;
    ts->regs = tstate->regs;
    ts->maxstack = tstate->maxstack;

    memset(newstack + oldsize, 0, (newsize - oldsize) * sizeof(Register));
    return 0;
}

static int
vm_init_stack(struct ThreadState *ts, Py_ssize_t stack_size)
{
    Register *stack = mi_malloc(stack_size * sizeof(Register));
    if (UNLIKELY(stack == NULL)) {
        return -1;
    }

    memset(stack, 0, stack_size * sizeof(Register));
    ts->stack = stack;
    ts->regs = stack;
    ts->maxstack = stack + stack_size - PY_STACK_EXTRA;
    return 0;
}

struct ThreadState *
vm_new_threadstate(PyThreadState *tstate)
{
    struct ThreadState *ts = PyMem_RawMalloc(sizeof(struct ThreadState));
    if (ts == NULL) {
        return NULL;
    }
    memset(ts, 0, sizeof(struct ThreadState));

    Py_ssize_t stack_size = 256;
    if (UNLIKELY(vm_init_stack(ts, stack_size) != 0)) {
        PyMem_RawFree(ts);
        return NULL;
    }
    ts->ts = tstate;
    return ts;
}

void
vm_free_threadstate(struct ThreadState *ts)
{
    assert(ts->prev == NULL);
    if (ts->regs != ts->stack) {
        assert(ts->regs == ts->stack + FRAME_EXTRA);
        Py_ssize_t frame_size = vm_regs_frame_size(ts->regs);
        while (frame_size) {
            --frame_size;            
        }
    }
    mi_free(ts->stack);
    ts->stack = ts->regs = ts->maxstack = NULL;
}

void
vm_push_thread_stack(PyThreadState *tstate, struct ThreadState *ts)
{
    struct ThreadState *prev = tstate->active;
    if (prev) {
        prev->pc = tstate->pc;
        prev->regs = tstate->regs;
        assert(prev->stack == tstate->stack);
        assert(prev->maxstack == tstate->maxstack);
    }
    ts->prev = prev;
    ts->ts = tstate;
    tstate->active = ts;
    tstate->regs = ts->regs;
    tstate->pc = ts->pc;
    tstate->stack = ts->stack;
    tstate->maxstack = ts->maxstack;
}

void
vm_pop_thread_stack(PyThreadState *tstate)
{
    struct ThreadState *active, *prev;
    active = tstate->active;
    prev = active->prev;

    assert(active->stack == tstate->stack);
    assert(active->maxstack == tstate->maxstack);
    active->regs = tstate->regs;
    active->pc = tstate->pc;
    active->prev = NULL;
    active->ts = NULL;

    tstate->active = prev;
    tstate->regs = prev->regs;
    tstate->pc = prev->pc;
    tstate->stack = prev->stack;
    tstate->maxstack = prev->maxstack;
}

int
vm_for_iter_exc(PyThreadState *ts)
{
    assert(PyErr_Occurred());
    PyThreadState *tstate = ts;
    if (!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
        return -1;
    }
    if (tstate->c_tracefunc != NULL) {
        vm_trace_active_exc(ts);
    }
    _PyErr_Clear(tstate);
    return 0;
}

void
vm_trace_stop_iteration(PyThreadState *ts)
{
    PyThreadState *tstate = ts;
    if (_PyErr_ExceptionMatches(tstate, PyExc_StopIteration) &&
        tstate->c_tracefunc != NULL) {
        vm_trace_active_exc(ts);
    }
}

int
vm_end_async_for(PyThreadState *ts, Py_ssize_t opA)
{
    PyObject *exc = AS_OBJ(ts->regs[opA + 2]);
    if (!PyErr_GivenExceptionMatches(exc, PyExc_StopAsyncIteration)) {
        Py_INCREF(exc);
        PyObject *type = (PyObject *)Py_TYPE(exc);
        Py_INCREF(type);
        PyObject *tb = PyException_GetTraceback(exc);
        _PyErr_Restore(ts, type, exc, tb);
        return -1;
    }
    CLEAR(ts->regs[opA + 2]);
    assert(ts->regs[opA + 1].as_int64 == -1);
    ts->regs[opA + 1].as_int64 = 0;
    CLEAR(ts->regs[opA]);
    return 0;
}

static PyObject *
vm_raise_assertion_error(PyObject *msg)
{
    if (msg == NULL) {
        PyErr_SetNone(PyExc_AssertionError);
    }
    else {
        PyObject *err = PyObject_CallOneArg(PyExc_AssertionError, msg);
        if (err == NULL) {
            return NULL;
        }
        PyErr_SetObject(PyExc_AssertionError, err);
        Py_DECREF(err);
    }
    return NULL;
}

void
vm_err_non_iterator(PyThreadState *ts, PyObject *o)
{
    PyErr_Format(PyExc_TypeError,
                 "iter() returned non-iterator "
                 "of type '%.100s'",
                 Py_TYPE(o)->tp_name);
}

void
vm_err_yield_from_coro(PyThreadState *ts)
{
    _PyErr_SetString(ts, PyExc_TypeError,
                     "cannot 'yield from' a coroutine object "
                     "in a non-coroutine generator");
}

void
vm_err_async_with_aenter(PyThreadState *ts, Register acc)
{
    PyTypeObject *type = Py_TYPE(AS_OBJ(acc));
    _PyErr_Format(ts, PyExc_TypeError,
                  "'async with' received an object from __aenter__ "
                  "that does not implement __await__: %.100s",
                  type->tp_name);
}

void
vm_err_coroutine_awaited(PyThreadState *ts)
{
    _PyErr_SetString(ts, PyExc_RuntimeError,
                    "coroutine is being awaited already");
}

static bool
is_freevar(PyCodeObject *co, Py_ssize_t varidx)
{
    for (Py_ssize_t i = co->co_ndefaultargs; i < co->co_nfreevars; i++) {
        if (co->co_free2reg[i*2+1] == varidx) {
            return true;
        }
    }
    return false;
}

void
vm_err_unbound(PyThreadState *ts, Py_ssize_t idx)
{
    /* Don't stomp existing exception */
    if (_PyErr_Occurred(ts)) {
        return;
    }
    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyCodeObject *co = _PyFunction_GET_CODE(func);
    PyObject *name = PyTuple_GET_ITEM(co->co_varnames, idx);
    int is_local = !is_freevar(co, idx);
    if (is_local) {
        PyErr_Format(
            PyExc_UnboundLocalError,
            "local variable %.200R referenced before assignment",
            name);
    }
    else {
        PyErr_Format(
            PyExc_NameError,
            "free variable %.200R referenced before assignment"
            " in enclosing scope",
            name);
    }
}

void
vm_err_async_for_aiter(PyThreadState *ts, PyTypeObject *type)
{
    _PyErr_Format(ts, PyExc_TypeError,
        "'async for' requires an object with "
        "__aiter__ method, got %.100s",
        type->tp_name);
}

void
vm_err_async_for_no_anext(PyThreadState *ts, PyTypeObject *type)
{
    _PyErr_Format(ts, PyExc_TypeError,
        "'async for' received an object from __aiter__ "
        "that does not implement __anext__: %.100s",
        type->tp_name);
}

void
vm_err_async_for_anext_invalid(PyThreadState *ts, Register res)
{
    _PyErr_FormatFromCause(PyExc_TypeError,
        "'async for' received an invalid object "
        "from __anext__: %.100s",
        Py_TYPE(AS_OBJ(res))->tp_name);
}

void
vm_err_dict_update(PyThreadState *ts, Register acc)
{
    if (_PyErr_ExceptionMatches(ts, PyExc_AttributeError)) {
        PyObject *obj = AS_OBJ(acc);
        _PyErr_Format(ts, PyExc_TypeError,
                      "'%.200s' object is not a mapping",
                      Py_TYPE(obj)->tp_name);
    }
}

void
vm_err_dict_merge(PyThreadState *ts, Register acc)
{

    /* _PyDict_MergeEx raises attribute
     * error (percolated from an attempt
     * to get 'keys' attribute) instead of
     * a type error if its second argument
     * is not a mapping.
     */
    PyThreadState *tstate = ts;

    PyObject *kwargs = AS_OBJ(acc);

    Py_ssize_t dict_reg = vm_oparg(ts->pc, 0);
    Py_ssize_t func_reg = dict_reg + FRAME_EXTRA;
    PyObject *func = AS_OBJ(ts->regs[func_reg]);

    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Clear(tstate);
        PyObject *funcstr = _PyObject_FunctionStr(func);
        if (funcstr != NULL) {
            _PyErr_Format(
                tstate, PyExc_TypeError,
                "%U argument after ** must be a mapping, not %.200s",
                funcstr, Py_TYPE(kwargs)->tp_name);
            Py_DECREF(funcstr);
        }
    }
    else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            _PyErr_Clear(tstate);
            PyObject *funcstr = _PyObject_FunctionStr(func);
            if (funcstr != NULL) {
                PyObject *key = PyTuple_GET_ITEM(val, 0);
                _PyErr_Format(
                    tstate, PyExc_TypeError,
                    "%U got multiple values for keyword argument '%S'",
                    funcstr, key);
                Py_DECREF(funcstr);
            }
            Py_XDECREF(exc);
            Py_XDECREF(val);
            Py_XDECREF(tb);
        }
        else {
            _PyErr_Restore(tstate, exc, val, tb);
        }
    }
}

void
vm_err_list_extend(PyThreadState *ts, Register acc)
{
    PyThreadState *tstate = ts;
    PyObject *iterable = AS_OBJ(acc);

    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
        (Py_TYPE(iterable)->tp_iter == NULL && !PySequence_Check(iterable)))
    {
        _PyErr_Clear(tstate);
        _PyErr_Format(tstate, PyExc_TypeError,
            "Value after * must be an iterable, not %.200s",
            Py_TYPE(iterable)->tp_name);
    }
}

PyObject *
vm_err_name(PyThreadState *ts, int oparg)
{
    PyObject *name = vm_constant(ts, oparg);
    const char *obj_str = PyUnicode_AsUTF8(name);
    if (obj_str == NULL) {
        return NULL;
    }
    _PyErr_Format(ts, PyExc_NameError, "name '%.200s' is not defined", obj_str);
    return NULL;
}

PyObject *
vm_load_method_err(PyThreadState *ts, Register acc)
{
    PyObject *owner = AS_OBJ(acc);
    PyObject *name = vm_constant(ts, 1);
    if (PyModule_CheckExact(owner)) {
        return _PyModule_MissingAttr(owner, name);
    }

    _PyErr_Format(ts, PyExc_AttributeError,
                 "'%.50s' object has no attribute '%U'",
                 Py_TYPE(owner)->tp_name, name);
    return NULL;
}

int
vm_init_thread_state(PyThreadState *tstate, PyGenObject *gen)
{
    struct ThreadState *ts = &gen->base.thread;
    memset(ts, 0, sizeof(*ts));

    Py_ssize_t generator_stack_size = 256;
    if (UNLIKELY(vm_init_stack(ts, generator_stack_size) != 0)) {
        return -1;
    }

    ts->thread_type = THREAD_GENERATOR;

    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(tstate->regs[-1]);
    PyCodeObject *code = _PyFunction_GET_CODE(func);

    // Copy over func and arguments, but not the frame object.
    // We don't want to copy the frame object because frame->f_offset
    // and frame->ts would be incorrect
    Py_ssize_t frame_delta = FRAME_EXTRA;
    ts->regs += frame_delta;
    ts->regs[-4].as_int64 = frame_delta;
    ts->regs[-3].as_int64 = FRAME_GENERATOR;
    ts->regs[-1] = STRONG_REF(tstate->regs[-1]);  // copy func

    // The new thread-state takes ownership of the "func".
    // We can't clear the old thread states function because it will be
    // referenced (and cleared) by RETURN_VALUE momentarily. Instead, just
    // mark it as a non-refcounted reference -- the generator owns them now.
    tstate->regs[-1].as_int64 |= NO_REFCOUNT_TAG;

    Py_ssize_t nargs = code->co_totalargcount;
    if (code->co_packed_flags & CODE_FLAG_VARARGS) {
        // FIXME(sgross): I think this is wrong now that varargs are prior to header
        nargs += 1;
    }
    if (code->co_packed_flags & CODE_FLAG_VARKEYWORDS) {
        // FIXME(sgross): I think this is wrong now that varargs are prior to header
        nargs += 1;
    }
    for (Py_ssize_t i = 0; i != nargs; i++) {
        // NB: we have to convert aliases into strong references. The
        // generator may outlive the calling frame.
        ts->regs[i] = STRONG_REF(tstate->regs[i]);
        tstate->regs[i].as_int64 = 0;
    }
    if (code->co_packed_flags & CODE_FLAG_LOCALS_DICT) {
        assert(nargs == 0);
        ts->regs[0] = tstate->regs[0];
        tstate->regs[0].as_int64 = 0;
    }
    for (Py_ssize_t i = code->co_ndefaultargs; i != code->co_nfreevars; i++) {
        Py_ssize_t r = code->co_free2reg[i*2+1];
        ts->regs[r] = tstate->regs[r];
        tstate->regs[r].as_int64 = 0;
    }
    for (Py_ssize_t i = 0; i != code->co_ncells; i++) {
        Py_ssize_t r = code->co_cell2reg[i];
        if (r >= nargs) {
            ts->regs[r] = tstate->regs[r];
            tstate->regs[r].as_int64 = 0;
        }
    }
    ts->ts = PyThreadState_GET();
    return 0;
}

static int
setup_frame_ex(PyThreadState *ts, PyObject *func, Py_ssize_t extra, Py_ssize_t nargs)
{
    assert(PyType_HasFeature(Py_TYPE(func), Py_TPFLAGS_FUNC_INTERFACE));
    Py_ssize_t frame_delta = vm_frame_size(ts) + FRAME_EXTRA + extra;
    Py_ssize_t frame_size = frame_delta + nargs;
    if (UNLIKELY(ts->regs + frame_size > ts->maxstack)) {
        if (vm_resize_stack(ts, frame_size) != 0) {
            return -1;
        }
    }

    ts->regs += frame_delta;

    ts->regs[-4].as_int64 = frame_delta;
    ts->regs[-3].as_int64 = -(intptr_t)ts->pc;
    ts->regs[-1] = PACK(func, NO_REFCOUNT_TAG); // this_func
    return 0;

}

static int
setup_frame(PyThreadState *ts, PyObject *func)
{
    return setup_frame_ex(ts, func, /*extra=*/0, /*nargs=*/0);
}

static int
vm_trace_enter_gen(PyThreadState *ts);

static PyObject *
_PyEval_Eval(PyThreadState *tstate, Register acc, const uint8_t *pc)
{
    PyObject *ret;
    PyObject *cargs[9];

    PyObject **prevargs = tstate->cargs;
    tstate->cargs = &cargs[1];
    ret = _PyEval_Fast(tstate, acc, pc);
    tstate->cargs = prevargs;
    return ret;
}

PyObject *
PyEval2_EvalGen(PyGenObject *gen, PyObject *opt_value)
{
    PyThreadState *tstate = PyThreadState_GET();
    struct ThreadState *ts = &gen->base.thread;

    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    assert(ts->prev == NULL);

    // push `ts` onto the list of active threads
    vm_push_thread_stack(tstate, ts);

    PyObject *ret = NULL;

    if (tstate->use_tracing) {
        if (vm_trace_enter_gen(tstate) != 0) {
            goto exit;
        }
    }

    gen->status = GEN_RUNNING;

    Register acc = opt_value ? PACK_INCREF(opt_value) : (Register){0};
    ret = _PyEval_Eval(tstate, acc, tstate->pc);

exit:
    // pop `ts` from the list of active threads
    vm_pop_thread_stack(tstate);

    _Py_LeaveRecursiveCall(tstate);
    return ret;
}

PyObject *
_PyEval_EvalFunc(PyObject *func, PyObject *locals)
{
    assert(PyFunction_Check(func));
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *ret = NULL;
    int err;

    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    err = setup_frame(tstate, func);
    if (UNLIKELY(err != 0)) {
        goto exit;
    }
    tstate->regs[0] = PACK(locals, NO_REFCOUNT_TAG);

    Register acc;
    acc.as_int64 = 0;
    ret = _PyEval_Eval(tstate, acc, ((PyFuncBase *)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}

int
vm_super_init(PyObject **out_obj, PyTypeObject **out_type)
{
    _Py_IDENTIFIER(__class__);

    PyThreadState *ts = _PyThreadState_GET();
    if (ts->regs == ts->stack) {
        PyErr_SetString(PyExc_RuntimeError,
                        "super(): no current frame");
        return -1;
    }

    /* The top frame is the invocation of super() */
    if (AS_OBJ(ts->regs[-1]) != (PyObject*)&PySuper_Type) {
        PyErr_SetString(PyExc_RuntimeError,
                        "super(): missing super frame");
        return -1;
    }

    /* The next frame is the function that called super() */
    intptr_t frame_delta = ts->regs[-4].as_int64;

    PyObject *func = AS_OBJ(ts->regs[-1 - frame_delta]);
    if (func == NULL || !PyFunction_Check(func)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "super(): no current function");
        return -1;
    }
    PyCodeObject *co = _PyFunction_GET_CODE((PyFunctionObject *)func);
    if (co->co_argcount == 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "super(): no arguments");
        return -1;
    }
    PyObject *obj = AS_OBJ(ts->regs[0 - frame_delta]);
    if (obj == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "super(): arg[0] deleted");
        return -1;
    }
    if (PyCell_Check(obj)) {
        /* The first argument might be a cell. */
        Py_ssize_t n = co->co_ncells;
        for (Py_ssize_t i = 0; i < n; i++) {
            if (co->co_cell2reg[i] == 0) {
                obj = PyCell_GET(obj);
                break;
            }
        }
    }
    for (Py_ssize_t i = co->co_ndefaultargs, n = co->co_nfreevars; i < n; i++) {
        Py_ssize_t r = co->co_free2reg[i*2+1];
        PyObject *name = PyTuple_GET_ITEM(co->co_varnames, r);
        if (_PyUnicode_EqualToASCIIId(name, &PyId___class__)) {
            PyObject *cell = AS_OBJ(ts->regs[r - frame_delta]);
            if (cell == NULL || !PyCell_Check(cell)) {
                PyErr_SetString(PyExc_RuntimeError,
                  "super(): bad __class__ cell");
                return -1;
            }
            PyTypeObject *type = (PyTypeObject *) PyCell_GET(cell);
            if (type == NULL) {
                PyErr_SetString(PyExc_RuntimeError,
                  "super(): empty __class__ cell");
                return -1;
            }
            if (!PyType_Check(type)) {
                PyErr_Format(PyExc_RuntimeError,
                  "super(): __class__ is not a type (%s)",
                  Py_TYPE(type)->tp_name);
                return -1;
            }

            *out_obj = obj;
            *out_type = type;
            return 0;
        }
    }

    PyErr_SetString(PyExc_RuntimeError,
                    "super(): __class__ cell not found");
    return -1;
}

PyObject *
vm_import_from(PyThreadState *ts, PyObject *v, PyObject *name)
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
    if (x == NULL && !_PyErr_Occurred(ts)) {
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
        _PyErr_Clear(ts);
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

int
vm_import_star(PyThreadState *ts, PyObject *v, PyObject *locals)
{
    _Py_IDENTIFIER(__all__);
    _Py_IDENTIFIER(__dict__);
    _Py_IDENTIFIER(__name__);
    PyObject *all, *dict, *name, *value;
    int skip_leading_underscores = 0;
    int pos, err;

    if (_PyObject_LookupAttrId(v, &PyId___all__, &all) < 0) {
        return -1; /* Unexpected error */
    }
    if (all == NULL) {
        if (_PyObject_LookupAttrId(v, &PyId___dict__, &dict) < 0) {
            return -1;
        }
        if (dict == NULL) {
            _PyErr_SetString(ts, PyExc_ImportError,
                    "from-import-* object has no __dict__ and no __all__");
            return -1;
        }
        all = PyMapping_Keys(dict);
        Py_DECREF(dict);
        if (all == NULL)
            return -1;
        skip_leading_underscores = 1;
    }

    for (pos = 0, err = 0; ; pos++) {
        name = PySequence_GetItem(all, pos);
        if (name == NULL) {
            if (!_PyErr_ExceptionMatches(ts, PyExc_IndexError)) {
                err = -1;
            }
            else {
                _PyErr_Clear(ts);
            }
            break;
        }
        if (!PyUnicode_Check(name)) {
            PyObject *modname = _PyObject_GetAttrId(v, &PyId___name__);
            if (modname == NULL) {
                Py_DECREF(name);
                err = -1;
                break;
            }
            if (!PyUnicode_Check(modname)) {
                _PyErr_Format(ts, PyExc_TypeError,
                              "module __name__ must be a string, not %.100s",
                              Py_TYPE(modname)->tp_name);
            }
            else {
                _PyErr_Format(ts, PyExc_TypeError,
                              "%s in %U.%s must be str, not %.100s",
                              skip_leading_underscores ? "Key" : "Item",
                              modname,
                              skip_leading_underscores ? "__dict__" : "__all__",
                              Py_TYPE(name)->tp_name);
            }
            Py_DECREF(modname);
            Py_DECREF(name);
            err = -1;
            break;
        }
        if (skip_leading_underscores) {
            if (PyUnicode_READY(name) == -1) {
                Py_DECREF(name);
                err = -1;
                break;
            }
            if (PyUnicode_READ_CHAR(name, 0) == '_') {
                Py_DECREF(name);
                continue;
            }
        }
        value = PyObject_GetAttr(v, name);
        if (value == NULL)
            err = -1;
        else if (PyDict_CheckExact(locals))
            err = PyDict_SetItem(locals, name, value);
        else
            err = PyObject_SetItem(locals, name, value);
        Py_DECREF(name);
        Py_XDECREF(value);
        if (err != 0)
            break;
    }
    Py_DECREF(all);
    return err;
}

// TODO: can we move this to funcobject2.c? should we?
PyObject *
_PyFunc_Call(PyObject *func, PyObject *args, PyObject *kwds)
{
    PyThreadState *tstate = PyThreadState_GET();
    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    Register acc;
    PyObject *ret = NULL;

    if (PyTuple_GET_SIZE(args) == 0 && kwds == NULL) {
        acc.as_int64 = 0;
        int err = setup_frame(tstate, func);
        if (UNLIKELY(err != 0)) {
            goto exit;
        }
    }
    else {
        acc.as_int64 = ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS;
        int err = setup_frame_ex(tstate, func, /*extra=*/2, /*nargs=*/0);
        if (UNLIKELY(err != 0)) {
            goto exit;
        }
        tstate->regs[-FRAME_EXTRA-2] = PACK(args, NO_REFCOUNT_TAG);
        if (kwds != NULL) {
            tstate->regs[-FRAME_EXTRA-1] = PACK(kwds, NO_REFCOUNT_TAG);
        }
    }

    ret = _PyEval_Eval(tstate, acc, ((PyFuncBase*)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}

PyObject *
_PyFunction_Vectorcall(PyObject *func, PyObject* const* stack,
                       size_t nargsf, PyObject *kwnames)
{
    PyThreadState *tstate = PyThreadState_GET();
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkwargs = (kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames));

    if (UNLIKELY(nargs >= 255 || nkwargs >= 256)) {
        return _PyObject_MakeTpCall(tstate, func, stack, nargs, kwnames);
    }

    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    // assert(tstate->active->regs == tstate->regs);
    PyObject *ret = NULL;
    Register acc;
    int err;

    acc.as_int64 = nargs + (nkwargs << 8);
    Py_ssize_t extra = (nkwargs == 0 ? 0 : nkwargs + 1);

    err = setup_frame_ex(tstate, func, extra, nargs);
    if (UNLIKELY(err != 0)) {
        goto exit;
    }

    // setup positional arguments
    for (Py_ssize_t i = 0; i < nargs; i++) {
        tstate->regs[i] = PACK(stack[i], NO_REFCOUNT_TAG);
    }

    // setup keyword arguments
    if (nkwargs != 0) {
        for (Py_ssize_t i = 0; i < nkwargs; i++) {
            tstate->regs[-FRAME_EXTRA - 1 - nkwargs + i] = PACK(stack[i + nargs], NO_REFCOUNT_TAG);
        }
        tstate->regs[-FRAME_EXTRA - 1] = PACK(kwnames, NO_REFCOUNT_TAG);
    }

    ret = _PyEval_Eval(tstate, acc, ((PyFuncBase*)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}


PyObject *
PyEval_GetGlobals(void)
{
    struct ThreadState *ts = vm_active(_PyThreadState_GET());
    if (ts == NULL) {
        return NULL;
    }

    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    if (vm_stack_walk(&w)) {
        PyObject *func = AS_OBJ(w.regs[-1]);
        return ((PyFunctionObject *)func)->globals;
    }
    // no frame
    return NULL;
}

// Returns borrowed reference
PyFrameObject *
vm_frame(PyThreadState *ts)
{
    return vm_frame_at_offset(vm_active(ts), PY_SSIZE_T_MAX);
}

PyFrameObject *
vm_frame_at_offset(struct ThreadState *ts, Py_ssize_t offset)
{
    if (ts == PyThreadState_GET()->active) {
        vm_active(PyThreadState_GET());
    }

    PyFrameObject *top = NULL;
    PyFrameObject *prev = NULL;

    bool done = false;
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w) && !done) {
        if (w.offset > offset) {
            continue;
        }

        PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(w.regs[-1]);
        PyCodeObject *co = _PyFunction_GET_CODE(func);

        assert((uintptr_t)w.pc >= (uintptr_t)func->func_base.first_instr);
        assert((uintptr_t)w.pc < (uintptr_t)(func->func_base.first_instr + co->co_size));

        PyFrameObject *frame = (PyFrameObject *)AS_OBJ(w.regs[-2]);
        if (frame == NULL) {
            frame = _PyFrame_NewFake(co, func->globals);
            if (frame == NULL) {
                return NULL;
            }
            // NOTE: allocating the frame may re-allocate regs!
            w.regs = &w.ts->stack[w.offset];
            w.regs[-2] = PACK(frame, REFCOUNT_TAG);

            frame->f_ts = w.ts;
            frame->f_offset = w.regs - w.ts->stack;
            frame->f_executing = true;
        }
        else {
            done = true;
        }

        // Update f_lasti
        int addrq = (w.pc - PyCode_FirstInstr(co));
        if (w.frame_link > 0 ||
            (w.ts->thread_type == THREAD_GENERATOR &&
             PyGen_FromThread(w.ts)->status == GEN_CREATED))
        {
            // TODO: this is an awful hack becuase sometimes w.pc points to
            // next instruction and sometimes to the current instruction.
            addrq -= 1; // :(
        }
        frame->f_lasti = addrq;

        if (top == NULL) {
            top = frame;
        }
        if (prev != NULL) {
            Py_INCREF(frame);
            assert(prev->f_back == NULL);
            Py_XSETREF(prev->f_back, frame);
        }
        prev = frame;
    }

    return top;
}

void
vm_clear_frame(PyThreadState *ts)
{
    PyFrameObject *frame = (PyFrameObject *)AS_OBJ(ts->regs[-2]);
    frame->f_executing = 0;
    frame->f_ts = NULL;
    frame->f_offset = 0;
    ts->regs[-2].as_int64 = 0;

    if (Py_REFCNT(frame) == 1) {
        Py_DECREF(frame);
        return;
    }

    PyObject *func = AS_OBJ(ts->regs[-1]);
    PyCodeObject *co = _PyFunction_GET_CODE((PyFunctionObject *)func);
    for (Py_ssize_t i = 0, n = co->co_nlocals; i < n; i++) {
        Register r = ts->regs[i];
        ts->regs[i].as_int64 = 0;

        PyObject *ob = AS_OBJ(r);
        if (r.as_int64 != 0 && !IS_RC(r)) {
            Py_INCREF(ob);
        }

        Py_XSETREF(frame->f_localsplus[i], ob);
    }
    Py_DECREF(frame);
}

PyObject *
vm_locals(PyFrameObject *frame)
{
    PyCodeObject *code = frame->f_code;
    if ((code->co_flags & CO_NEWLOCALS) == 0) {
        PyObject *locals = frame->f_locals;
        if (locals == NULL) {
            if (frame->f_ts) {
                Register *regs = &frame->f_ts->stack[frame->f_offset];
                locals = AS_OBJ(regs[0]);
            }
            else {
                locals = frame->f_localsplus[0];
            }
            Py_INCREF(locals);
            frame->f_locals = locals;
        }
        return locals;
    }

    PyObject *locals = frame->f_locals;
    if (locals == NULL) {
        locals = frame->f_locals = PyDict_New();
        if (locals == NULL) {
            return NULL;
        }
    }

    PyObject **vars = PyMem_RawMalloc(code->co_nlocals * sizeof(PyObject*));
    if (vars == NULL) {
        return PyErr_NoMemory();
    }

    if (frame->f_ts) {
        Register *regs = &frame->f_ts->stack[frame->f_offset];
        for (Py_ssize_t i = 0, n = code->co_nlocals; i < n; i++) {
            vars[i] = AS_OBJ(regs[i]);
        }
    }
    else {
        for (Py_ssize_t i = 0, n = code->co_nlocals; i < n; i++) {
            vars[i] = frame->f_localsplus[i];
        }
    }

    for (Py_ssize_t i = 0, n = code->co_ncells; i < n; i++) {
        Py_ssize_t reg = code->co_cell2reg[i];
        if (vars[reg]) {
            assert(PyCell_Check(vars[reg]));
            vars[reg] = PyCell_GET(vars[reg]);
        }
    }

    Py_ssize_t ndefaults = code->co_ndefaultargs;
    for (Py_ssize_t i = ndefaults, n = code->co_nfreevars; i < n; i++) {
        Py_ssize_t reg = code->co_free2reg[i*2+1];
        if (vars[reg]) {
            assert(PyCell_Check(vars[reg]));
            vars[reg] = PyCell_GET(vars[reg]);
        }
    }

    for (Py_ssize_t i = 0, n = code->co_nlocals; i < n; i++) {
        PyObject *name = PyTuple_GET_ITEM(code->co_varnames, i);
        PyObject *value = vars[i];
        int err;

        if (value == NULL) {
            err = PyObject_DelItem(locals, name);
            if (err != 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_Clear();
                continue;
            }
        }
        else {
            err = PyDict_SetItem(locals, name, vars[i]);
        }
        if (err != 0) {
            PyMem_RawFree(vars);
            return NULL;
        }
    }

    PyMem_RawFree(vars);
    return locals;
}

int
vm_eval_breaker(PyThreadState *ts)
{
    int opcode = vm_opcode(ts->pc);
    if (opcode == YIELD_FROM) {
        return 0;
    }
    return _PyEval_HandleBreaker(ts);
}

PyObject *
PyEval_GetLocals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyFrameObject *frame = vm_frame(tstate);
    if (frame == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError, "frame does not exist");
        return NULL;
    }

    return vm_locals(frame);
}

static int
call_trace(PyThreadState *ts, PyFrameObject *frame,
           int what, PyObject *arg)
{
    int result;
    PyThreadState *tstate = ts;
    Py_tracefunc func = tstate->c_tracefunc;
    PyObject *obj = tstate->c_traceobj;
    PyObject *type, *value, *traceback;
    if (func == NULL) {
        return 0;
    }

    _PyErr_Fetch(tstate, &type, &value, &traceback);
    tstate->tracing++;
    tstate->use_tracing = 0;
    result = func(obj, frame, what, arg);
    tstate->use_tracing = ((tstate->c_tracefunc != NULL)
                           || (tstate->c_profilefunc != NULL));
    tstate->tracing--;
    if (result == 0) {
        _PyErr_Restore(tstate, type, value, traceback);
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
    }
    return result;
}

static int
call_profile(PyThreadState *ts, PyFrameObject *frame,
             int what, PyObject *arg)
{
    int result;
    PyThreadState *tstate = ts;
    Py_tracefunc func = tstate->c_profilefunc;
    PyObject *obj = tstate->c_profileobj;
    PyObject *type, *value, *traceback;

    _PyErr_Fetch(tstate, &type, &value, &traceback);
    tstate->tracing++;
    result = func(obj, frame, what, arg);
    tstate->tracing--;
    if (result == 0) {
        _PyErr_Restore(tstate, type, value, traceback);
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
    }
    return result;
}

PyObject *
vm_trace_cfunc(PyThreadState *ts, Register acc)
{
    int err;
    PyObject *res = NULL;

    PyThreadState *tstate = ts;
    if (tstate->tracing || tstate->c_profilefunc == NULL) {
        if (ts->pc[0] == FUNC_TPCALL_HEADER) {
            return vm_tpcall_function(ts, acc);
        }
        else {
            return vm_call_cfunction(ts, acc);
        }
    }

    PyFrameObject *frame = vm_frame(ts);
    if (frame == NULL) {
        return NULL;
    }

    PyObject *func = AS_OBJ(ts->regs[-1]);
    if (Py_IS_TYPE(func, &PyMethodDescr_Type)) {
        // We need to create a temporary bound method as argument for
        // profiling.
        PyObject *self = NULL;
        if (ACC_ARGCOUNT(acc) > 0) {
            self = AS_OBJ(ts->regs[0]);
        }
        else if ((acc.as_int64 & ACC_FLAG_VARARGS) != 0) {
            PyObject *varargs = AS_OBJ(ts->regs[-FRAME_EXTRA - 2]);
            Py_ssize_t argcount = PyTuple_GET_SIZE(varargs);
            if (argcount > 0) {
                self = PyTuple_GET_ITEM(varargs, 0);
            }
        }

        if (self == NULL) {
            // If nargs == 0, then this cannot work because we have no
            // "self". In any case, the call itself would raise
            // TypeError (foo needs an argument), so we just skip profiling.
            return vm_call_cfunction(ts, acc);
        }

        func = Py_TYPE(func)->tp_descr_get(func, self, (PyObject*)Py_TYPE(self));
        if (func == NULL) {
            return NULL;
        }
    }
    else {
        Py_INCREF(func);
    }

    if (call_profile(ts, frame, PyTrace_C_CALL, func) != 0) {
        Py_DECREF(func);
        return NULL;
    }

    // NOTE: CFUNC_HEADER and FUNC_TPCALL_HEADER do not have WIDE variants
    int opcode = ts->pc[0];
    // assert(opcode == CFUNC_HEADER || opcode == FUNC_TPCALL_HEADER);
    if (opcode == FUNC_TPCALL_HEADER) {
        res = vm_tpcall_function(ts, acc);
    }
    else {
        res = vm_call_cfunction(ts, acc);
    }

    if (tstate->c_profilefunc != NULL) {
        if (res == NULL) {
            PyObject *exc = NULL, *val = NULL, *tb = NULL;
            _PyErr_Fetch(ts, &exc, &val, &tb);
            err = call_profile(ts, frame, PyTrace_C_EXCEPTION, func);
            if (err != 0) {
                Py_XDECREF(exc);
                Py_XDECREF(val);
                Py_XDECREF(tb);
            }
            else {
                PyErr_Restore(exc, val, tb);
            }
        }
        else {
            err = call_profile(ts, frame, PyTrace_C_RETURN, func);
            if (err != 0) {
                Py_CLEAR(res);
            }
        }
    }

    Py_DECREF(func);
    return res;
}

int
vm_profile(PyThreadState *ts, const uint8_t *last_pc, Register acc)
{
    int opcode = vm_opcode(ts->pc);
    int last_opcode = last_pc ? vm_opcode(last_pc) : -1;

    if (last_opcode == FUNC_HEADER) {
        PyCodeObject *co = PyCode_FromFirstInstr(last_pc);
        if (!(co->co_packed_flags & CODE_FLAG_GENERATOR)) {
            // trace calls into functions, but not ones that create generators
            // because that's how CPython profiling has worked historically
            PyFrameObject *frame = vm_frame(ts);
            if (frame == NULL) {
                return -1;
            }
            if (call_profile(ts, frame, PyTrace_CALL, Py_None) != 0) {
                return -1;
            }
        }
    }

    if (opcode == RETURN_VALUE || opcode == YIELD_VALUE) {
        PyFrameObject *frame = vm_frame(ts);
        if (frame == NULL) {
            return -1;
        }
        if (call_profile(ts, frame, PyTrace_RETURN, AS_OBJ(acc)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int
vm_last_line(PyCodeObject *code, const uint8_t *last_pc)
{
    if (last_pc == NULL) {
        return -1;
    }
    const uint8_t *first_instr = PyCode_FirstInstr(code);
    ptrdiff_t delta = last_pc - first_instr;
    if (delta < 0 || delta >= code->co_size) {
        return -1;
    }
    return PyCode_Addr2Line(code, (int)delta);
}

static int
vm_trace(PyThreadState *ts, const uint8_t *last_pc, Register acc)
{
    PyObject *callable = AS_OBJ(ts->regs[-1]);
    if (!PyFunction_Check(callable)) {
        return 0;
    }

    PyFrameObject *frame = vm_frame(ts);
    if (frame == NULL) {
        return -1;
    }

    PyFunctionObject *func = (PyFunctionObject *)callable;
    PyCodeObject *code = _PyFunction_GET_CODE(func);

    int opcode = vm_opcode(ts->pc);
    int last_opcode = last_pc ? vm_opcode(last_pc) : -1;

    int addrq = (ts->pc - func->func_base.first_instr);
    assert(addrq >= 0 && addrq < code->co_size);
    int line = frame->f_lineno;

    if (addrq < frame->instr_lb || addrq >= frame->instr_ub) {
        PyAddrPair bounds;
        line = _PyCode_CheckLineNumber(code, addrq, &bounds);
        frame->instr_lb = bounds.ap_lower;
        frame->instr_ub = bounds.ap_upper;
    }

    int trace_line = (addrq == frame->instr_lb);
    if (addrq >= frame->instr_prev && ts->pc != last_pc) {
        int last_line = vm_last_line(code, last_pc);
        if (last_line == line) {
            trace_line = 0;
        }
    }

    frame->f_lasti = addrq;
    frame->instr_prev = addrq;
    if (opcode == FUNC_HEADER) {
        frame->seen_func_header = 1;
        trace_line = 0;
        frame->f_lineno = line;
    }
    else if (last_opcode == FUNC_HEADER && code == PyCode_FromFirstInstr(last_pc)) {
        frame->f_lasti = 0;
        frame->traced_func = 1;
        trace_line = 1;

        // set pc to point at FUNC_HEADER
        const uint8_t *pc = ts->pc;
        ts->pc = last_pc;
        int err = call_trace(ts, frame, PyTrace_CALL, Py_None);
        ts->pc = pc;

        if (err != 0) {
            return -1;
        }
        frame->f_lasti = addrq;
    }

    /* If the last instruction falls at the start of a line or if it
       represents a jump backwards, update the frame's line number and
       then call the trace function if we're tracing source lines.
    */
    if (trace_line) {
        frame->f_lineno = line;
        frame->last_line = line;
        if (frame->f_trace_lines) {
            int err = call_trace(ts, frame, PyTrace_LINE, Py_None);
            if (err != 0) {
                return -1;
            }
        }
    }

    /* Always emit an opcode event if we're tracing all opcodes. */
    if (frame->f_trace_opcodes) {
        int err = call_trace(ts, frame, PyTrace_OPCODE, Py_None);
        if (err != 0) {
            return -1;
        }
    }

    if (opcode == RETURN_VALUE || opcode == YIELD_VALUE) {
        int err = call_trace(ts, frame, PyTrace_RETURN, AS_OBJ(acc));
        if (err != 0) {
            return -1;
        }
    }

    return 0;
}

int
vm_trace_handler(PyThreadState *ts, const uint8_t *last_pc, Register acc)
{
    PyThreadState *tstate = ts;
    if (tstate->tracing || ts->regs == ts->stack) {
        return 0;
    }

    int err;
    if (tstate->c_tracefunc != NULL) {
        err = vm_trace(ts, last_pc, acc);
        if (err != 0) {
            return -1;
        }
    }

    if (tstate->c_profilefunc != NULL) {
        err = vm_profile(ts, last_pc, acc);
        if (err != 0) {
            return -1;
        }
    }

    return 0;
}

static void
vm_trace_err(PyThreadState *tstate, PyObject **type, PyObject **value, PyObject **traceback)
{
    if (tstate->tracing) {
        return;
    }

    PyFrameObject *frame = vm_frame(tstate);
    if (frame == NULL) {
        PyErr_WriteUnraisable(NULL);
        return;
    }

    _PyErr_NormalizeException(tstate, type, value, traceback);
    PyObject *tb = *traceback;
    PyObject *arg = PyTuple_Pack(3, *type, *value, tb ? tb : Py_None);
    if (arg == NULL) {
        PyErr_WriteUnraisable(NULL);
        return;
    }

    int err;
    if (tstate->c_tracefunc != NULL) {
        err = call_trace(tstate, frame, PyTrace_EXCEPTION, arg);
        if (err != 0) {
            Py_CLEAR(*type);
            Py_CLEAR(*value);
            Py_CLEAR(*traceback);
            PyErr_Fetch(type, value, traceback);
        }
    }

    Py_DECREF(arg);
}

static void
vm_trace_active_exc(PyThreadState *ts)
{
    PyObject *type, *value, *tb;
    _PyErr_Fetch(ts, &type, &value, &tb);
    vm_trace_err(ts, &type, &value, &tb);
    _PyErr_Restore(ts, type, value, tb);
}

static int
vm_trace_return(PyThreadState *tstate)
{
    if (tstate->tracing) {
        return 0;
    }

    PyFrameObject *frame = vm_frame(tstate);
    if (frame == NULL) {
        return -1;
    }

    if (tstate->c_tracefunc != NULL) {
        if (call_trace(tstate, frame, PyTrace_RETURN, NULL) != 0) {
            return -1;
        }
    }

    if (tstate->c_profilefunc != NULL) {
        if (call_profile(tstate, frame, PyTrace_RETURN, NULL) != 0) {
            return -1;
        }
    }
    return 0;
}

static int
vm_trace_enter_gen(PyThreadState *tstate)
{
    if (tstate->tracing) {
        return 0;
    }

    PyFrameObject *frame = vm_frame(tstate);
    if (frame == NULL) {
        return -1;
    }

    if (tstate->c_tracefunc != NULL) {
        if (call_trace(tstate, frame, PyTrace_CALL, Py_None) != 0) {
            return -1;
        }
    }

    if (tstate->c_profilefunc != NULL) {
        if (call_profile(tstate, frame, PyTrace_CALL, Py_None) != 0) {
            return -1;
        }
    }

    return 0;
}

PyObject *
_Py_method_call(PyObject *obj, PyObject *args, PyObject *kwds)
{
    PyMethodObject *method = (PyMethodObject *)obj;
    if (UNLIKELY(!PyFunction_Check(method->im_func))) {
        return PyVectorcall_Call(obj, args, kwds);
    }
    if (kwds != NULL || PyTuple_GET_SIZE(args) >= 255) {
        return _PyFunc_Call(obj, args, kwds);
    }

    // optimization for positional arguments only
    PyThreadState *tstate = PyThreadState_GET();
    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    // assert(tstate->active->regs == tstate->regs);
    Py_ssize_t nargs = 1 + PyTuple_GET_SIZE(args);
    PyObject *ret = NULL;
    PyObject *func = method->im_func;

    int err = setup_frame_ex(tstate, func, /*extra=*/0, /*nargs=*/nargs);
    if (UNLIKELY(err != 0)) {
        goto exit;
    }

    tstate->regs[0] = PACK(method->im_self, NO_REFCOUNT_TAG);
    for (Py_ssize_t i = 1; i < nargs; i++) {
        tstate->regs[i] = PACK(PyTuple_GET_ITEM(args, i - 1), NO_REFCOUNT_TAG);
    }

    Register acc;
    acc.as_int64 = nargs;
    ret = _PyEval_Eval(tstate, acc, ((PyFuncBase*)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}



/* Mechanism whereby asynchronously executing callbacks (e.g. UNIX
   signal handlers or Mac I/O completion routines) can schedule calls
   to a function to be called synchronously.
   The synchronous function is called with one void* argument.
   It should return 0 for success or -1 for failure -- failure should
   be accompanied by an exception.

   If registry succeeds, the registry function returns 0; if it fails
   (e.g. due to too many pending calls) it returns -1 (without setting
   an exception condition).

   Note that because registry may occur from within signal handlers,
   or other asynchronous events, calling malloc() is unsafe!

   Any thread can schedule pending calls, but only the main thread
   will execute them.
   There is no facility to schedule calls to a particular thread, but
   that should be easy to change, should that ever be required.  In
   that case, the static variables here should go into the python
   threadstate.
*/

/* Push one item onto the queue while holding the lock. */
static int
_push_pending_call(struct _pending_calls *pending,
                   int (*func)(void *), void *arg)
{
    int i = pending->last;
    int j = (i + 1) % NPENDINGCALLS;
    if (j == pending->first) {
        return -1; /* Queue full */
    }
    pending->calls[i].func = func;
    pending->calls[i].arg = arg;
    pending->last = j;
    return 0;
}

/* Pop one item off the queue while holding the lock. */
static void
_pop_pending_call(struct _pending_calls *pending,
                  int (**func)(void *), void **arg)
{
    int i = pending->first;
    if (i == pending->last) {
        return; /* Queue empty */
    }

    *func = pending->calls[i].func;
    *arg = pending->calls[i].arg;
    pending->first = (i + 1) % NPENDINGCALLS;
}

/* This implementation is thread-safe.  It allows
   scheduling to be made from any thread, and even from an executing
   callback.
 */

int
_PyEval_AddPendingCall(PyInterpreterState *interp,
                       int (*func)(void *), void *arg)
{
    struct _pending_calls *pending = &interp->ceval.pending;

    /* Ensure that _PyEval_InitPendingCalls() was called
       and that _PyEval_FiniPendingCalls() is not called yet. */
    assert(pending->lock != NULL);

    PyThread_acquire_lock(pending->lock, WAIT_LOCK);
    int result = _push_pending_call(pending, func, arg);
    PyThread_release_lock(pending->lock);

    /* signal main loop */
    _PyThreadState_Signal(_PyRuntime.main_tstate, EVAL_PENDING_CALLS);
    return result;
}

int
Py_AddPendingCall(int (*func)(void *), void *arg)
{
    /* Best-effort to support subinterpreters and calls with the GIL released.

       First attempt _PyThreadState_GET() since it supports subinterpreters.

       If the GIL is released, _PyThreadState_GET() returns NULL . In this
       case, use PyGILState_GetThisThreadState() which works even if the GIL
       is released.

       Sadly, PyGILState_GetThisThreadState() doesn't support subinterpreters:
       see bpo-10915 and bpo-15751.

       Py_AddPendingCall() doesn't require the caller to hold the GIL. */
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL) {
        tstate = PyGILState_GetThisThreadState();
    }

    PyInterpreterState *interp;
    if (tstate != NULL) {
        interp = tstate->interp;
    }
    else {
        /* Last resort: use the main interpreter */
        interp = _PyRuntime.interpreters.main;
    }
    return _PyEval_AddPendingCall(interp, func, arg);
}

static int
handle_signals(PyThreadState *tstate)
{
    if (!_Py_ThreadCanHandleSignals(tstate->interp)) {
        return 0;
    }

    _PyThreadState_Unsignal(tstate, EVAL_PENDING_SIGNALS);
    if (_PyErr_CheckSignalsTstate(tstate) < 0) {
        /* On failure, re-schedule a call to handle_signals(). */
        _PyThreadState_Signal(tstate, EVAL_PENDING_SIGNALS);
        return -1;
    }
    return 0;
}

static int
make_pending_calls(PyThreadState *tstate)
{
    /* only execute pending calls on main thread */
    if (!_Py_ThreadCanHandlePendingCalls()) {
        return 0;
    }

    /* don't perform recursive pending calls */
    static int busy = 0;
    if (busy) {
        _PyThreadState_Signal(tstate, EVAL_PENDING_CALLS);
        return 0;
    }
    busy = 1;

    /* unsignal before starting to call callbacks, so that any callback
       added in-between re-signals */
    _PyThreadState_Unsignal(tstate, EVAL_PENDING_CALLS);
    int res = 0;

    /* perform a bounded number of calls, in case of recursion */
    struct _pending_calls *pending = &tstate->interp->ceval.pending;
    for (int i=0; i<NPENDINGCALLS; i++) {
        int (*func)(void *) = NULL;
        void *arg = NULL;

        /* pop one item off the queue while holding the lock */
        PyThread_acquire_lock(pending->lock, WAIT_LOCK);
        _pop_pending_call(pending, &func, &arg);
        PyThread_release_lock(pending->lock);

        /* having released the lock, perform the callback */
        if (func == NULL) {
            break;
        }
        res = func(arg);
        if (res) {
            goto error;
        }
    }

    busy = 0;
    return res;

error:
    busy = 0;
    _PyThreadState_Signal(tstate, EVAL_PENDING_CALLS);
    return res;
}

void
_Py_FinishPendingCalls(PyThreadState *tstate)
{
    assert(PyGILState_Check());

    struct _pending_calls *pending = &tstate->interp->ceval.pending;

    if (!_Py_atomic_load_relaxed(&(pending->calls_to_do))) {
        return;
    }

    if (make_pending_calls(tstate) < 0) {
        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        PyErr_BadInternalCall();
        _PyErr_ChainExceptions(exc, val, tb);
        _PyErr_Print(tstate);
    }
}

/* Py_MakePendingCalls() is a simple wrapper for the sake
   of backward-compatibility. */
int
Py_MakePendingCalls(void)
{
    assert(PyGILState_Check());

    PyThreadState *tstate = _PyThreadState_GET();

    /* Python signal handler doesn't really queue a callback: it only signals
       that a signal was received, see _PyEval_SignalReceived(). */
    if (_Py_ThreadCanHandleSignals(tstate->interp)) {
        if (handle_signals(tstate) < 0) {
            return -1;
        }
    }

    if (_Py_ThreadCanHandlePendingCalls()) {
        if (make_pending_calls(tstate) < 0) {
            return -1;
        }
    }

    return 0;
}

/* The interpreter's recursion limit */

#ifndef Py_DEFAULT_RECURSION_LIMIT
#define Py_DEFAULT_RECURSION_LIMIT 1000
#endif

int _Py_CheckRecursionLimit = Py_DEFAULT_RECURSION_LIMIT;

int
_PyEval_InitState(struct _ceval_state *ceval)
{
    ceval->recursion_limit = Py_DEFAULT_RECURSION_LIMIT;

    struct _pending_calls *pending = &ceval->pending;
    assert(pending->lock == NULL);

    pending->lock = PyThread_allocate_lock();
    if (pending->lock == NULL) {
        return -1;
    }

    return 0;
}

void
_PyEval_FiniState(struct _ceval_state *ceval)
{
    struct _pending_calls *pending = &ceval->pending;
    if (pending->lock != NULL) {
        PyThread_free_lock(pending->lock);
        pending->lock = NULL;
    }
}

int
Py_GetRecursionLimit(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->interp->ceval.recursion_limit;
}

void
Py_SetRecursionLimit(int new_limit)
{
    PyThreadState *tstate = _PyThreadState_GET();
    tstate->interp->ceval.recursion_limit = new_limit;
    if (_Py_IsMainInterpreter(tstate)) {
        _Py_CheckRecursionLimit = new_limit;
    }
}

/* The function _Py_EnterRecursiveCall() only calls _Py_CheckRecursiveCall()
   if the recursion_depth reaches _Py_CheckRecursionLimit.
   If USE_STACKCHECK, the macro decrements _Py_CheckRecursionLimit
   to guarantee that _Py_CheckRecursiveCall() is regularly called.
   Without USE_STACKCHECK, there is no need for this. */
int
_Py_CheckRecursiveCall(PyThreadState *tstate, const char *where)
{
    int recursion_limit = tstate->interp->ceval.recursion_limit;

#ifdef USE_STACKCHECK
    tstate->stackcheck_counter = 0;
    if (PyOS_CheckStack()) {
        --tstate->recursion_depth;
        _PyErr_SetString(tstate, PyExc_MemoryError, "Stack overflow");
        return -1;
    }
    if (_Py_IsMainInterpreter(tstate)) {
        /* Needed for ABI backwards-compatibility (see bpo-31857) */
        _Py_CheckRecursionLimit = recursion_limit;
    }
#endif
    if (tstate->recursion_critical)
        /* Somebody asked that we don't check for recursion. */
        return 0;
    if (tstate->overflowed) {
        if (tstate->recursion_depth > recursion_limit + 50 || tstate->overflowed > 50) {
            /* Overflowing while handling an overflow. Give up. */
            Py_FatalError("Cannot recover from stack overflow.");
        }
        return 0;
    }
    if (tstate->recursion_depth > recursion_limit) {
        tstate->overflowed++;
        _PyErr_Format(tstate, PyExc_RecursionError,
                    "maximum recursion depth exceeded%s",
                    where);
        tstate->overflowed--;
        --tstate->recursion_depth;
        return -1;
    }
    return 0;
}

PyObject *
_PyEval_EvalFrameDefault(PyThreadState *tstate, PyFrameObject *f, int throwflag)
{
    PyErr_SetString(PyExc_SystemError, "_PyEval_EvalFrameDefault not implemented");
    return NULL;
}

PyObject *
PyEval_EvalCode(PyObject *co, PyObject *globals, PyObject *locals)
{

    PyFunctionObject *func = (PyFunctionObject *)PyFunction_New(co, globals);
    if (func == NULL) {
        return NULL;
    }
    PyObject *ret = _PyEval_EvalFunc((PyObject *)func, locals);
    Py_DECREF(func);
    return ret;
}

PyObject *
PyEval_EvalCodeEx(PyObject *_co, PyObject *globals, PyObject *locals,
                  PyObject *const *args, int argcount,
                  PyObject *const *kws, int kwcount,
                  PyObject *const *defs, int defcount,
                  PyObject *kwdefs, PyObject *closure)
{
    PyObject *func = PyFunction_New(_co, globals);
    if (func == NULL) {
        return NULL;
    }
    if (defcount > 0) {
        if (_PyFunction_SetDefaults(func, defs, defcount) < 0) {
            goto error;
        }
    }
    if (kwdefs != NULL) {
        if (PyFunction_SetKwDefaults(func, kwdefs) < 0) {
            goto error;
        }
    }
    if (closure != NULL) {
        if (PyFunction_SetClosure(func, closure) < 0) {
            goto error;
        }
    }

error:
    Py_DECREF(func);
    return NULL;
}

/* Interpreter main loop */

PyObject *
PyEval_EvalFrame(PyFrameObject *f)
{
    /* Function kept for backward compatibility */
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_EvalFrame(tstate, f, 0);
}

PyObject *
PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_EvalFrame(tstate, f, throwflag);
}


/* Handle signals, pending calls, GIL drop request
   and asynchronous exception */
int
_PyEval_HandleBreaker(PyThreadState *tstate)
{
    /* don't handle signals or pending calls if we can't stop */
    if (tstate->cant_stop_wont_stop) {
        return 0;
    }

    /* load eval breaker */
    uintptr_t b = _Py_atomic_load_uintptr(&tstate->eval_breaker);

    /* Stop-the-world */
    if ((b & EVAL_PLEASE_STOP) != 0) {
        if (!tstate->cant_stop_wont_stop) {
            _PyThreadState_Unsignal(tstate, EVAL_PLEASE_STOP);
            _PyThreadState_GC_Stop(tstate);
        }
    }

    if ((b & EVAL_EXPLICIT_MERGE) != 0) {
        _PyThreadState_Unsignal(tstate, EVAL_EXPLICIT_MERGE);
        _Py_queue_process(tstate);
    }

    /* Pending signals */
    if ((b & EVAL_PENDING_SIGNALS) != 0) {
        _PyThreadState_Unsignal(tstate, EVAL_PENDING_SIGNALS);
        assert(_Py_ThreadCanHandleSignals(tstate->interp));
        if (handle_signals(tstate) != 0) {
            return -1;
        }
    }

    /* Pending calls */
    if ((b & EVAL_PENDING_CALLS) != 0) {
        assert(_Py_ThreadCanHandlePendingCalls());
        _PyThreadState_Unsignal(tstate, EVAL_PENDING_CALLS);
        if (make_pending_calls(tstate) != 0) {
            return -1;
        }
    }

    if ((b & EVAL_DROP_GIL) != 0) {
        _PyThreadState_Unsignal(tstate, EVAL_DROP_GIL);

        /* Give another thread a chance */
        PyEval_ReleaseThread(tstate);

        /* Other threads may run now */

        PyEval_AcquireThread(tstate);
    }

    /* Check for asynchronous exception. */
    if ((b & EVAL_ASYNC_EXC) != 0) {
        _PyThreadState_Unsignal(tstate, EVAL_ASYNC_EXC);
        PyObject *exc = _Py_atomic_exchange_ptr(&tstate->async_exc, NULL);
        if (exc) {
            _PyErr_SetNone(tstate, exc);
            Py_DECREF(exc);
            return -1;
        }
    }

    return 0;
}

PyObject *
_PyEval_CallTracing(PyObject *func, PyObject *args)
{
    PyThreadState *tstate = _PyThreadState_GET();
    int save_tracing = tstate->tracing;
    int save_use_tracing = tstate->use_tracing;
    PyObject *result;

    tstate->tracing = 0;
    tstate->use_tracing = ((tstate->c_tracefunc != NULL)
                           || (tstate->c_profilefunc != NULL));
    result = PyObject_Call(func, args, NULL);
    tstate->tracing = save_tracing;
    tstate->use_tracing = save_use_tracing;
    return result;
}

static void
update_use_tracing(PyThreadState *tstate)
{
    int use_tracing = (tstate->c_tracefunc   != NULL ||
                       tstate->c_profilefunc != NULL);

    /* Flag that tracing or profiling is turned on */
    tstate->use_tracing = use_tracing;

#ifdef HAVE_COMPUTED_GOTOS
    static uint8_t trace_cfunc[128] = {
        [CFUNC_HEADER] = 1,
        [CFUNC_HEADER_NOARGS] = 1,
        [CFUNC_HEADER_O] = 1,
        [CMETHOD_O] = 1,
        [CMETHOD_NOARGS] = 1,
        [FUNC_TPCALL_HEADER] = 1,
    };

    /* Update opcode handlers */
    for (int i = 1; i < 128; i++) {
        if (use_tracing) {
            if (trace_cfunc[i]) {
                tstate->opcode_targets[i-1] = tstate->trace_cfunc_target;
            }
            else {
                tstate->opcode_targets[i-1] = tstate->trace_target;
            }
        }
        else {
            tstate->opcode_targets[i-1] = tstate->opcode_targets_base[i];
        }
    }
#endif
}

int
_PyEval_SetProfile(PyThreadState *tstate, Py_tracefunc func, PyObject *arg)
{
    /* The caller must hold the GIL */
    assert(PyGILState_Check());

    /* Call _PySys_Audit() in the context of the current thread state,
       even if tstate is not the current thread state. */
    PyThreadState *current_tstate = _PyThreadState_GET();
    if (_PySys_Audit(current_tstate, "sys.setprofile", NULL) < 0) {
        return -1;
    }

    PyObject *profileobj = tstate->c_profileobj;

    tstate->c_profilefunc = NULL;
    tstate->c_profileobj = NULL;
    /* Must make sure that tracing is not ignored if 'profileobj' is freed */
    tstate->use_tracing = tstate->c_tracefunc != NULL;
    Py_XDECREF(profileobj);

    Py_XINCREF(arg);
    tstate->c_profileobj = arg;
    tstate->c_profilefunc = func;

    /* Flag that tracing or profiling is turned on */
    update_use_tracing(tstate);
    return 0;
}

void
PyEval_SetProfile(Py_tracefunc func, PyObject *arg)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyEval_SetProfile(tstate, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        _PyErr_WriteUnraisableMsg("in PyEval_SetProfile", NULL);
    }
}

int
_PyEval_SetTrace(PyThreadState *tstate, Py_tracefunc func, PyObject *arg)
{
    assert(!_PyMem_IsPtrFreed(tstate));
    /* The caller must hold the GIL */
    assert(PyGILState_Check());

    /* Call _PySys_Audit() in the context of the current thread state,
       even if tstate is not the current thread state. */
    PyThreadState *current_tstate = _PyThreadState_GET();
    if (_PySys_Audit(current_tstate, "sys.settrace", NULL) < 0) {
        return -1;
    }

    struct _ceval_state *ceval2 = &tstate->interp->ceval;
    PyObject *traceobj = tstate->c_traceobj;
    ceval2->tracing_possible += (func != NULL) - (tstate->c_tracefunc != NULL);

    tstate->c_tracefunc = NULL;
    tstate->c_traceobj = NULL;
    /* Must make sure that profiling is not ignored if 'traceobj' is freed */
    tstate->use_tracing = (tstate->c_profilefunc != NULL);
    Py_XDECREF(traceobj);

    Py_XINCREF(arg);
    tstate->c_traceobj = arg;
    tstate->c_tracefunc = func;

    /* Flag that tracing or profiling is turned on */
    update_use_tracing(tstate);
    return 0;
}

void
PyEval_SetTrace(Py_tracefunc func, PyObject *arg)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyEval_SetTrace(tstate, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        _PyErr_WriteUnraisableMsg("in PyEval_SetTrace", NULL);
    }
}


void
_PyEval_SetCoroutineOriginTrackingDepth(PyThreadState *tstate, int new_depth)
{
    assert(new_depth >= 0);
    tstate->coroutine_origin_tracking_depth = new_depth;
}

int
_PyEval_GetCoroutineOriginTrackingDepth(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->coroutine_origin_tracking_depth;
}

int
_PyEval_SetAsyncGenFirstiter(PyObject *firstiter)
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (_PySys_Audit(tstate, "sys.set_asyncgen_hook_firstiter", NULL) < 0) {
        return -1;
    }

    Py_XINCREF(firstiter);
    Py_XSETREF(tstate->async_gen_firstiter, firstiter);
    return 0;
}

PyObject *
_PyEval_GetAsyncGenFirstiter(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_firstiter;
}

int
_PyEval_SetAsyncGenFinalizer(PyObject *finalizer)
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (_PySys_Audit(tstate, "sys.set_asyncgen_hook_finalizer", NULL) < 0) {
        return -1;
    }

    Py_XINCREF(finalizer);
    Py_XSETREF(tstate->async_gen_finalizer, finalizer);
    return 0;
}

PyObject *
_PyEval_GetAsyncGenFinalizer(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_finalizer;
}

struct _frame *vm_frame(PyThreadState *ts);

PyFrameObject *
PyEval_GetFrame(void)
{
    return vm_frame(_PyThreadState_GET());
}

PyObject *
PyEval_GetBuiltins(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->interp->builtins;
}

/* Convenience function to get a builtin from its name */
PyObject *
_PyEval_GetBuiltinId(_Py_Identifier *name)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *attr = _PyDict_GetItemIdWithError(PyEval_GetBuiltins(), name);
    if (attr) {
        Py_INCREF(attr);
    }
    else if (!_PyErr_Occurred(tstate)) {
        _PyErr_SetObject(tstate, PyExc_AttributeError, _PyUnicode_FromId(name));
    }
    return attr;
}

int
PyEval_MergeCompilerFlags(PyCompilerFlags *cf)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyFrameObject *current_frame = vm_frame(tstate);
    int result = cf->cf_flags != 0;

    if (current_frame != NULL) {
        const int codeflags = current_frame->f_code->co_flags;
        const int compilerflags = codeflags & PyCF_MASK;
        if (compilerflags) {
            result = 1;
            cf->cf_flags |= compilerflags;
        }
#if 0 /* future keyword */
        if (codeflags & CO_GENERATOR_ALLOWED) {
            result = 1;
            cf->cf_flags |= CO_GENERATOR_ALLOWED;
        }
#endif
    }
    return result;
}


const char *
PyEval_GetFuncName(PyObject *func)
{
    if (PyMethod_Check(func))
        return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
    else if (PyFunction_Check(func))
        return PyUnicode_AsUTF8(((PyFunctionObject*)func)->func_name);
    else if (PyCFunction_Check(func))
        return ((PyCFunctionObject*)func)->m_ml->ml_name;
    else
        return Py_TYPE(func)->tp_name;
}

const char *
PyEval_GetFuncDesc(PyObject *func)
{
    if (PyMethod_Check(func))
        return "()";
    else if (PyFunction_Check(func))
        return "()";
    else if (PyCFunction_Check(func))
        return "()";
    else
        return " object";
}

/* Extract a slice index from a PyLong or an object with the
   nb_index slot defined, and store in *pi.
   Silently reduce values larger than PY_SSIZE_T_MAX to PY_SSIZE_T_MAX,
   and silently boost values less than PY_SSIZE_T_MIN to PY_SSIZE_T_MIN.
   Return 0 on error, 1 on success.
*/
int
_PyEval_SliceIndex(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (v != Py_None) {
        Py_ssize_t x;
        if (_PyIndex_Check(v)) {
            x = PyNumber_AsSsize_t(v, NULL);
            if (x == -1 && _PyErr_Occurred(tstate))
                return 0;
        }
        else {
            _PyErr_SetString(tstate, PyExc_TypeError,
                             "slice indices must be integers or "
                             "None or have an __index__ method");
            return 0;
        }
        *pi = x;
    }
    return 1;
}

int
_PyEval_SliceIndexNotNone(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    Py_ssize_t x;
    if (_PyIndex_Check(v)) {
        x = PyNumber_AsSsize_t(v, NULL);
        if (x == -1 && _PyErr_Occurred(tstate))
            return 0;
    }
    else {
        _PyErr_SetString(tstate, PyExc_TypeError,
                         "slice indices must be integers or "
                         "have an __index__ method");
        return 0;
    }
    *pi = x;
    return 1;
}

Py_ssize_t
_PyEval_RequestCodeExtraIndex(freefunc free)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    Py_ssize_t new_index;

    if (interp->co_extra_user_count == MAX_CO_EXTRA_USERS - 1) {
        return -1;
    }
    new_index = interp->co_extra_user_count++;
    interp->co_extra_freefuncs[new_index] = free;
    return new_index;
}


/* Implement Py_EnterRecursiveCall() and Py_LeaveRecursiveCall() as functions
   for the limited API. */

#undef Py_EnterRecursiveCall

int Py_EnterRecursiveCall(const char *where)
{
    return _Py_EnterRecursiveCall_inline(where);
}

#undef Py_LeaveRecursiveCall

void Py_LeaveRecursiveCall(void)
{
    _Py_LeaveRecursiveCall_inline();
}

#include "ceval_intrinsics.h"
