#ifndef Py_INTERNAL_PYSTATE_H
#define Py_INTERNAL_PYSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_gil.h"       /* struct _gil_runtime_state  */
#include "pycore_llist.h"
#include "pycore_pymem.h"     /* struct _gc_runtime_state */
#include "pycore_warnings.h"  /* struct _warnings_runtime_state */
#include "pycore_llist.h"
#include "pycore_qsbr.h"
#include "lock.h"

/* Forward declaration */
struct _Py_hashtable_t;


typedef enum {
    _Py_THREAD_DETACHED=0,
    _Py_THREAD_ATTACHED,
    _Py_THREAD_GC,
} _Py_thread_status;

enum {
    EVAL_PLEASE_STOP = 1U << 0,
    EVAL_PENDING_SIGNALS = 1U << 1,
    EVAL_PENDING_CALLS = 1U << 2,
    EVAL_DROP_GIL = 1U << 3,
    EVAL_ASYNC_EXC = 1U << 4,
    EVAL_EXPLICIT_MERGE = 1U << 5,
    EVAL_QSBR = 1U << 6
};

/* ceval state */

struct _pending_calls {
    int finishing;
    PyThread_type_lock lock;
    /* Request for running pending calls. */
    _Py_atomic_int calls_to_do;
    /* Request for looking at the `async_exc` field of the current
       thread state.
       Guarded by the GIL. */
    int async_exc;
#define NPENDINGCALLS 32
    struct {
        int (*func)(void *);
        void *arg;
    } calls[NPENDINGCALLS];
    int first;
    int last;
};

struct _ceval_runtime_state {
    int recursion_limit;
    /* Records whether tracing is on for any thread.  Counts the number
       of threads for which tstate->c_tracefunc is non-NULL, so if the
       value is 0, we know we don't have to check this thread's
       c_tracefunc.  This speeds up the if statement in
       PyEval_EvalFrameEx() after fast_next_opcode. */
    int tracing_possible;
    struct _pending_calls pending;
    struct _gil_runtime_state gil;
};

/* interpreter state */

typedef PyObject* (*_PyFrameEvalFunction)(struct _frame *, int);

#define _PY_NSMALLPOSINTS           257
#define _PY_NSMALLNEGINTS           5

// The PyInterpreterState typedef is in Include/pystate.h.
struct _is {

    struct _is *next;
    struct _ts *tstate_head;

    /* Reference to the _PyRuntime global variable. This field exists
       to not have to pass runtime in addition to tstate to a function.
       Get runtime from tstate: tstate->interp->runtime. */
    struct pyruntimestate *runtime;

    int64_t id;
    int64_t id_refcount;
    int requires_idref;
    PyThread_type_lock id_mutex;

    int finalizing;

    struct _gc_runtime_state gc;

    PyObject *modules;
    PyObject *modules_by_index;
    PyObject *sysdict;
    PyObject *builtins;
    PyObject *importlib;

    /* Used in Modules/_threadmodule.c. */
    long num_threads;
    /* Support for runtime thread stack size tuning.
       A value of 0 means using the platform's default stack size
       or the size specified by the THREAD_STACK_SIZE macro. */
    /* Used in Python/thread.c. */
    size_t pythread_stacksize;

    PyObject *codec_search_path;
    PyObject *codec_search_cache;
    PyObject *codec_error_registry;
    int codecs_initialized;

    /* fs_codec.encoding is initialized to NULL.
       Later, it is set to a non-NULL string by _PyUnicode_InitEncodings(). */
    struct {
        char *encoding;   /* Filesystem encoding (encoded to UTF-8) */
        int utf8;         /* encoding=="utf-8"? */
        char *errors;     /* Filesystem errors (encoded to UTF-8) */
        _Py_error_handler error_handler;
    } fs_codec;

    PyConfig config;
#ifdef HAVE_DLOPEN
    int dlopenflags;
#endif

    PyObject *dict;  /* Stores per-interpreter state */

    PyObject *builtins_copy;
    PyObject *import_func;
    /* Initialized to PyEval_EvalFrameDefault(). */
    _PyFrameEvalFunction eval_frame;

    _PyRecursiveMutex consts_mutex;
    struct _Py_hashtable_t *consts;

    Py_ssize_t co_extra_user_count;
    freefunc co_extra_freefuncs[MAX_CO_EXTRA_USERS];

#ifdef HAVE_FORK
    PyObject *before_forkers;
    PyObject *after_forkers_parent;
    PyObject *after_forkers_child;
#endif
    /* AtExit module */
    void (*pyexitfunc)(PyObject *);
    PyObject *pyexitmodule;

    uint64_t tstate_next_unique_id;

    struct _warnings_runtime_state warnings;

    PyObject *audit_hooks;

    struct {
        struct {
            int level;
            int atbol;
        } listnode;
    } parser;
};

PyAPI_FUNC(struct _is*) _PyInterpreterState_LookUpID(PY_INT64_T);

PyAPI_FUNC(int) _PyInterpreterState_IDInitref(struct _is *);
PyAPI_FUNC(void) _PyInterpreterState_IDIncref(struct _is *);
PyAPI_FUNC(void) _PyInterpreterState_IDDecref(struct _is *);
PyAPI_FUNC(void) _PyInterpreterState_WaitForThreads(struct _is *);



/* cross-interpreter data registry */

/* For now we use a global registry of shareable classes.  An
   alternative would be to add a tp_* slot for a class's
   crossinterpdatafunc. It would be simpler and more efficient. */

struct _xidregitem;

struct _xidregitem {
    PyTypeObject *cls;
    crossinterpdatafunc getdata;
    struct _xidregitem *next;
};

/* runtime audit hook state */

typedef struct _Py_AuditHookEntry {
    struct _Py_AuditHookEntry *next;
    Py_AuditHookFunction hookCFunction;
    void *userData;
} _Py_AuditHookEntry;

/* GIL state */

struct _gilstate_runtime_state {
    int check_enabled;
    /* Assuming the current thread holds the GIL, this is the
       PyThreadState for the current thread. */
    _Py_atomic_address tstate_current;
    PyThreadFrameGetter getframe;
    /* The single PyInterpreterState used by this process'
       GILState implementation
    */
    /* TODO: Given interp_main, it may be possible to kill this ref */
    PyInterpreterState *autoInterpreterState;
    Py_tss_t autoTSSkey;
};

/* hook for PyEval_GetFrame(), requested for Psyco */
#define _PyThreadState_GetFrame _PyRuntime.gilstate.getframe

/* Issue #26558: Flag to disable PyGILState_Check().
   If set to non-zero, PyGILState_Check() always return 1. */
#define _PyGILState_check_enabled _PyRuntime.gilstate.check_enabled


/* Full Python runtime state */

typedef struct pyruntimestate {
    /* Is running Py_PreInitialize()? */
    int preinitializing;

    /* Is Python preinitialized? Set to 1 by Py_PreInitialize() */
    int preinitialized;

    /* Is Python core initialized? Set to 1 by _Py_InitializeCore() */
    int core_initialized;

    /* Is Python fully initialized? Set to 1 by Py_Initialize() */
    int initialized;

    /* Is Python stopping all threads? */
    int stop_the_world;

    /* Set by Py_FinalizeEx(). Only reset to NULL if Py_Initialize()
       is called again. */
    PyThreadState *finalizing;

    struct pyinterpreters {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        /* _next_interp_id is an auto-numbered sequence of small
           integers.  It gets initialized in _PyInterpreterState_Enable(),
           which is called in Py_Initialize(), and used in
           PyInterpreterState_New().  A negative interpreter ID
           indicates an error occurred.  The main interpreter will
           always have an ID of 0.  Overflow results in a RuntimeError.
           If that becomes a problem later then we can adjust, e.g. by
           using a Python int. */
        int64_t next_id;
    } interpreters;
    // XXX Remove this field once we have a tp_* slot.
    struct _xidregistry {
        PyThread_type_lock mutex;
        struct _xidregitem *head;
    } xidregistry;

    struct qsbr_shared qsbr;

    unsigned long main_thread;
    PyThreadState *main_tstate;

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _ceval_runtime_state ceval;
    struct _gilstate_runtime_state gilstate;

    PyPreConfig preconfig;

    Py_OpenCodeHookFunction open_code_hook;
    void *open_code_userdata;
    _Py_AuditHookEntry *audit_hook_head;

    /* Used for types for now */
    _PyMutex mutex;

    _PyMutex stoptheworld_mutex;

    intptr_t ref_total;

    // XXX Consolidate globals found via the check-c-globals script.
} _PyRuntimeState;

#define _PyRuntimeState_INIT \
    {.preinitialized = 0, .core_initialized = 0, .initialized = 0}
/* Note: _PyRuntimeState_INIT sets other fields to 0/NULL */

PyAPI_DATA(_PyRuntimeState) _PyRuntime;
PyAPI_FUNC(PyStatus) _PyRuntimeState_Init(_PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyRuntimeState_Fini(_PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyRuntimeState_ReInitThreads(_PyRuntimeState *runtime);

PyAPI_FUNC(void) _PyRuntimeState_StopTheWorld(_PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyRuntimeState_StartTheWorld(_PyRuntimeState *runtime);

PyAPI_FUNC(intptr_t) _PyRuntimeState_GetRefTotal(void);

/* Initialize _PyRuntimeState.
   Return NULL on success, or return an error message on failure. */
PyAPI_FUNC(PyStatus) _PyRuntime_Initialize(void);

PyAPI_FUNC(void) _PyRuntime_Finalize(void);

#define _Py_CURRENTLY_FINALIZING(runtime, tstate) \
    (runtime->finalizing == tstate)

PyAPI_FUNC(int) _Py_IsMainInterpreter(PyThreadState* tstate);


/* Variable and macro for in-line access to current thread
   and interpreter state */
#if defined(__GNUC__) && !defined(Py_ENABLE_SHARED)
__attribute__((tls_model("local-exec")))
#endif
extern Py_DECL_THREAD PyThreadState *_Py_current_tstate;

static inline PyThreadState *
_PyRuntimeState_GetThreadState(_PyRuntimeState *runtime)
{
    return _Py_current_tstate;
}

static inline void
_PyRuntimeState_SetThreadState(_PyRuntimeState *runtime, PyThreadState *tstate)
{
    _Py_current_tstate = tstate;
}

PyAPI_FUNC(void) _PyThreadState_Shutdown(PyThreadState *tstate);

static inline void
_PyThreadState_CheckForShutdown(PyThreadState *tstate)
{
    PyThreadState *finalizing = _Py_atomic_load_ptr_relaxed(&_PyRuntime.finalizing);
    if (finalizing != NULL && finalizing != tstate) {
        _PyThreadState_Shutdown(tstate);
    }
}


/* Get the current Python thread state.

   Efficient macro reading directly the 'gilstate.tstate_current' atomic
   variable. The macro is unsafe: it does not check for error and it can
   return NULL.

   The caller must hold the GIL.

   See also PyThreadState_Get() and PyThreadState_GET(). */
#define _PyThreadState_GET() _PyRuntimeState_GetThreadState(&_PyRuntime)

/* Redefine PyThreadState_GET() as an alias to _PyThreadState_GET() */
#undef PyThreadState_GET
#define PyThreadState_GET() _PyThreadState_GET()

/* Get the current interpreter state.

   The macro is unsafe: it does not check for error and it can return NULL.

   The caller must hold the GIL.

   See also _PyInterpreterState_Get()
   and _PyGILState_GetInterpreterStateUnsafe(). */
#define _PyInterpreterState_GET_UNSAFE() (_PyThreadState_GET()->interp)


/* Other */
struct brc_queued_object;

struct PyThreadStateOS {
    PyThreadState *tstate;

    struct _PyBrcState {
        struct llist_node node;
        uintptr_t thread_id;
        struct brc_queued_object *queue;
    } brc;
};

PyAPI_FUNC(void) _PyThreadState_Init(
    PyThreadState *tstate);
PyAPI_FUNC(PyThreadState *) _PyThreadState_UnlinkExceptCurrent(
    _PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyThreadState_DeleteGarbage(PyThreadState *garbage);
PyAPI_FUNC(int) _PyThreadState_GetStatus(PyThreadState *tstate);
PyAPI_FUNC(void) _PyThreadState_GC_Park(PyThreadState *tstate);
PyAPI_FUNC(void) _PyThreadState_GC_Stop(PyThreadState *tstate);
PyAPI_FUNC(void) _PyThreadState_Signal(PyThreadState *tstate, uintptr_t bit);
PyAPI_FUNC(void) _PyThreadState_Unsignal(PyThreadState *tstate, uintptr_t bit);

PyAPI_FUNC(PyThreadState *) _PyThreadState_Swap(
    struct _gilstate_runtime_state *gilstate,
    PyThreadState *newts);

PyAPI_FUNC(PyStatus) _PyInterpreterState_Enable(_PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyInterpreterState_DeleteExceptMain(_PyRuntimeState *runtime);

/* Used by _PyImport_Cleanup() */
extern void _PyInterpreterState_ClearModules(PyInterpreterState *interp);

PyAPI_FUNC(void) _PyGILState_Reinit(_PyRuntimeState *runtime);


PyAPI_FUNC(int) _PyState_AddModule(
    PyThreadState *tstate,
    PyObject* module,
    struct PyModuleDef* def);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYSTATE_H */
