#ifndef Py_INTERNAL_PYSTATE_H
#define Py_INTERNAL_PYSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_llist.h"     /* llist_data */
#include "pycore_runtime.h"   /* PyRuntimeState */

enum _threadstatus {
    _Py_THREAD_DETACHED = 0,
    _Py_THREAD_ATTACHED = 1,
    _Py_THREAD_GC = 2
};

enum {
    EVAL_PLEASE_STOP = 1U << 0,
    EVAL_PENDING_SIGNALS = 1U << 1,
    EVAL_PENDING_CALLS = 1U << 2,
    EVAL_DROP_GIL = 1U << 3,
    EVAL_ASYNC_EXC = 1U << 4,
    EVAL_EXPLICIT_MERGE = 1U << 5,
    EVAL_GC = 1U << 6
};

#define for_each_thread(t)                                                      \
    for (PyInterpreterState *i = _PyRuntime.interpreters.head; i; i = i->next)  \
        for (t = i->threads.head; t; t = t->next)

/* Check if the current thread is the main thread.
   Use _Py_IsMainInterpreter() to check if it's the main interpreter. */
static inline int
_Py_IsMainThread(void)
{
    unsigned long thread = PyThread_get_thread_ident();
    return (thread == _PyRuntime.main_thread);
}


static inline PyInterpreterState *
_PyInterpreterState_Main(void)
{
    return _PyRuntime.interpreters.main;
}

static inline int
_Py_IsMainInterpreter(PyInterpreterState *interp)
{
    return (interp == _PyInterpreterState_Main());
}


static inline const PyConfig *
_Py_GetMainConfig(void)
{
    PyInterpreterState *interp = _PyInterpreterState_Main();
    if (interp == NULL) {
        return NULL;
    }
    return _PyInterpreterState_GetConfig(interp);
}


/* Only handle signals on the main thread of the main interpreter. */
static inline int
_Py_ThreadCanHandleSignals(PyInterpreterState *interp)
{
    return (_Py_IsMainThread() && _Py_IsMainInterpreter(interp));
}


/* Only execute pending calls on the main thread. */
static inline int
_Py_ThreadCanHandlePendingCalls(void)
{
    return _Py_IsMainThread();
}


/* Variable and macro for in-line access to current thread
   and interpreter state */
#if defined(__GNUC__) && !defined(Py_ENABLE_SHARED)
__attribute__((tls_model("local-exec")))
#endif
extern Py_DECL_THREAD PyThreadState *_Py_current_tstate;

/* Get the current Python thread state.

   Efficient macro reading directly the 'gilstate.tstate_current' atomic
   variable. The macro is unsafe: it does not check for error and it can
   return NULL.

   The caller must hold the GIL.

   See also PyThreadState_Get() and _PyThreadState_UncheckedGet(). */
static inline PyThreadState*
_PyThreadState_GET(void)
{
#if defined(Py_BUILD_CORE_MODULE)
    return _PyThreadState_UncheckedGet();
#else
    return _Py_current_tstate;
#endif
}

static inline void
_PyThreadState_SET(PyThreadState *tstate)
{
    _Py_current_tstate = tstate;
}

static inline PyThreadState*
_PyRuntimeState_GetThreadState(_PyRuntimeState *runtime)
{
    return _PyThreadState_GET();
}

static inline void
_Py_EnsureFuncTstateNotNULL(const char *func, PyThreadState *tstate)
{
    if (tstate == NULL) {
        _Py_FatalErrorFunc(func,
            "the function must be called with the GIL held, "
            "after Python initialization and before Python finalization, "
            "but the GIL is released (the current Python thread state is NULL)");
    }
}

// Call Py_FatalError() if tstate is NULL
#define _Py_EnsureTstateNotNULL(tstate) \
    _Py_EnsureFuncTstateNotNULL(__func__, (tstate))


/* Get the current interpreter state.

   The macro is unsafe: it does not check for error and it can return NULL.

   The caller must hold the GIL.

   See also _PyInterpreterState_Get()
   and _PyGILState_GetInterpreterStateUnsafe(). */
static inline PyInterpreterState* _PyInterpreterState_GET(void) {
    PyThreadState *tstate = _PyThreadState_GET();
#ifdef Py_DEBUG
    _Py_EnsureTstateNotNULL(tstate);
#endif
    return tstate->interp;
}


// PyThreadState functions

PyAPI_FUNC(void) _PyThreadState_SetCurrent(PyThreadState *tstate);
// We keep this around exclusively for stable ABI compatibility.
/* Other */

PyAPI_FUNC(void) _PyThreadState_Init(
    PyThreadState *tstate);
PyAPI_FUNC(void) _PyThreadState_DeleteExcept(
    _PyRuntimeState *runtime,
    PyThreadState *tstate);
PyAPI_FUNC(PyThreadState *) _PyThreadState_UnlinkExcept(
    _PyRuntimeState *runtime,
    PyThreadState *tstate,
    int already_dead);
PyAPI_FUNC(void) _PyThreadState_DeleteGarbage(PyThreadState *garbage);

extern void _PyThreadState_Exit(PyThreadState *tstate);

static inline void
_PyThreadState_Signal(PyThreadState *tstate, uintptr_t bit)
{
    _Py_atomic_or_uintptr(&tstate->eval_breaker, bit);
}

static inline void
_PyThreadState_Unsignal(PyThreadState *tstate, uintptr_t bit)
{
    _Py_atomic_and_uintptr(&tstate->eval_breaker, ~bit);
}

static inline int
_PyThreadState_IsSignalled(PyThreadState *tstate, uintptr_t bit)
{
    uintptr_t b = _Py_atomic_load_uintptr_relaxed(&tstate->eval_breaker);
    return (b & bit) != 0;
}

static inline void
_Py_ScheduleGC(PyThreadState *tstate)
{
    if (!_PyThreadState_IsSignalled(tstate, EVAL_GC)) {
        _PyThreadState_Signal(tstate, EVAL_GC);
    }
}


static inline void
_PyThreadState_UpdateTracingState(PyThreadState *tstate)
{
    bool use_tracing =
        (tstate->tracing == 0) &&
        (tstate->c_tracefunc != NULL || tstate->c_profilefunc != NULL);
    tstate->cframe->use_tracing = (use_tracing ? 255 : 0);
}


/* Other */
PyAPI_FUNC(void) _PyThreadState_GC_Park(PyThreadState *tstate);
PyAPI_FUNC(void) _PyThreadState_GC_Stop(PyThreadState *tstate);

PyAPI_FUNC(PyThreadState *) _PyThreadState_Swap(
    struct _gilstate_runtime_state *gilstate,
    PyThreadState *newts);

PyAPI_FUNC(PyStatus) _PyInterpreterState_Enable(_PyRuntimeState *runtime);

#ifdef HAVE_FORK
extern PyStatus _PyInterpreterState_DeleteExceptMain(_PyRuntimeState *runtime);
extern PyStatus _PyGILState_Reinit(_PyRuntimeState *runtime);
extern void _PySignal_AfterFork(void);
#endif


PyAPI_FUNC(int) _PyState_AddModule(
    PyThreadState *tstate,
    PyObject* module,
    PyModuleDef* def);


PyAPI_FUNC(int) _PyOS_InterruptOccurred(PyThreadState *tstate);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYSTATE_H */
