#include "Python.h"
#include "ceval2_meta.h"
#include "pycore_call.h"
#include "pycore_ceval.h"
#include "pycore_code.h"
#include "pycore_object.h"
#include "pycore_refcnt.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"
#include "pycore_stackwalk.h"
#include "pycore_tupleobject.h"
#include "pycore_qsbr.h"
#include "pycore_traceback.h"

#include "code2.h"
#include "dictobject.h"
#include "frameobject.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"
#include "opcode2.h"
#include "opcode_names2.h"
#include "pycore_generator.h"

#include <ctype.h>
#include <alloca.h>

static struct ThreadState *current_thread_state(void);

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

static Py_ssize_t
vm_regs_frame_size(Register *regs)
{
    PyObject *this_func = AS_OBJ(regs[-1]);
    if (this_func == NULL) {
        return 0;
    }
    if (!PyFunc_Check(this_func)) {
        return regs[-2].as_int64;
    }
    return PyCode2_FromFunc((PyFunc *)this_func)->co_framesize;
}

static Py_ssize_t
vm_frame_size(struct ThreadState *ts)
{
    if (ts->regs == ts->stack) {
        return 0;
    }
    return vm_regs_frame_size(ts->regs);
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
vm_setup_async_with(struct ThreadState *ts, Py_ssize_t opA)
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
vm_clear_regs(struct ThreadState *ts, Py_ssize_t lo, Py_ssize_t hi);

void
vm_dump_stack(void)
{
    struct ThreadState *ts = current_thread_state();
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        Register *regs = vm_stack_walk_regs(&w);
        PyObject *callable = AS_OBJ(regs[-1]);
        if (!PyFunc_Check(callable)) {
            continue;
        }

        PyFunc *func = (PyFunc *)callable;
        PyCodeObject2 *co = PyCode2_FromFunc(func);
        int line = PyCode2_Addr2Line(co, (int)(w.pc - PyCode2_GET_CODE(co)));

        fprintf(stderr, "File \"%s\", line %d, in %s\n",
            PyUnicode_AsUTF8(co->co_filename),
            line,
            PyUnicode_AsUTF8(func->func_name));
    }
}

static Py_ssize_t
vm_stack_depth(struct ThreadState *ts)
{
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    Py_ssize_t n = 0;
    while (vm_stack_walk(&w)) {
        n++;
    }
    return n;
}

/* returns the currently handled exception or NULL */
PyObject *
vm_handled_exc(struct ThreadState *ts)
{
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        Register *regs = vm_stack_walk_regs(&w);
        PyObject *callable = AS_OBJ(regs[-1]);
        if (!PyFunc_Check(callable)) {
            continue;
        }

        PyFunc *func = (PyFunc *)callable;
        PyCodeObject2 *code = PyCode2_FromFunc(func);

        const uint8_t *first_instr = PyCode2_GET_CODE(code);
        Py_ssize_t instr_offset = (w.pc - first_instr);

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
                if (regs[link_reg].as_int64 != -1) {
                    // not handling an exception
                    continue;
                }
                return AS_OBJ(regs[link_reg+1]);
            }
        }
    }
    return NULL;
}

#define IS_OBJ(r) (((r).as_int64 & NON_OBJECT_TAG) != NON_OBJECT_TAG)

int
vm_traverse_stack(struct ThreadState *ts, visitproc visit, void *arg)
{
    if (ts->prev != NULL) {
        return 0;
    }

    Register *max = ts->maxstack;
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        Register *regs = vm_stack_walk_regs(&w);
        Register *top = regs + vm_regs_frame_size(regs);
        if (top > max) {
            top = max;
        }

        Register *bot;
        for (bot = &regs[-1]; bot != top; bot++) {
            // TODO: handle deferred refcounting
            Register r = *bot;
            if (!IS_OBJ(r) || !IS_RC(r)) {
                continue;
            }

            PyObject *obj = AS_OBJ(*bot);
            Py_VISIT(obj);
        }

        // don't visit the frame header
        max = regs - FRAME_EXTRA;
    }

    return 0;
}

PyObject *
vm_compute_cr_origin(struct ThreadState *ts)
{
    int origin_depth = ts->ts->coroutine_origin_tracking_depth;
    assert(origin_depth > 0);

    // First count how many frames we have
    int frame_count = 0;

    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    vm_stack_walk(&w);  // skip the first frame
    while (vm_stack_walk(&w) && frame_count < origin_depth) {
        Register *regs = vm_stack_walk_regs(&w);
        if (!PyFunc_Check(AS_OBJ(regs[-1]))) {
            continue;
        }
        ++frame_count;
    }

    // Now collect them
    PyObject *cr_origin = PyTuple_New(frame_count);
    if (cr_origin == NULL) {
        return NULL;
    }

    int i = 0;
    vm_stack_walk_init(&w, ts);
    vm_stack_walk(&w);  // skip the first frame
    while (vm_stack_walk(&w) && i < frame_count) {
        Register *regs = vm_stack_walk_regs(&w);
        if (!PyFunc_Check(AS_OBJ(regs[-1]))) {
            continue;
        }

        PyFunc *func = (PyFunc *)AS_OBJ(regs[-1]);
        PyCodeObject2 *code = PyCode2_FromFunc(func);
        int addrq = w.pc - func->func_base.first_instr;

        PyObject *frameinfo = Py_BuildValue(
            "OiO",
            code->co_filename,
            PyCode2_Addr2Line(code, addrq),
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

PyObject *
vm_cur_handled_exc(void)
{
    struct ThreadState *ts = PyThreadState_GET()->active;
    return vm_handled_exc(ts);
}

static int
vm_exit_with_exc(struct ThreadState *ts, Py_ssize_t opA)
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
vm_exit_with_res(struct ThreadState *ts, Py_ssize_t opA, PyObject *exit_res)
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
vm_exit_with(struct ThreadState *ts, Py_ssize_t opA)
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
    res = _PyObject_VectorcallTstate(ts->ts, exit, stack + 1, nargsf, NULL);
    CLEAR(ts->regs[opA]);
    CLEAR(ts->regs[opA + 1]);
    if (UNLIKELY(res == NULL)) {
        return -1;
    }
    Py_DECREF(res);
    return 0;
}

int
vm_exit_async_with(struct ThreadState *ts, Py_ssize_t opA)
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
    PyObject *obj = _PyObject_VectorcallTstate(ts->ts, exit, stack + 1, nargsf, NULL);
    Py_DECREF(stack[3]);
    if (obj == NULL) {
        return -1;
    }
    CLEAR(ts->regs[opA]);
    CLEAR(ts->regs[opA + 1]);
    ts->regs[opA] = PACK_OBJ(obj);

    // convert obj to awaitable (effectively GET_AWAITABLE)
    if (PyCoro2_CheckExact(obj)) {
        PyObject *yf = ((PyCoroObject2 *)obj)->base.yield_from;
        if (UNLIKELY(yf != NULL)) {
            vm_err_coroutine_awaited(ts);
            return -1;
        }
    }
    else {
        PyObject *iter = _PyCoro2_GetAwaitableIter(obj);
        if (iter == NULL) {
            _PyErr_Format(ts->ts, PyExc_TypeError,
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
vm_clear_regs(struct ThreadState *ts, Py_ssize_t lo, Py_ssize_t hi)
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
vm_pop_frame(struct ThreadState *ts)
{
    assert(ts->regs > ts->stack);
    Py_ssize_t frame_size = vm_frame_size(ts);
    if (ts->regs + frame_size > ts->maxstack) {
        // Ensure we don't exceed maxstack in case we're popping a partially
        // setup frame (e.g. CALL_FUNCTION_EX).
        frame_size = ts->maxstack - ts->regs;
    }
    Py_ssize_t from = -1;
    if (PyFunc_Check(AS_OBJ(ts->regs[-1]))) {
        from = -2;
    }
    vm_clear_regs(ts, from, frame_size);
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
vm_exception_handler(PyCodeObject2 *code, const uint8_t *pc)
{
    const uint8_t *first_instr = PyCode2_GET_CODE(code);
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

static PyFrameObject *
new_fake_frame(struct ThreadState *ts, Py_ssize_t offset, const uint8_t *pc);

static PyObject *
traceback_here(struct ThreadState *ts, PyObject *tb)
{
    PyFrameObject *frame = new_fake_frame(ts, 0, ts->pc);
    if (frame == NULL) {
        return NULL;
    }

    PyObject *newtb = _PyTraceBack_FromFrame(tb, frame);
    Py_DECREF(frame);
    return newtb;
}

// Unwinds the stack looking for the nearest exception handler. Returns
// the program counter (PC) of the exception handler block, or NULL if
// there are no handlers before the next C frame.
const uint8_t *
vm_exception_unwind(struct ThreadState *ts, bool skip_first_frame)
{
    assert(PyErr_Occurred());
    assert(ts->regs > ts->stack);

    PyObject *exc = NULL, *val = NULL, *tb = NULL;
    _PyErr_Fetch(ts->ts, &exc, &val, &tb);

    bool skip_frame = skip_first_frame;
    const uint8_t *pc = ts->pc;
    for (;;) {
        if (pc == NULL) {
            // pc is NULL if we set up the call frame, but haven't
            // started executing it. See CALL_FUNCTION_EX in ceval2.c
            goto next;
        }

        PyObject *callable = AS_OBJ(ts->regs[-1]);
        if (!PyFunc_Check(callable)) {
            goto next;
        }

        PyFunc *func = (PyFunc *)callable;
        PyCodeObject2 *code = PyCode2_FromFunc(func);
        if (pc == func->func_base.first_instr) {
            goto next;
        }

        if (!skip_frame) {
            PyObject *newtb = traceback_here(ts, tb);
            if (newtb != NULL) {
                Py_XSETREF(tb, newtb);
            }
            else {
                _PyErr_ChainExceptions(exc, val, tb);
                PyErr_Fetch(&exc, &val, &tb);
            }
        }
        else {
            skip_frame = false;
        }

        ExceptionHandler *handler = vm_exception_handler(code, pc);
        if (handler != NULL) {
            /* Make the raw exception data
                available to the handler,
                so a program can emulate the
                Python main loop. */
            _PyErr_NormalizeException(ts->ts, &exc, &val, &tb);
            PyException_SetTraceback(val, tb ? tb : Py_None);

            vm_clear_regs(ts, handler->reg, code->co_framesize);

            Py_ssize_t link_reg = handler->reg;
            ts->regs[link_reg].as_int64 = -1;
            assert(!_PyObject_IS_IMMORTAL(val));
            ts->regs[link_reg + 1] = PACK(val, REFCOUNT_TAG);
            Py_DECREF(exc);
            Py_XDECREF(tb);
            return PyCode2_GET_CODE(code) + handler->handler;
        }

      next: ;
        // No handler found in this call frame. Clears the entire frame and
        // unwinds the call stack.
        intptr_t frame_link = vm_pop_frame(ts);
        if (frame_link <= 0) {
            _PyErr_Restore(ts->ts, exc, val, tb);
            if (frame_link == FRAME_GENERATOR) {
                PyGenObject2 *gen = PyGen2_FromThread(ts);
                assert(PyGen2_CheckExact(gen) || PyCoro2_CheckExact(gen) || PyAsyncGen2_CheckExact(gen));
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

static PyFrameObject *
new_fake_frame(struct ThreadState *ts, Py_ssize_t offset, const uint8_t *pc)
{
    assert(PyFunc_Check(AS_OBJ(ts->regs[offset-1])));

    PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[offset-1]);
    PyCodeObject2 *co = PyCode2_FromFunc(func);

    PyFrameObject *frame = _PyFrame_NewFake(co, func->globals);
    if (frame == NULL) {
        return NULL;
    }

    int addrq = (pc - PyCode2_GET_CODE(co));
    frame->f_lasti = addrq;
    frame->f_lineno = PyCode2_Addr2Line(co, (int)addrq);

    for (Py_ssize_t i = 0, nlocals = co->co_nlocals; i < nlocals; i++) {
        // PyObject *op = AS_OBJ(ts->regs[i]);
        // Py_XINCREF(op);
        // frame->f_localsplus[i] = op;
    }

    return frame;
}

PyObject *
vm_traceback_here(struct ThreadState *ts)
{
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        Register *regs = vm_stack_walk_regs(&w);
        if (PyFunc_Check(AS_OBJ(regs[-1]))) {
            PyFrameObject *frame = new_fake_frame(ts, w.offset, w.pc);
            if (frame == NULL) {
                return NULL;
            }

            PyObject *tb = _PyTraceBack_FromFrame(NULL, frame);
            Py_DECREF(frame);
            return tb;
        }
    }
    return NULL;
}

static int
is_importlib_frame(PyFunc *func)
{
    _Py_IDENTIFIER(importlib);
    _Py_IDENTIFIER(_bootstrap);

    PyObject *filename, *importlib_string, *bootstrap_string;
    int contains;

    filename = PyCode2_FromFunc(func)->co_filename;
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
vm_frame_info(PyFunc **out_func, int *out_addrq, int depth,
              bool skip_importlib_frames)
{
    struct ThreadState *ts = _PyThreadState_GET()->active;

    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    while (vm_stack_walk(&w)) {
        Register *regs = vm_stack_walk_regs(&w);
        PyObject *callable = AS_OBJ(regs[-1]);
        if (!PyFunc_Check(callable) || w.pc == NULL) {
            continue;
        }

        PyFunc *func = (PyFunc *)callable;
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
            *out_addrq = w.pc - func->func_base.first_instr;
            return 1;
        }
    }

    *out_func = NULL;
    *out_addrq = 0;
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
vm_reraise(struct ThreadState *ts, Register reg)
{
    assert(IS_RC(reg) || _PyObject_IS_IMMORTAL(AS_OBJ(reg)));
    PyObject *exc = AS_OBJ(reg);
    PyObject *type = (PyObject *)Py_TYPE(exc);
    Py_INCREF(type);
    PyObject *tb = PyException_GetTraceback(exc);
    _PyErr_Restore(ts->ts, type, exc, tb);
    return -2;
}

int
vm_raise(struct ThreadState *ts, PyObject *exc)
{
    if (exc == NULL) {
        exc = vm_handled_exc(ts);
        if (exc == NULL) {
            _PyErr_SetString(ts->ts, PyExc_RuntimeError,
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

const uint8_t *
vm_exc_match(struct ThreadState *ts, PyObject *tp, PyObject *exc, const uint8_t *pc, int opD)
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
                _PyErr_SetString(ts->ts, PyExc_TypeError,
                                 CANNOT_CATCH_MSG);
                return NULL;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(tp)) {
            _PyErr_SetString(ts->ts, PyExc_TypeError,
                             CANNOT_CATCH_MSG);
            return NULL;
        }
    }
    assert(exc == vm_handled_exc(ts));
    int res = PyErr_GivenExceptionMatches(exc, tp);
    if (res > 0) {
        /* Exception matches -- Do nothing */;
        return pc + OP_SIZE_JUMP_IF_NOT_EXC_MATCH;
    }
    else if (res == 0) {
        return pc + opD;
    }
    else {
        return NULL;
    }
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
vm_unpack(struct ThreadState *ts, PyObject *v, Py_ssize_t base,
          Py_ssize_t argcnt, Py_ssize_t argcntafter)
{
    Py_ssize_t ll = 0;
    PyObject *it;  /* iter(v) */
    PyObject *w;
    PyObject *l = NULL; /* variable list */

    assert(v != NULL);

    if (UNLIKELY(Py_TYPE(v)->tp_iter == NULL && !PySequence_Check(v))) {
        _PyErr_Format(ts->ts, PyExc_TypeError,
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
            if (!_PyErr_Occurred(ts->ts)) {
                if (argcntafter == 0) {
                    _PyErr_Format(ts->ts, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected %d, got %d)",
                                  argcnt, i);
                }
                else {
                    _PyErr_Format(ts->ts, PyExc_ValueError,
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
            if (_PyErr_Occurred(ts->ts))
                goto Error;
            Py_DECREF(it);
            return 0;
        }
        Py_DECREF(w);
        _PyErr_Format(ts->ts, PyExc_ValueError,
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
        _PyErr_Format(ts->ts, PyExc_ValueError,
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
vm_load_name(struct ThreadState *ts, PyObject *locals, PyObject *name)
{
    if (UNLIKELY(!PyDict_CheckExact(locals))) {
        PyObject *value = PyObject_GetItem(locals, name);
        if (value == NULL && _PyErr_ExceptionMatches(ts->ts, PyExc_KeyError)) {
            _PyErr_Clear(ts->ts);
        }
        return value;
    }
    return PyDict_GetItemWithError2(locals, name);
}

Register
vm_load_class_deref(struct ThreadState *ts, Py_ssize_t opA, PyObject *name)
{
    PyObject *locals = AS_OBJ(ts->regs[0]);
    if (PyDict_CheckExact(locals)) {
        PyObject *value = PyDict_GetItemWithError2(locals, name);
        if (value != NULL) {
            return PACK_OBJ(value);
        }
        else if (_PyErr_Occurred(ts->ts)) {
            return (Register){0};
        }
    }
    else {
        PyObject *value = PyObject_GetItem(locals, name);
        if (value != NULL) {
            return PACK_OBJ(value);
        }
        else if (!_PyErr_ExceptionMatches(ts->ts, PyExc_KeyError)) {
            return (Register){0};
        }
        else {
            _PyErr_Clear(ts->ts);
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

PyObject *
vm_name_error(struct ThreadState *ts, PyObject *name)
{
    const char *obj_str = PyUnicode_AsUTF8(name);
    if (obj_str == NULL) {
        return NULL;
    }
    _PyErr_Format(ts->ts, PyExc_NameError, "name '%.200s' is not defined", obj_str);
    return NULL;
}

int
vm_delete_name(struct ThreadState *ts, PyObject *name)
{
    PyObject *locals = AS_OBJ(ts->regs[0]);
    assert(PyDict_Check(locals));
    int err = PyObject_DelItem(locals, name);
    if (UNLIKELY(err != 0)) {
        vm_name_error(ts, name);
        return -1;
    }
    return 0;
}

static PyObject *
vm_import_name_custom(struct ThreadState *ts, PyFunc *this_func, PyObject *arg,
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
vm_import_name(struct ThreadState *ts, PyFunc *this_func, PyObject *arg)
{
    _Py_IDENTIFIER(__import__);
    PyObject *import_func, *builtins;

    builtins = this_func->builtins;
    import_func = _PyDict_GetItemIdWithError(builtins, &PyId___import__);
    if (import_func == NULL) {
        if (!_PyErr_Occurred(ts->ts)) {
            _PyErr_SetString(ts->ts, PyExc_ImportError, "__import__ not found");
        }
        return NULL;
    }

    if (UNLIKELY(import_func != ts->ts->interp->import_func)) {
        return vm_import_name_custom(ts, this_func, arg, import_func);
    }

    assert(PyTuple_CheckExact(arg) && PyTuple_GET_SIZE(arg) == 3);
    PyObject *name = PyTuple_GET_ITEM(arg, 0);
    PyObject *fromlist = PyTuple_GET_ITEM(arg, 1);
    PyObject *level = PyTuple_GET_ITEM(arg, 2);
    int ilevel = _PyLong_AsInt(level);
    if (ilevel == -1 && _PyErr_Occurred(ts->ts)) {
        return NULL;
    }
    ts->ts->use_new_bytecode = 1;
    return PyImport_ImportModuleLevelObject(
        name,
        this_func->globals,
        Py_None,
        fromlist,
        ilevel);
}

Register
vm_load_build_class(struct ThreadState *ts, PyObject *builtins)
{
    _Py_IDENTIFIER(__build_class__);

    PyObject *bc;
    if (PyDict_CheckExact(builtins)) {
        bc = _PyDict_GetItemIdWithError(builtins, &PyId___build_class__);
        if (bc == NULL) {
            if (!_PyErr_Occurred(ts->ts)) {
                _PyErr_SetString(ts->ts, PyExc_NameError,
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
            if (_PyErr_ExceptionMatches(ts->ts, PyExc_KeyError))
                _PyErr_SetString(ts->ts, PyExc_NameError,
                                    "__build_class__ not found");
            return (Register){0};
        }
        return PACK(bc, REFCOUNT_TAG);
    }
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
vm_call_function_ex(struct ThreadState *ts)
{
    PyObject *callable = AS_OBJ(ts->regs[-1]);
    PyObject *args = AS_OBJ(ts->regs[-FRAME_EXTRA - 2]);
    PyObject *kwargs = AS_OBJ(ts->regs[-FRAME_EXTRA - 1]);
    PyObject *res = PyObject_Call(callable, args, kwargs);
    CLEAR(ts->regs[-FRAME_EXTRA - 1]);
    CLEAR(ts->regs[-FRAME_EXTRA - 2]);
    return res;
}

static PyObject * _Py_NO_INLINE
vm_call_cfunction_slow(struct ThreadState *ts, Register acc)
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
    PyObject *res = _PyObject_VectorcallTstate(ts->ts, args[0], args + 1, nargsf, kwnames);
    if (ACC_KWCOUNT(acc) > 0) {
        for (Py_ssize_t i =  -FRAME_EXTRA - ACC_KWCOUNT(acc) - 1; i != -FRAME_EXTRA; i++) {
            CLEAR(ts->regs[i]);
        }
    }
    return res;
}

PyObject *
vm_call_cfunction(struct ThreadState *ts, Register acc)
{
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
vm_call_function(struct ThreadState *ts, Register acc)
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
    return _PyObject_VectorcallTstate(ts->ts, args[0], args + 1, nargsf, NULL);
}

static PyObject *
build_tuple(struct ThreadState *ts, Py_ssize_t base, Py_ssize_t n);

static PyObject *
build_kwargs(struct ThreadState *ts, Py_ssize_t n);

PyObject *
vm_tpcall_function(struct ThreadState *ts, Register acc)
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
    }
    else if (UNLIKELY(ACC_KWCOUNT(acc) != 0)) {
        _PyErr_Format(ts->ts, PyExc_TypeError,
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
build_kwargs(struct ThreadState *ts, Py_ssize_t kwcount)
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
vm_make_function(struct ThreadState *ts, PyCodeObject2 *code)
{
    PyFunc *this_func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyFunc *func = (PyFunc *)PyFunc_New(
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
positional_only_passed_as_keyword(struct ThreadState *ts, PyCodeObject2 *co,
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
        _PyErr_Format(ts->ts, PyExc_TypeError,
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
unexpected_keyword_argument(struct ThreadState *ts, PyCodeObject2 *co,
                            PyObject *keyword, Py_ssize_t kwcount,
                            PyObject **kwnames)
{
    if (co->co_posonlyargcount == 0 ||
        !positional_only_passed_as_keyword(ts, co, kwcount, kwnames))
    {
        _PyErr_Format(ts->ts, PyExc_TypeError,
                    "%U() got an unexpected keyword argument '%S'",
                    co->co_name, keyword);
    }
    return -1;
}

static int _Py_NO_INLINE
unexpected_keyword_argument_dict(struct ThreadState *ts, PyCodeObject2 *co,
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
duplicate_keyword_argument(struct ThreadState *ts, PyCodeObject2 *co, PyObject *keyword)
{
    _PyErr_Format(ts->ts, PyExc_TypeError,
                    "%U() got multiple values for argument '%S'",
                    co->co_name, keyword);
    return -1;
}

static void
format_missing(struct ThreadState *ts, const char *kind,
               PyCodeObject2 *co, PyObject *names)
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
    _PyErr_Format(ts->ts, PyExc_TypeError,
                  "%U() missing %i required %s argument%s: %U",
                  co->co_name,
                  len,
                  kind,
                  len == 1 ? "" : "s",
                  name_str);
    Py_DECREF(name_str);
}

int _Py_NO_INLINE
missing_arguments(struct ThreadState *ts)
{
    PyObject *positional = NULL;
    PyObject *kwdonly = NULL;
    PyObject *name = NULL;

    PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyCodeObject2 *co = PyCode2_FromFunc(func);
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

int _Py_NO_INLINE
too_many_positional(struct ThreadState *ts,
                    Py_ssize_t given,
                    Py_ssize_t kwcount)
{
    int plural;
    PyObject *sig, *kwonly_sig;
    PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyCodeObject2 *co = PyCode2_FromFunc(func);
    Py_ssize_t co_argcount = co->co_argcount;
    Py_ssize_t co_totalargcount = co->co_totalargcount;

    assert((co->co_flags & CO_VARARGS) == 0);

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
    _PyErr_Format(ts->ts, PyExc_TypeError,
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

// Setup up a function frame when invoked like `func(*args, **kwargs)`.
int
vm_setup_ex(struct ThreadState *ts, PyCodeObject2 *co, Register acc)
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
    while (kwargs && PyDict_Next(kwargs, &i, &keyword, &value)) {
        if (keyword == NULL || !PyUnicode_Check(keyword)) {
            _PyErr_Format(ts->ts, PyExc_TypeError,
                          "%U() keywords must be strings",
                          co->co_name);
            return -1;
        }

        /* Speed hack: do raw pointer compares. As names are
           normally interned this should almost always hit. */
        PyObject **co_varnames = ((PyTupleObject *)(co->co_varnames))->ob_item;
        Py_ssize_t j;
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = co_varnames[j];
            if (name == keyword) {
                goto kw_found;
            }
        }

        /* Slow fallback, just in case */
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = co_varnames[j];
            int cmp = PyObject_RichCompareBool(keyword, name, Py_EQ);
            if (cmp > 0) {
                goto kw_found;
            }
            else if (cmp < 0) {
                return -1;
            }
        }

        assert(j >= total_args);
        if (kwdict == NULL) {
            return unexpected_keyword_argument_dict(ts, co, keyword, kwargs);
        }

        if (PyDict_SetItem(kwdict, keyword, value) == -1) {
            return -1;
        }
        continue;

      kw_found:
        if (ts->regs[j].as_int64 != 0) {
            return duplicate_keyword_argument(ts, co, keyword);
        }
        ts->regs[j] = PACK_INCREF(value);
    }

    /* Check the number of positional arguments */
    if ((argcount > co->co_argcount) && !(co->co_flags & CO_VARARGS)) {
        Py_ssize_t kwcount = kwargs ? PyDict_Size(kwargs) : 0;
        return too_many_positional(ts, argcount, kwcount);
    }

    CLEAR(ts->regs[-FRAME_EXTRA - 2]);
    if (kwargs) {
        CLEAR(ts->regs[-FRAME_EXTRA - 1]);
    }
    return 0;
}

int
vm_setup_varargs(struct ThreadState *ts, PyCodeObject2 *co, Register acc)
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
vm_setup_kwargs(struct ThreadState *ts, PyCodeObject2 *co, Register acc, PyObject **kwnames)
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
        if (prev.as_int64 != 0) {
            DECREF(prev);
        }
    }
    return 0;
}

// Clears the arguments to a failed function call. This is necessary
// if the function is called from C, but for simplicity we clean-up here
// for functions called from both C and Python.
void
vm_setup_err(struct ThreadState *ts, Register acc)
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

static PyObject *
build_tuple(struct ThreadState *ts, Py_ssize_t base, Py_ssize_t n)
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
vm_callargs_to_tuple(struct ThreadState *ts)
{
    PyObject *args = AS_OBJ(ts->regs[-FRAME_EXTRA-2]);
    PyObject *res = PySequence_Tuple(args);
    if (UNLIKELY(res == NULL)) {
        if (Py_TYPE(args)->tp_iter == NULL && !PySequence_Check(args)) {
            PyErr_Clear();
            PyObject *funcstr = _PyObject_FunctionStr(AS_OBJ(ts->regs[-1]));
            if (funcstr != NULL) {
                _PyErr_Format(ts->ts, PyExc_TypeError,
                            "%U argument after * must be an iterable, not %.200s",
                            funcstr, Py_TYPE(args)->tp_name);
                Py_DECREF(funcstr);
            }
        }
        return -1;
    }
    Register prev = ts->regs[-FRAME_EXTRA-2];
    ts->regs[-FRAME_EXTRA-2] = PACK_OBJ(res);
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
vm_kwargs_to_dict(struct ThreadState *ts)
{
    PyObject *d = PyDict_New();
    if (d == NULL) {
        return -1;
    }
    PyObject *kwargs = AS_OBJ(ts->regs[-FRAME_EXTRA-1]);
    if (_PyDict_MergeEx(d, kwargs, 2) < 0) {
        Py_DECREF(d);
        format_kwargs_error(ts->ts, AS_OBJ(ts->regs[-1]), kwargs);
        return -1;
    }
    Register prev = ts->regs[-FRAME_EXTRA-1];
    ts->regs[-FRAME_EXTRA-1] = PACK_OBJ(d);
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
vm_setup_annotations(struct ThreadState *ts, PyObject *locals)
{
    _Py_IDENTIFIER(__annotations__);
    PyObject *ann_dict;
    int err;
    if (PyDict_CheckExact(locals)) {
        ann_dict = _PyDict_GetItemIdWithError(locals, &PyId___annotations__);
        if (ann_dict != NULL) {
            return 0;
        }
        if (_PyErr_Occurred(ts->ts)) {
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
        if (!_PyErr_ExceptionMatches(ts->ts, PyExc_KeyError)) {
            return -1;
        }
        _PyErr_Clear(ts->ts);
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

#define MAX_STACK_SIZE (1073741824/sizeof(Register))
#define PY_STACK_EXTRA 1

int
vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed)
{
    Py_ssize_t oldsize = ts->maxstack - ts->stack + PY_STACK_EXTRA;
    Py_ssize_t newsize = oldsize * 2;
    while (newsize < oldsize + needed) {
        if (newsize > (Py_ssize_t)MAX_STACK_SIZE) {
            PyErr_SetString(PyExc_MemoryError, "stack overflow");
            return -1;
        }
        newsize *= 2;
    }

    if (UNLIKELY(newsize > 4 * _Py_CheckRecursionLimit)) {
        if (vm_stack_depth(ts) > _Py_CheckRecursionLimit) {
            PyErr_SetString(PyExc_RecursionError,
                            "maximum recursion depth exceeded");
            return -1;
        }
    }

    Py_ssize_t offset = ts->regs - ts->stack;
    Register *newstack = (Register *)mi_realloc(ts->stack, newsize * sizeof(Register));
    if (newstack == NULL) {
        PyErr_SetString(PyExc_MemoryError, "unable to allocate stack");
        return -1;
    }
    ts->stack = newstack;
    ts->regs = newstack + offset;
    ts->maxstack = newstack + newsize - PY_STACK_EXTRA;
    memset(ts->stack + oldsize, 0, (newsize - oldsize) * sizeof(Register));
    return 0;
}

int
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

void vm_free_threadstate(struct ThreadState *ts)
{
    while (ts->regs != ts->stack) {
        vm_pop_frame(ts);
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
vm_end_async_for(struct ThreadState *ts, Py_ssize_t opA)
{
    PyObject *exc = AS_OBJ(ts->regs[opA + 2]);
    if (!PyErr_GivenExceptionMatches(exc, PyExc_StopAsyncIteration)) {
        Py_INCREF(exc);
        PyObject *type = (PyObject *)Py_TYPE(exc);
        Py_INCREF(type);
        PyObject *tb = PyException_GetTraceback(exc);
        _PyErr_Restore(ts->ts, type, exc, tb);
        return -1;
    }
    CLEAR(ts->regs[opA + 2]);
    assert(ts->regs[opA + 1].as_int64 == -1);
    ts->regs[opA + 1].as_int64 = 0;
    CLEAR(ts->regs[opA]);
    // else if (tstate->c_tracefunc != NULL) {
    //     call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, f);
    // }
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
vm_err_non_iterator(struct ThreadState *ts, PyObject *o)
{
    PyErr_Format(PyExc_TypeError,
                 "iter() returned non-iterator "
                 "of type '%.100s'",
                 Py_TYPE(o)->tp_name);
}

void
vm_err_yield_from_coro(struct ThreadState *ts)
{
    _PyErr_SetString(ts->ts, PyExc_TypeError,
                     "cannot 'yield from' a coroutine object "
                     "in a non-coroutine generator");
}

void
vm_err_async_with_aenter(struct ThreadState *ts, Register acc)
{
    PyTypeObject *type = Py_TYPE(AS_OBJ(acc));
    _PyErr_Format(ts->ts, PyExc_TypeError,
                  "'async with' received an object from __aenter__ "
                  "that does not implement __await__: %.100s",
                  type->tp_name);
}

void
vm_err_coroutine_awaited(struct ThreadState *ts)
{
    _PyErr_SetString(ts->ts, PyExc_RuntimeError,
                    "coroutine is being awaited already");
}

static bool
is_freevar(PyCodeObject2 *co, Py_ssize_t varidx)
{
    for (Py_ssize_t i = co->co_ndefaultargs; i < co->co_nfreevars; i++) {
        if (co->co_free2reg[i*2+1] == varidx) {
            return true;
        }
    }
    return false;
}

void
vm_err_unbound(struct ThreadState *ts, Py_ssize_t idx)
{
    /* Don't stomp existing exception */
    if (_PyErr_Occurred(ts->ts)) {
        return;
    }
    PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyCodeObject2 *co = PyCode2_FROM_FUNC(func);
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
vm_err_async_for_aiter(struct ThreadState *ts, PyTypeObject *type)
{
    _PyErr_Format(ts->ts, PyExc_TypeError,
        "'async for' requires an object with "
        "__aiter__ method, got %.100s",
        type->tp_name);
}

void
vm_err_async_for_no_anext(struct ThreadState *ts, PyTypeObject *type)
{
    _PyErr_Format(ts->ts, PyExc_TypeError,
        "'async for' received an object from __aiter__ "
        "that does not implement __anext__: %.100s",
        type->tp_name);
}

void
vm_err_async_for_anext_invalid(struct ThreadState *ts, Register res)
{
    _PyErr_FormatFromCause(PyExc_TypeError,
        "'async for' received an invalid object "
        "from __anext__: %.100s",
        Py_TYPE(AS_OBJ(res))->tp_name);
}

void
vm_err_dict_update(struct ThreadState *ts, Register acc)
{
    if (_PyErr_ExceptionMatches(ts->ts, PyExc_AttributeError)) {
        PyObject *obj = AS_OBJ(acc);
        _PyErr_Format(ts->ts, PyExc_TypeError,
                      "'%.200s' object is not a mapping",
                      Py_TYPE(obj)->tp_name);
    }
}

void
vm_err_dict_merge(struct ThreadState *ts, Register acc)
{

    /* _PyDict_MergeEx raises attribute
     * error (percolated from an attempt
     * to get 'keys' attribute) instead of
     * a type error if its second argument
     * is not a mapping.
     */
    PyThreadState *tstate = ts->ts;

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
    Py_ssize_t frame_delta = FRAME_EXTRA;
    ts->regs += frame_delta;
    ts->regs[-4].as_int64 = frame_delta;
    ts->regs[-2] = old->regs[-2];  // PyFrameObject
    old->regs[-2].as_int64 = 0;
    ts->regs[-3].as_int64 = FRAME_GENERATOR;
    ts->regs[-1] = STRONG_REF(old->regs[-1]);  // copy func

    // The new thread-state takes ownership of the "func".
    // We can't clear the old thread states function because it will be
    // referenced (and cleared) by RETURN_VALUE momentarily. Instead, just
    // mark it as a non-refcounted reference -- the generator owns them now.
    old->regs[-1].as_int64 |= NO_REFCOUNT_TAG;

    Py_ssize_t nargs = code->co_totalargcount;
    if (code->co_packed_flags & CODE_FLAG_VARARGS) {
        // FIXME: I think this is wrong now that varargs are prior to header
        nargs += 1;
    }
    if (code->co_packed_flags & CODE_FLAG_VARKEYWORDS) {
        // FIXME: I think this is wrong now that varargs are prior to header
        nargs += 1;
    }
    for (Py_ssize_t i = 0; i != nargs; i++) {
        // NB: we have to convert aliases into strong references. The
        // generator may outlive the calling frame.
        ts->regs[i] = STRONG_REF(old->regs[i]);
        old->regs[i].as_int64 = 0;
    }
    if (code->co_packed_flags & CODE_FLAG_LOCALS_DICT) {
        assert(nargs == 0);
        ts->regs[0] = old->regs[0];
        old->regs[0].as_int64 = 0;
    }
    for (Py_ssize_t i = code->co_ndefaultargs; i != code->co_nfreevars; i++) {
        Py_ssize_t r = code->co_free2reg[i*2+1];
        ts->regs[r] = old->regs[r];
        old->regs[r].as_int64 = 0;
    }
    for (Py_ssize_t i = 0; i != code->co_ncells; i++) {
        Py_ssize_t r = code->co_cell2reg[i];
        if (r >= nargs) {
            ts->regs[r] = old->regs[r];
            old->regs[r].as_int64 = 0;
        }
    }
    ts->ts = PyThreadState_GET();
    return 0;
}

PyObject *
vm_builtins_from_globals(PyObject *globals)
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
        // _PyObject_SET_DEFERRED_RC(builtins);
        // Py_DECREF(builtins);
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

    // Py_INCREF_STACK(builtins);
    Py_INCREF(builtins);
    return builtins;
}

static int
setup_frame_ex(struct ThreadState *ts, PyObject *func, Py_ssize_t extra, Py_ssize_t nargs)
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
setup_frame(struct ThreadState *ts, PyObject *func)
{
    return setup_frame_ex(ts, func, /*extra=*/0, /*nargs=*/0);
}

static struct ThreadState *
current_thread_state(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    return tstate->active;
}

PyObject *
PyEval2_EvalGen(PyGenObject2 *gen, PyObject *opt_value)
{
    PyThreadState *tstate;
    struct ThreadState *ts;

    tstate = PyThreadState_GET();
    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    ts = &gen->base.thread;
    assert(ts->prev == NULL);

    ts->ts = tstate;

    // push `ts` onto the list of active threads
    ts->prev = tstate->active;
    tstate->active = ts;

    gen->status = GEN_RUNNING;

    Register acc = opt_value ? PACK_INCREF(opt_value) : (Register){0};
    PyObject *ret = _PyEval_Fast(ts, acc, ts->pc);

    // pop `ts` from the list of active threads
    tstate->active = ts->prev;
    ts->prev = NULL;

    _Py_LeaveRecursiveCall(tstate);
    return ret;
}

PyObject *
_PyEval2_EvalFunc(PyObject *func, PyObject *locals)
{
    assert(PyFunc_Check(func));
    PyThreadState *tstate = PyThreadState_GET();
    struct ThreadState *ts = tstate->active;
    PyObject *ret = NULL;
    int err;

    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    err = setup_frame(ts, func);
    if (UNLIKELY(err != 0)) {
        goto exit;
    }
    ts->regs[0] = PACK(locals, NO_REFCOUNT_TAG);

    Register acc;
    acc.as_int64 = 0;
    ret = _PyEval_Fast(ts, acc, ((PyFuncBase *)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}

PyObject *
PyEval2_EvalCode(PyObject *co, PyObject *globals, PyObject *locals)
{
    PyFunc *func = (PyFunc *)PyFunc_New(co, globals, NULL);
    if (func == NULL) {
        return NULL;
    }

#ifdef Py_REF_DEBUG
    intptr_t oldrc = _PyThreadState_GET()->thread_ref_total;
#endif

    PyObject *ret = _PyEval2_EvalFunc((PyObject *)func, locals);
    Py_DECREF(func);

#ifdef Py_REF_DEBUG
    intptr_t newrc = _PyThreadState_GET()->thread_ref_total;
    // fprintf(stderr, "RC %ld to %ld (%ld)\n", (long)oldrc, (long)newrc, (long)(newrc - oldrc));
#endif

    return ret;
}

int
vm_super_init(PyObject **out_obj, PyTypeObject **out_type)
{
    _Py_IDENTIFIER(__class__);

    struct ThreadState *ts = current_thread_state();
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
    if (func == NULL || !PyFunc_Check(func)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "super(): no current function");
        return -1;
    }
    PyCodeObject2 *co = PyCode2_FromFunc((PyFunc *)func);
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

int
vm_import_star(struct ThreadState *ts, PyObject *v, PyObject *locals)
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
            _PyErr_SetString(ts->ts, PyExc_ImportError,
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
            if (!_PyErr_ExceptionMatches(ts->ts, PyExc_IndexError)) {
                err = -1;
            }
            else {
                _PyErr_Clear(ts->ts);
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
                _PyErr_Format(ts->ts, PyExc_TypeError,
                              "module __name__ must be a string, not %.100s",
                              Py_TYPE(modname)->tp_name);
            }
            else {
                _PyErr_Format(ts->ts, PyExc_TypeError,
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

    struct ThreadState *ts = tstate->active;
    Register acc;
    PyObject *ret = NULL;

    if (PyTuple_GET_SIZE(args) == 0 && kwds == NULL) {
        acc.as_int64 = 0;
        int err = setup_frame(ts, func);
        if (UNLIKELY(err != 0)) {
            goto exit;
        }
    }
    else {
        acc.as_int64 = ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS;
        int err = setup_frame_ex(ts, func, /*extra=*/2, /*nargs=*/0);
        if (UNLIKELY(err != 0)) {
            goto exit;
        }
        ts->regs[-FRAME_EXTRA-2] = PACK(args, NO_REFCOUNT_TAG);
        if (kwds != NULL) {
            ts->regs[-FRAME_EXTRA-1] = PACK(kwds, NO_REFCOUNT_TAG);
        }
    }

    ret = _PyEval_Fast(ts, acc, ((PyFuncBase*)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}

PyObject *
_PyFunc_Vectorcall(PyObject *func, PyObject* const* stack,
                   size_t nargsf, PyObject *kwnames)
{
    PyThreadState *tstate = PyThreadState_GET();
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

    if (UNLIKELY(nargs >= 255 || 
          (kwnames != NULL && PyTuple_GET_SIZE(kwnames) >= 256))) {
        return _PyObject_MakeTpCall(tstate, func, stack, nargs, kwnames);
    }

    if (UNLIKELY(_Py_EnterRecursiveCall(tstate, ""))) {
        return NULL;
    }

    struct ThreadState *ts = tstate->active;
    PyObject *ret = NULL;
    Register acc;
    int err;

    if (LIKELY(kwnames == NULL)) {
        acc.as_int64 = nargs;
        err = setup_frame_ex(ts, func, /*extra=*/0, /*nargs=*/nargs);
        if (UNLIKELY(err != 0)) {
            goto exit;
        }
        for (Py_ssize_t i = 0; i < nargs; i++) {
            ts->regs[i] = PACK(stack[i], NO_REFCOUNT_TAG);
        }
    }
    else {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        err = setup_frame_ex(ts, func, /*extra=*/nkwargs + 1, /*nargs=*/nargs);
        if (UNLIKELY(err != 0)) {
            goto exit;
        }
        for (Py_ssize_t i = 0; i < nargs; i++) {
            ts->regs[i] = PACK(stack[i], NO_REFCOUNT_TAG);
        }
        for (Py_ssize_t i = 0; i < nkwargs; i++) {
            ts->regs[-FRAME_EXTRA - 1 - nkwargs + i] = PACK(stack[i + nargs], NO_REFCOUNT_TAG);
        }
        ts->regs[-FRAME_EXTRA - 1] = PACK(kwnames, NO_REFCOUNT_TAG);
        acc.as_int64 = nargs + (nkwargs << 8);
    }

    ret = _PyEval_Fast(ts, acc, ((PyFuncBase*)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}


PyObject *
PyEval2_GetGlobals(void)
{
    struct ThreadState *ts = current_thread_state();
    Py_ssize_t offset = 0;
    while (&ts->regs[offset] > ts->stack) {
        PyObject *func = AS_OBJ(ts->regs[offset-1]);
        if (PyFunc_Check(func)) {
            return ((PyFunc *)func)->globals;
        }

        intptr_t frame_delta = ts->regs[offset-4].as_int64;
        offset -= frame_delta;
    }

    // no frame
    return NULL;
}

PyFrameObject *
vm_frame(struct ThreadState *ts)
{
    return vm_frame_at_offset(ts, 0);
}

PyFrameObject *
vm_frame_at_offset(struct ThreadState *ts, Py_ssize_t offset)
{
    PyFrameObject *top = NULL;
    PyFrameObject *prev = NULL;

    bool done = false;
    struct stack_walk w;
    vm_stack_walk_init(&w, ts);
    w.next_offset = offset;
    while (vm_stack_walk(&w) && !done) {
        Register *regs = vm_stack_walk_regs(&w);
        PyObject *callable = AS_OBJ(regs[-1]);
        if (!PyFunc_Check(callable)) {
            continue;
        }

        PyFrameObject *frame;
        if (regs[-2].as_int64 != 0) {
            frame = (PyFrameObject *)AS_OBJ(regs[-2]);
            // Update f_lasti
            PyCodeObject2 *co = PyCode2_FromFunc((PyFunc *)callable);
            const uint8_t *first_instr = PyCode2_GET_CODE(co);
            frame->f_lasti = w.pc - first_instr;
            done = true;
        }
        else {
            const uint8_t *pc = w.pc;
            if (w.ts->thread_type == THREAD_GENERATOR &&
                PyGen2_FromThread(w.ts)->status == GEN_CREATED)
            {
                // want address of the current or previously executed
                // instruction
                pc -= 1;
            }
            frame = new_fake_frame(w.ts, w.offset, pc);
            if (frame == NULL) {
                return NULL;
            }
            // FIXME: above may re-allocate regs!
            regs[-2] = PACK(frame, REFCOUNT_TAG);
        }

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

static PyObject *
frame_to_locals(struct ThreadState *ts, Py_ssize_t offset)
{
    PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[offset-1]);
    assert(PyFunc_Check(func));
    PyCodeObject2 *code = PyCode2_FromFunc(func);
    if ((code->co_flags & CO_NEWLOCALS) == 0) {
        PyObject *locals = AS_OBJ(ts->regs[offset]);
        assert(PyMapping_Check(locals));
        return locals;
    }

    PyFrameObject *frame = vm_frame(ts);
    if (frame == NULL) {
        return NULL;
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
        return NULL;
    }

    for (Py_ssize_t i = 0, n = code->co_nlocals; i < n; i++) {
        vars[i] = AS_OBJ(ts->regs[offset+i]);
    }

    for (Py_ssize_t i = 0, n = code->co_ncells; i < n; i++) {
        Py_ssize_t reg = code->co_cell2reg[i];
        assert(PyCell_Check(vars[reg]));
        vars[reg] = PyCell_GET(vars[reg]);
    }

    Py_ssize_t ndefaults = code->co_ndefaultargs;
    for (Py_ssize_t i = ndefaults, n = code->co_nfreevars; i < n; i++) {
        Py_ssize_t reg = code->co_free2reg[i*2+1];
        assert(PyCell_Check(vars[reg]));
        vars[reg] = PyCell_GET(vars[reg]);
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
vm_eval_breaker(struct ThreadState *ts)
{
    int opcode = vm_opcode(ts->pc);
    if (opcode == YIELD_FROM) {
        return 0;
    }
    return _PyEval_HandleBreaker(ts->ts);
}

PyObject *
PyEval2_GetLocals(void)
{
    struct ThreadState *ts = current_thread_state();
    Py_ssize_t offset = 0;
    while (&ts->regs[offset] > ts->stack) {
        PyObject *func = AS_OBJ(ts->regs[offset-1]);
        if (PyFunc_Check(func)) {
            return frame_to_locals(ts, offset);
        }

        intptr_t frame_delta = ts->regs[offset-4].as_int64;
        offset -= frame_delta;
    }

    _PyErr_SetString(ts->ts, PyExc_SystemError, "frame does not exist!");
    return NULL;
}

PyObject *
_Py_method_call(PyObject *obj, PyObject *args, PyObject *kwds)
{
    PyMethodObject *method = (PyMethodObject *)obj;
    if (UNLIKELY(!PyFunc_Check(method->im_func))) {
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

    struct ThreadState *ts = tstate->active;
    Py_ssize_t nargs = 1 + PyTuple_GET_SIZE(args);
    PyObject *ret = NULL;
    PyObject *func = method->im_func;

    int err = setup_frame_ex(ts, func, /*extra=*/0, /*nargs=*/nargs);
    if (UNLIKELY(err != 0)) {
        goto exit;
    }

    ts->regs[0] = PACK(method->im_self, NO_REFCOUNT_TAG);
    for (Py_ssize_t i = 1; i < nargs; i++) {
        ts->regs[i] = PACK(PyTuple_GET_ITEM(args, i - 1), NO_REFCOUNT_TAG);
    }

    Register acc;
    acc.as_int64 = nargs;
    ret = _PyEval_Fast(ts, acc, ((PyFuncBase*)func)->first_instr);
exit:
    _Py_LeaveRecursiveCall(tstate);
    return ret;
}

#include "ceval_intrinsics.h"
