/* Generator object implementation */

#include "Python.h"
#include "pycore_object.h"
#include "pycore_pystate.h"
#include "structmember.h"
#include "ceval2_meta.h" // remove ?
#include "genobject2.h"
#include "code2.h" // remove ?

static PyTypeObject *coro_types[] = {
    NULL,
    &PyGen2_Type,
    &PyCoro2_Type,
    &PyAsyncGen2_Type,
};

static PyGenObject2 *
gen_new_with_qualname(PyTypeObject *type, struct ThreadState *ts)
{
    int err;

    // TODO: zero initialize and simplify vm_init_thread_state ?
    PyGenObject2 *gen = PyObject_GC_New(PyGenObject2, type);
    if (UNLIKELY(gen == NULL)) {
        // FIXME: free thread state!
        return NULL;
    }

    err = vm_init_thread_state(ts, &gen->base.thread);
    if (UNLIKELY(err != 0)) {
        _Py_DECREF_TOTAL;
        PyObject_GC_Del(gen);
    }

    PyFunc *func = (PyFunc *)AS_OBJ(ts->regs[-1]);
    PyCodeObject2 *code = PyCode2_FromFunc(func);

    gen->weakreflist = NULL;
    // TODO: name should be from func, not code
    gen->name = code->co_name;
    gen->qualname = code->co_name; /// ???
    gen->return_value = NULL;
    gen->yield_from = NULL;
    gen->status = GEN_STARTED; // fixme enum or something
    Py_INCREF(gen->name);
    Py_INCREF(gen->qualname);

    _PyObject_GC_TRACK(gen);
    return gen;
}

PyGenObject2 *
PyGen2_NewWithSomething(struct ThreadState *ts, int typeidx)
{
    assert(typeidx >= 1 && typeidx <= 3);
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
    // Py_VISIT((PyObject *)gen->gi_frame);
    // Py_VISIT(gen->gi_code);
    // Py_VISIT(gen->gi_name);
    // Py_VISIT(gen->gi_qualname);
    // /* No need to visit cr_origin, because it's just tuples/str/int, so can't
    //    participate in a reference cycle. */
    // return exc_state_traverse(&gen->gi_exc_state, visit, arg);
    return 0;
}

static void
gen_dealloc(PyGenObject2 *gen)
{
    // printf("gen_dealloc\n");
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

    // fixme: delete and clear ts



    // if (PyAsyncGen_CheckExact(gen)) {
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
    if (PyAsyncGen_CheckExact(gen)) {
        return "async generator";
    }
    else if (PyCoro_CheckExact(gen)) {
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
gen_send_internal(PyGenObject2 *gen, Register acc)
{
    struct ThreadState *ts = &gen->base.thread;
    gen->status = GEN_RUNNING;
    PyObject *res = _PyEval_Fast(ts, acc.as_int64, ts->next_instr);
    if (LIKELY(res != NULL)) {
        assert(gen->status == GEN_YIELD);
        return res;
    }

    if (gen->status == GEN_FINISHED) {
        assert(gen->return_value != NULL);
        if (LIKELY(gen->return_value == Py_None)) {
            gen->return_value = NULL;
            PyErr_SetNone(PyAsyncGen2_CheckExact(gen)
                            ? PyExc_StopAsyncIteration
                            : PyExc_StopIteration);
            return NULL;
        }
        else {
            return _PyGen2_SetStopIterationValue(gen);
        }
    }

    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        _PyErr_FormatFromCause(
            PyExc_RuntimeError,
            "%s raised StopIteration",
            gen_typename(gen));
    }
    return NULL;
}

static PyObject *
gen_status_error(PyGenObject2 *gen, PyObject *arg)
{
    if (gen->status == GEN_RUNNING) {
        PyErr_Format(PyExc_ValueError, "%s already executing",
            gen_typename(gen));
        return NULL;
    }

    assert(gen->status == GEN_FINISHED || gen->status == GEN_ERROR);
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
    if (PyAsyncGen_CheckExact(gen)) {
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
    if (UNLIKELY(gen->status = GEN_STARTED && arg != Py_None)) {
        PyErr_Format(
            PyExc_TypeError,
            "can't send non-None value to a just-started %s",
            gen_typename(gen));
        return NULL;
    }

    Register acc = PACK_INCREF(arg);
    PyObject *res = gen_send_internal(gen, acc);
    return res;
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
_PyObject_YieldFrom(PyObject *awaitable, PyObject *arg)
{
    if (LIKELY(PyGen2_CheckExact(awaitable) || PyCoro2_CheckExact(awaitable))) {
        return _PyGen2_Send((PyGenObject2 *)awaitable, arg);
    }
    return _PyObject_YieldFrom_ex(awaitable, arg);
}

static int
gen_is_coroutine(PyObject *o)
{
    // TODO
    // if (PyGen2_CheckExact(o)) {
    //     PyGenObject2 *gen = (PyGenObject2 *)o;
    //     PyCodeObject2 *code = (PyCodeObject2 *)PyCode2_FromInstr(gen->
    //     if (code->co_flags & CO_ITERABLE_COROUTINE) {
    //         return 1;
    //     }
    // }
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
    const uint32_t *next_instr = vm_exception_unwind(ts, ts->next_instr);
    if (next_instr == NULL) {
        assert(gen->status == GEN_ERROR);
        return NULL;
    }
    ts->next_instr = next_instr;
    Register acc = {0};
    return gen_send_internal(gen, acc);
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
        assert(gen->status == GEN_YIELD);
        PyObject *ret;
        int err;
        if (PyErr_GivenExceptionMatches(typ, PyExc_GeneratorExit) &&
            close_on_genexit
        ) {
            /* Asynchronous generators *should not* be closed right away.
               We have to allow some awaits to work it through, hence the
               `close_on_genexit` parameter here.
            */
            // gen->gi_running = 1;
            err = gen_close_iter(yf);
            // gen->gi_running = 0;
            if (err < 0) {
                return gen_throw_current(gen);
            }
            goto throw_here;
        }
        if (PyGen2_CheckExact(yf) || PyCoro_CheckExact(yf)) {
            /* `yf` is a generator or a coroutine. */
            // gen->gi_running = 1;
            /* Close the generator that we are currently iterating with
               'yield from' or awaiting on with 'await'. */
            ret = _gen_throw((PyGenObject2 *)yf, close_on_genexit,
                             typ, val, tb);
            // gen->gi_running = 0;
        } else {
            /* `yf` is an iterator or a coroutine-like object. */
            PyObject *meth;
            if (_PyObject_LookupAttrId(yf, &PyId_throw, &meth) < 0) {
                return NULL;
            }
            if (meth == NULL) {
                goto throw_here;
            }
            // gen->gi_running = 1;
            ret = PyObject_CallFunctionObjArgs(meth, typ, val, tb, NULL);
            // gen->gi_running = 0;
            Py_DECREF(meth);
        }
        if (!ret) {
            PyObject *val;
            /* Pop subiterator from stack */
            // ret = *(--gen->gi_frame->f_stacktop);
            // assert(ret == yf);
            // Py_DECREF(ret);
            // /* Termination repetition of YIELD_FROM */
            // assert(gen->gi_frame->f_lasti >= 0);
            // gen->gi_frame->f_lasti += sizeof(_Py_CODEUNIT);
            if (_PyGen_FetchStopIterationValue(&val) == 0) {
                // send val to generator... weird TODO: is this even right???
                ret = gen_send_internal(gen, PACK(val, REFCOUNT_TAG));
            } else {
                ret = gen_throw_current(gen);
            }
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

    if (gen->yield_from) {
        char old_status = gen->status;
        gen->status = GEN_RUNNING; // why??
        err = gen_close_iter(gen->yield_from);
        gen->status = old_status;
    }
    if (err == 0)
        PyErr_SetNone(PyExc_GeneratorExit);
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
    Register acc = PACK(Py_None, NO_REFCOUNT_TAG);
    return gen_send_internal(gen, acc);
}

static void
_PyGen2_Finalize(PyObject *self)
{
    // printf("_PyGen2_Finalize NYI\n");
}

static PyObject *
gen_repr(PyGenObject2 *gen)
{
    return PyUnicode_FromFormat("<%s object %S at %p>",
                                Py_TYPE(gen)->tp_name, gen->qualname, gen);
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

static PyObject *
async_gen_anext(PyAsyncGenObject2 *o)
{
    PyErr_SetString(PyExc_SystemError, "async_gen_anext NYI");
    return NULL;
    // if (async_gen_init_hooks(o)) {
    //     return NULL;
    // }
    // return async_gen_asend_new(o, NULL);
}

static PyGetSetDef gen_getsetlist[] = {
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the generator")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the generator")},
    {NULL} /* Sentinel */
};

static PyMemberDef gen_memberlist[] = {
    // {"gi_frame",     T_OBJECT, offsetof(PyGenObject, gi_frame),    READONLY},
    // {"gi_running",   T_BOOL,   offsetof(PyGenObject, gi_running),  READONLY},
    // {"gi_code",      T_OBJECT, offsetof(PyGenObject, gi_code),     READONLY},
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
    .tp_getattro = PyObject_GenericGetAttr, // necessary ???
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

PyTypeObject PyCoro2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "coroutine",
    .tp_basicsize = sizeof(PyCoroObject2),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_repr = (reprfunc)gen_repr,
    .tp_getattro = PyObject_GenericGetAttr, // necessary ???
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)gen_traverse,
    .tp_weaklistoffset = offsetof(PyCoroObject2, base.weakreflist),
    .tp_methods = gen_methods,
    .tp_members = gen_memberlist,
    .tp_getset = gen_getsetlist,
    .tp_finalize = _PyGen2_Finalize
};

static PyAsyncMethods async_gen_as_async = {
    0,                                          /* am_await */
    PyObject_SelfIter,                          /* am_aiter */
    (unaryfunc)async_gen_anext                  /* am_anext */
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
    .tp_traverse = (traverseproc)gen_traverse,
    .tp_weaklistoffset = offsetof(PyAsyncGenObject2, base.weakreflist),
    .tp_methods = gen_methods,
    .tp_members = gen_memberlist,
    .tp_getset = gen_getsetlist,
    .tp_finalize = _PyGen2_Finalize
};
