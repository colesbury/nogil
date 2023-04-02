
/* Thread and interpreter state structures and their interfaces */

#include "Python.h"
#include "pycore_ceval.h"
#include "pycore_initconfig.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pymem.h"         // _PyMem_SetDefaultAllocator()
#include "pycore_pystate.h"       // _PyThreadState_GET()
#include "pycore_qsbr.h"
#include "pycore_sysmodule.h"
#include "pycore_refcnt.h"

#include "ceval_meta.h"
#include "lock.h"
#include "parking_lot.h"
#include "mimalloc.h"
#include "mimalloc-internal.h"

/* --------------------------------------------------------------------------
CAUTION

Always use PyMem_RawMalloc() and PyMem_RawFree() directly in this file.  A
number of these functions are advertised as safe to call when the GIL isn't
held, and in a debug build Python redirects (e.g.) PyMem_NEW (etc) to Python's
debugging obmalloc functions.  Those aren't thread-safe (they rely on the GIL
to avoid the expense of doing their own locking).
-------------------------------------------------------------------------- */

#ifdef HAVE_DLOPEN
#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#if !HAVE_DECL_RTLD_LAZY
#define RTLD_LAZY 1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if PY_NUM_HEAPS != MI_NUM_HEAPS
#error "PY_NUM_HEAPS does not match MI_NUM_HEAPS"
#endif

#define _PyRuntimeGILState_GetThreadState(gilstate) _PyThreadState_GET()
#define _PyRuntimeGILState_SetThreadState(gilstate, value) _PyThreadState_SET(value)

/* Forward declarations */
static PyThreadState *_PyGILState_GetThisThreadState(struct _gilstate_runtime_state *gilstate);
static void _PyThreadState_Delete(PyThreadState *tstate, int check_current);

Py_DECL_THREAD PyThreadState *_Py_current_tstate;


static PyStatus
_PyRuntimeState_Init_impl(_PyRuntimeState *runtime)
{
    /* We preserve the hook across init, because there is
       currently no public API to set it between runtime
       initialization and interpreter initialization. */
    void *open_code_hook = runtime->open_code_hook;
    void *open_code_userdata = runtime->open_code_userdata;
    _Py_AuditHookEntry *audit_hook_head = runtime->audit_hook_head;

    memset(runtime, 0, sizeof(*runtime));

    runtime->open_code_hook = open_code_hook;
    runtime->open_code_userdata = open_code_userdata;
    runtime->audit_hook_head = audit_hook_head;

    _PyGC_ResetHeap();
    _PyEval_InitRuntimeState(&runtime->ceval);

    PyPreConfig_InitPythonConfig(&runtime->preconfig);

    runtime->gilstate.check_enabled = 1;

    /* A TSS key must be initialized with Py_tss_NEEDS_INIT
       in accordance with the specification. */
    Py_tss_t initial = Py_tss_NEEDS_INIT;
    runtime->gilstate.autoTSSkey = initial;

    runtime->interpreters.mutex = PyThread_allocate_lock();
    if (runtime->interpreters.mutex == NULL) {
        return _PyStatus_ERR("Can't initialize threads for interpreter");
    }
    runtime->interpreters.next_id = -1;

    runtime->xidregistry.mutex = PyThread_allocate_lock();
    if (runtime->xidregistry.mutex == NULL) {
        return _PyStatus_ERR("Can't initialize threads for cross-interpreter data registry");
    }

    // Set it to the ID of the main thread of the main interpreter.
    runtime->main_thread = PyThread_get_thread_ident();

    return _PyStatus_OK();
}

PyStatus
_PyRuntimeState_Init(_PyRuntimeState *runtime)
{
    /* Force default allocator, since _PyRuntimeState_Fini() must
       use the same allocator than this function. */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    PyStatus status = _PyRuntimeState_Init_impl(runtime);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
    return status;
}

void
_PyRuntimeState_Fini(_PyRuntimeState *runtime)
{
    /* Force the allocator used by _PyRuntimeState_Init(). */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (runtime->interpreters.mutex != NULL) {
        PyThread_free_lock(runtime->interpreters.mutex);
        runtime->interpreters.mutex = NULL;
    }

    if (runtime->xidregistry.mutex != NULL) {
        PyThread_free_lock(runtime->xidregistry.mutex);
        runtime->xidregistry.mutex = NULL;
    }

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    _PyTypeId_Finalize(&_PyRuntime.typeids);
}

#ifdef HAVE_FORK
/* This function is called from PyOS_AfterFork_Child to ensure that
 * newly created child processes do not share locks with the parent.
 */

void
_PyRuntimeState_ReInitThreads(_PyRuntimeState *runtime)
{
    // This was initially set in _PyRuntimeState_Init().
    runtime->main_thread = PyThread_get_thread_ident();

    /* Force default allocator, since _PyRuntimeState_Fini() must
       use the same allocator than this function. */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    // re-init stop-the-world mutex as LOCKED but with no waiters
    runtime->stoptheworld_mutex.v = LOCKED;

    int interp_mutex = _PyThread_at_fork_reinit(&runtime->interpreters.mutex);
    int xidregistry_mutex = _PyThread_at_fork_reinit(&runtime->xidregistry.mutex);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    /* bpo-42540: id_mutex is freed by _PyInterpreterState_Delete, which does
     * not force the default allocator. */
    int main_interp_id_mutex = _PyThread_at_fork_reinit(&runtime->interpreters.main->id_mutex);

    if (interp_mutex < 0) {
        Py_FatalError("Can't initialize lock for runtime interpreters");
    }

    if (main_interp_id_mutex < 0) {
        Py_FatalError("Can't initialize ID lock for main interpreter");
    }

    if (xidregistry_mutex < 0) {
        Py_FatalError("Can't initialize lock for cross-interpreter data registry");
    }
}
#endif

#define HEAD_LOCK(runtime) \
    PyThread_acquire_lock((runtime)->interpreters.mutex, WAIT_LOCK)
#define HEAD_UNLOCK(runtime) \
    PyThread_release_lock((runtime)->interpreters.mutex)

/* Forward declaration */
static void _PyGILState_NoteThreadState(
    struct _gilstate_runtime_state *gilstate, PyThreadState* tstate);

int
_PyThreadState_GetStatus(PyThreadState *tstate)
{
    return _Py_atomic_load_int32_relaxed(&tstate->status);
}

static int
_PyThreadState_Attach(PyThreadState *tstate)
{
    if (_Py_atomic_compare_exchange_int32(
            &tstate->status,
            _Py_THREAD_DETACHED,
            _Py_THREAD_ATTACHED)) {
        // online for QSBR too
        _Py_qsbr_online(((PyThreadStateImpl *)tstate)->qsbr);

        // resume previous critical section
        if (tstate->critical_section != 0) {
            _Py_critical_section_resume(tstate);
        }
        return 1;
    }
    return 0;
}

static void
_PyThreadState_Detach(PyThreadState *tstate)
{
    _Py_qsbr_offline(((PyThreadStateImpl *)tstate)->qsbr);

    if (tstate->critical_section != 0) {
        _Py_critical_section_end_all(tstate);
    }

    _Py_atomic_store_int32(&tstate->status, _Py_THREAD_DETACHED);
}

void
_PyThreadState_GC_Stop(PyThreadState *tstate)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    struct _gc_runtime_state *gc = &tstate->interp->gc;
    int gc_pending;

    assert(tstate->status == _Py_THREAD_ATTACHED);

    HEAD_LOCK(runtime);
    gc_pending = (gc->gc_thread_countdown > 0);
    HEAD_UNLOCK(runtime);

    if (!gc_pending) {
        // We might be processing a stale EVAL_PLEASE_STOP, in which
        // case there is nothing to do. This can happen if a thread
        // asks us to stop for a previous GC at the same time we detach.
        return;
    }

    _Py_qsbr_offline(((PyThreadStateImpl *)tstate)->qsbr);

    if (tstate->critical_section != 0) {
        _Py_critical_section_end_all(tstate);
    }

    _Py_atomic_store_int32(&tstate->status, _Py_THREAD_GC);

    HEAD_LOCK(runtime);
    // Decrease gc_thread_countdown. If we're the last thread to stop,
    // notify the thread that requested the stop-the-world.
    gc->gc_thread_countdown--;
    assert(gc->gc_thread_countdown >= 0);
    if (gc->gc_thread_countdown == 0) {
        _PyRawEvent_Notify(&gc->gc_stop_event);
    }
    HEAD_UNLOCK(runtime);

    _PyThreadState_GC_Park(tstate);
}

void
_PyThreadState_GC_Park(PyThreadState *tstate)
{
    assert(!tstate->cant_stop_wont_stop);

    int count = 0;
    for (;;) {
        // Wait until we're switched out of GC to DETACHED.
        _PyParkingLot_ParkInt32(&tstate->status, _Py_THREAD_GC);

        // Once we're back in DETACHED we can re-attach
        if (_PyThreadState_Attach(tstate)) {
            // We gucci
            return;
        }

        count++;
    }
}

static void
assert_all_stopped(_PyRuntimeState *runtime, PyThreadState *this_tstate)
{
    // Check that all threads (other than this thread) are in _Py_THREAD_GC
    // state.
#ifdef Py_DEBUG
    PyThreadState *t;
    HEAD_LOCK(runtime);
    for_each_thread(t) {
        if (t == this_tstate) {
            assert(_PyThreadState_GetStatus(t) == _Py_THREAD_ATTACHED);
        }
        else {
            assert(_PyThreadState_GetStatus(t) == _Py_THREAD_GC);
        }
    }
    HEAD_UNLOCK(runtime);
#endif
}

static int
park_detached_threads(_PyRuntimeState *runtime, PyThreadState *this_tstate)
{
    int num_parked = 0;

    PyThreadState *t;
    for_each_thread(t) {
        int status = _PyThreadState_GetStatus(t);

        if (status == _Py_THREAD_DETACHED &&
            !_Py_atomic_load_int32_relaxed(&t->cant_stop_wont_stop) &&
            _Py_atomic_compare_exchange_int32(
                &t->status,
                _Py_THREAD_DETACHED,
                _Py_THREAD_GC)) {

            num_parked++;
        }
        else if (status == _Py_THREAD_ATTACHED && t != this_tstate) {
            _PyThreadState_Signal(t, EVAL_PLEASE_STOP);
        }
    }

    return num_parked;
}

void
_PyRuntimeState_StopTheWorld(_PyRuntimeState *runtime)
{
    PyThreadState *this_tstate = PyThreadState_Get();
    struct _gc_runtime_state *gc = &this_tstate->interp->gc;

    assert(_PyMutex_is_locked(&runtime->stoptheworld_mutex));

    HEAD_LOCK(runtime);
    if (runtime->stop_the_world) {
        // TODO(sgross): document how the re-entrant stop the world can happen.
        assert(_PyRuntimeState_GetFinalizing(runtime) == this_tstate);
        runtime->stop_the_world++;
        HEAD_UNLOCK(runtime);
        return;
    }

    runtime->stop_the_world = 1;
    gc->gc_thread_countdown = 0;

    PyThreadState *t;
    for_each_thread(t) {
#ifdef Py_DEBUG
        int s = _PyThreadState_GetStatus(t);
        assert(s == _Py_THREAD_ATTACHED || s == _Py_THREAD_DETACHED);
#endif
        gc->gc_thread_countdown++;
    }

    // Don't wait our own thread
    assert(this_tstate->status == _Py_THREAD_ATTACHED);
    gc->gc_thread_countdown--;

    // Switch threads that are detached to the GC stopped state
    int parked = park_detached_threads(runtime, this_tstate);
    gc->gc_thread_countdown -= parked;

    assert(gc->gc_thread_countdown >= 0);
    int stopped_all_threads = gc->gc_thread_countdown == 0;
    HEAD_UNLOCK(runtime);

    // We're done if we successfully transitioned all other threads to
    // _Py_THREAD_GC (or if we are the only thread).
    while (!stopped_all_threads) {
        // Otherwise we need to wait until the remaining threads stop themselves.
        int64_t wait_ns = 1000*1000;
        if (_PyRawEvent_TimedWait(&gc->gc_stop_event, wait_ns)) {
            assert(gc->gc_thread_countdown == 0);
            assert_all_stopped(runtime, this_tstate);
            _PyRawEvent_Reset(&gc->gc_stop_event);
            break;
        }

        // Ask nicely: park_detached_threads sets eval_breaker to trigger this soon.
        HEAD_LOCK(runtime);
        int num_detached = park_detached_threads(runtime, this_tstate);
        gc->gc_thread_countdown -= num_detached;
        assert(gc->gc_thread_countdown >= 0);
        stopped_all_threads = (num_detached > 0) && (gc->gc_thread_countdown == 0);
        HEAD_UNLOCK(runtime);
    }
}

void
_PyRuntimeState_StartTheWorld(_PyRuntimeState *runtime)
{
    assert(_PyMutex_is_locked(&runtime->stoptheworld_mutex));

    HEAD_LOCK(runtime);
    if (runtime->stop_the_world > 1) {
        assert(_PyRuntimeState_GetFinalizing(runtime) == PyThreadState_GET());
        runtime->stop_the_world--;
        HEAD_UNLOCK(runtime);
        return;
    }

    runtime->stop_the_world = 0;
    PyThreadState *t;
    for_each_thread(t) {
        int status = _PyThreadState_GetStatus(t);
        if (status == _Py_THREAD_GC &&
            _Py_atomic_compare_exchange_int32(
                &t->status,
                _Py_THREAD_GC,
                _Py_THREAD_DETACHED)) {

            _PyParkingLot_UnparkAll(&t->status);
        }
    }
    HEAD_UNLOCK(runtime);
}

void
_PyThreadState_Signal(PyThreadState *tstate, uintptr_t bit)
{
    // TODO: use atomic bitwise instructions when available
    uintptr_t *eval_breaker = &tstate->eval_breaker;
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr_relaxed(eval_breaker);
        uintptr_t newv = v | bit;
        if (_Py_atomic_compare_exchange_uintptr(eval_breaker, v, newv)) {
            break;
        }
    }
}

void
_PyThreadState_Unsignal(PyThreadState *tstate, uintptr_t bit)
{
    uintptr_t *eval_breaker = &tstate->eval_breaker;
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr_relaxed(eval_breaker);
        uintptr_t newv = v & ~bit;
        if (_Py_atomic_compare_exchange_uintptr(eval_breaker, v, newv)) {
            break;
        }
    }
}

intptr_t
_PyRuntimeState_GetRefTotal(_PyRuntimeState *runtime)
{
    Py_ssize_t total = runtime->ref_total;

    HEAD_LOCK(runtime);
    PyInterpreterState *interp = runtime->interpreters.head;
    for (PyThreadState *p = interp->tstate_head; p != NULL; p = p->next) {
        total += p->ref_total;
    }
    HEAD_UNLOCK(runtime);

    return total;
}

PyStatus
_PyInterpreterState_Enable(_PyRuntimeState *runtime)
{
    struct pyinterpreters *interpreters = &runtime->interpreters;
    interpreters->next_id = 0;

    /* Py_Finalize() calls _PyRuntimeState_Fini() which clears the mutex.
       Create a new mutex if needed. */
    if (interpreters->mutex == NULL) {
        /* Force default allocator, since _PyRuntimeState_Fini() must
           use the same allocator than this function. */
        PyMemAllocatorEx old_alloc;
        _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

        interpreters->mutex = PyThread_allocate_lock();

        PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

        if (interpreters->mutex == NULL) {
            return _PyStatus_ERR("Can't initialize threads for interpreter");
        }
    }

    return _PyStatus_OK();
}

PyInterpreterState *
PyInterpreterState_New(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    /* tstate is NULL when Py_InitializeFromConfig() calls
       PyInterpreterState_New() to create the main interpreter. */
    if (_PySys_Audit(tstate, "cpython.PyInterpreterState_New", NULL) < 0) {
        return NULL;
    }

    PyInterpreterState *interp = PyMem_RawCalloc(1, sizeof(PyInterpreterState));
    if (interp == NULL) {
        return NULL;
    }

    interp->id_refcount = -1;

    /* Don't get runtime from tstate since tstate can be NULL */
    _PyRuntimeState *runtime = &_PyRuntime;
    interp->runtime = runtime;

    if (_PyEval_InitState(&interp->ceval) < 0) {
        goto out_of_memory;
    }

    _PyGC_InitState(&interp->gc);
    PyConfig_InitPythonConfig(&interp->config);

    interp->eval_frame = _PyEval_EvalFrameDefault;
#ifdef HAVE_DLOPEN
#if HAVE_DECL_RTLD_NOW
    interp->dlopenflags = RTLD_NOW;
#else
    interp->dlopenflags = RTLD_LAZY;
#endif
#endif

    struct pyinterpreters *interpreters = &runtime->interpreters;

    HEAD_LOCK(runtime);
    if (interpreters->next_id < 0) {
        /* overflow or Py_Initialize() not called! */
        if (tstate != NULL) {
            _PyErr_SetString(tstate, PyExc_RuntimeError,
                             "failed to get an interpreter ID");
        }
        PyMem_RawFree(interp);
        interp = NULL;
    }
    else {
        interp->id = interpreters->next_id;
        interpreters->next_id += 1;
        interp->next = interpreters->head;
        if (interpreters->main == NULL) {
            interpreters->main = interp;
        }
        interpreters->head = interp;
    }
    HEAD_UNLOCK(runtime);

    if (interp == NULL) {
        return NULL;
    }

    interp->tstate_next_unique_id = 0;

    interp->audit_hooks = NULL;

    return interp;

out_of_memory:
    if (tstate != NULL) {
        _PyErr_NoMemory(tstate);
    }

    PyMem_RawFree(interp);
    return NULL;
}


void
PyInterpreterState_Clear(PyInterpreterState *interp)
{
    _PyRuntimeState *runtime = interp->runtime;

    /* Use the current Python thread state to call audit hooks,
       not the current Python thread state of 'interp'. */
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PySys_Audit(tstate, "cpython.PyInterpreterState_Clear", NULL) < 0) {
        _PyErr_Clear(tstate);
    }

    HEAD_LOCK(runtime);
    for (PyThreadState *p = interp->tstate_head; p != NULL; p = p->next) {
        PyThreadState_Clear(p);
    }
    HEAD_UNLOCK(runtime);

    Py_CLEAR(interp->audit_hooks);

    PyConfig_Clear(&interp->config);
    Py_CLEAR(interp->codec_search_path);
    Py_CLEAR(interp->codec_search_cache);
    Py_CLEAR(interp->codec_error_registry);
    Py_CLEAR(interp->modules);
    Py_CLEAR(interp->modules_by_index);
    Py_CLEAR(interp->sysdict);
    Py_CLEAR(interp->builtins);
    Py_CLEAR(interp->builtins_copy);
    Py_CLEAR(interp->importlib);
    Py_CLEAR(interp->import_func);
    Py_CLEAR(interp->dict);
#ifdef HAVE_FORK
    Py_CLEAR(interp->before_forkers);
    Py_CLEAR(interp->after_forkers_parent);
    Py_CLEAR(interp->after_forkers_child);
#endif
    if (_PyRuntimeState_GetFinalizing(runtime) == NULL) {
        _PyWarnings_Fini(interp);
    }
    // XXX Once we have one allocator per interpreter (i.e.
    // per-interpreter GC) we must ensure that all of the interpreter's
    // objects have been cleaned up at the point.
}


static void
zapthreads(PyInterpreterState *interp, int check_current)
{
    PyThreadState *tstate;
    /* No need to lock the mutex here because this should only happen
       when the threads are all really dead (XXX famous last words). */
    while ((tstate = interp->tstate_head) != NULL) {
        _PyThreadState_Delete(tstate, check_current);
    }
}


void
PyInterpreterState_Delete(PyInterpreterState *interp)
{
    _PyRuntimeState *runtime = interp->runtime;
    struct pyinterpreters *interpreters = &runtime->interpreters;

    /* Delete current thread. After this, many C API calls become crashy. */
    _PyThreadState_Swap(&runtime->gilstate, NULL);

    zapthreads(interp, 0);

    _PyEval_FiniState(&interp->ceval);

    HEAD_LOCK(runtime);
    PyInterpreterState **p;
    for (p = &interpreters->head; ; p = &(*p)->next) {
        if (*p == NULL) {
            Py_FatalError("NULL interpreter");
        }
        if (*p == interp) {
            break;
        }
    }
    if (interp->tstate_head != NULL) {
        Py_FatalError("remaining threads");
    }
    *p = interp->next;

    if (interpreters->main == interp) {
        interpreters->main = NULL;
        if (interpreters->head != NULL) {
            Py_FatalError("remaining subinterpreters");
        }
    }
    HEAD_UNLOCK(runtime);

    if (interp->id_mutex != NULL) {
        PyThread_free_lock(interp->id_mutex);
    }
    PyMem_RawFree(interp);
}


/*
 * Delete all interpreter states except the main interpreter.  If there
 * is a current interpreter state, it *must* be the main interpreter.
 */
void
_PyInterpreterState_DeleteExceptMain(_PyRuntimeState *runtime)
{
    struct _gilstate_runtime_state *gilstate = &runtime->gilstate;
    struct pyinterpreters *interpreters = &runtime->interpreters;

    PyThreadState *tstate = _PyThreadState_Swap(gilstate, NULL);
    if (tstate != NULL && tstate->interp != interpreters->main) {
        Py_FatalError("not main interpreter");
    }

    HEAD_LOCK(runtime);
    PyInterpreterState *interp = interpreters->head;
    interpreters->head = NULL;
    while (interp != NULL) {
        if (interp == interpreters->main) {
            interpreters->main->next = NULL;
            interpreters->head = interp;
            interp = interp->next;
            continue;
        }

        PyInterpreterState_Clear(interp);  // XXX must activate?
        zapthreads(interp, 1);
        if (interp->id_mutex != NULL) {
            PyThread_free_lock(interp->id_mutex);
        }
        PyInterpreterState *prev_interp = interp;
        interp = interp->next;
        PyMem_RawFree(prev_interp);
    }
    HEAD_UNLOCK(runtime);

    if (interpreters->head == NULL) {
        Py_FatalError("missing main interpreter");
    }
    _PyThreadState_Swap(gilstate, tstate);
}


PyInterpreterState *
PyInterpreterState_Get(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    _Py_EnsureTstateNotNULL(tstate);
    PyInterpreterState *interp = tstate->interp;
    if (interp == NULL) {
        Py_FatalError("no current interpreter");
    }
    return interp;
}


int64_t
PyInterpreterState_GetID(PyInterpreterState *interp)
{
    if (interp == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "no interpreter provided");
        return -1;
    }
    return interp->id;
}


static PyInterpreterState *
interp_look_up_id(_PyRuntimeState *runtime, int64_t requested_id)
{
    PyInterpreterState *interp = runtime->interpreters.head;
    while (interp != NULL) {
        int64_t id = PyInterpreterState_GetID(interp);
        if (id < 0) {
            return NULL;
        }
        if (requested_id == id) {
            return interp;
        }
        interp = PyInterpreterState_Next(interp);
    }
    return NULL;
}

PyInterpreterState *
_PyInterpreterState_LookUpID(int64_t requested_id)
{
    PyInterpreterState *interp = NULL;
    if (requested_id >= 0) {
        _PyRuntimeState *runtime = &_PyRuntime;
        HEAD_LOCK(runtime);
        interp = interp_look_up_id(runtime, requested_id);
        HEAD_UNLOCK(runtime);
    }
    if (interp == NULL && !PyErr_Occurred()) {
        PyErr_Format(PyExc_RuntimeError,
                     "unrecognized interpreter ID %lld", requested_id);
    }
    return interp;
}


int
_PyInterpreterState_IDInitref(PyInterpreterState *interp)
{
    if (interp->id_mutex != NULL) {
        return 0;
    }
    interp->id_mutex = PyThread_allocate_lock();
    if (interp->id_mutex == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to create init interpreter ID mutex");
        return -1;
    }
    interp->id_refcount = 0;
    return 0;
}


void
_PyInterpreterState_IDIncref(PyInterpreterState *interp)
{
    if (interp->id_mutex == NULL) {
        return;
    }
    PyThread_acquire_lock(interp->id_mutex, WAIT_LOCK);
    interp->id_refcount += 1;
    PyThread_release_lock(interp->id_mutex);
}


void
_PyInterpreterState_IDDecref(PyInterpreterState *interp)
{
    if (interp->id_mutex == NULL) {
        return;
    }
    struct _gilstate_runtime_state *gilstate = &_PyRuntime.gilstate;
    PyThread_acquire_lock(interp->id_mutex, WAIT_LOCK);
    assert(interp->id_refcount != 0);
    interp->id_refcount -= 1;
    int64_t refcount = interp->id_refcount;
    PyThread_release_lock(interp->id_mutex);

    if (refcount == 0 && interp->requires_idref) {
        // XXX Using the "head" thread isn't strictly correct.
        PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
        // XXX Possible GILState issues?
        PyThreadState *save_tstate = _PyThreadState_Swap(gilstate, tstate);
        Py_EndInterpreter(tstate);
        _PyThreadState_Swap(gilstate, save_tstate);
    }
}

int
_PyInterpreterState_RequiresIDRef(PyInterpreterState *interp)
{
    return interp->requires_idref;
}

void
_PyInterpreterState_RequireIDRef(PyInterpreterState *interp, int required)
{
    interp->requires_idref = required ? 1 : 0;
}

PyObject *
_PyInterpreterState_GetMainModule(PyInterpreterState *interp)
{
    if (interp->modules == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "interpreter not initialized");
        return NULL;
    }
    return PyMapping_GetItemString(interp->modules, "__main__");
}

PyObject *
PyInterpreterState_GetDict(PyInterpreterState *interp)
{
    if (interp->dict == NULL) {
        interp->dict = PyDict_New();
        if (interp->dict == NULL) {
            PyErr_Clear();
        }
    }
    /* Returning NULL means no per-interpreter dict is available. */
    return interp->dict;
}

void
_PyInterpreterState_WaitForThreads(PyInterpreterState *interp)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = PyThreadState_Get();

    if (tstate->done_event) {
        /* First, mark the active thread as done */
        _PyEventRc *done_event = tstate->done_event;
        tstate->done_event = NULL;
        _PyEvent_Notify(&done_event->event);
        _PyEventRc_Decref(done_event);
    }

    for (;;) {
        _PyEventRc *done_event = NULL;

        // Find a thread that's not yet finished.
        HEAD_LOCK(runtime);
        for (PyThreadState *p = interp->tstate_head; p != NULL; p = p->next) {
            if (p == tstate) {
                continue;
            }
            if (p->done_event && !p->daemon) {
                done_event = p->done_event;
                _PyEventRc_Incref(done_event);
                break;
            }
        }
        HEAD_UNLOCK(runtime);

        if (!done_event) {
            // No more non-daemon threads to wait on!
            break;
        }

        // Wait for the other thread to finish. If we're interrupted, such
        // as by a ctrl-c we print the error and exit early.
        for (;;) {
            if (_PyEvent_TimedWait(&done_event->event, -1)) {
                break;
            }

            // interrupted
            if (Py_MakePendingCalls() < 0) {
                PyErr_WriteUnraisable(NULL);
                _PyEventRc_Decref(done_event);
                return;
            }
        }

        _PyEventRc_Decref(done_event);
    }
}

static PyThreadState *
new_threadstate(PyInterpreterState *interp, int init, _PyEventRc *done_event)
{
    _PyRuntimeState *runtime = interp->runtime;

    if (done_event == NULL) {
        done_event = _PyEventRc_New();
        if (done_event == NULL) {
            return NULL;
        }
    }
    else {
        _PyEventRc_Incref(done_event);
    }

    PyThreadStateImpl *tstate_impl = PyMem_RawMalloc(sizeof(PyThreadStateImpl));
    if (tstate_impl == NULL) {
        _PyEventRc_Decref(done_event);
        return NULL;
    }

    memset(tstate_impl, 0, sizeof(PyThreadStateImpl));

    PyThreadState *tstate = &tstate_impl->tstate;
    tstate->interp = interp;

    tstate->status = _Py_THREAD_DETACHED;
    tstate->frame = NULL;
    tstate->recursion_depth = 0;
    tstate->overflowed = 0;
    tstate->recursion_critical = 0;
    tstate->stackcheck_counter = 0;
    tstate->tracing = 0;
    tstate->use_tracing = 0;
    tstate->cant_stop_wont_stop = 0;
    tstate->gilstate_counter = 0;
    tstate->async_exc = NULL;
    tstate->thread_id = PyThread_get_thread_ident();

    tstate->dict = NULL;

    tstate->curexc_type = NULL;
    tstate->curexc_value = NULL;
    tstate->curexc_traceback = NULL;

    tstate->exc_state.exc_type = NULL;
    tstate->exc_state.exc_value = NULL;
    tstate->exc_state.exc_traceback = NULL;
    tstate->exc_state.previous_item = NULL;
    tstate->exc_info = &tstate->exc_state;

    tstate->c_profilefunc = NULL;
    tstate->c_tracefunc = NULL;
    tstate->c_profileobj = NULL;
    tstate->c_traceobj = NULL;

    tstate->trash_delete_nesting = 0;
    tstate->trash_delete_later = NULL;

    tstate->critical_section = 0;

    tstate->coroutine_origin_tracking_depth = 0;

    tstate->async_gen_firstiter = NULL;
    tstate->async_gen_finalizer = NULL;

    tstate->context = NULL;
    tstate->context_ver = 1;

    tstate->ref_total = 0;
    tstate->done_event = done_event;

    tstate_impl->qsbr = _Py_qsbr_register(&_PyRuntime.qsbr_shared, tstate);
    if (tstate_impl->qsbr == NULL) {
        PyMem_RawFree(tstate);
        return NULL;
    }

    // FIXME: leaks thread-state
    struct _PyThreadStack *ts = vm_new_threadstate(tstate);
    if (ts == NULL) {
        PyMem_RawFree(tstate);
        return NULL;
    }
    vm_push_thread_stack(tstate, ts);

    if (init) {
        _PyThreadState_Init(tstate);
    }

    HEAD_LOCK(runtime);
    tstate->id = ++interp->tstate_next_unique_id;
    tstate->prev = NULL;
    tstate->next = interp->tstate_head;
    if (tstate->next)
        tstate->next->prev = tstate;
    interp->tstate_head = tstate;
    if (runtime->stop_the_world) {
        tstate->status = _Py_THREAD_GC;
    }
    HEAD_UNLOCK(runtime);

    return tstate;
}

PyThreadState *
PyThreadState_New(PyInterpreterState *interp)
{
    return new_threadstate(interp, 1, NULL);
}

PyThreadState *
_PyThreadState_Prealloc(PyInterpreterState *interp, _PyEventRc *done_event)
{
    return new_threadstate(interp, 0, done_event);
}

void
_PyThreadState_Init(PyThreadState *tstate)
{
    tstate->fast_thread_id = _Py_ThreadId();
    mi_tld_t *tld = mi_heap_get_default()->tld;
    mi_atomic_add_acq_rel(&tld->refcount, 1);
    for (int tag = 0; tag < Py_NUM_HEAPS; tag++) {
        tstate->heaps[tag] = &tld->heaps[tag];
    }
    _PyParkingLot_InitThread();
    _Py_queue_create(tstate);
    _PyGILState_NoteThreadState(&tstate->interp->runtime->gilstate, tstate);
}

PyObject*
PyState_FindModule(struct PyModuleDef* module)
{
    Py_ssize_t index = module->m_base.m_index;
    PyInterpreterState *state = _PyInterpreterState_GET();
    PyObject *res;
    if (module->m_slots) {
        return NULL;
    }
    if (index == 0)
        return NULL;
    if (state->modules_by_index == NULL)
        return NULL;
    if (index >= PyList_GET_SIZE(state->modules_by_index))
        return NULL;
    res = PyList_GET_ITEM(state->modules_by_index, index);
    return res==Py_None ? NULL : res;
}

int
_PyState_AddModule(PyThreadState *tstate, PyObject* module, struct PyModuleDef* def)
{
    if (!def) {
        assert(_PyErr_Occurred(tstate));
        return -1;
    }
    if (def->m_slots) {
        _PyErr_SetString(tstate,
                         PyExc_SystemError,
                         "PyState_AddModule called on module with slots");
        return -1;
    }

    PyInterpreterState *interp = tstate->interp;
    if (!interp->modules_by_index) {
        interp->modules_by_index = PyList_New(0);
        if (!interp->modules_by_index) {
            return -1;
        }
    }

    while (PyList_GET_SIZE(interp->modules_by_index) <= def->m_base.m_index) {
        if (PyList_Append(interp->modules_by_index, Py_None) < 0) {
            return -1;
        }
    }

    Py_INCREF(module);
    return PyList_SetItem(interp->modules_by_index,
                          def->m_base.m_index, module);
}

int
PyState_AddModule(PyObject* module, struct PyModuleDef* def)
{
    if (!def) {
        Py_FatalError("module definition is NULL");
        return -1;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;
    Py_ssize_t index = def->m_base.m_index;
    if (interp->modules_by_index &&
        index < PyList_GET_SIZE(interp->modules_by_index) &&
        module == PyList_GET_ITEM(interp->modules_by_index, index))
    {
        _Py_FatalErrorFormat(__func__, "module %p already added", module);
        return -1;
    }
    return _PyState_AddModule(tstate, module, def);
}

int
PyState_RemoveModule(struct PyModuleDef* def)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    if (def->m_slots) {
        _PyErr_SetString(tstate,
                         PyExc_SystemError,
                         "PyState_RemoveModule called on module with slots");
        return -1;
    }

    Py_ssize_t index = def->m_base.m_index;
    if (index == 0) {
        Py_FatalError("invalid module index");
    }
    if (interp->modules_by_index == NULL) {
        Py_FatalError("Interpreters module-list not accessible.");
    }
    if (index > PyList_GET_SIZE(interp->modules_by_index)) {
        Py_FatalError("Module index out of bounds.");
    }

    Py_INCREF(Py_None);
    return PyList_SetItem(interp->modules_by_index, index, Py_None);
}

/* Used by PyImport_Cleanup() */
void
_PyInterpreterState_ClearModules(PyInterpreterState *interp)
{
    if (!interp->modules_by_index) {
        return;
    }

    Py_ssize_t i;
    for (i = 0; i < PyList_GET_SIZE(interp->modules_by_index); i++) {
        PyObject *m = PyList_GET_ITEM(interp->modules_by_index, i);
        if (PyModule_Check(m)) {
            /* cleanup the saved copy of module dicts */
            PyModuleDef *md = PyModule_GetDef(m);
            if (md) {
                Py_CLEAR(md->m_base.m_copy);
            }
        }
    }

    /* Setting modules_by_index to NULL could be dangerous, so we
       clear the list instead. */
    if (PyList_SetSlice(interp->modules_by_index,
                        0, PyList_GET_SIZE(interp->modules_by_index),
                        NULL)) {
        PyErr_WriteUnraisable(interp->modules_by_index);
    }
}

void
PyThreadState_Clear(PyThreadState *tstate)
{
    int verbose = _PyInterpreterState_GetConfig(tstate->interp)->verbose;

    if (verbose && tstate->frame != NULL) {
        /* bpo-20526: After the main thread calls
           _PyRuntimeState_SetFinalizing() in Py_FinalizeEx(), threads must
           exit when trying to take the GIL. If a thread exit in the middle of
           _PyEval_EvalFrameDefault(), tstate->frame is not reset to its
           previous value. It is more likely with daemon threads, but it can
           happen with regular threads if threading._shutdown() fails
           (ex: interrupted by CTRL+C). */
        fprintf(stderr,
          "PyThreadState_Clear: warning: thread still has a frame\n");
    }

    _Py_queue_destroy(tstate);

    /* Don't clear tstate->frame: it is a borrowed reference */

    Py_CLEAR(tstate->dict);
    Py_CLEAR(tstate->async_exc);

    Py_CLEAR(tstate->curexc_type);
    Py_CLEAR(tstate->curexc_value);
    Py_CLEAR(tstate->curexc_traceback);

    Py_CLEAR(tstate->exc_state.exc_type);
    Py_CLEAR(tstate->exc_state.exc_value);
    Py_CLEAR(tstate->exc_state.exc_traceback);

    /* The stack of exception states should contain just this thread. */
    if (verbose && tstate->exc_info != &tstate->exc_state) {
        fprintf(stderr,
          "PyThreadState_Clear: warning: thread still has a generator\n");
    }

    tstate->c_profilefunc = NULL;
    tstate->c_tracefunc = NULL;
    Py_CLEAR(tstate->c_profileobj);
    Py_CLEAR(tstate->c_traceobj);

    Py_CLEAR(tstate->async_gen_firstiter);
    Py_CLEAR(tstate->async_gen_finalizer);

    Py_CLEAR(tstate->context);

    _PyTypeId_MergeRefcounts(&_PyRuntime.typeids, tstate);
}

/* Common code for PyThreadState_Delete() and PyThreadState_DeleteCurrent() */
static void
tstate_delete_common(PyThreadState *tstate,
                     struct _gilstate_runtime_state *gilstate,
                     int is_current)
{
    assert(is_current ? tstate->status == _Py_THREAD_ATTACHED
                      : tstate->status != _Py_THREAD_ATTACHED);

    _Py_EnsureTstateNotNULL(tstate);
    PyInterpreterState *interp = tstate->interp;
    if (interp == NULL) {
        Py_FatalError("NULL interpreter");
    }

    if (gilstate->autoInterpreterState &&
        PyThread_tss_get(&gilstate->autoTSSkey) == tstate)
    {
        PyThread_tss_set(&gilstate->autoTSSkey, NULL);
    }

    _PyTypeId_MergeRefcounts(&_PyRuntime.typeids, tstate);

    PyThreadStateImpl *tstate_impl = (PyThreadStateImpl *)tstate;
    if (is_current) {
        _Py_qsbr_offline(tstate_impl->qsbr);
    }
    _Py_qsbr_unregister(tstate_impl->qsbr);
    tstate_impl->qsbr = NULL;

    if (tstate->heaps[0] != NULL) {
        _mi_thread_abandon(tstate->heaps[0]->tld);
    }

    for (int tag = 0; tag < Py_NUM_HEAPS; tag++) {
        tstate->heaps[tag] = NULL;
    }

    _PyEventRc *done_event;
    _PyRuntimeState *runtime = interp->runtime;
    HEAD_LOCK(runtime);
    if (tstate->prev) {
        tstate->prev->next = tstate->next;
    }
    else {
        interp->tstate_head = tstate->next;
    }
    if (tstate->next) {
        tstate->next->prev = tstate->prev;
    }
    done_event = tstate->done_event;
    tstate->done_event = NULL;
#ifdef Py_REF_DEBUG
    runtime->ref_total += tstate->ref_total;
    tstate->ref_total = 0;
#endif

    if (runtime->stop_the_world &&
        tstate->status != _Py_THREAD_GC &&
        tstate != _PyRuntimeState_GetFinalizing(&_PyRuntime)) {
        // If another thread is waiting for us to stop, decrease gc_thread_countdown
        // and potentially notify them.
        struct _gc_runtime_state *gc = &tstate->interp->gc;
        gc->gc_thread_countdown--;
        assert(gc->gc_thread_countdown >= 0);
        if (gc->gc_thread_countdown == 0) {
            _PyRawEvent_Notify(&gc->gc_stop_event);
        }
    }

    HEAD_UNLOCK(runtime);

    // Notify threads waiting on Thread.join(). This should happen after the
    // thread state is unlinked, but must happen before parking lot is
    // deinitialized.
    if (done_event) {
        _PyEvent_Notify(&done_event->event);
        _PyEventRc_Decref(done_event);
    }

    if (is_current) {
        _PyThreadState_SET(NULL);
        _PyParkingLot_DeinitThread();
    }
}


static void
_PyThreadState_Delete(PyThreadState *tstate, int check_current)
{
    struct _gilstate_runtime_state *gilstate = &tstate->interp->runtime->gilstate;
    if (check_current) {
        if (tstate == _PyRuntimeGILState_GetThreadState(gilstate)) {
            _Py_FatalErrorFormat(__func__, "tstate %p is still current", tstate);
        }
    }
    tstate_delete_common(tstate, gilstate, 0);
    PyMem_RawFree(tstate);
}


void
PyThreadState_Delete(PyThreadState *tstate)
{
    _PyThreadState_Delete(tstate, 1);
}


void
_PyThreadState_DeleteCurrent(PyThreadState *tstate)
{
    _Py_EnsureTstateNotNULL(tstate);
    struct _gilstate_runtime_state *gilstate = &tstate->interp->runtime->gilstate;
    tstate_delete_common(tstate, gilstate, 1);
    _PyRuntimeGILState_SetThreadState(gilstate, NULL);
    _PyEval_ReleaseLock(tstate);
    PyMem_RawFree(tstate);
}

void
PyThreadState_DeleteCurrent(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    _PyThreadState_DeleteCurrent(tstate);
}


/*
 * Detaches all thread states except the one passed as argument.
 * Note that, if there is a current thread state, it *must* be the one
 * passed as argument.  Also, this won't touch any other interpreters
 * than the current one, since we don't know which thread state should
 * be kept in those other interpreters.
 */
PyThreadState *
_PyThreadState_UnlinkExcept(_PyRuntimeState *runtime, PyThreadState *tstate, int already_dead)
{
    PyInterpreterState *interp = tstate->interp;
    HEAD_LOCK(runtime);
    /* Remove all thread states, except tstate, from the linked list of
       thread states.  This will allow calling PyThreadState_Clear()
       without holding the lock. */
    PyThreadState *garbage = interp->tstate_head;
    if (garbage == tstate)
        garbage = tstate->next;
    if (tstate->prev)
        tstate->prev->next = tstate->next;
    if (tstate->next)
        tstate->next->prev = tstate->prev;
    tstate->prev = tstate->next = NULL;
    interp->tstate_head = tstate;
    interp->num_threads = 1;
    HEAD_UNLOCK(runtime);

    for (PyThreadState *p = garbage; p; p = p->next) {
        if (p->heaps[0] != NULL) {
            mi_tld_t *tld = p->heaps[0]->tld;
            if (already_dead) {
                assert(tld->status == 0);
                tld->status = MI_THREAD_DEAD;
            }
            _mi_thread_abandon(tld);
        }
    }

    return garbage;
}

void
_PyThreadState_DeleteGarbage(PyThreadState *garbage)
{
    PyThreadState *next;
    for (PyThreadState *p = garbage; p; p = next) {
        next = p->next;
        PyThreadState_Clear(p);
        PyMem_RawFree(p);
    }
}

void
_PyThreadState_DeleteExcept(_PyRuntimeState *runtime, PyThreadState *tstate)
{
    PyThreadState *garbage = _PyThreadState_UnlinkExcept(runtime, tstate, 0);
    _PyThreadState_DeleteGarbage(garbage);
}

PyThreadState *
_PyThreadState_UncheckedGet(void)
{
    return _PyThreadState_GET();
}


PyThreadState *
PyThreadState_Get(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    _Py_EnsureTstateNotNULL(tstate);
    return tstate;
}


PyThreadState *
_PyThreadState_Swap(struct _gilstate_runtime_state *gilstate, PyThreadState *newts)
{
    PyThreadState *oldts = _Py_current_tstate;

#if defined(Py_DEBUG)
    // The new thread-state should correspond to the current native thread
    // XXX: breaks subinterpreter tests
    if (newts && newts->fast_thread_id != _Py_ThreadId()) {
        Py_FatalError("Invalid thread state for this thread");
    }
#endif

    if (oldts != NULL) {
        int32_t status = _Py_atomic_load_int32(&oldts->status);
        assert(status == _Py_THREAD_ATTACHED || status == _Py_THREAD_GC);

        if (status == _Py_THREAD_ATTACHED) {
            _PyThreadState_Detach(oldts);
        }
    }

    _Py_current_tstate = newts;

    if (newts) {
        int attached = _PyThreadState_Attach(newts);
        if (!attached) {
            _PyThreadState_GC_Park(newts);
        }

        assert(_Py_atomic_load_int32(&newts->status) == _Py_THREAD_ATTACHED);
    }

    /* It should not be possible for more than one thread state
       to be used for a thread.  Check this the best we can in debug
       builds.
    */
#if defined(Py_DEBUG)
    if (newts) {
        /* This can be called from PyEval_RestoreThread(). Similar
           to it, we need to ensure errno doesn't change.
        */
        int err = errno;
        if (oldts && oldts->interp == newts->interp && oldts != newts)
            Py_FatalError("Invalid thread state for this thread");
        errno = err;
    }
#endif
    return oldts;
}

PyThreadState *
PyThreadState_Swap(PyThreadState *newts)
{
    return _PyThreadState_Swap(&_PyRuntime.gilstate, newts);
}

/* An extension mechanism to store arbitrary additional per-thread state.
   PyThreadState_GetDict() returns a dictionary that can be used to hold such
   state; the caller should pick a unique key and store its state there.  If
   PyThreadState_GetDict() returns NULL, an exception has *not* been raised
   and the caller should assume no per-thread state is available. */

PyObject *
_PyThreadState_GetDict(PyThreadState *tstate)
{
    assert(tstate != NULL);
    if (tstate->dict == NULL) {
        tstate->dict = PyDict_New();
        if (tstate->dict == NULL) {
            _PyErr_Clear(tstate);
        }
    }
    return tstate->dict;
}


PyObject *
PyThreadState_GetDict(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL) {
        return NULL;
    }
    return _PyThreadState_GetDict(tstate);
}


PyInterpreterState *
PyThreadState_GetInterpreter(PyThreadState *tstate)
{
    assert(tstate != NULL);
    return tstate->interp;
}


PyFrameObject*
PyThreadState_GetFrame(PyThreadState *tstate)
{
    assert(tstate != NULL);
    PyFrameObject *frame = tstate->frame;
    Py_XINCREF(frame);
    return frame;
}


uint64_t
PyThreadState_GetID(PyThreadState *tstate)
{
    assert(tstate != NULL);
    return tstate->id;
}

Py_ssize_t
_PyThreadState_GetRecursionDepth(PyThreadState *tstate)
{
    return vm_stack_depth(tstate);
}


/* Asynchronously raise an exception in a thread.
   Requested by Just van Rossum and Alex Martelli.
   To prevent naive misuse, you must write your own extension
   to call this, or use ctypes.  Must be called with the GIL held.
   Returns the number of tstates modified (normally 1, but 0 if `id` didn't
   match any known thread id).  Can be called with exc=NULL to clear an
   existing async exception.  This raises no exceptions. */

int
PyThreadState_SetAsyncExc(unsigned long id, PyObject *exc)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyInterpreterState *interp = _PyRuntimeState_GetThreadState(runtime)->interp;

    /* Although the GIL is held, a few C API functions can be called
     * without the GIL held, and in particular some that create and
     * destroy thread and interpreter states.  Those can mutate the
     * list of thread states we're traversing, so to prevent that we lock
     * head_mutex for the duration.
     */
    HEAD_LOCK(runtime);
    for (PyThreadState *tstate = interp->tstate_head; tstate != NULL; tstate = tstate->next) {
        if (tstate->thread_id != id) {
            continue;
        }

        /* Tricky:  we need to decref the current value
         * (if any) in tstate->async_exc, but that can in turn
         * allow arbitrary Python code to run, including
         * perhaps calls to this function.  To prevent
         * deadlock, we need to release head_mutex before
         * the decref.
         */
        Py_XINCREF(exc);
        PyObject *old_exc = _Py_atomic_exchange_ptr(&tstate->async_exc, exc);
        HEAD_UNLOCK(runtime);

        Py_XDECREF(old_exc);
        _PyThreadState_Signal(tstate, EVAL_ASYNC_EXC);
        return 1;
    }
    HEAD_UNLOCK(runtime);
    return 0;
}


/* Routines for advanced debuggers, requested by David Beazley.
   Don't use unless you know what you are doing! */

PyInterpreterState *
PyInterpreterState_Head(void)
{
    return _PyRuntime.interpreters.head;
}

PyInterpreterState *
PyInterpreterState_Main(void)
{
    return _PyRuntime.interpreters.main;
}

PyInterpreterState *
PyInterpreterState_Next(PyInterpreterState *interp) {
    return interp->next;
}

PyThreadState *
PyInterpreterState_ThreadHead(PyInterpreterState *interp) {
    return interp->tstate_head;
}

PyThreadState *
PyThreadState_Next(PyThreadState *tstate) {
    return tstate->next;
}

/* The implementation of sys._current_frames().  This is intended to be
   called with the GIL held, as it will be when called via
   sys._current_frames().  It's possible it would work fine even without
   the GIL held, but haven't thought enough about that.
*/
PyObject *
_PyThread_CurrentFrames(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PySys_Audit(tstate, "sys._current_frames", NULL) < 0) {
        return NULL;
    }

    PyObject *result = PyDict_New();
    if (result == NULL) {
        return NULL;
    }

    _PyMutex_lock(&_PyRuntime.stoptheworld_mutex);
    _PyRuntimeState_StopTheWorld(&_PyRuntime);
    tstate->cant_stop_wont_stop++;

    /* for i in all interpreters:
     *     for t in all of i's thread states:
     *          if t's frame isn't NULL, map t's id to its frame
     * Because these lists can mutate even when the GIL is held, we
     * need to grab head_mutex for the duration.
     */
    _PyRuntimeState *runtime = tstate->interp->runtime;
    HEAD_LOCK(runtime);
    PyThreadState *t;
    for_each_thread(t) {
        PyFrameObject *frame = vm_frame(t);
        if (frame == NULL) {
            if (_PyErr_Occurred(tstate)) {
                goto fail;
            }
            continue;
        }
        PyObject *id = PyLong_FromUnsignedLong(t->thread_id);
        if (id == NULL) {
            goto fail;
        }
        int stat = PyDict_SetItem(result, id, (PyObject *)frame);
        Py_DECREF(id);
        if (stat < 0) {
            goto fail;
        }
    }
    goto done;

fail:
    Py_CLEAR(result);

done:
    HEAD_UNLOCK(runtime);
    tstate->cant_stop_wont_stop--;
    _PyRuntimeState_StartTheWorld(&_PyRuntime);
    _PyMutex_unlock(&_PyRuntime.stoptheworld_mutex);
    return result;
}

/* Python "auto thread state" API. */

/* Keep this as a static, as it is not reliable!  It can only
   ever be compared to the state for the *current* thread.
   * If not equal, then it doesn't matter that the actual
     value may change immediately after comparison, as it can't
     possibly change to the current thread's state.
   * If equal, then the current thread holds the lock, so the value can't
     change until we yield the lock.
*/
static int
PyThreadState_IsCurrent(PyThreadState *tstate)
{
    /* Must be the tstate for this thread */
    assert(_PyGILState_GetThisThreadState(&_PyRuntime.gilstate) == tstate);
    return tstate == _PyThreadState_GET();
}

/* Internal initialization/finalization functions called by
   Py_Initialize/Py_FinalizeEx
*/
PyStatus
_PyGILState_Init(PyThreadState *tstate)
{
    if (!_Py_IsMainInterpreter(tstate)) {
        /* Currently, PyGILState is shared by all interpreters. The main
         * interpreter is responsible to initialize it. */
        return _PyStatus_OK();
    }

    /* must init with valid states */
    assert(tstate != NULL);
    assert(tstate->interp != NULL);

    struct _gilstate_runtime_state *gilstate = &tstate->interp->runtime->gilstate;

    if (PyThread_tss_create(&gilstate->autoTSSkey) != 0) {
        return _PyStatus_NO_MEMORY();
    }
    gilstate->autoInterpreterState = tstate->interp;
    assert(PyThread_tss_get(&gilstate->autoTSSkey) == NULL);
    assert(tstate->gilstate_counter == 0);

    _PyGILState_NoteThreadState(gilstate, tstate);
    return _PyStatus_OK();
}

PyInterpreterState *
_PyGILState_GetInterpreterStateUnsafe(void)
{
    return _PyRuntime.gilstate.autoInterpreterState;
}

void
_PyGILState_Fini(PyThreadState *tstate)
{
    struct _gilstate_runtime_state *gilstate = &tstate->interp->runtime->gilstate;
    PyThread_tss_delete(&gilstate->autoTSSkey);
    gilstate->autoInterpreterState = NULL;
}

/* Reset the TSS key - called by PyOS_AfterFork_Child().
 * This should not be necessary, but some - buggy - pthread implementations
 * don't reset TSS upon fork(), see issue #10517.
 */
void
_PyGILState_Reinit(_PyRuntimeState *runtime)
{
    struct _gilstate_runtime_state *gilstate = &runtime->gilstate;
    PyThreadState *tstate = _PyGILState_GetThisThreadState(gilstate);

    PyThread_tss_delete(&gilstate->autoTSSkey);
    if (PyThread_tss_create(&gilstate->autoTSSkey) != 0) {
        Py_FatalError("Could not allocate TSS entry");
    }

    /* If the thread had an associated auto thread state, reassociate it with
     * the new key. */
    if (tstate &&
        PyThread_tss_set(&gilstate->autoTSSkey, (void *)tstate) != 0)
    {
        Py_FatalError("Couldn't create autoTSSkey mapping");
    }
}

/* When a thread state is created for a thread by some mechanism other than
   PyGILState_Ensure, it's important that the GILState machinery knows about
   it so it doesn't try to create another thread state for the thread (this is
   a better fix for SF bug #1010677 than the first one attempted).
*/
static void
_PyGILState_NoteThreadState(struct _gilstate_runtime_state *gilstate, PyThreadState* tstate)
{
    /* If autoTSSkey isn't initialized, this must be the very first
       threadstate created in Py_Initialize().  Don't do anything for now
       (we'll be back here when _PyGILState_Init is called). */
    if (!gilstate->autoInterpreterState) {
        return;
    }

    /* Stick the thread state for this thread in thread specific storage.

       The only situation where you can legitimately have more than one
       thread state for an OS level thread is when there are multiple
       interpreters.

       You shouldn't really be using the PyGILState_ APIs anyway (see issues
       #10915 and #15751).

       The first thread state created for that given OS level thread will
       "win", which seems reasonable behaviour.
    */
    if (PyThread_tss_get(&gilstate->autoTSSkey) == NULL) {
        if ((PyThread_tss_set(&gilstate->autoTSSkey, (void *)tstate)) != 0) {
            Py_FatalError("Couldn't create autoTSSkey mapping");
        }
    }

    /* PyGILState_Release must not try to delete this thread state. */
    tstate->gilstate_counter = 1;
}

/* The public functions */
static PyThreadState *
_PyGILState_GetThisThreadState(struct _gilstate_runtime_state *gilstate)
{
    if (gilstate->autoInterpreterState == NULL)
        return NULL;
    return (PyThreadState *)PyThread_tss_get(&gilstate->autoTSSkey);
}

PyThreadState *
PyGILState_GetThisThreadState(void)
{
    return _PyGILState_GetThisThreadState(&_PyRuntime.gilstate);
}

int
PyGILState_Check(void)
{
    struct _gilstate_runtime_state *gilstate = &_PyRuntime.gilstate;
    if (!gilstate->check_enabled) {
        return 1;
    }

    if (!PyThread_tss_is_created(&gilstate->autoTSSkey)) {
        return 1;
    }

    PyThreadState *tstate = _PyRuntimeGILState_GetThreadState(gilstate);
    if (tstate == NULL) {
        return 0;
    }

    return (tstate == _PyGILState_GetThisThreadState(gilstate));
}

PyGILState_STATE
PyGILState_Ensure(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    struct _gilstate_runtime_state *gilstate = &runtime->gilstate;

    /* Note that we do not auto-init Python here - apart from
       potential races with 2 threads auto-initializing, pep-311
       spells out other issues.  Embedders are expected to have
       called Py_Initialize(). */

    /* Ensure that _PyEval_InitThreads() and _PyGILState_Init() have been
       called by Py_Initialize() */
    assert(_PyEval_ThreadsInitialized(runtime));
    assert(gilstate->autoInterpreterState);

    PyThreadState *tcur = (PyThreadState *)PyThread_tss_get(&gilstate->autoTSSkey);
    int current;
    if (tcur == NULL) {
        /* Create a new Python thread state for this thread */
        tcur = PyThreadState_New(gilstate->autoInterpreterState);
        if (tcur == NULL) {
            Py_FatalError("Couldn't create thread-state for new thread");
        }

        /* This is our thread state!  We'll need to delete it in the
           matching call to PyGILState_Release(). */
        tcur->gilstate_counter = 0;
        current = 0; /* new thread state is never current */
    }
    else {
        current = PyThreadState_IsCurrent(tcur);
    }

    if (current == 0) {
        PyEval_RestoreThread(tcur);
    }

    /* Update our counter in the thread-state - no need for locks:
       - tcur will remain valid as we hold the GIL.
       - the counter is safe as we are the only thread "allowed"
         to modify this value
    */
    ++tcur->gilstate_counter;

    return current ? PyGILState_LOCKED : PyGILState_UNLOCKED;
}

void
PyGILState_Release(PyGILState_STATE oldstate)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = PyThread_tss_get(&runtime->gilstate.autoTSSkey);
    if (tstate == NULL) {
        Py_FatalError("auto-releasing thread-state, "
                      "but no thread-state for this thread");
    }

    /* We must hold the GIL and have our thread state current */
    /* XXX - remove the check - the assert should be fine,
       but while this is very new (April 2003), the extra check
       by release-only users can't hurt.
    */
    if (!PyThreadState_IsCurrent(tstate)) {
        _Py_FatalErrorFormat(__func__,
                             "thread state %p must be current when releasing",
                             tstate);
    }
    assert(PyThreadState_IsCurrent(tstate));
    --tstate->gilstate_counter;
    assert(tstate->gilstate_counter >= 0); /* illegal counter value */

    /* If we're going to destroy this thread-state, we must
     * clear it while the GIL is held, as destructors may run.
     */
    if (tstate->gilstate_counter == 0) {
        /* can't have been locked when we created it */
        assert(oldstate == PyGILState_UNLOCKED);
        PyThreadState_Clear(tstate);
        /* Delete the thread-state.  Note this releases the GIL too!
         * It's vital that the GIL be held here, to avoid shutdown
         * races; see bugs 225673 and 1061968 (that nasty bug has a
         * habit of coming back).
         */
        assert(_PyRuntimeGILState_GetThreadState(&runtime->gilstate) == tstate);
        _PyThreadState_DeleteCurrent(tstate);
    }
    /* Release the lock if necessary */
    else if (oldstate == PyGILState_UNLOCKED)
        PyEval_SaveThread();
}


/**************************/
/* cross-interpreter data */
/**************************/

/* cross-interpreter data */

crossinterpdatafunc _PyCrossInterpreterData_Lookup(PyObject *);

/* This is a separate func from _PyCrossInterpreterData_Lookup in order
   to keep the registry code separate. */
static crossinterpdatafunc
_lookup_getdata(PyObject *obj)
{
    crossinterpdatafunc getdata = _PyCrossInterpreterData_Lookup(obj);
    if (getdata == NULL && PyErr_Occurred() == 0)
        PyErr_Format(PyExc_ValueError,
                     "%S does not support cross-interpreter data", obj);
    return getdata;
}

int
_PyObject_CheckCrossInterpreterData(PyObject *obj)
{
    crossinterpdatafunc getdata = _lookup_getdata(obj);
    if (getdata == NULL) {
        return -1;
    }
    return 0;
}

static int
_check_xidata(PyThreadState *tstate, _PyCrossInterpreterData *data)
{
    // data->data can be anything, including NULL, so we don't check it.

    // data->obj may be NULL, so we don't check it.

    if (data->interp < 0) {
        _PyErr_SetString(tstate, PyExc_SystemError, "missing interp");
        return -1;
    }

    if (data->new_object == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError, "missing new_object func");
        return -1;
    }

    // data->free may be NULL, so we don't check it.

    return 0;
}

int
_PyObject_GetCrossInterpreterData(PyObject *obj, _PyCrossInterpreterData *data)
{
    // PyThreadState_Get() aborts if tstate is NULL.
    PyThreadState *tstate = PyThreadState_Get();
    PyInterpreterState *interp = tstate->interp;

    // Reset data before re-populating.
    *data = (_PyCrossInterpreterData){0};
    data->free = PyMem_RawFree;  // Set a default that may be overridden.

    // Call the "getdata" func for the object.
    Py_INCREF(obj);
    crossinterpdatafunc getdata = _lookup_getdata(obj);
    if (getdata == NULL) {
        Py_DECREF(obj);
        return -1;
    }
    int res = getdata(obj, data);
    Py_DECREF(obj);
    if (res != 0) {
        return -1;
    }

    // Fill in the blanks and validate the result.
    data->interp = interp->id;
    if (_check_xidata(tstate, data) != 0) {
        _PyCrossInterpreterData_Release(data);
        return -1;
    }

    return 0;
}

static void
_release_xidata(void *arg)
{
    _PyCrossInterpreterData *data = (_PyCrossInterpreterData *)arg;
    if (data->free != NULL) {
        data->free(data->data);
    }
    Py_XDECREF(data->obj);
}

static void
_call_in_interpreter(struct _gilstate_runtime_state *gilstate,
                     PyInterpreterState *interp,
                     void (*func)(void *), void *arg)
{
    /* We would use Py_AddPendingCall() if it weren't specific to the
     * main interpreter (see bpo-33608).  In the meantime we take a
     * naive approach.
     */
    PyThreadState *save_tstate = NULL;
    if (interp != _PyRuntimeGILState_GetThreadState(gilstate)->interp) {
        // XXX Using the "head" thread isn't strictly correct.
        PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
        // XXX Possible GILState issues?
        save_tstate = _PyThreadState_Swap(gilstate, tstate);
    }

    func(arg);

    // Switch back.
    if (save_tstate != NULL) {
        _PyThreadState_Swap(gilstate, save_tstate);
    }
}

void
_PyCrossInterpreterData_Release(_PyCrossInterpreterData *data)
{
    if (data->data == NULL && data->obj == NULL) {
        // Nothing to release!
        return;
    }

    // Switch to the original interpreter.
    PyInterpreterState *interp = _PyInterpreterState_LookUpID(data->interp);
    if (interp == NULL) {
        // The interpreter was already destroyed.
        if (data->free != NULL) {
            // XXX Someone leaked some memory...
        }
        return;
    }

    // "Release" the data and/or the object.
    struct _gilstate_runtime_state *gilstate = &_PyRuntime.gilstate;
    _call_in_interpreter(gilstate, interp, _release_xidata, data);
}

PyObject *
_PyCrossInterpreterData_NewObject(_PyCrossInterpreterData *data)
{
    return data->new_object(data);
}

/* registry of {type -> crossinterpdatafunc} */

/* For now we use a global registry of shareable classes.  An
   alternative would be to add a tp_* slot for a class's
   crossinterpdatafunc. It would be simpler and more efficient. */

static int
_register_xidata(struct _xidregistry *xidregistry, PyTypeObject *cls,
                 crossinterpdatafunc getdata)
{
    // Note that we effectively replace already registered classes
    // rather than failing.
    struct _xidregitem *newhead = PyMem_RawMalloc(sizeof(struct _xidregitem));
    if (newhead == NULL)
        return -1;
    newhead->cls = cls;
    newhead->getdata = getdata;
    newhead->next = xidregistry->head;
    xidregistry->head = newhead;
    return 0;
}

static void _register_builtins_for_crossinterpreter_data(struct _xidregistry *xidregistry);

int
_PyCrossInterpreterData_RegisterClass(PyTypeObject *cls,
                                       crossinterpdatafunc getdata)
{
    if (!PyType_Check(cls)) {
        PyErr_Format(PyExc_ValueError, "only classes may be registered");
        return -1;
    }
    if (getdata == NULL) {
        PyErr_Format(PyExc_ValueError, "missing 'getdata' func");
        return -1;
    }

    // Make sure the class isn't ever deallocated.
    Py_INCREF((PyObject *)cls);

    struct _xidregistry *xidregistry = &_PyRuntime.xidregistry ;
    PyThread_acquire_lock(xidregistry->mutex, WAIT_LOCK);
    if (xidregistry->head == NULL) {
        _register_builtins_for_crossinterpreter_data(xidregistry);
    }
    int res = _register_xidata(xidregistry, cls, getdata);
    PyThread_release_lock(xidregistry->mutex);
    return res;
}

/* Cross-interpreter objects are looked up by exact match on the class.
   We can reassess this policy when we move from a global registry to a
   tp_* slot. */

crossinterpdatafunc
_PyCrossInterpreterData_Lookup(PyObject *obj)
{
    struct _xidregistry *xidregistry = &_PyRuntime.xidregistry ;
    PyObject *cls = PyObject_Type(obj);
    crossinterpdatafunc getdata = NULL;
    PyThread_acquire_lock(xidregistry->mutex, WAIT_LOCK);
    struct _xidregitem *cur = xidregistry->head;
    if (cur == NULL) {
        _register_builtins_for_crossinterpreter_data(xidregistry);
        cur = xidregistry->head;
    }
    for(; cur != NULL; cur = cur->next) {
        if (cur->cls == (PyTypeObject *)cls) {
            getdata = cur->getdata;
            break;
        }
    }
    Py_DECREF(cls);
    PyThread_release_lock(xidregistry->mutex);
    return getdata;
}

/* cross-interpreter data for builtin types */

struct _shared_bytes_data {
    char *bytes;
    Py_ssize_t len;
};

static PyObject *
_new_bytes_object(_PyCrossInterpreterData *data)
{
    struct _shared_bytes_data *shared = (struct _shared_bytes_data *)(data->data);
    return PyBytes_FromStringAndSize(shared->bytes, shared->len);
}

static int
_bytes_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    struct _shared_bytes_data *shared = PyMem_NEW(struct _shared_bytes_data, 1);
    if (PyBytes_AsStringAndSize(obj, &shared->bytes, &shared->len) < 0) {
        return -1;
    }
    data->data = (void *)shared;
    Py_INCREF(obj);
    data->obj = obj;  // Will be "released" (decref'ed) when data released.
    data->new_object = _new_bytes_object;
    data->free = PyMem_Free;
    return 0;
}

struct _shared_str_data {
    int kind;
    const void *buffer;
    Py_ssize_t len;
};

static PyObject *
_new_str_object(_PyCrossInterpreterData *data)
{
    struct _shared_str_data *shared = (struct _shared_str_data *)(data->data);
    return PyUnicode_FromKindAndData(shared->kind, shared->buffer, shared->len);
}

static int
_str_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    struct _shared_str_data *shared = PyMem_NEW(struct _shared_str_data, 1);
    shared->kind = PyUnicode_KIND(obj);
    shared->buffer = PyUnicode_DATA(obj);
    shared->len = PyUnicode_GET_LENGTH(obj);
    data->data = (void *)shared;
    Py_INCREF(obj);
    data->obj = obj;  // Will be "released" (decref'ed) when data released.
    data->new_object = _new_str_object;
    data->free = PyMem_Free;
    return 0;
}

static PyObject *
_new_long_object(_PyCrossInterpreterData *data)
{
    return PyLong_FromSsize_t((Py_ssize_t)(data->data));
}

static int
_long_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    /* Note that this means the size of shareable ints is bounded by
     * sys.maxsize.  Hence on 32-bit architectures that is half the
     * size of maximum shareable ints on 64-bit.
     */
    Py_ssize_t value = PyLong_AsSsize_t(obj);
    if (value == -1 && PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_OverflowError)) {
            PyErr_SetString(PyExc_OverflowError, "try sending as bytes");
        }
        return -1;
    }
    data->data = (void *)value;
    data->obj = NULL;
    data->new_object = _new_long_object;
    data->free = NULL;
    return 0;
}

static PyObject *
_new_none_object(_PyCrossInterpreterData *data)
{
    // XXX Singleton refcounts are problematic across interpreters...
    Py_INCREF(Py_None);
    return Py_None;
}

static int
_none_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    data->data = NULL;
    // data->obj remains NULL
    data->new_object = _new_none_object;
    data->free = NULL;  // There is nothing to free.
    return 0;
}

static void
_register_builtins_for_crossinterpreter_data(struct _xidregistry *xidregistry)
{
    // None
    if (_register_xidata(xidregistry, (PyTypeObject *)PyObject_Type(Py_None), _none_shared) != 0) {
        Py_FatalError("could not register None for cross-interpreter sharing");
    }

    // int
    if (_register_xidata(xidregistry, &PyLong_Type, _long_shared) != 0) {
        Py_FatalError("could not register int for cross-interpreter sharing");
    }

    // bytes
    if (_register_xidata(xidregistry, &PyBytes_Type, _bytes_shared) != 0) {
        Py_FatalError("could not register bytes for cross-interpreter sharing");
    }

    // str
    if (_register_xidata(xidregistry, &PyUnicode_Type, _str_shared) != 0) {
        Py_FatalError("could not register str for cross-interpreter sharing");
    }
}


_PyFrameEvalFunction
_PyInterpreterState_GetEvalFrameFunc(PyInterpreterState *interp)
{
    return interp->eval_frame;
}


void
_PyInterpreterState_SetEvalFrameFunc(PyInterpreterState *interp,
                                     _PyFrameEvalFunction eval_frame)
{
    interp->eval_frame = eval_frame;
}


const PyConfig*
_PyInterpreterState_GetConfig(PyInterpreterState *interp)
{
    return &interp->config;
}


PyStatus
_PyInterpreterState_SetConfig(PyInterpreterState *interp,
                              const PyConfig *config)
{
    return _PyConfig_Copy(&interp->config, config);
}


const PyConfig*
_Py_GetConfig(void)
{
    assert(PyGILState_Check());
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyInterpreterState_GetConfig(tstate->interp);
}

#ifdef __cplusplus
}
#endif
