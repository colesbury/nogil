#ifndef Py_CEVAL2_META_H
#define Py_CEVAL2_META_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union _Register {
    int64_t as_int64;
} Register;

#define REFCOUNT_TAG 0x0
#define NO_REFCOUNT_TAG 0x1
#define NON_OBJECT_TAG 0x3
#define REFCOUNT_MASK 0x1

enum {
    FRAME_GENERATOR = -1,
    FRAME_AUX_STATE = -2,
};

#define FRAME_EXTRA     4
#define CALLARGS_IDX    (-FRAME_EXTRA-2)
#define KWARGS_IDX      (-FRAME_EXTRA-1)


/*

  idx      Python frame
       +-------------------+
  -4   |    frame delta    |
       |- - - - - - - - - -|
  -3   |    frame link     |
       |- - - - - - - - - -|
  -2   |  [PyFrameObject]  |
       |- - - - - - - - - -|
  -1   |      PyFunc       |
  -----+-------------------+---
   0   |     argument 0    | <- regs
  ...  |        ...        |
  n-1  |    argument n-1   |
       |- - - - - - - - - -|
   n   |      local 0      |
  ...  |        ...        |
  n+k  |     local k-1     |
       |- - - - - - - - - -|
 n+k+1 |    temporary 0    |
  ...  |        ...        |
 n+k+t |   temporary t-1   |
  -----+-------------------+


  idx     C function frame
       +-------------------+
  -4   |    frame delta    |
       |- - - - - - - - - -|
  -3   |    frame link     |
       |- - - - - - - - - -|
  -2   |    frame size     |
       |- - - - - - - - - -|
  -1   |     PyObject      |
  -----+-------------------+---
   0   |     argument 0    | <- regs
  ...  |        ...        |
  n-1  |    argument n-1   |
  -----+-------------------+
*/
#define UNLIKELY _PY_UNLIKELY
#define LIKELY _PY_LIKELY

static inline bool
IS_RC(Register r)
{
    return (r.as_int64 & REFCOUNT_MASK) == REFCOUNT_TAG;
}

static inline PyObject *
AS_OBJ(Register r)
{
    return (PyObject *)(r.as_int64 & ~REFCOUNT_MASK);
}

#define PACK(o, tag) ((Register){((intptr_t)(o)) | tag})

static inline Register
PACK_OBJ(PyObject *o)
{
    Register r;
    r.as_int64 = (intptr_t)o | _PyObject_IS_IMMORTAL(o);
    return r;
}

#define PACK_INCREF(op) _PACK_INCREF(op, _Py_ThreadId())

static _Py_ALWAYS_INLINE Register
_PACK_INCREF(PyObject *obj, intptr_t tid)
{
    Register r;
    r.as_int64 = (intptr_t)obj;
    uint32_t refcount = obj->ob_ref_local;
    if ((refcount & (_Py_REF_IMMORTAL_MASK | _Py_REF_DEFERRED_MASK)) == 0) {
        _Py_INC_REFTOTAL;
        if (LIKELY(_Py_ThreadMatches(obj, tid))) {
            refcount += (1 << _Py_REF_LOCAL_SHIFT);
            obj->ob_ref_local = refcount;
        }
        else {
            _Py_atomic_add_uint32(&obj->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT));
        }
    }
    else {
        r.as_int64 |= NO_REFCOUNT_TAG;
    }
    return r;
}

static inline Register
STRONG_REF(Register r)
{
    if (!IS_RC(r)) {
        return PACK_INCREF(AS_OBJ(r));
    }
    return r;
}

#define CLEAR(reg) do {     \
    Register _tmp = (reg);  \
    (reg).as_int64 = 0;     \
    DECREF(_tmp);           \
} while (0)

#define XCLEAR(reg) do {        \
    Register _tmp = (reg);      \
    if (_tmp.as_int64 != 0) {   \
        (reg).as_int64 = 0;     \
        DECREF(_tmp);           \
    }                           \
} while (0)

struct _ts;
struct _frame;

enum {
    THREAD_THREAD = 1,
    THREAD_GENERATOR = 2
};

typedef struct _PyThreadStack {
    // registers for current function (points within stack)
    Register *regs;

    // Next instruction to be executed. Updated before calling into ceval_meta.
    const uint8_t *pc;

    // true bottom of stack
    Register *stack;

    // top of stack
    Register *maxstack;

    struct _ts *ts;

    struct _PyThreadStack *prev;

    char thread_type;
    char gc_visited;
} _PyThreadStack;

// ceval2.c
PyObject *_PyEval_Fast(PyThreadState *ts, Register acc, const uint8_t *pc);
PyObject *PyEval2_EvalGen(PyGenObject *gen, PyObject *opt_value);

// Used by pstate.c
_PyThreadStack *vm_new_threadstate(PyThreadState *tstate);
void vm_free_threadstate(_PyThreadStack *ts);
_PyThreadStack *vm_active(PyThreadState *tstate);

void vm_push_thread_stack(PyThreadState *tstate, _PyThreadStack *ts);
void vm_pop_thread_stack(PyThreadState *tstate);

PyObject *vm_locals(struct _frame *frame);

// used by genobject2.c
PyObject *vm_compute_cr_origin(PyThreadState *ts);

struct _frame *vm_frame(PyThreadState *ts);
struct _frame *vm_frame_at_offset(_PyThreadStack *ts, Py_ssize_t offset);
void vm_clear_frame(PyThreadState *ts);
Py_ssize_t vm_regs_frame_size(Register *regs);

// private
PyObject *_PyFunc_Call(PyObject *func, PyObject *args, PyObject *kwds);
PyObject *_Py_method_call(PyObject *obj, PyObject *args, PyObject *kwds);

Register vm_unknown_opcode(intptr_t opcode);

int vm_raise(PyThreadState *ts, PyObject *exc);
int vm_reraise(PyThreadState *ts, Register exc);

Register vm_setup_with(PyThreadState *ts, Py_ssize_t opA);
Register vm_setup_async_with(PyThreadState *ts, Py_ssize_t opA);
int vm_setup_annotations(PyThreadState *ts, PyObject *locals);
int vm_exit_with(PyThreadState *ts, Py_ssize_t opA);
int vm_exit_async_with(PyThreadState *ts, Py_ssize_t opA);
int vm_exit_with_res(PyThreadState *ts, Py_ssize_t opA, PyObject *exit_res);

PyObject *vm_handled_exc(PyThreadState *ts);
PyObject *vm_handled_exc2(_PyThreadStack *ts);
int vm_set_handled_exc(PyThreadState *ts, PyObject *exc);

const uint8_t *
vm_exception_unwind(PyThreadState *ts, Register acc, bool skip_first_frame);

void
vm_error_with_result(PyThreadState *ts, Register acc);

// decrefs x!
Register vm_to_bool(Register x);

int vm_unpack(PyThreadState *ts, PyObject *v, Py_ssize_t base,
              Py_ssize_t argcnt, Py_ssize_t argcntafter);

typedef PyObject* (*intrinsic1)(PyObject *arg);
typedef PyObject* (*intrinsicN)(PyObject *const *args, Py_ssize_t n);

extern union intrinsic {
    intrinsic1 intrinsic1;
    intrinsicN intrinsicN;
} intrinsics_table[];

PyObject *
vm_call_intrinsic(PyThreadState *ts, Py_ssize_t id, Py_ssize_t opA, Py_ssize_t nargs);

PyObject *vm_load_name(PyThreadState *ts, PyObject *locals, PyObject *name);
PyObject *vm_load_global(PyThreadState *ts, PyObject *key, intptr_t *meta);
PyObject *vm_try_load(PyObject *op, PyObject *key, intptr_t *meta);
Register vm_load_class_deref(PyThreadState *ts, Py_ssize_t opA, PyObject *name);

void vm_err_non_iterator(PyThreadState *ts, PyObject *o);
void vm_err_coroutine_awaited(PyThreadState *ts);
void vm_err_unbound(PyThreadState *ts, Py_ssize_t idx);
void vm_err_async_for_aiter(PyThreadState *ts, PyTypeObject *type);
void vm_err_async_for_no_anext(PyThreadState *ts, PyTypeObject *type);
void vm_err_async_for_anext_invalid(PyThreadState *ts, Register res);
void vm_err_async_with_aenter(PyThreadState *ts, Register acc);
void vm_err_dict_update(PyThreadState *ts, Register acc);
void vm_err_dict_merge(PyThreadState *ts, Register acc);
void vm_err_list_extend(PyThreadState *ts, Register acc);
PyObject *vm_err_name(PyThreadState *ts, int oparg);
PyObject *vm_load_method_err(PyThreadState *ts, Register acc);

PyObject *vm_import_name(PyThreadState *ts, PyFunctionObject *this_func, PyObject *arg);
PyObject *vm_import_from(PyThreadState *ts, PyObject *v, PyObject *name);
int vm_import_star(PyThreadState *ts, PyObject *module, PyObject *locals);


Register vm_build_set(PyThreadState *ts, Py_ssize_t base, Py_ssize_t n);
Register vm_tuple_prepend(PyObject *tuple, PyObject *obj);
PyObject *vm_build_slice(PyThreadState *ts, Py_ssize_t base);

int vm_callargs_to_tuple(PyThreadState *ts, Py_ssize_t idx);
int vm_kwargs_to_dict(PyThreadState *ts, Py_ssize_t idx);

PyObject *vm_call_cfunction(PyThreadState *ts, Register acc);
PyObject *vm_call_function(PyThreadState *ts, Register acc);
PyObject *vm_tpcall_function(PyThreadState *ts, Register acc);

Register
vm_make_function(PyThreadState *ts, PyCodeObject *code);

int duplicate_keyword_argument(PyThreadState *ts, PyCodeObject *co, PyObject *keyword);
int missing_arguments(PyThreadState *ts);
void too_many_positional(PyThreadState *ts, Register acc);


int vm_setup_ex(PyThreadState *ts, PyCodeObject *co, Register acc);
int vm_setup_varargs(PyThreadState *ts, PyCodeObject *co, Register acc);
int vm_setup_kwargs(PyThreadState *ts, PyCodeObject *co, Register acc, PyObject **kwnames);
int vm_setup_kwdefaults(PyThreadState *ts, Py_ssize_t idx);
int vm_setup_cells(PyThreadState *ts, PyCodeObject *code);
void vm_setup_err(PyThreadState *ts, Register acc);

Register
vm_load_build_class(PyThreadState *ts, PyObject *builtins);

int vm_resize_stack(PyThreadState *tstate, Py_ssize_t needed);

Py_ssize_t
vm_jump_side_table(PyThreadState *ts, const uint8_t *pc);

int
vm_exc_match(PyThreadState *ts, PyObject *tp, PyObject *exc);

int vm_for_iter_exc(PyThreadState *ts);
PyObject *vm_get_iter(PyObject *obj);
int vm_end_async_for(PyThreadState *ts, Py_ssize_t opA);

int
vm_init_thread_state(PyThreadState *tstate, PyGenObject *gen);

Py_ssize_t vm_stack_depth(PyThreadState *ts);

int vm_eval_breaker(PyThreadState *ts, const uint8_t *last_pc);
int vm_trace_handler(PyThreadState *ts, const uint8_t *last_pc, Register acc);
PyObject *vm_trace_cfunc(PyThreadState *ts, Register acc);
int vm_trace_return(PyThreadState *ts, PyObject *return_value);
void vm_trace_stop_iteration(PyThreadState *ts);


#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL_H */
