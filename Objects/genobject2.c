/* Generator object implementation */

#include "Python.h"
#include "pycore_object.h"
#include "pycore_pystate.h"
#include "structmember.h"
#include "ceval2_meta.h" // remove ?
#include "genobject2.h"
#include "code2.h" // remove ?

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
    gen->name = code->co_name;
    gen->qualname = code->co_name; /// ???
    gen->return_value = NULL;
    gen->status = GEN_STARTED; // fixme enum or something
    Py_INCREF(gen->name);
    Py_INCREF(gen->qualname);

    _PyObject_GC_TRACK(gen);
    return gen;
}

PyGenObject2 *
PyGen2_NewWithSomething(struct ThreadState *ts)
{
    return gen_new_with_qualname(&PyGen2_Type, ts);
}

static PyObject *
gen_repr2(PyGenObject2 *gen)
{
    return PyUnicode_FromFormat("<generator object %S at %p>",
                                gen->qualname, gen);
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

static PyObject *
gen_send_ex(PyGenObject2 *gen, PyObject *arg)
{
    if (gen->status != 1) {
        // ????
        // already running or whatever
    }

    struct ThreadState *ts = &gen->base.thread;
    Register acc = PACK_INCREF(arg);
    PyObject *res = _PyEval_Fast(ts, acc.as_int64, ts->next_instr);
    return res;
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
    if (UNLIKELY(gen->status > GEN_YIELD)) {
        // ERRORROORORORO!
        abort();
    }
    gen->status = GEN_RUNNING;
    struct ThreadState *ts = &gen->base.thread;
    PyObject *res = _PyEval_Fast(ts, 0, ts->next_instr);
    if (LIKELY(res != NULL)) {
        assert(gen->status == GEN_YIELD);
        return res;
    }
    else if (gen->status == GEN_FINISHED) {
        assert(gen->return_value != NULL);
        if (LIKELY(gen->return_value == Py_None)) {
            gen->return_value = NULL;
            return NULL;
        }
        else {
            return _PyGen2_SetStopIterationValue(gen);
        }
    }
    else {
        // FIXME: add message about "generator raised StopIteration"
        // if we get a StopIteration
        return NULL;
    }
}

PyObject *
_PyGen_Send2(PyGenObject2 *gen, PyObject *arg)
{
    return gen_send_ex(gen, arg);
}

static void
_PyGen2_Finalize(PyObject *self)
{
    // printf("_PyGen2_Finalize NYI\n");
}

static PyGetSetDef gen_getsetlist[] = {
    // {"__name__", (getter)gen_get_name, (setter)gen_set_name,
    //  PyDoc_STR("name of the generator")},
    // {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
    //  PyDoc_STR("qualified name of the generator")},
    // {"gi_yieldfrom", (getter)gen_getyieldfrom, NULL,
    //  PyDoc_STR("object being iterated by yield from, or None")},
    {NULL} /* Sentinel */
};

static PyMemberDef gen_memberlist[] = {
    // {"gi_frame",     T_OBJECT, offsetof(PyGenObject, gi_frame),    READONLY},
    // {"gi_running",   T_BOOL,   offsetof(PyGenObject, gi_running),  READONLY},
    // {"gi_code",      T_OBJECT, offsetof(PyGenObject, gi_code),     READONLY},
    {NULL}      /* Sentinel */
};

static PyMethodDef gen_methods[] = {
    // {"send",(PyCFunction)_PyGen_Send, METH_O, send_doc},
    // {"throw",(PyCFunction)gen_throw, METH_VARARGS, throw_doc},
    // {"close",(PyCFunction)gen_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject PyGen2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "generator",
    .tp_basicsize = sizeof(PyGenObject2),
    .tp_dealloc = (destructor)gen_dealloc,
    .tp_repr = (reprfunc)gen_repr2,
    .tp_getattro = PyObject_GenericGetAttr, // necessary ???
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)gen_traverse,
    .tp_weaklistoffset = offsetof(PyGenObject, gi_weakreflist),
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)gen_iternext,
    .tp_methods = gen_methods,
    .tp_members = gen_memberlist,
    .tp_getset = gen_getsetlist,
    .tp_finalize = _PyGen2_Finalize
};
