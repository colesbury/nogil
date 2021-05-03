/* Generator object implementation */

#include "Python.h"
#include "pycore_object.h"
#include "pycore_pystate.h"
#include "structmember.h"
#include "ceval2_meta.h" // remove ?
#include "pycore_generator.h"
#include "code2.h" // remove ?
#include "opcode2.h"

static PyTypeObject *coro_types[] = {
    [CORO_HEADER_GENERATOR]       = &PyGen2_Type,
    [CORO_HEADER_COROUTINE]       = &PyCoro2_Type,
    [CORO_HEADER_ASYNC_GENERATOR] = &PyAsyncGen2_Type
};

static PyObject *async_gen_asend_new(PyAsyncGenObject2 *, PyObject *);
static PyObject *async_gen_athrow_new(PyAsyncGenObject2 *, PyObject *);

static PyTypeObject _PyAsyncGenASend2_Type;
static PyTypeObject _PyAsyncGenAThrow2_Type;

static char *NON_INIT_CORO_MSG = "can't send non-None value to a "
                                 "just-started coroutine";

static char *ASYNC_GEN_IGNORED_EXIT_MSG =
                                 "async generator ignored GeneratorExit";

static PyGenObject2 *
gen_new_with_qualname(PyTypeObject *type, struct ThreadState *ts)
{
    int err;

    PyGenObject2 *gen = (PyGenObject2 *)_PyObject_GC_Calloc(type->tp_basicsize);
    if (UNLIKELY(gen == NULL)) {
        return NULL;
    }
    PyObject_INIT(gen, type);

    err = vm_init_thread_state(ts, &gen->base.thread);
    if (UNLIKELY(err != 0)) {
        _Py_DECREF_TOTAL;
        PyObject_GC_Del(gen);
    }

    PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyCodeObject2 *code = PyCode2_FromFunc(func);

    gen->name = func->func_name;
    gen->qualname = func->func_qualname;
    gen->code = (PyObject *)code;
    gen->status = GEN_CREATED;
    Py_INCREF(gen->name);
    Py_INCREF(gen->qualname);
    Py_INCREF(gen->code); // FIXME: defer rc

    if (PyCoro2_CheckExact(gen) && ts->ts->coroutine_origin_tracking_depth > 0) {
        PyCoroObject2 *coro = (PyCoroObject2 *)gen;
        coro->origin = vm_compute_cr_origin(ts);
        if (coro->origin == NULL) {
            Py_DECREF(gen);
            return NULL;
        }
    }

    _PyObject_GC_TRACK(gen);
    return gen;
}

PyGenObject2 *
PyGen2_NewWithSomething(struct ThreadState *ts, int typeidx)
{
    assert(typeidx > 0 && (size_t)typeidx < (sizeof(coro_types)/sizeof(*coro_types)));
    PyTypeObject *type = coro_types[typeidx];
    return gen_new_with_qualname(type, ts);
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
_PyGen2_FetchStopIterationValue(void)
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
gen_traverse(PyGenObject2 *gen, visitproc visit, void *arg)
{
    Py_VISIT(gen->code);
    Py_VISIT(gen->name);
    Py_VISIT(gen->qualname);
    Py_VISIT(gen->return_value);
    Py_VISIT(gen->yield_from);

    return vm_traverse_stack(&gen->base.thread, visit, arg);
}

static void
gen_dealloc(PyGenObject2 *gen)
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
    Py_CLEAR(gen->code);



    // if (PyAsyncGen2_CheckExact(gen)) {
    //     /* We have to handle this case for asynchronous generators
    //        right here, because this code has to be between UNTRACK
    //        and GC_Del. */
    //     Py_CLEAR(((PyAsyncGenObject*)gen)->ag_finalizer);
    // }
    // if (gen->gi_frame != NULL) {
    //     gen->gi_frame->f_gen = NULL;
    //     Py_CLEAR(gen->gi_frame);
    // }
    // if (((PyCodeObject *)gen->gi_code)->co_flags & CO_COROUTINE) {
    //     Py_CLEAR(((PyCoroObject *)gen)->cr_origin);
    // }
    // PyObject *code = gen->gi_code;
    // gen->gi_code = NULL;
    // Py_DECREF_STACK(code);
    // if (gen->gi_retains_code) {
    //     Py_DECREF(code);
    // }
    // Py_CLEAR(gen->gi_name);
    // Py_CLEAR(gen->gi_qualname);
    // exc_state_clear(&gen->gi_exc_state);
    PyObject_GC_Del(gen);
}

static const char *
gen_typename(PyGenObject2 *gen)
{
    if (PyAsyncGen2_CheckExact(gen)) {
        return "async generator";
    }
    else if (PyCoro2_CheckExact(gen)) {
        return "coroutine";
    }
    else {
        assert(PyGen2_CheckExact(gen));
        return "generator";
    }
}

static PyObject *
_PyGen2_SetStopIterationValue(PyGenObject2 *gen);

static PyObject *
gen_send_internal(PyGenObject2 *gen, PyObject *opt_value)
{
    PyObject *res = PyEval2_EvalGen(gen, opt_value);

    if (LIKELY(res != NULL)) {
        assert(gen->status == GEN_SUSPENDED);
        return res;
    }

    if (LIKELY(gen->return_value == Py_None)) {
        gen->return_value = NULL;
        PyErr_SetNone(PyAsyncGen2_CheckExact(gen)
                        ? PyExc_StopAsyncIteration
                        : PyExc_StopIteration);
        return NULL;
    }
    else if (gen->return_value != NULL) {
        return _PyGen2_SetStopIterationValue(gen);
    }
    else {
        if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
            _PyErr_FormatFromCause(
                PyExc_RuntimeError,
                "%s raised StopIteration",
                gen_typename(gen));
        }
        else if (PyAsyncGen2_CheckExact(gen) &&
                 PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
            _PyErr_FormatFromCause(
                PyExc_RuntimeError,
                "%s raised StopAsyncIteration",
                gen_typename(gen));
        }
        return NULL;
    }
}

static PyObject *
gen_status_error(PyGenObject2 *gen, PyObject *arg)
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

    if (PyCoro2_CheckExact(gen)) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited coroutine");
    }
    else if (PyAsyncGen2_CheckExact(gen)) {
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
_PyGen2_Send(PyGenObject2 *gen, PyObject *arg)
{
    assert(arg != NULL);
    if (UNLIKELY(gen->status >= GEN_RUNNING)) {
        return gen_status_error(gen, arg);
    }
    if (UNLIKELY(gen->status == GEN_CREATED && arg != Py_None)) {
        PyErr_Format(
            PyExc_TypeError,
            "can't send non-None value to a just-started %s",
            gen_typename(gen));
        return NULL;
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
_PyGen_YieldFrom(PyGenObject2 *gen, PyObject *awaitable, PyObject *arg)
{
    Py_CLEAR(gen->yield_from);
    PyObject *res;
    if (LIKELY(PyGen2_CheckExact(awaitable) || PyCoro2_CheckExact(awaitable))) {
        res = _PyGen2_Send((PyGenObject2 *)awaitable, arg);
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
    if (PyGen2_CheckExact(o)) {
        PyCodeObject2 *code = (PyCodeObject2 *)((PyGenObject2 *)o)->code;
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
_PyCoro2_GetAwaitableIter(PyObject *o)
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
            if (PyCoro2_CheckExact(res) || gen_is_coroutine(res)) {
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

static PyObject *gen_close(PyGenObject2 *, PyObject *);

static int
gen_close_iter(PyObject *yf)
{
    PyObject *retval = NULL;
    _Py_IDENTIFIER(close);

    if (PyGen2_CheckExact(yf) || PyCoro_CheckExact(yf)) {
        retval = gen_close((PyGenObject2 *)yf, NULL);
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
gen_throw_current(PyGenObject2 *gen)
{
    struct ThreadState *ts = &gen->base.thread;
    if (gen->status == GEN_CLOSED) {
        if (PyCoro2_CheckExact(gen)) {
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
    gen->status = GEN_CLOSED;  // TODO: awkward, maybe GEN_RUNNING but set closed in vm_pop_frame?
    const uint8_t *pc = vm_exception_unwind(ts, false);
    if (pc == NULL) {
        assert(gen->status == GEN_CLOSED);
        return NULL;
    }
    gen->status = GEN_SUSPENDED;
    ts->pc = pc;
    return gen_send_internal(gen, NULL);
}


PyDoc_STRVAR(throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in generator,\n\
return next yielded value or raise StopIteration.");

static PyObject *
_gen_throw(PyGenObject2 *gen, int close_on_genexit,
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
        if (PyGen2_CheckExact(yf) || PyCoro_CheckExact(yf)) {
            /* `yf` is a generator or a coroutine. */
            /* Close the generator that we are currently iterating with
               'yield from' or awaiting on with 'await'. */
            char old_status = gen->status;
            gen->status = GEN_RUNNING;
            ret = _gen_throw((PyGenObject2 *)yf, close_on_genexit,
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
            struct ThreadState *ts = &gen->base.thread;
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
gen_throw(PyGenObject2 *gen, PyObject *args)
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
gen_close(PyGenObject2 *gen, PyObject *args)
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
_PyGen2_SetStopIterationValue(PyGenObject2 *gen)
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
gen_iternext(PyGenObject2 *gen)
{
    if (UNLIKELY(gen->status >= GEN_RUNNING)) {
        return gen_status_error(gen, NULL);
    }
    return gen_send_internal(gen, Py_None);
}

static void
_PyGen2_Finalize(PyObject *self)
{
    PyGenObject2 *gen = (PyGenObject2 *)self;
    PyObject *res = NULL;
    PyObject *error_type, *error_value, *error_traceback;

    if (PyCoro2_CheckExact(gen) && gen->status != GEN_CLOSED) {
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

    if (PyAsyncGen2_CheckExact(self)) {
        PyAsyncGenObject2 *agen = (PyAsyncGenObject2 *)self;
        PyObject *finalizer = agen->finalizer;
        if (finalizer && !agen->closed) {
            /* Save the current exception, if any. */
            PyErr_Fetch(&error_type, &error_value, &error_traceback);

            res = _PyObject_CallOneArg(finalizer, self);

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

    // FIXME(sgross): can there be an active exception here?
    /* Save the current exception, if any. */
    PyErr_Fetch(&error_type, &error_value, &error_traceback);

    // TODO: handle coroutine generators
    /* If `gen` is a coroutine, and if it was never awaited on,
       issue a RuntimeWarning. */
    // if (gen->gi_code != NULL &&
    //     ((PyCodeObject *)gen->gi_code)->co_flags & CO_COROUTINE &&
    //     gen->gi_frame->f_lasti == -1)
    // {
    //     _PyErr_WarnUnawaitedCoroutine((PyObject *)gen);
    // }
    // else {
        res = gen_close(gen, NULL);
    // }

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
gen_repr(PyGenObject2 *gen)
{
    return PyUnicode_FromFormat("<%s object %S at %p>",
                                Py_TYPE(gen)->tp_name, gen->qualname, gen);
}

static PyObject *
gen_get_running(PyGenObject2 *op, void *Py_UNUSED(ignored))
{
    return PyBool_FromLong(op->status == GEN_RUNNING);
}

static PyObject *
gen_get_state(PyGenObject2 *op, void *Py_UNUSED(ignored))
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
gen_get_name(PyGenObject2 *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->name);
    return op->name;
}

static int
gen_set_name(PyGenObject2 *op, PyObject *value, void *Py_UNUSED(ignored))
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
gen_get_qualname(PyGenObject2 *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->qualname);
    return op->qualname;
}

static int
gen_set_qualname(PyGenObject2 *op, PyObject *value, void *Py_UNUSED(ignored))
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
    PyAsyncGenObject2 *ags_gen;

    /* Can be NULL, when in the __anext__() mode
       (equivalent of "asend(None)") */
    PyObject *ags_sendval;

    AwaitableState ags_state;
} PyAsyncGenASend;


typedef struct {
    PyObject_HEAD
    PyAsyncGenObject2 *agt_gen;

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
async_gen_traverse(PyAsyncGenObject2 *gen, visitproc visit, void *arg)
{
    Py_VISIT(gen->finalizer);
    return gen_traverse((PyGenObject2 *)gen, visit, arg);
}


static int
async_gen_init_hooks(PyAsyncGenObject2 *o)
{
    // FIXME: implement async gen hooks
    return 0;
}


static PyObject *
async_gen_anext(PyAsyncGenObject2 *o)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_asend_new(o, NULL);
}


static PyObject *
async_gen_asend(PyAsyncGenObject2 *o, PyObject *arg)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_asend_new(o, arg);
}


static PyObject *
async_gen_aclose(PyAsyncGenObject2 *o, PyObject *arg)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_athrow_new(o, NULL);
}

static PyObject *
async_gen_athrow(PyAsyncGenObject2 *o, PyObject *args)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_athrow_new(o, args);
}

typedef struct {
    PyObject_HEAD
    PyCoroObject2 *coroutine;
} PyCoroWrapper;

static PyObject *
coro_await(PyCoroObject2 *coro)
{
    PyCoroWrapper *cw = PyObject_GC_New(PyCoroWrapper, &_PyCoroWrapper2_Type);
    if (cw == NULL) {
        return NULL;
    }
    Py_INCREF(coro);
    cw->coroutine = coro;
    _PyObject_GC_TRACK(cw);
    return (PyObject *)cw;
}

static PyObject *
coro_get_cr_await(PyCoroObject2 *coro, void *Py_UNUSED(ignored))
{
    PyObject *yf = coro->base.yield_from;
    if (yf == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(yf);
    return yf;
}

static PyObject *
coro_get_state(PyGenObject2 *op, void *Py_UNUSED(ignored))
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
    return _PyGen2_Send((PyGenObject2 *)cw->coroutine, Py_None);
}

PyDoc_STRVAR(coro_send_doc,
"send(arg) -> send 'arg' into coroutine,\n\
return next iterated value or raise StopIteration.");

static PyObject *
coro_wrapper_send(PyCoroWrapper *cw, PyObject *arg)
{
    return _PyGen2_Send((PyGenObject2 *)cw->coroutine, arg);
}

PyDoc_STRVAR(coro_throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in coroutine,\n\
return next iterated value or raise StopIteration.");

static PyObject *
coro_wrapper_throw(PyCoroWrapper *cw, PyObject *args)
{
    return gen_throw((PyGenObject2 *)cw->coroutine, args);
}

PyDoc_STRVAR(coro_close_doc,
"close() -> raise GeneratorExit inside coroutine.");

static PyObject *
coro_wrapper_close(PyCoroWrapper *cw, PyObject *args)
{
    return gen_close((PyGenObject2 *)cw->coroutine, args);
}

static int
coro_wrapper_traverse(PyCoroWrapper *cw, visitproc visit, void *arg)
{
    Py_VISIT((PyObject *)cw->coroutine);
    return 0;
}


static PyObject *
async_gen_unwrap_value(PyAsyncGenObject2 *gen, PyObject *result)
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
    result = _PyGen2_Send((PyGenObject2 *)o->ags_gen, arg);
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

    result = gen_throw((PyGenObject2*)o->ags_gen, args);
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
    PyGenObject2 *gen = (PyGenObject2*)o->agt_gen;
    PyObject *retval;

    if (o->agt_state == AWAITABLE_STATE_CLOSED || 
        gen->status == GEN_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited aclose()/athrow()");
        return NULL;
    }

    if (o->agt_state == AWAITABLE_STATE_INIT) {
        if (o->agt_gen->running_async) {
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

            retval = _gen_throw((PyGenObject2 *)gen,
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

            retval = _gen_throw((PyGenObject2 *)gen,
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

    retval = _PyGen2_Send((PyGenObject2 *)gen, arg);
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
    PyErr_SetString(
        PyExc_RuntimeError, ASYNC_GEN_IGNORED_EXIT_MSG);
    return NULL;

check_error:
    o->agt_gen->running_async = 0;
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

    retval = gen_throw((PyGenObject2*)o->agt_gen, args);
    if (o->agt_args) {
        return async_gen_unwrap_value(o->agt_gen, retval);
    } else {
        /* aclose() mode */
        if (retval && _PyAsyncGenWrappedValue_CheckExact(retval)) {
            o->agt_gen->running_async = 0;
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
async_gen_asend_new(PyAsyncGenObject2 *gen, PyObject *sendval)
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

static PyObject *
async_gen_athrow_new(PyAsyncGenObject2 *gen, PyObject *args)
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
    {"_genstate",   (getter)gen_get_state, NULL, NULL },
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the generator")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the generator")},
    {NULL} /* Sentinel */
};

static PyMemberDef gen_memberlist[] = {
    // {"gi_frame",     T_OBJECT, offsetof(PyGenObject, gi_frame),    READONLY},
    {"gi_code",      T_OBJECT, offsetof(PyGenObject2, code),     READONLY},
    {"gi_yieldfrom", T_OBJECT, offsetof(PyGenObject2, yield_from), 
     READONLY, PyDoc_STR("object being iterated by yield from, or None")},
    {NULL}      /* Sentinel */
};

static PyMethodDef gen_methods[] = {
    {"send",(PyCFunction)_PyGen2_Send, METH_O, send_doc},
    {"throw",(PyCFunction)gen_throw, METH_VARARGS, throw_doc},
    {"close",(PyCFunction)gen_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject PyGen2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "generator",
    .tp_basicsize = sizeof(PyGenObject2),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_repr = (reprfunc)gen_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)gen_traverse,
    .tp_weaklistoffset = offsetof(PyGenObject2, weakreflist),
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)gen_iternext,
    .tp_methods = gen_methods,
    .tp_members = gen_memberlist,
    .tp_getset = gen_getsetlist,
    .tp_finalize = _PyGen2_Finalize
};

static PyAsyncMethods coro_as_async = {
    .am_await = (unaryfunc)coro_await
};

static PyMethodDef coro_methods[] = {
    {"send",(PyCFunction)_PyGen2_Send, METH_O, coro_send_doc},
    {"throw",(PyCFunction)gen_throw, METH_VARARGS, coro_throw_doc},
    {"close",(PyCFunction)gen_close, METH_NOARGS, coro_close_doc},
    {NULL, NULL}        /* Sentinel */
};

static PyMemberDef coro_memberlist[] = {
    {"cr_code",      T_OBJECT, offsetof(PyGenObject2,  code),   READONLY},
    {"cr_origin",    T_OBJECT, offsetof(PyCoroObject2, origin), READONLY},
    {NULL}      /* Sentinel */
};

static PyGetSetDef coro_getsetlist[] = {
    {"cr_running",   (getter)gen_get_running, NULL, NULL },
    {"_corostate",   (getter)coro_get_state, NULL, NULL },
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the coroutine")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the coroutine")},
    {"cr_await", (getter)coro_get_cr_await, NULL,
     PyDoc_STR("object being awaited on, or None")},
    {NULL} /* Sentinel */
};

PyTypeObject PyCoro2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "coroutine",
    .tp_basicsize = sizeof(PyCoroObject2),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_as_async = &coro_as_async,
    .tp_repr = (reprfunc)gen_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)gen_traverse,
    .tp_weaklistoffset = offsetof(PyCoroObject2, base.weakreflist),
    .tp_methods = coro_methods,
    .tp_members = coro_memberlist,
    .tp_getset = coro_getsetlist,
    .tp_finalize = _PyGen2_Finalize
};

static PyMethodDef coro_wrapper_methods[] = {
    {"send",(PyCFunction)coro_wrapper_send, METH_O, coro_send_doc},
    {"throw",(PyCFunction)coro_wrapper_throw, METH_VARARGS, coro_throw_doc},
    {"close",(PyCFunction)coro_wrapper_close, METH_NOARGS, coro_close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject _PyCoroWrapper2_Type = {
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
    {"ag_running", T_BOOL,   offsetof(PyAsyncGenObject2, running_async),
        READONLY},
    {NULL}      /* Sentinel */
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
    {NULL, NULL}        /* Sentinel */
};

static PyAsyncMethods async_gen_as_async = {
    .am_aiter = PyObject_SelfIter,
    .am_anext = (unaryfunc)async_gen_anext
};

PyTypeObject PyAsyncGen2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "async_generator",
    .tp_basicsize = sizeof(PyAsyncGenObject2),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_as_async = &async_gen_as_async,
    .tp_repr = (reprfunc)gen_repr,
    .tp_getattro = PyObject_GenericGetAttr, // necessary ???
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)async_gen_traverse,
    .tp_weaklistoffset = offsetof(PyAsyncGenObject2, base.weakreflist),
    .tp_methods = async_gen_methods,
    .tp_members = async_gen_memberlist,
    .tp_getset = gen_getsetlist,
    .tp_finalize = _PyGen2_Finalize
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
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)async_gen_athrow_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)async_gen_athrow_iternext,
    .tp_methods = async_gen_athrow_methods
};
