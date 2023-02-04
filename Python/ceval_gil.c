
#include "Python.h"
#include "pycore_atomic.h"        // _Py_atomic_int
#include "pycore_ceval.h"         // _PyEval_SignalReceived()
#include "pycore_pyerrors.h"      // _PyErr_Fetch()
#include "pycore_pylifecycle.h"   // _PyErr_Print()
#include "pycore_initconfig.h"    // _PyStatus_OK()
#include "pycore_interp.h"        // _Py_RunGC()
#include "pycore_pymem.h"         // _PyMem_IsPtrFreed()
#include "pycore_refcnt.h"

/*
   Notes about the implementation:

   - The GIL is just a boolean variable (locked) whose access is protected
     by a mutex (gil_mutex), and whose changes are signalled by a condition
     variable (gil_cond). gil_mutex is taken for short periods of time,
     and therefore mostly uncontended.

   - In the GIL-holding thread, the main loop (PyEval_EvalFrameEx) must be
     able to release the GIL on demand by another thread. A volatile boolean
     variable (gil_drop_request) is used for that purpose, which is checked
     at every turn of the eval loop. That variable is set after a wait of
     `interval` microseconds on `gil_cond` has timed out.

      [Actually, another volatile boolean variable (eval_breaker) is used
       which ORs several conditions into one. Volatile booleans are
       sufficient as inter-thread signalling means since Python is run
       on cache-coherent architectures only.]

   - A thread wanting to take the GIL will first let pass a given amount of
     time (`interval` microseconds) before setting gil_drop_request. This
     encourages a defined switching period, but doesn't enforce it since
     opcodes can take an arbitrary time to execute.

     The `interval` value is available for the user to read and modify
     using the Python API `sys.{get,set}switchinterval()`.

   - When a thread releases the GIL and gil_drop_request is set, that thread
     ensures that another GIL-awaiting thread gets scheduled.
     It does so by waiting on a condition variable (switch_cond) until
     the value of last_holder is changed to something else than its
     own thread state pointer, indicating that another thread was able to
     take the GIL.

     This is meant to prohibit the latency-adverse behaviour on multi-core
     machines where one thread would speculatively release the GIL, but still
     run and end up being the first to re-acquire it, making the "timeslices"
     much longer than expected.
     (Note: this mechanism is enabled with FORCE_SWITCHING above)
*/

// GH-89279: Force inlining by using a macro.
#if defined(_MSC_VER) && SIZEOF_INT == 4
#define _Py_atomic_load_relaxed_int32(ATOMIC_VAL) (assert(sizeof((ATOMIC_VAL)->_value) == 4), *((volatile int*)&((ATOMIC_VAL)->_value)))
#else
#define _Py_atomic_load_relaxed_int32(ATOMIC_VAL) _Py_atomic_load_relaxed(ATOMIC_VAL)
#endif

#ifndef NDEBUG
/* Ensure that tstate is valid */
static int
is_tstate_valid(PyThreadState *tstate)
{
    assert(!_PyMem_IsPtrFreed(tstate));
    assert(!_PyMem_IsPtrFreed(tstate->interp));
    return 1;
}
#endif

/*
 * Implementation of the Global Interpreter Lock (GIL).
 */

#include <stdlib.h>
#include <errno.h>

#include "pycore_atomic.h"


#include "condvar.h"

#define MUTEX_INIT(mut) \
    if (PyMUTEX_INIT(&(mut))) { \
        Py_FatalError("PyMUTEX_INIT(" #mut ") failed"); };
#define MUTEX_FINI(mut) \
    if (PyMUTEX_FINI(&(mut))) { \
        Py_FatalError("PyMUTEX_FINI(" #mut ") failed"); };
#define MUTEX_LOCK(mut) \
    if (PyMUTEX_LOCK(&(mut))) { \
        Py_FatalError("PyMUTEX_LOCK(" #mut ") failed"); };
#define MUTEX_UNLOCK(mut) \
    if (PyMUTEX_UNLOCK(&(mut))) { \
        Py_FatalError("PyMUTEX_UNLOCK(" #mut ") failed"); };

#define COND_INIT(cond) \
    if (PyCOND_INIT(&(cond))) { \
        Py_FatalError("PyCOND_INIT(" #cond ") failed"); };
#define COND_FINI(cond) \
    if (PyCOND_FINI(&(cond))) { \
        Py_FatalError("PyCOND_FINI(" #cond ") failed"); };
#define COND_SIGNAL(cond) \
    if (PyCOND_SIGNAL(&(cond))) { \
        Py_FatalError("PyCOND_SIGNAL(" #cond ") failed"); };
#define COND_WAIT(cond, mut) \
    if (PyCOND_WAIT(&(cond), &(mut))) { \
        Py_FatalError("PyCOND_WAIT(" #cond ") failed"); };
#define COND_TIMED_WAIT(cond, mut, microseconds, timeout_result) \
    { \
        int r = PyCOND_TIMEDWAIT(&(cond), &(mut), (microseconds)); \
        if (r < 0) \
            Py_FatalError("PyCOND_WAIT(" #cond ") failed"); \
        if (r) /* 1 == timeout, 2 == impl. can't say, so assume timeout */ \
            timeout_result = 1; \
        else \
            timeout_result = 0; \
    } \


#define DEFAULT_INTERVAL 5000

static void _gil_initialize(struct _gil_runtime_state *gil)
{
    _Py_atomic_int uninitialized = {-1};
    gil->locked = uninitialized;
    gil->interval = DEFAULT_INTERVAL;
    gil->holder = NULL;
}

static int gil_created(struct _gil_runtime_state *gil)
{
    return (_Py_atomic_load_explicit(&gil->locked, _Py_memory_order_acquire) >= 0);
}

static void create_gil(struct _gil_runtime_state *gil)
{
    MUTEX_INIT(gil->mutex);
    if (gil->enabled) {
#ifdef FORCE_SWITCHING
        MUTEX_INIT(gil->switch_mutex);
#endif
        COND_INIT(gil->cond);
#ifdef FORCE_SWITCHING
        COND_INIT(gil->switch_cond);
#endif
        _Py_atomic_store_relaxed(&gil->last_holder, 0);
        gil->holder = NULL;
    }
    _Py_ANNOTATE_RWLOCK_CREATE(&gil->locked);
    _Py_atomic_store_explicit(&gil->locked, 0, _Py_memory_order_release);
}

static void destroy_gil(struct _gil_runtime_state *gil)
{
    /* some pthread-like implementations tie the mutex to the cond
     * and must have the cond destroyed first.
     */
    if (gil->enabled) {
        COND_FINI(gil->cond);
        MUTEX_FINI(gil->mutex);
#ifdef FORCE_SWITCHING
        COND_FINI(gil->switch_cond);
        MUTEX_FINI(gil->switch_mutex);
#endif
    }
    _Py_atomic_store_explicit(&gil->locked, -1,
                              _Py_memory_order_release);
    _Py_ANNOTATE_RWLOCK_DESTROY(&gil->locked);
}

#ifdef HAVE_FORK
static void recreate_gil(struct _gil_runtime_state *gil)
{
    _Py_ANNOTATE_RWLOCK_DESTROY(&gil->locked);
    /* XXX should we destroy the old OS resources here? */
    create_gil(gil);
}
#endif


/* Check if a Python thread must exit immediately, rather than taking the GIL
   if Py_Finalize() has been called.

   When this function is called by a daemon thread after Py_Finalize() has been
   called, the GIL does no longer exist.

   tstate must be non-NULL. */
static inline int
tstate_must_exit(PyThreadState *tstate)
{
    /* bpo-39877: Access _PyRuntime directly rather than using
       tstate->interp->runtime to support calls from Python daemon threads.
       After Py_Finalize() has been called, tstate can be a dangling pointer:
       point to PyThreadState freed memory. */
    PyThreadState *finalizing = _PyRuntimeState_GetFinalizing(&_PyRuntime);
    return (finalizing != NULL && finalizing != tstate);
}

static void
drop_gil(struct _ceval_runtime_state *ceval, struct _ceval_state *ceval2,
         PyThreadState *tstate)
{
    struct _gil_runtime_state *gil = &ceval->gil;
    if (!gil->enabled) {
        return;
    }
    if (!_Py_atomic_load_relaxed(&gil->locked)) {
        Py_FatalError("drop_gil: GIL is not locked");
    }

    /* tstate is allowed to be NULL (early interpreter init) */
    if (tstate != NULL) {
        /* Sub-interpreter support: threads might have been switched
           under our feet using PyThreadState_Swap(). Fix the GIL last
           holder variable so that our heuristics work. */
        _Py_atomic_store_relaxed(&gil->last_holder, (uintptr_t)tstate);
    }

    MUTEX_LOCK(gil->mutex);
    _Py_ANNOTATE_RWLOCK_RELEASED(&gil->locked, /*is_write=*/1);
    _Py_atomic_store_relaxed(&gil->locked, 0);
    gil->holder = NULL;
    COND_SIGNAL(gil->cond);
    MUTEX_UNLOCK(gil->mutex);

#ifdef FORCE_SWITCHING
    if (tstate != NULL && !tstate_must_exit(tstate) &&
        (_Py_atomic_load_uintptr_relaxed(&tstate->eval_breaker) & EVAL_DROP_GIL)) {
        MUTEX_LOCK(gil->switch_mutex);
        /* Not switched yet => wait */
        if (((PyThreadState*)_Py_atomic_load_relaxed(&gil->last_holder)) == tstate)
        {
            assert(is_tstate_valid(tstate));
            _PyThreadState_Unsignal(tstate, EVAL_DROP_GIL);
            /* NOTE: if COND_WAIT does not atomically start waiting when
               releasing the mutex, another thread can run through, take
               the GIL and drop it again, and reset the condition
               before we even had a chance to wait for it. */
            COND_WAIT(gil->switch_cond, gil->switch_mutex);
        }
        MUTEX_UNLOCK(gil->switch_mutex);
    }
#endif
}


/* Take the GIL.

   The function saves errno at entry and restores its value at exit.

   tstate must be non-NULL. */
static void
take_gil(PyThreadState *tstate)
{
    int err = errno;

    assert(tstate != NULL);

    if (tstate_must_exit(tstate)) {
        /* bpo-39877: If Py_Finalize() has been called and tstate is not the
           thread which called Py_Finalize(), exit immediately the thread.

           This code path can be reached by a daemon thread after Py_Finalize()
           completes. In this case, tstate is a dangling pointer: points to
           PyThreadState freed memory. */
        PyThread_exit_thread();
    }

    assert(is_tstate_valid(tstate));
    PyInterpreterState *interp = tstate->interp;
    struct _ceval_runtime_state *ceval = &interp->runtime->ceval;
    struct _ceval_state *ceval2 = &interp->ceval;
    struct _gil_runtime_state *gil = &ceval->gil;

    if (!gil->enabled) {
        return;
    }

    /* Check that _PyEval_InitThreads() was called to create the lock */
    assert(gil_created(gil));

    MUTEX_LOCK(gil->mutex);

    if (!_Py_atomic_load_relaxed(&gil->locked)) {
        goto _ready;
    }

    int drop_requested = 0;
    while (_Py_atomic_load_relaxed(&gil->locked)) {
        unsigned long saved_switchnum = gil->switch_number;

        unsigned long interval = (gil->interval >= 1 ? gil->interval : 1);
        int timed_out = 0;
        COND_TIMED_WAIT(gil->cond, gil->mutex, interval, timed_out);

        /* If we timed out and no switch occurred in the meantime, it is time
           to ask the GIL-holding thread to drop it. */
        if (timed_out &&
            _Py_atomic_load_relaxed(&gil->locked) &&
            gil->switch_number == saved_switchnum)
        {
            if (tstate_must_exit(tstate)) {
                // gh-96387: If the loop requested a drop request in a previous
                // iteration, reset the request. Otherwise, drop_gil() can
                // block forever waiting for the thread which exited. Drop
                // requests made by other threads are also reset: these threads
                // may have to request again a drop request (iterate one more
                // time).
                if (drop_requested) {
                    _PyThreadState_Unsignal(gil->holder, EVAL_DROP_GIL);
                }
                MUTEX_UNLOCK(gil->mutex);
                PyThread_exit_thread();
            }
            assert(is_tstate_valid(tstate));

            _PyThreadState_Signal(gil->holder, EVAL_DROP_GIL);
            drop_requested = 1;
        }
    }

_ready:
#ifdef FORCE_SWITCHING
    /* This mutex must be taken before modifying gil->last_holder:
       see drop_gil(). */
    MUTEX_LOCK(gil->switch_mutex);
#endif
    /* We now hold the GIL */
    _Py_atomic_store_relaxed(&gil->locked, 1);
    _Py_ANNOTATE_RWLOCK_ACQUIRED(&gil->locked, /*is_write=*/1);
    gil->holder = tstate;

    if (tstate != (PyThreadState*)_Py_atomic_load_relaxed(&gil->last_holder)) {
        _Py_atomic_store_relaxed(&gil->last_holder, (uintptr_t)tstate);
        ++gil->switch_number;
    }

#ifdef FORCE_SWITCHING
    COND_SIGNAL(gil->switch_cond);
    MUTEX_UNLOCK(gil->switch_mutex);
#endif

    if (tstate_must_exit(tstate)) {
        /* bpo-36475: If Py_Finalize() has been called and tstate is not
           the thread which called Py_Finalize(), exit immediately the
           thread.

           This code path can be reached by a daemon thread which was waiting
           in take_gil() while the main thread called
           wait_for_thread_shutdown() from Py_Finalize(). */
        MUTEX_UNLOCK(gil->mutex);
        drop_gil(ceval, ceval2, tstate);
        PyThread_exit_thread();
    }
    assert(is_tstate_valid(tstate));

    if (_PyThreadState_IsSignalled(tstate, EVAL_DROP_GIL)) {
        _PyThreadState_Unsignal(tstate, EVAL_DROP_GIL);
    }

    /* Don't access tstate if the thread must exit */
    if (_Py_atomic_load_ptr_relaxed(&tstate->async_exc) != NULL) {
        _PyThreadState_Signal(tstate, EVAL_ASYNC_EXC);
    }

    MUTEX_UNLOCK(gil->mutex);

    errno = err;
}

void _PyEval_SetSwitchInterval(unsigned long microseconds)
{
    struct _gil_runtime_state *gil = &_PyRuntime.ceval.gil;
    gil->interval = microseconds;
}

unsigned long _PyEval_GetSwitchInterval()
{
    struct _gil_runtime_state *gil = &_PyRuntime.ceval.gil;
    return gil->interval;
}


int
_PyEval_ThreadsInitialized(_PyRuntimeState *runtime)
{
    return gil_created(&runtime->ceval.gil);
}

int
PyEval_ThreadsInitialized(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    return _PyEval_ThreadsInitialized(runtime);
}

PyStatus
_PyEval_InitGIL(PyThreadState *tstate)
{
    if (!_Py_IsMainInterpreter(tstate->interp)) {
        /* Currently, the GIL is shared by all interpreters,
           and only the main interpreter is responsible to create
           and destroy it. */
        return _PyStatus_OK();
    }

    struct _gil_runtime_state *gil = &tstate->interp->runtime->ceval.gil;
    gil->enabled = !_PyRuntime.preconfig.disable_gil;
    assert(!gil_created(gil));

    PyThread_init_thread();
    create_gil(gil);

    take_gil(tstate);

    assert(gil_created(gil));
    return _PyStatus_OK();
}

void
_PyEval_FiniGIL(PyInterpreterState *interp)
{
    if (!_Py_IsMainInterpreter(interp)) {
        /* Currently, the GIL is shared by all interpreters,
           and only the main interpreter is responsible to create
           and destroy it. */
        return;
    }

    struct _gil_runtime_state *gil = &interp->runtime->ceval.gil;
    if (!gil_created(gil)) {
        /* First Py_InitializeFromConfig() call: the GIL doesn't exist
           yet: do nothing. */
        return;
    }

    destroy_gil(gil);
    assert(!gil_created(gil));
}

void
PyEval_InitThreads(void)
{
    /* Do nothing: kept for backward compatibility */
}

void
_PyEval_Fini(void)
{
#ifdef Py_STATS
    _Py_PrintSpecializationStats(1);
#endif
}
void
PyEval_AcquireLock(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    _Py_EnsureTstateNotNULL(tstate);

    take_gil(tstate);
}

void
PyEval_ReleaseLock(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    /* This function must succeed when the current thread state is NULL.
       We therefore avoid PyThreadState_Get() which dumps a fatal error
       in debug mode. */
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    struct _ceval_state *ceval2 = &tstate->interp->ceval;
    drop_gil(ceval, ceval2, tstate);
}

void
_PyEval_ReleaseLock(PyThreadState *tstate)
{
    if (!_PyRuntime.ceval.gil.enabled) {
        return;
    }
    struct _ceval_runtime_state *ceval = &tstate->interp->runtime->ceval;
    struct _ceval_state *ceval2 = &tstate->interp->ceval;
    drop_gil(ceval, ceval2, tstate);
}

void
PyEval_AcquireThread(PyThreadState *tstate)
{
    _Py_EnsureTstateNotNULL(tstate);

    take_gil(tstate);

    struct _gilstate_runtime_state *gilstate = &tstate->interp->runtime->gilstate;
    if (_PyThreadState_Swap(gilstate, tstate) != NULL) {
        Py_FatalError("non-NULL old thread state");
    }
}

void
PyEval_ReleaseThread(PyThreadState *tstate)
{
    assert(is_tstate_valid(tstate));

    _PyRuntimeState *runtime = tstate->interp->runtime;
    PyThreadState *new_tstate = _PyThreadState_Swap(&runtime->gilstate, NULL);
    if (new_tstate != tstate) {
        Py_FatalError("wrong thread state");
    }
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    struct _ceval_state *ceval2 = &tstate->interp->ceval;
    drop_gil(ceval, ceval2, tstate);
}

#ifdef HAVE_FORK
/* This function is called from PyOS_AfterFork_Child to destroy all threads
   which are not running in the child process, and clear internal locks
   which might be held by those threads. */
PyStatus
_PyEval_ReInitThreads(PyThreadState *tstate)
{
    _PyRuntimeState *runtime = tstate->interp->runtime;

    struct _gil_runtime_state *gil = &runtime->ceval.gil;
    if (!gil_created(gil)) {
        return _PyStatus_OK();
    }
    recreate_gil(gil);

    take_gil(tstate);

    struct _pending_calls *pending = &tstate->interp->ceval.pending;
    if (_PyThread_at_fork_reinit(&pending->lock) < 0) {
        return _PyStatus_ERR("Can't reinitialize pending calls lock");
    }
    return _PyStatus_OK();
}
#endif

PyThreadState *
PyEval_SaveThread(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = _PyThreadState_Swap(&runtime->gilstate, NULL);
    _Py_EnsureTstateNotNULL(tstate);

    struct _ceval_runtime_state *ceval = &runtime->ceval;
    struct _ceval_state *ceval2 = &tstate->interp->ceval;
    assert(gil_created(&ceval->gil));
    drop_gil(ceval, ceval2, tstate);
    return tstate;
}

void
PyEval_RestoreThread(PyThreadState *tstate)
{
    _Py_EnsureTstateNotNULL(tstate);

    take_gil(tstate);

    struct _gilstate_runtime_state *gilstate = &tstate->interp->runtime->gilstate;
    _PyThreadState_Swap(gilstate, tstate);
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
    /* Ensure that _PyEval_InitState() was called
       and that _PyEval_FiniState() is not called yet. */
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
        interp = _PyInterpreterState_Main();
    }
    return _PyEval_AddPendingCall(interp, func, arg);
}

static int
handle_signals(PyThreadState *tstate)
{
    assert(is_tstate_valid(tstate));
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

    PyInterpreterState *interp = tstate->interp;
    /* don't perform recursive pending calls */
    if (interp->ceval.pending.busy) {
        _PyThreadState_Signal(tstate, EVAL_PENDING_CALLS);
        return 0;
    }
    interp->ceval.pending.busy = 1;

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

    interp->ceval.pending.busy = 0;
    return res;

error:
    interp->ceval.pending.busy = 0;
    _PyThreadState_Signal(tstate, EVAL_PENDING_CALLS);
    return res;
}

void
_Py_FinishPendingCalls(PyThreadState *tstate)
{
    assert(PyGILState_Check());
    assert(is_tstate_valid(tstate));

    struct _pending_calls *pending = &tstate->interp->ceval.pending;

    if (!_Py_atomic_load_relaxed_int32(&(pending->calls_to_do))) {
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
    assert(is_tstate_valid(tstate));

    /* Python signal handler doesn't really queue a callback: it only signals
       that a signal was received, see _PyEval_SignalReceived(). */
    int res = handle_signals(tstate);
    if (res != 0) {
        return res;
    }

    res = make_pending_calls(tstate);
    if (res != 0) {
        return res;
    }

    return 0;
}

/* The interpreter's recursion limit */

void
_PyEval_InitRuntimeState(struct _ceval_runtime_state *ceval)
{
    _gil_initialize(&ceval->gil);
}

void
_PyEval_InitState(struct _ceval_state *ceval, PyThread_type_lock pending_lock)
{
    struct _pending_calls *pending = &ceval->pending;
    assert(pending->lock == NULL);

    pending->lock = pending_lock;
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

/* Handle signals, pending calls, GIL drop request
   and asynchronous exception */
int
_Py_HandlePending(PyThreadState *tstate)
{
    _PyRuntimeState * const runtime = &_PyRuntime;
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    struct _ceval_state *interp_ceval_state = &tstate->interp->ceval;

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

    /* GC scheduled to run */
    if ((b & EVAL_GC) != 0) {
        _PyThreadState_Unsignal(tstate, EVAL_GC);
        _Py_RunGC(tstate);
    }

    /* GIL drop request */
    if ((b & EVAL_DROP_GIL) != 0) {
        _PyThreadState_Unsignal(tstate, EVAL_DROP_GIL);
        /* Give another thread a chance */
        if (_PyThreadState_Swap(&runtime->gilstate, NULL) != tstate) {
            Py_FatalError("tstate mix-up");
        }
        drop_gil(ceval, interp_ceval_state, tstate);

        /* Other threads may run now */

        take_gil(tstate);

        if (_PyThreadState_Swap(&runtime->gilstate, tstate) != NULL) {
            Py_FatalError("orphan tstate");
        }
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

void
_PyEval_TakeGIL(PyThreadState *tstate)
{
    _PyThreadState_SET(tstate);
    take_gil(tstate);
}

void
_PyEval_DropGIL(PyThreadState *tstate)
{
    _PyThreadState_SET(NULL);
    _PyEval_ReleaseLock(tstate);
}
