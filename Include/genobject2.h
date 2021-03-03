#ifndef Py_LIMITED_API
#ifndef Py_CEVAL2_GEN_H
#define Py_CEVAL2_GEN_H
#ifdef __cplusplus
extern "C" {
#endif

enum PyGeneratorStatus {
    GEN_STARTED = 0,
    GEN_YIELD = 1,
    GEN_RUNNING = 2,
    GEN_ERROR = 3,
    GEN_FINISHED = 4
};

/* Generator object interface: move to genobject.h */
typedef struct {
    struct PyVirtualThread base;
    PyObject *weakreflist;
    PyObject *name;
    PyObject *qualname;
    PyObject *return_value;
    PyObject *yield_from;  /* object being iterated by yield from, or None */
    char status;
} PyGenObject2;

typedef struct {
    PyGenObject2 base;
    PyObject *origin;
} PyCoroObject2;

/* Asynchronous Generators */

typedef struct {
    PyGenObject2 base;
    PyObject *finalizer;

    /* Flag is set to 1 when hooks set up by sys.set_asyncgen_hooks
       were called on the generator, to avoid calling them more
       than once. */
    int hooks_inited;

    /* Flag is set to 1 when aclose() is called for the first time, or
       when a StopAsyncIteration exception is raised. */
    int closed;

    int running_async;
} PyAsyncGenObject2;

PyAPI_DATA(PyTypeObject) PyGen2_Type;
PyAPI_DATA(PyTypeObject) PyCoro2_Type;
PyAPI_DATA(PyTypeObject) PyAsyncGen2_Type;

#define PyGen2_Check(op) PyObject_TypeCheck(op, &PyGen2_Type)
#define PyGen2_CheckExact(op) (Py_TYPE(op) == &PyGen2_Type)
#define PyCoro2_Check(op) PyObject_TypeCheck(op, &PyCoro2_Type)
#define PyCoro2_CheckExact(op) (Py_TYPE(op) == &PyCoro2_Type)

PyGenObject2 *
PyGen2_NewWithSomething(struct ThreadState *ts, int typeidx);

PyAPI_FUNC(PyObject *) _PyGen2_FetchStopIterationValue(void);
PyAPI_FUNC(PyObject *) _PyGen2_Send(PyGenObject2 *, PyObject *);

PyObject *_PyCoro2_GetAwaitableIter(PyObject *o);

static inline void
PyGen2_SetNextInstr(PyGenObject2 *gen, const uint32_t *next_instr)
{
    gen->base.thread.next_instr = next_instr;
}

static inline PyGenObject2 *
PyGen2_FromThread(struct ThreadState *ts)
{
    assert(ts->thread_type == THREAD_GENERATOR);
    return (PyGenObject2 *)((char*)ts - offsetof(PyGenObject2, base.thread));
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL2_GEN_H */
#endif /* Py_LIMITED_API */
