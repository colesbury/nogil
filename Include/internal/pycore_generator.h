#ifndef Py_INTERNAL_GENERATOR_H
#define Py_INTERNAL_GENERATOR_H

#include "ceval2_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

enum PyGeneratorStatus {
    GEN_STARTED = 0,
    GEN_YIELD = 1,
    GEN_RUNNING = 2,
    GEN_ERROR = 3,
    GEN_FINISHED = 4
};

/* Generator object interface: move to genobject.h */
typedef struct PyGenObject2 {
    struct PyVirtualThread base;
    PyObject *weakreflist;
    PyObject *name;
    PyObject *qualname;
    PyObject *return_value;
    PyObject *yield_from;  /* object being iterated by yield from, or None */
    PyObject *code;
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

PyGenObject2 *
PyGen2_NewWithSomething(struct ThreadState *ts, int typeidx);

PyAPI_FUNC(PyObject *) _PyGen2_FetchStopIterationValue(void);
PyAPI_FUNC(PyObject *) _PyGen_YieldFrom(PyGenObject2 *gen, PyObject *awaitable, PyObject *arg);

PyObject *_PyCoro2_GetAwaitableIter(PyObject *o);

static inline void
PyGen2_SetPC(PyGenObject2 *gen, const uint8_t *pc)
{
    gen->base.thread.pc = pc;
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
#endif /* !Py_INTERNAL_GENERATOR_H */
