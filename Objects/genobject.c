/* Generator object implementation */

#include "Python.h"
#include "pycore_gc.h"            // _PyGC_TraverseStack()
#include "pycore_generator.h"
#include "pycore_object.h"
#include "pycore_pyerrors.h"      // _PyErr_ClearExcState()
#include "pycore_pystate.h"
#include "pycore_stackwalk.h"
#include "structmember.h"
#include "opcode.h"

static PyObject *async_gen_asend_new(PyAsyncGenObject *, PyObject *);
static PyObject *async_gen_athrow_new(PyAsyncGenObject *, PyObject *);

static PyTypeObject _PyAsyncGenASend2_Type;
static PyTypeObject _PyAsyncGenAThrow2_Type;

static char *NON_INIT_CORO_MSG = "can't send non-None value to a "
                                 "just-started coroutine";

static char *ASYNC_GEN_IGNORED_EXIT_MSG =
                                 "async generator ignored GeneratorExit";

static PyGenObject *
gen_new_with_qualname(PyTypeObject *type, PyThreadState *tstate)
{
    PyGenObject *gen = (PyGenObject *)_PyObject_GC_Calloc(type->tp_basicsize);
    if (UNLIKELY(gen == NULL)) {
        return NULL;
    }
    PyObject_INIT(gen, type);

    int err = vm_init_thread_state(tstate, gen);
    if (UNLIKELY(err != 0)) {
        _Py_DEC_REFTOTAL;
        PyObject_GC_Del(gen);
    }

    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(tstate->regs[-1]);
    PyCodeObject *code = _PyFunction_GET_CODE(func);

    gen->name = func->func_name;
    gen->qualname = func->func_qualname;
    gen->code = (PyObject *)code;
    gen->status = GEN_CREATED;
    if (!_PyObject_IS_DEFERRED_RC(code)) {
        // code almost always uses deferred rc, but it might be
        // disabled if the code object was resurrected by a finalizer.
        gen->retains_code = 1;
        Py_INCREF(code);
    }
    Py_INCREF(gen->name);
    Py_INCREF(gen->qualname);
    _PyObject_GC_TRACK(gen);

    if (PyCoro_CheckExact(gen) && tstate->coroutine_origin_tracking_depth > 0) {
        PyCoroObject *coro = (PyCoroObject *)gen;
        coro->origin = vm_compute_cr_origin(tstate);
        if (coro->origin == NULL) {
            Py_DECREF(gen);
            return NULL;
        }
    }

    return gen;
}

PyGenObject *
PyGen_NewWithCode(PyThreadState *tstate, PyCodeObject *co)
{
    if (co->co_flags & CO_COROUTINE) {
        return gen_new_with_qualname(&PyCoro_Type, tstate);
    }
    else if (co->co_flags & CO_ASYNC_GENERATOR) {
        return gen_new_with_qualname(&PyAsyncGen_Type, tstate);
    }
    else {
        return gen_new_with_qualname(&PyGen_Type, tstate);
    }
}

/*
 *   If StopIteration exception is set, fetches its 'value'
 *   attribute if any, otherwise sets pvalue to None.
 *
 *   Returns 0 if no exception or StopIteration is set.
 *   If any other exception is set, returns -1 and leaves
 *   pvalue unchanged.
 */

PyObject *
_PyGen_FetchStopIterationValue2(void)
{
    PyObject *et, *ev, *tb;
    PyObject *value = NULL;

    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Fetch(&et, &ev, &tb);
        if (ev) {
            /* exception will usually be normalised already */
            if (PyObject_TypeCheck(ev, (PyTypeObject *) et)) {
                value = ((PyStopIterationObject *)ev)->value;
                Py_INCREF(value);
                Py_DECREF(ev);
            } else if (et == PyExc_StopIteration && !PyTuple_Check(ev)) {
                /* Avoid normalisation and take ev as value.
                 *
                 * Normalization is required if the value is a tuple, in
                 * that case the value of StopIteration would be set to
                 * the first element of the tuple.
                 *
                 * (See _PyErr_CreateException code for details.)
                 */
                value = ev;
            } else {
                /* normalisation required */
                PyErr_NormalizeException(&et, &ev, &tb);
                if (!PyObject_TypeCheck(ev, (PyTypeObject *)PyExc_StopIteration)) {
                    PyErr_Restore(et, ev, tb);
                    return NULL;
                }
                value = ((PyStopIterationObject *)ev)->value;
                Py_INCREF(value);
                Py_DECREF(ev);
            }
        }
        Py_XDECREF(et);
        Py_XDECREF(tb);
    } else if (PyErr_Occurred()) {
        return NULL;
    }
    return value == NULL ? Py_None : value;
}

static int
gen_traverse(PyGenObject *gen, visitproc visit, void *arg)
{
    Py_VISIT(gen->name);
    Py_VISIT(gen->qualname);
    Py_VISIT(gen->return_value);
    Py_VISIT(gen->yield_from);
    if (gen->base.thread.prev == NULL) {
        _PyGC_TraverseStack(&gen->base.thread, visit, arg);
    }
    if (gen->retains_code || _PyGC_VisitorType(visit) != _Py_GC_VISIT_DECREF) {
        Py_VISIT(gen->code);
    }
    return 0;
}

static void
gen_dealloc(PyGenObject *gen)
{
    assert(gen->status != GEN_RUNNING);
    PyObject *self = (PyObject *) gen;
    _PyObject_GC_UNTRACK(gen);

    if (gen->weakreflist != NULL) {
        PyObject_ClearWeakRefs(self);
    }

    _PyObject_GC_TRACK(self);
    if (PyObject_CallFinalizerFromDealloc(self))
        return;                     /* resurrected.  :( */
    _PyObject_GC_UNTRACK(self);

    vm_free_threadstate(&gen->base.thread);
    Py_CLEAR(gen->name);
    Py_CLEAR(gen->qualname);
    Py_CLEAR(gen->return_value);
    Py_CLEAR(gen->yield_from);
    if (gen->retains_code) {
        Py_CLEAR(gen->code);
    }
    else {
        gen->code = NULL;
    }
    if (PyAsyncGen_CheckExact(gen)) {
        /* We have to handle this case for asynchronous generators
           right here, because this code has to be between UNTRACK
           and GC_Del. */
        Py_CLEAR(((PyAsyncGenObject *)gen)->finalizer);
    }
    if (PyCoro_CheckExact(gen)) {
        Py_CLEAR(((PyCoroObject *)gen)->origin);
    }
    PyObject_GC_Del(gen);
}

static const char *
gen_typename(PyGenObject *gen)
{
    if (PyAsyncGen_CheckExact(gen)) {
        return "async generator";
    }
    else if (PyCoro_CheckExact(gen)) {
        return "coroutine";
    }
    else {
        assert(PyGen_CheckExact(gen));
        return "generator";
    }
}

static PyObject *
_PyGen_SetStopIterationValue2(PyGenObject *gen);

static PyObject *
gen_wrap_exception(PyGenObject *gen)
{
    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        _PyErr_FormatFromCause(
            PyExc_RuntimeError,
            "%s raised StopIteration",
            gen_typename(gen));
    }
    else if (PyAsyncGen_CheckExact(gen) &&
                PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
        _PyErr_FormatFromCause(
            PyExc_RuntimeError,
            "%s raised StopAsyncIteration",
            gen_typename(gen));
    }
    return NULL;
}

static PyObject *
gen_send_internal(PyGenObject *gen, PyObject *opt_value)
{
    PyObject *res;

    if (gen->status == GEN_CREATED) {
        if (UNLIKELY(opt_value != Py_None)) {
            PyErr_Format(
                PyExc_TypeError,
                "can't send non-None value to a just-started %s",
                gen_typename(gen));
            return NULL;
        }
        opt_value = NULL;
    }

    res = PyEval2_EvalGen(gen, opt_value);

    if (LIKELY(res != NULL)) {
        assert(gen->status == GEN_SUSPENDED);
        return res;
    }

    if (LIKELY(gen->return_value == Py_None)) {
        gen->return_value = NULL;
        PyErr_SetNone(PyAsyncGen_CheckExact(gen)
                        ? PyExc_StopAsyncIteration
                        : PyExc_StopIteration);
        return NULL;
    }
    else if (gen->return_value != NULL) {
        return _PyGen_SetStopIterationValue2(gen);
    }
    else {
        return gen_wrap_exception(gen);
    }
}

static PyObject *
gen_status_error(PyGenObject *gen, PyObject *arg)
{
    if (gen->status == GEN_RUNNING) {
        PyErr_Format(PyExc_ValueError, "%s already executing",
            gen_typename(gen));
        return NULL;
    }

    assert(gen->status == GEN_CLOSED);
    // if (PyCoro_CheckExact(gen) && !closing) {
    //     /* `gen` is an exhausted coroutine: raise an error,
    //        except when called from gen_close(), which should
    //        always be a silent method. */
    //     PyErr_SetString(
    //         PyExc_RuntimeError,
    //         "cannot reuse already awaited coroutine");
    // }
    // else if (arg && !exc) {
    /* `gen` is an exhausted generator:
        only set exception if called from send(). */

    if (PyCoro_CheckExact(gen)) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited coroutine");
    }
    else if (PyAsyncGen_CheckExact(gen)) {
        PyErr_SetNone(PyExc_StopAsyncIteration);
    }
    else {
        PyErr_SetNone(PyExc_StopIteration);
    }
    return NULL;
}

PyDoc_STRVAR(send_doc,
"send(arg) -> send 'arg' into generator,\n\
return next yielded value or raise StopIteration.");

PyObject *
_PyGen_Send(PyGenObject *gen, PyObject *arg)
{
    assert(arg != NULL);
    if (UNLIKELY(gen->status >= GEN_RUNNING)) {
        return gen_status_error(gen, arg);
    }

    return gen_send_internal(gen, arg);
}

static PyObject *
_PyObject_YieldFrom_ex(PyObject *awaitable, PyObject *arg)
{
    _Py_IDENTIFIER(send);
    if (arg == Py_None) {
        return Py_TYPE(awaitable)->tp_iternext(awaitable);
    }
    return _PyObject_CallMethodIdOneArg(awaitable, &PyId_send, arg);
}

PyObject *
_PyGen_YieldFrom(PyGenObject *gen, PyObject *awaitable, PyObject *arg)
{
    Py_CLEAR(gen->yield_from);
    PyObject *res;
    if (LIKELY(PyGen_CheckExact(awaitable) || PyCoro_CheckExact(awaitable))) {
        res = _PyGen_Send((PyGenObject *)awaitable, arg);
    }
    else {
        res = _PyObject_YieldFrom_ex(awaitable, arg);
    }
    if (res != NULL) {
        assert(gen->yield_from == NULL);
        Py_XINCREF(awaitable);
        gen->yield_from = awaitable;
    }
    return res;
}

static int
gen_is_coroutine(PyObject *o)
{
    if (PyGen_CheckExact(o)) {
        PyCodeObject *code = (PyCodeObject *)((PyGenObject *)o)->code;
        if (code->co_flags & CO_ITERABLE_COROUTINE) {
            return 1;
        }
    }
    return 0;
}

/*
 *   This helper function returns an awaitable for `o`:
 *     - `o` if `o` is a coroutine-object;
 *     - `type(o)->tp_as_async->am_await(o)`
 *
 *   Raises a TypeError if it's not possible to return
 *   an awaitable and returns NULL.
 */
PyObject *
_PyCoro_GetAwaitableIter(PyObject *o)
{
    unaryfunc getter = NULL;
    PyTypeObject *ot;

    if (gen_is_coroutine(o)) {
        /* 'o' is a coroutine. */
        Py_INCREF(o);
        return o;
    }

    ot = Py_TYPE(o);
    if (ot->tp_as_async != NULL) {
        getter = ot->tp_as_async->am_await;
    }
    if (getter != NULL) {
        PyObject *res = (*getter)(o);
        if (res != NULL) {
            if (PyCoro_CheckExact(res) || gen_is_coroutine(res)) {
                /* __await__ must return an *iterator*, not
                   a coroutine or another awaitable (see PEP 492) */
                PyErr_SetString(PyExc_TypeError,
                                "__await__() returned a coroutine");
                Py_CLEAR(res);
            } else if (!PyIter_Check(res)) {
                PyErr_Format(PyExc_TypeError,
                             "__await__() returned non-iterator "
                             "of type '%.100s'",
                             Py_TYPE(res)->tp_name);
                Py_CLEAR(res);
            }
        }
        return res;
    }

    PyErr_Format(PyExc_TypeError,
                 "object %.100s can't be used in 'await' expression",
                 ot->tp_name);
    return NULL;
}

PyDoc_STRVAR(close_doc,
"close() -> raise GeneratorExit inside generator.");

/*
 *   This helper function is used by gen_close and gen_throw to
 *   close a subiterator being delegated to by yield-from.
 */

static PyObject *gen_close(PyGenObject *, PyObject *);

static int
gen_close_iter(PyObject *yf)
{
    PyObject *retval = NULL;
    _Py_IDENTIFIER(close);

    if (PyGen_CheckExact(yf) || PyCoro_CheckExact(yf)) {
        retval = gen_close((PyGenObject *)yf, NULL);
        if (retval == NULL)
            return -1;
    }
    else {
        PyObject *meth;
        if (_PyObject_LookupAttrId(yf, &PyId_close, &meth) < 0) {
            PyErr_WriteUnraisable(yf);
        }
        if (meth) {
            retval = _PyObject_CallNoArg(meth);
            Py_DECREF(meth);
            if (retval == NULL)
                return -1;
        }
    }
    Py_XDECREF(retval);
    return 0;
}

static PyObject *
gen_throw_current(PyGenObject *gen)
{
    struct _PyThreadStack *ts = &gen->base.thread;
    if (gen->status == GEN_CLOSED) {
        if (PyCoro_CheckExact(gen)) {
            return gen_status_error(gen, NULL);
        }
        return NULL;
    }
    if (gen->status == GEN_RUNNING) {
        return gen_status_error(gen, NULL);
    }
    if (gen->status == GEN_CREATED) {
        // If the generator has just started, the PC points to the *next*
        // instruction, which may be inside an exception handler. During
        // normal execution the PC points to the *current* instruction.
        // Backs up the PC by one byte: this will be in the middle of the
        // COROGEN_HEADER, but that's OK -- we will not actually execute
        // from this PC.
        ts->pc -= 1;
    }
    // ts->ts = PyThreadState_GET();
    _PyErr_ChainExceptionsFrom(ts);
    return gen_send_internal(gen, Py_None);
}


PyDoc_STRVAR(throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in generator,\n\
return next yielded value or raise StopIteration.");

static PyObject *
_gen_throw(PyGenObject *gen, int close_on_genexit,
           PyObject *typ, PyObject *val, PyObject *tb)
{
    _Py_IDENTIFIER(throw);

    PyObject *yf = gen->yield_from;
    if (yf != NULL) {
        gen->yield_from = NULL;
        assert(gen->status == GEN_SUSPENDED);
        PyObject *ret;
        if (PyErr_GivenExceptionMatches(typ, PyExc_GeneratorExit) &&
            close_on_genexit
        ) {
            // Asynchronous generators *should not* be closed right away.
            // We have to allow some awaits to work it through, hence the
            // `close_on_genexit` parameter here.
            char old_status = gen->status;
            gen->status = GEN_RUNNING;
            int err = gen_close_iter(yf);
            gen->status = old_status;
            Py_DECREF(yf);
            if (err < 0) {
                return gen_throw_current(gen);
            }
            goto throw_here;
        }
        if (PyGen_CheckExact(yf) || PyCoro_CheckExact(yf)) {
            /* `yf` is a generator or a coroutine. */
            /* Close the generator that we are currently iterating with
               'yield from' or awaiting on with 'await'. */
            char old_status = gen->status;
            gen->status = GEN_RUNNING;
            ret = _gen_throw((PyGenObject *)yf, close_on_genexit,
                             typ, val, tb);
            gen->status = old_status;
        } else {
            /* `yf` is an iterator or a coroutine-like object. */
            PyObject *meth;
            if (_PyObject_LookupAttrId(yf, &PyId_throw, &meth) < 0) {
                Py_DECREF(yf);
                return NULL;
            }
            if (meth == NULL) {
                Py_DECREF(yf);
                goto throw_here;
            }
            char old_status = gen->status;
            gen->status = GEN_RUNNING;
            ret = PyObject_CallFunctionObjArgs(meth, typ, val, tb, NULL);
            gen->status = old_status;
            Py_DECREF(meth);
        }
        if (ret == NULL) {
            // Terminate repetition of YIELD_FROM
            struct _PyThreadStack *ts = &gen->base.thread;
            if (ts->pc[0] == WIDE) {
                assert(ts->pc[1] == YIELD_FROM);
                ts->pc += OP_SIZE_WIDE_YIELD_FROM;
            }
            else {
                assert(ts->pc[0] == YIELD_FROM);
                ts->pc += OP_SIZE_YIELD_FROM;
            }

            PyObject *val;
            if (_PyGen_FetchStopIterationValue(&val) == 0) {
                // If the delegated subgenerator returned a value
                // (via StopIteration); send it to the calling generator.
                ret = gen_send_internal(gen, val);
                Py_DECREF(val);
            } else {
                ret = gen_throw_current(gen);
            }

            Py_DECREF(yf);
        }
        else {
            // Restore the `yield_from` if the exception was caught by the
            // delegated subgenerator.
            gen->yield_from = yf;
        }
        return ret;
    }

throw_here:
    /* First, check the traceback argument, replacing None with
       NULL. */
    if (tb == Py_None) {
        tb = NULL;
    }
    else if (tb != NULL && !PyTraceBack_Check(tb)) {
        PyErr_SetString(PyExc_TypeError,
            "throw() third argument must be a traceback object");
        return NULL;
    }

    Py_INCREF(typ);
    Py_XINCREF(val);
    Py_XINCREF(tb);

    if (PyExceptionClass_Check(typ))
        PyErr_NormalizeException(&typ, &val, &tb);

    else if (PyExceptionInstance_Check(typ)) {
        /* Raising an instance.  The value should be a dummy. */
        if (val && val != Py_None) {
            PyErr_SetString(PyExc_TypeError,
              "instance exception may not have a separate value");
            goto failed_throw;
        }
        else {
            /* Normalize to raise <class>, <instance> */
            Py_XDECREF(val);
            val = typ;
            typ = PyExceptionInstance_Class(typ);
            Py_INCREF(typ);

            if (tb == NULL)
                /* Returns NULL if there's no traceback */
                tb = PyException_GetTraceback(val);
        }
    }
    else {
        /* Not something you can raise.  throw() fails. */
        PyErr_Format(PyExc_TypeError,
                     "exceptions must be classes or instances "
                     "deriving from BaseException, not %s",
                     Py_TYPE(typ)->tp_name);
        goto failed_throw;
    }

    PyErr_Restore(typ, val, tb);
    return gen_throw_current(gen);

failed_throw:
    /* Didn't use our arguments, so restore their original refcounts */
    Py_DECREF(typ);
    Py_XDECREF(val);
    Py_XDECREF(tb);
    return NULL;
}


static PyObject *
gen_throw(PyGenObject *gen, PyObject *args)
{
    PyObject *typ;
    PyObject *tb = NULL;
    PyObject *val = NULL;

    if (!PyArg_UnpackTuple(args, "throw", 1, 3, &typ, &val, &tb)) {
        return NULL;
    }

    return _gen_throw(gen, 1, typ, val, tb);
}

static PyObject *
gen_close(PyGenObject *gen, PyObject *args)
{
    PyObject *retval;
    int err = 0;

    if (gen->status == GEN_CLOSED) {
        assert(gen->yield_from == NULL);
        Py_RETURN_NONE;
    }

    PyObject *yf = gen->yield_from;
    if (yf) {
        gen->yield_from = NULL;
        char old_status = gen->status;
        gen->status = GEN_RUNNING;
        err = gen_close_iter(yf);
        gen->status = old_status;
        Py_DECREF(yf);
    }

    if (err == 0) {
        PyErr_SetNone(PyExc_GeneratorExit);
    }

    retval = gen_throw_current(gen);

    if (retval) {
        Py_DECREF(retval);
        PyErr_Format(PyExc_RuntimeError,
            "%s ignored GeneratorExit",
            gen_typename(gen));
        return NULL;
    }
    if (PyErr_ExceptionMatches(PyExc_StopIteration)
        || PyErr_ExceptionMatches(PyExc_GeneratorExit)) {
        PyErr_Clear();          /* ignore these errors */
        Py_RETURN_NONE;
    }
    return NULL;
}

static PyObject *
_PyGen_SetStopIterationValue2(PyGenObject *gen)
{
    PyObject *e;
    PyObject *value = gen->return_value;

    if (value == NULL ||
        (!PyTuple_Check(value) && !PyExceptionInstance_Check(value)))
    {
        /* Delay exception instantiation if we can */
        PyErr_SetObject(PyExc_StopIteration, value);
        Py_CLEAR(gen->return_value);
        return NULL;
    }

    /* Construct an exception instance manually with
     * _PyObject_CallOneArg and pass it to PyErr_SetObject.
     *
     * We do this to handle a situation when "value" is a tuple, in which
     * case PyErr_SetObject would set the value of StopIteration to
     * the first element of the tuple.
     *
     * (See PyErr_SetObject/_PyErr_CreateException code for details.)
     */
    e = _PyObject_CallOneArg(PyExc_StopIteration, value);
    if (e == NULL) {
        return NULL;
    }

    PyErr_SetObject(PyExc_StopIteration, e);
    Py_DECREF(e);
    Py_CLEAR(gen->return_value);
    return NULL;
}

static PyObject *
gen_iternext(PyGenObject *gen)
{
    if (UNLIKELY(gen->status >= GEN_RUNNING)) {
        return gen_status_error(gen, NULL);
    }
    return gen_send_internal(gen, Py_None);
}

/*
 * Set StopIteration with specified value.  Value can be arbitrary object
 * or NULL.
 *
 * Returns 0 if StopIteration is set and -1 if any other exception is set.
 */
int
_PyGen_SetStopIterationValue(PyObject *value)
{
    PyObject *e;

    if (value == NULL ||
        (!PyTuple_Check(value) && !PyExceptionInstance_Check(value)))
    {
        /* Delay exception instantiation if we can */
        PyErr_SetObject(PyExc_StopIteration, value);
        return 0;
    }
    /* Construct an exception instance manually with
     * PyObject_CallOneArg and pass it to PyErr_SetObject.
     *
     * We do this to handle a situation when "value" is a tuple, in which
     * case PyErr_SetObject would set the value of StopIteration to
     * the first element of the tuple.
     *
     * (See PyErr_SetObject/_PyErr_CreateException code for details.)
     */
    e = PyObject_CallOneArg(PyExc_StopIteration, value);
    if (e == NULL) {
        return -1;
    }
    PyErr_SetObject(PyExc_StopIteration, e);
    Py_DECREF(e);
    return 0;
}

/*
 *   If StopIteration exception is set, fetches its 'value'
 *   attribute if any, otherwise sets pvalue to None.
 *
 *   Returns 0 if no exception or StopIteration is set.
 *   If any other exception is set, returns -1 and leaves
 *   pvalue unchanged.
 */

int
_PyGen_FetchStopIterationValue(PyObject **pvalue)
{
    PyObject *et, *ev, *tb;
    PyObject *value = NULL;

    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Fetch(&et, &ev, &tb);
        if (ev) {
            /* exception will usually be normalised already */
            if (PyObject_TypeCheck(ev, (PyTypeObject *) et)) {
                value = ((PyStopIterationObject *)ev)->value;
                Py_INCREF(value);
                Py_DECREF(ev);
            } else if (et == PyExc_StopIteration && !PyTuple_Check(ev)) {
                /* Avoid normalisation and take ev as value.
                 *
                 * Normalization is required if the value is a tuple, in
                 * that case the value of StopIteration would be set to
                 * the first element of the tuple.
                 *
                 * (See _PyErr_CreateException code for details.)
                 */
                value = ev;
            } else {
                /* normalisation required */
                PyErr_NormalizeException(&et, &ev, &tb);
                if (!PyObject_TypeCheck(ev, (PyTypeObject *)PyExc_StopIteration)) {
                    PyErr_Restore(et, ev, tb);
                    return -1;
                }
                value = ((PyStopIterationObject *)ev)->value;
                Py_INCREF(value);
                Py_DECREF(ev);
            }
        }
        Py_XDECREF(et);
        Py_XDECREF(tb);
    } else if (PyErr_Occurred()) {
        return -1;
    }
    if (value == NULL) {
        value = Py_None;
        Py_INCREF(value);
    }
    *pvalue = value;
    return 0;
}

void
_PyGen_Finalize(PyObject *self)
{
    PyGenObject *gen = (PyGenObject *)self;
    PyObject *error_type, *error_value, *error_traceback;

    if (PyCoro_CheckExact(gen) && gen->status == GEN_CREATED) {
        /* Save the current exception, if any. */
        PyErr_Fetch(&error_type, &error_value, &error_traceback);

        _PyErr_WarnUnawaitedCoroutine((PyObject *)gen);

        /* Restore the saved exception. */
        PyErr_Restore(error_type, error_value, error_traceback);
        return;
    }

    if (gen->status != GEN_SUSPENDED) {
        /* Generator isn't paused, so no need to close */
        return;
    }

    if (PyAsyncGen_CheckExact(self)) {
        PyAsyncGenObject *agen = (PyAsyncGenObject *)self;
        PyObject *finalizer = agen->finalizer;
        if (finalizer && !agen->closed) {
            /* Save the current exception, if any. */
            PyErr_Fetch(&error_type, &error_value, &error_traceback);

            PyObject *res = _PyObject_CallOneArg(finalizer, self);

            if (res == NULL) {
                PyErr_WriteUnraisable(self);
            } else {
                Py_DECREF(res);
            }
            /* Restore the saved exception. */
            PyErr_Restore(error_type, error_value, error_traceback);
            return;
        }
    }

    /* Save the current exception, if any. */
    PyErr_Fetch(&error_type, &error_value, &error_traceback);

    PyObject *res = gen_close(gen, NULL);
    if (res == NULL) {
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    else {
        Py_DECREF(res);
    }

    /* Restore the saved exception. */
    PyErr_Restore(error_type, error_value, error_traceback);
}

static PyObject *
gen_repr(PyGenObject *gen)
{
    return PyUnicode_FromFormat("<%s object %S at %p>",
                                Py_TYPE(gen)->tp_name, gen->qualname, gen);
}

static PyObject *
gen_get_running(PyGenObject *op, void *Py_UNUSED(ignored))
{
    return PyBool_FromLong(op->status == GEN_RUNNING);
}

static PyObject *
gen_get_frame(PyGenObject *op, void *Py_UNUSED(ignored))
{
    if (op->status == GEN_CLOSED) {
        return Py_None;
    }

    // get the bottom frame in the thread
    struct _PyThreadStack *ts = &op->base.thread;
    PyObject *frame = (PyObject *)vm_frame_at_offset(ts, FRAME_EXTRA);
    if (frame == NULL && !PyErr_Occurred()) {
        return Py_None;
    }
    Py_INCREF(frame);
    return frame;
}

static PyObject *
gen_get_state(PyGenObject *op, void *Py_UNUSED(ignored))
{
    static const char *states[] = {
        [GEN_CREATED]   = "GEN_CREATED",
        [GEN_SUSPENDED] = "GEN_SUSPENDED",
        [GEN_RUNNING]   = "GEN_RUNNING",
        [GEN_CLOSED]    = "GEN_CLOSED"
    };
    return PyUnicode_FromString(states[(int)op->status]);
}

static PyObject *
gen_get_name(PyGenObject *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->name);
    return op->name;
}

static int
gen_set_name(PyGenObject *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del gen.gi_name or to set it to anything
     * other than a string object. */
    if (value == NULL || !PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__name__ must be set to a string object");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(op->name, value);
    return 0;
}

static PyObject *
gen_get_qualname(PyGenObject *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->qualname);
    return op->qualname;
}

static int
gen_set_qualname(PyGenObject *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del gen.__qualname__ or to set it to anything
     * other than a string object. */
    if (value == NULL || !PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__qualname__ must be set to a string object");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(op->qualname, value);
    return 0;
}

/* ========= Asynchronous Generators ========= */


typedef enum {
    AWAITABLE_STATE_INIT,   /* new awaitable, has not yet been iterated */
    AWAITABLE_STATE_ITER,   /* being iterated */
    AWAITABLE_STATE_CLOSED, /* closed */
} AwaitableState;


typedef struct {
    PyObject_HEAD
    PyAsyncGenObject *ags_gen;

    /* Can be NULL, when in the __anext__() mode
       (equivalent of "asend(None)") */
    PyObject *ags_sendval;

    AwaitableState ags_state;
} PyAsyncGenASend;


typedef struct {
    PyObject_HEAD
    PyAsyncGenObject *agt_gen;

    /* Can be NULL, when in the "aclose()" mode
       (equivalent of "athrow(GeneratorExit)") */
    PyObject *agt_args;

    AwaitableState agt_state;
} PyAsyncGenAThrow;


typedef struct {
    PyObject_HEAD
    PyObject *agw_val;
} _PyAsyncGenWrappedValue;


#define _PyAsyncGenWrappedValue_CheckExact(o) \
                    (Py_TYPE(o) == &_PyAsyncGenWrappedValue_Type)

#define PyAsyncGenASend_CheckExact(o) \
                    (Py_TYPE(o) == &_PyAsyncGenASend_Type)


static int
async_gen_traverse(PyAsyncGenObject *gen, visitproc visit, void *arg)
{
    Py_VISIT(gen->finalizer);
    return gen_traverse((PyGenObject *)gen, visit, arg);
}


static int
async_gen_init_hooks(PyAsyncGenObject *o)
{
    PyThreadState *tstate;
    PyObject *finalizer;
    PyObject *firstiter;

    if (o->hooks_inited) {
        return 0;
    }

    o->hooks_inited = 1;

    tstate = _PyThreadState_GET();

    finalizer = tstate->async_gen_finalizer;
    if (finalizer) {
        Py_INCREF(finalizer);
        o->finalizer = finalizer;
    }

    firstiter = tstate->async_gen_firstiter;
    if (firstiter) {
        PyObject *res;

        Py_INCREF(firstiter);
        res = _PyObject_CallOneArg(firstiter, (PyObject *)o);
        Py_DECREF(firstiter);
        if (res == NULL) {
            return 1;
        }
        Py_DECREF(res);
    }

    return 0;
}


static PyObject *
async_gen_anext(PyAsyncGenObject *o)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_asend_new(o, NULL);
}


static PyObject *
async_gen_asend(PyAsyncGenObject *o, PyObject *arg)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_asend_new(o, arg);
}


static PyObject *
async_gen_aclose(PyAsyncGenObject *o, PyObject *arg)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_athrow_new(o, NULL);
}

static PyObject *
async_gen_athrow(PyAsyncGenObject *o, PyObject *args)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_athrow_new(o, args);
}

typedef struct {
    PyObject_HEAD
    PyCoroObject *coroutine;
} PyCoroWrapper;

static PyObject *
coro_await(PyCoroObject *coro)
{
    PyCoroWrapper *cw = PyObject_GC_New(PyCoroWrapper, &_PyCoroWrapper_Type);
    if (cw == NULL) {
        return NULL;
    }
    Py_INCREF(coro);
    cw->coroutine = coro;
    _PyObject_GC_TRACK(cw);
    return (PyObject *)cw;
}

static PyObject *
coro_get_cr_await(PyCoroObject *coro, void *Py_UNUSED(ignored))
{
    PyObject *yf = coro->base.yield_from;
    if (yf == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(yf);
    return yf;
}

static PyObject *
coro_get_state(PyGenObject *op, void *Py_UNUSED(ignored))
{
    static const char *states[] = {
        [GEN_CREATED]   = "CORO_CREATED",
        [GEN_SUSPENDED] = "CORO_SUSPENDED",
        [GEN_RUNNING]   = "CORO_RUNNING",
        [GEN_CLOSED]    = "CORO_CLOSED"
    };
    return PyUnicode_FromString(states[(int)op->status]);
}

static void
coro_wrapper_dealloc(PyCoroWrapper *cw)
{
    _PyObject_GC_UNTRACK((PyObject *)cw);
    Py_CLEAR(cw->coroutine);
    PyObject_GC_Del(cw);
}

static PyObject *
coro_wrapper_iternext(PyCoroWrapper *cw)
{
    return _PyGen_Send((PyGenObject *)cw->coroutine, Py_None);
}

PyDoc_STRVAR(coro_send_doc,
"send(arg) -> send 'arg' into coroutine,\n\
return next iterated value or raise StopIteration.");

static PyObject *
coro_wrapper_send(PyCoroWrapper *cw, PyObject *arg)
{
    return _PyGen_Send((PyGenObject *)cw->coroutine, arg);
}

PyDoc_STRVAR(coro_throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in coroutine,\n\
return next iterated value or raise StopIteration.");

static PyObject *
coro_wrapper_throw(PyCoroWrapper *cw, PyObject *args)
{
    return gen_throw((PyGenObject *)cw->coroutine, args);
}

PyDoc_STRVAR(coro_close_doc,
"close() -> raise GeneratorExit inside coroutine.");

static PyObject *
coro_wrapper_close(PyCoroWrapper *cw, PyObject *args)
{
    return gen_close((PyGenObject *)cw->coroutine, args);
}

static int
coro_wrapper_traverse(PyCoroWrapper *cw, visitproc visit, void *arg)
{
    Py_VISIT((PyObject *)cw->coroutine);
    return 0;
}


static PyObject *
async_gen_unwrap_value(PyAsyncGenObject *gen, PyObject *result)
{
    if (result == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetNone(PyExc_StopAsyncIteration);
        }

        if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)
            || PyErr_ExceptionMatches(PyExc_GeneratorExit)
        ) {
            gen->closed = 1;
        }

        gen->running_async = 0;
        return NULL;
    }

    if (_PyAsyncGenWrappedValue_CheckExact(result)) {
        /* async yield */
        _PyGen_SetStopIterationValue(((_PyAsyncGenWrappedValue*)result)->agw_val);
        Py_DECREF(result);
        gen->running_async = 0;
        return NULL;
    }

    return result;
}

/* ---------- Async Generator ASend Awaitable ------------ */


static void
async_gen_asend_dealloc(PyAsyncGenASend *o)
{
    _PyObject_GC_UNTRACK((PyObject *)o);
    Py_CLEAR(o->ags_gen);
    Py_CLEAR(o->ags_sendval);
    PyObject_GC_Del(o);
}

static int
async_gen_asend_traverse(PyAsyncGenASend *o, visitproc visit, void *arg)
{
    Py_VISIT(o->ags_gen);
    Py_VISIT(o->ags_sendval);
    return 0;
}


static PyObject *
async_gen_asend_send(PyAsyncGenASend *o, PyObject *arg)
{
    PyObject *result;

    if (o->ags_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited __anext__()/asend()");
        return NULL;
    }

    if (o->ags_state == AWAITABLE_STATE_INIT) {
        if (o->ags_gen->running_async) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "anext(): asynchronous generator is already running");
            return NULL;
        }

        if (arg == NULL || arg == Py_None) {
            arg = o->ags_sendval;
        }
        o->ags_state = AWAITABLE_STATE_ITER;
    }
    if (arg == NULL) {
        arg = Py_None;
    }

    o->ags_gen->running_async = 1;
    result = _PyGen_Send((PyGenObject *)o->ags_gen, arg);
    result = async_gen_unwrap_value(o->ags_gen, result);

    if (result == NULL) {
        o->ags_state = AWAITABLE_STATE_CLOSED;
    }

    return result;
}


static PyObject *
async_gen_asend_iternext(PyAsyncGenASend *o)
{
    return async_gen_asend_send(o, NULL);
}


static PyObject *
async_gen_asend_throw(PyAsyncGenASend *o, PyObject *args)
{
    PyObject *result;

    if (o->ags_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited __anext__()/asend()");
        return NULL;
    }

    result = gen_throw((PyGenObject*)o->ags_gen, args);
    result = async_gen_unwrap_value(o->ags_gen, result);

    if (result == NULL) {
        o->ags_state = AWAITABLE_STATE_CLOSED;
    }

    return result;
}


static PyObject *
async_gen_asend_close(PyAsyncGenASend *o, PyObject *args)
{
    o->ags_state = AWAITABLE_STATE_CLOSED;
    Py_RETURN_NONE;
}


/* ---------- Async Generator AThrow awaitable ------------ */


static void
async_gen_athrow_dealloc(PyAsyncGenAThrow *o)
{
    _PyObject_GC_UNTRACK((PyObject *)o);
    Py_CLEAR(o->agt_gen);
    Py_CLEAR(o->agt_args);
    PyObject_GC_Del(o);
}


static int
async_gen_athrow_traverse(PyAsyncGenAThrow *o, visitproc visit, void *arg)
{
    Py_VISIT(o->agt_gen);
    Py_VISIT(o->agt_args);
    return 0;
}


static PyObject *
async_gen_athrow_send(PyAsyncGenAThrow *o, PyObject *arg)
{
    PyGenObject *gen = (PyGenObject*)o->agt_gen;
    PyObject *retval;

    if (o->agt_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited aclose()/athrow()");
        return NULL;
    }

    if (gen->status == GEN_CLOSED) {
        o->agt_state = AWAITABLE_STATE_CLOSED;
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }

    if (o->agt_state == AWAITABLE_STATE_INIT) {
        if (o->agt_gen->running_async) {
            o->agt_state = AWAITABLE_STATE_CLOSED;
            if (o->agt_args == NULL) {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "aclose(): asynchronous generator is already running");
            }
            else {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "athrow(): asynchronous generator is already running");
            }
            return NULL;
        }

        if (o->agt_gen->closed) {
            o->agt_state = AWAITABLE_STATE_CLOSED;
            PyErr_SetNone(PyExc_StopAsyncIteration);
            return NULL;
        }

        if (arg != Py_None) {
            PyErr_SetString(PyExc_RuntimeError, NON_INIT_CORO_MSG);
            return NULL;
        }

        o->agt_state = AWAITABLE_STATE_ITER;
        o->agt_gen->running_async = 1;

        if (o->agt_args == NULL) {
            /* aclose() mode */
            o->agt_gen->closed = 1;

            retval = _gen_throw((PyGenObject *)gen,
                                0,  /* Do not close generator when
                                       PyExc_GeneratorExit is passed */
                                PyExc_GeneratorExit, NULL, NULL);

            if (retval && _PyAsyncGenWrappedValue_CheckExact(retval)) {
                Py_DECREF(retval);
                goto yield_close;
            }
        } else {
            PyObject *typ;
            PyObject *tb = NULL;
            PyObject *val = NULL;

            if (!PyArg_UnpackTuple(o->agt_args, "athrow", 1, 3,
                                   &typ, &val, &tb)) {
                return NULL;
            }

            retval = _gen_throw((PyGenObject *)gen,
                                0,  /* Do not close generator when
                                       PyExc_GeneratorExit is passed */
                                typ, val, tb);
            retval = async_gen_unwrap_value(o->agt_gen, retval);
        }
        if (retval == NULL) {
            goto check_error;
        }
        return retval;
    }

    assert(o->agt_state == AWAITABLE_STATE_ITER);

    retval = _PyGen_Send((PyGenObject *)gen, arg);
    if (o->agt_args) {
        return async_gen_unwrap_value(o->agt_gen, retval);
    } else {
        /* aclose() mode */
        if (retval) {
            if (_PyAsyncGenWrappedValue_CheckExact(retval)) {
                o->agt_gen->running_async = 0;
                Py_DECREF(retval);
                goto yield_close;
            }
            else {
                return retval;
            }
        }
        else {
            goto check_error;
        }
    }

yield_close:
    o->agt_gen->running_async = 0;
    o->agt_state = AWAITABLE_STATE_CLOSED;
    PyErr_SetString(
        PyExc_RuntimeError, ASYNC_GEN_IGNORED_EXIT_MSG);
    return NULL;

check_error:
    o->agt_gen->running_async = 0;
    o->agt_state = AWAITABLE_STATE_CLOSED;
    if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration) ||
            PyErr_ExceptionMatches(PyExc_GeneratorExit))
    {
        o->agt_state = AWAITABLE_STATE_CLOSED;
        if (o->agt_args == NULL) {
            /* when aclose() is called we don't want to propagate
               StopAsyncIteration or GeneratorExit; just raise
               StopIteration, signalling that this 'aclose()' await
               is done.
            */
            PyErr_Clear();
            PyErr_SetNone(PyExc_StopIteration);
        }
    }
    return NULL;
}


static PyObject *
async_gen_athrow_throw(PyAsyncGenAThrow *o, PyObject *args)
{
    PyObject *retval;

    if (o->agt_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited aclose()/athrow()");
        return NULL;
    }

    retval = gen_throw((PyGenObject*)o->agt_gen, args);
    if (o->agt_args) {
        return async_gen_unwrap_value(o->agt_gen, retval);
    } else {
        /* aclose() mode */
        if (retval && _PyAsyncGenWrappedValue_CheckExact(retval)) {
            o->agt_gen->running_async = 0;
            o->agt_state = AWAITABLE_STATE_CLOSED;
            Py_DECREF(retval);
            PyErr_SetString(PyExc_RuntimeError, ASYNC_GEN_IGNORED_EXIT_MSG);
            return NULL;
        }
        if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration) ||
            PyErr_ExceptionMatches(PyExc_GeneratorExit))
        {
            /* when aclose() is called we don't want to propagate
               StopAsyncIteration or GeneratorExit; just raise
               StopIteration, signalling that this 'aclose()' await
               is done.
            */
            PyErr_Clear();
            PyErr_SetNone(PyExc_StopIteration);
        }
        return retval;
    }
}


static PyObject *
async_gen_athrow_iternext(PyAsyncGenAThrow *o)
{
    return async_gen_athrow_send(o, Py_None);
}


static PyObject *
async_gen_athrow_close(PyAsyncGenAThrow *o, PyObject *args)
{
    o->agt_state = AWAITABLE_STATE_CLOSED;
    Py_RETURN_NONE;
}

static PyObject *
async_gen_asend_new(PyAsyncGenObject *gen, PyObject *sendval)
{
    PyAsyncGenASend *o;
    o = PyObject_GC_New(PyAsyncGenASend, &_PyAsyncGenASend2_Type);
    if (o == NULL) {
        return NULL;
    }

    Py_INCREF(gen);
    o->ags_gen = gen;

    Py_XINCREF(sendval);
    o->ags_sendval = sendval;

    o->ags_state = AWAITABLE_STATE_INIT;

    _PyObject_GC_TRACK((PyObject*)o);
    return (PyObject*)o;
}


/* ---------- Async Generator Value Wrapper ------------ */


static void
async_gen_wrapped_val_dealloc(_PyAsyncGenWrappedValue *o)
{
    _PyObject_GC_UNTRACK((PyObject *)o);
    Py_CLEAR(o->agw_val);
    PyObject_GC_Del(o);
}


static int
async_gen_wrapped_val_traverse(_PyAsyncGenWrappedValue *o,
                               visitproc visit, void *arg)
{
    Py_VISIT(o->agw_val);
    return 0;
}


PyTypeObject _PyAsyncGenWrappedValue_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "async_generator_wrapped_value",            /* tp_name */
    sizeof(_PyAsyncGenWrappedValue),            /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)async_gen_wrapped_val_dealloc,  /* tp_dealloc */
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
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)async_gen_wrapped_val_traverse, /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
};


PyObject *
_PyAsyncGenValueWrapperNew(PyObject *val)
{
    _PyAsyncGenWrappedValue *o;
    assert(val);

    o = PyObject_GC_New(_PyAsyncGenWrappedValue,
                        &_PyAsyncGenWrappedValue_Type);
    if (o == NULL) {
        return NULL;
    }
    o->agw_val = val;
    Py_INCREF(val);
    _PyObject_GC_TRACK((PyObject*)o);
    return (PyObject*)o;
}

static PyObject *
async_gen_athrow_new(PyAsyncGenObject *gen, PyObject *args)
{
    PyAsyncGenAThrow *o;
    o = PyObject_GC_New(PyAsyncGenAThrow, &_PyAsyncGenAThrow2_Type);
    if (o == NULL) {
        return NULL;
    }
    o->agt_gen = gen;
    o->agt_args = args;
    o->agt_state = AWAITABLE_STATE_INIT;
    Py_INCREF(gen);
    Py_XINCREF(args);
    _PyObject_GC_TRACK((PyObject*)o);
    return (PyObject*)o;
}


static PyGetSetDef gen_getsetlist[] = {
    {"gi_running",   (getter)gen_get_running, NULL, NULL },
    {"gi_frame",     (getter)gen_get_frame,   NULL, NULL },
    {"_genstate",    (getter)gen_get_state,   NULL, NULL },
    {"__name__",     (getter)gen_get_name,     (setter)gen_set_name,
     PyDoc_STR("name of the generator")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the generator")},
    {NULL} /* Sentinel */
};

static PyMemberDef gen_memberlist[] = {
    // {"gi_frame",     T_OBJECT, offsetof(PyGenObject, gi_frame),    READONLY},
    {"gi_code",      T_OBJECT, offsetof(PyGenObject, code),     READONLY},
    {"gi_yieldfrom", T_OBJECT, offsetof(PyGenObject, yield_from), 
     READONLY, PyDoc_STR("object being iterated by yield from, or None")},
    {NULL}      /* Sentinel */
};

static PyMethodDef gen_methods[] = {
    {"send",(PyCFunction)_PyGen_Send, METH_O, send_doc},
    {"throw",(PyCFunction)gen_throw, METH_VARARGS, throw_doc},
    {"close",(PyCFunction)gen_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject PyGen_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "generator",
    .tp_basicsize = sizeof(PyGenObject),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_repr = (reprfunc)gen_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)gen_traverse,
    .tp_weaklistoffset = offsetof(PyGenObject, weakreflist),
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)gen_iternext,
    .tp_methods = gen_methods,
    .tp_members = gen_memberlist,
    .tp_getset = gen_getsetlist,
    .tp_finalize = _PyGen_Finalize
};

static PyAsyncMethods coro_as_async = {
    .am_await = (unaryfunc)coro_await
};

static PyMethodDef coro_methods[] = {
    {"send",(PyCFunction)_PyGen_Send, METH_O, coro_send_doc},
    {"throw",(PyCFunction)gen_throw, METH_VARARGS, coro_throw_doc},
    {"close",(PyCFunction)gen_close, METH_NOARGS, coro_close_doc},
    {NULL, NULL}        /* Sentinel */
};

static PyMemberDef coro_memberlist[] = {
    {"cr_code",      T_OBJECT, offsetof(PyGenObject,  code),   READONLY},
    {"cr_origin",    T_OBJECT, offsetof(PyCoroObject, origin), READONLY},
    {NULL}      /* Sentinel */
};

static PyGetSetDef coro_getsetlist[] = {
    {"cr_running",   (getter)gen_get_running, NULL, NULL },
    {"cr_frame",     (getter)gen_get_frame,   NULL, NULL },
    {"_corostate",   (getter)coro_get_state, NULL, NULL },
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the coroutine")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the coroutine")},
    {"cr_await", (getter)coro_get_cr_await, NULL,
     PyDoc_STR("object being awaited on, or None")},
    {NULL} /* Sentinel */
};

PyTypeObject PyCoro_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "coroutine",
    .tp_basicsize = sizeof(PyCoroObject),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_as_async = &coro_as_async,
    .tp_repr = (reprfunc)gen_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)gen_traverse,
    .tp_weaklistoffset = offsetof(PyCoroObject, base.weakreflist),
    .tp_methods = coro_methods,
    .tp_members = coro_memberlist,
    .tp_getset = coro_getsetlist,
    .tp_finalize = _PyGen_Finalize
};

static PyMethodDef coro_wrapper_methods[] = {
    {"send",(PyCFunction)coro_wrapper_send, METH_O, coro_send_doc},
    {"throw",(PyCFunction)coro_wrapper_throw, METH_VARARGS, coro_throw_doc},
    {"close",(PyCFunction)coro_wrapper_close, METH_NOARGS, coro_close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject _PyCoroWrapper_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "coroutine_wrapper",
    .tp_basicsize = sizeof(PyCoroWrapper),
    .tp_dealloc = (destructor)coro_wrapper_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "A wrapper object implementing __await__ for coroutines.",
    .tp_traverse = (traverseproc)coro_wrapper_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)coro_wrapper_iternext,
    .tp_methods = coro_wrapper_methods
};

static PyMemberDef async_gen_memberlist[] = {
    {"ag_running", T_BOOL,   offsetof(PyAsyncGenObject, running_async),
        READONLY},
    {"ag_code",    T_OBJECT, offsetof(PyGenObject,  code),
        READONLY},
    {NULL}      /* Sentinel */
};

static PyGetSetDef async_gen_getsetlist[] = {
    {"ag_running",   (getter)gen_get_running, NULL, NULL },
    {"ag_frame",     (getter)gen_get_frame,   NULL, NULL },
    {"ag_await", (getter)coro_get_cr_await, NULL,
     PyDoc_STR("object being awaited on, or None")},
    {"_genstate",   (getter)gen_get_state, NULL, NULL },
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the async generator")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the async generator")},
    {NULL} /* Sentinel */
};

PyDoc_STRVAR(async_aclose_doc,
"aclose() -> raise GeneratorExit inside generator.");

PyDoc_STRVAR(async_asend_doc,
"asend(v) -> send 'v' in generator.");

PyDoc_STRVAR(async_athrow_doc,
"athrow(typ[,val[,tb]]) -> raise exception in generator.");

static PyMethodDef async_gen_methods[] = {
    {"asend", (PyCFunction)async_gen_asend, METH_O, async_asend_doc},
    {"athrow",(PyCFunction)async_gen_athrow, METH_VARARGS, async_athrow_doc},
    {"aclose", (PyCFunction)async_gen_aclose, METH_NOARGS, async_aclose_doc},
    {"__class_getitem__",    (PyCFunction)Py_GenericAlias,
    METH_O|METH_CLASS,       PyDoc_STR("See PEP 585")},
    {NULL, NULL}        /* Sentinel */
};

static PyAsyncMethods async_gen_as_async = {
    .am_aiter = PyObject_SelfIter,
    .am_anext = (unaryfunc)async_gen_anext
};

PyTypeObject PyAsyncGen_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "async_generator",
    .tp_basicsize = sizeof(PyAsyncGenObject),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_as_async = &async_gen_as_async,
    .tp_repr = (reprfunc)gen_repr,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)async_gen_traverse,
    .tp_weaklistoffset = offsetof(PyAsyncGenObject, base.weakreflist),
    .tp_methods = async_gen_methods,
    .tp_members = async_gen_memberlist,
    .tp_getset = async_gen_getsetlist,
    .tp_finalize = _PyGen_Finalize
};

static PyMethodDef async_gen_asend_methods[] = {
    {"send", (PyCFunction)async_gen_asend_send, METH_O, send_doc},
    {"throw", (PyCFunction)async_gen_asend_throw, METH_VARARGS, throw_doc},
    {"close", (PyCFunction)async_gen_asend_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};


static PyAsyncMethods async_gen_asend_as_async = {
    .am_await = PyObject_SelfIter
};


static PyTypeObject _PyAsyncGenASend2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "async_generator_asend",
    .tp_basicsize = sizeof(PyAsyncGenASend),
    .tp_dealloc = (destructor)async_gen_asend_dealloc,
    .tp_as_async = &async_gen_asend_as_async,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)async_gen_asend_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)async_gen_asend_iternext,
    .tp_methods = async_gen_asend_methods
};


static PyMethodDef async_gen_athrow_methods[] = {
    {"send", (PyCFunction)async_gen_athrow_send, METH_O, send_doc},
    {"throw", (PyCFunction)async_gen_athrow_throw, METH_VARARGS, throw_doc},
    {"close", (PyCFunction)async_gen_athrow_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};

static PyAsyncMethods async_gen_athrow_as_async = {
    .am_await = PyObject_SelfIter
};

static PyTypeObject _PyAsyncGenAThrow2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "async_generator_athrow",
    .tp_basicsize = sizeof(PyAsyncGenAThrow),
    .tp_dealloc = (destructor)async_gen_athrow_dealloc,
    .tp_as_async = &async_gen_athrow_as_async,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)async_gen_athrow_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)async_gen_athrow_iternext,
    .tp_methods = async_gen_athrow_methods
};
