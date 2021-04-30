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
#define CFRAME_EXTRA    4


/*

  idx      Python frame
       +-------------------+
  -4   |    frame delta    |
       |- - - - - - - - - -|
  -3   |     constants     |
       |- - - - - - - - - -|
  -2   |  frame link | tag |
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
       +-------------------+
  -3   |    frame size     |
       |- - - - - - - - - -|
  -2   |  frame link | tag |
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

static inline Register
_PACK_INCREF(PyObject *obj, intptr_t tid)
{
    Register r;
    r.as_int64 = (intptr_t)obj;
    if ((obj->ob_ref_local & 0x3) == 0) {
        _Py_INCREF_TOTAL
        if (LIKELY(_Py_ThreadMatches(obj, tid))) {
            uint32_t refcount = obj->ob_ref_local;
            refcount += 4;
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

#define XCLEAR(reg) do {    \
    Register _tmp = (reg);  \
    (reg).as_int64 = 0;     \
    if (_tmp.as_int64 != 0) \
        DECREF(_tmp);       \
} while (0)

struct _ts;

// struct VirtualThread {};

// typedef struct {
//     struct PyVirtualThread thread;
// } PyGenObject2;

enum {
    THREAD_THREAD = 1,
    THREAD_GENERATOR = 2,
    THREAD_COROUTINE = 3
};

struct ThreadState {
    // registers for current function (points within stack)
    Register *regs;

    // Next instruction to be executed. Updated before calling into ceval_meta.
    const uint8_t *pc;

    // true bottom of stack
    Register *stack;

    // top of stack
    Register *maxstack;

    struct _ts *ts;

    struct ThreadState *prev;

    char thread_type;
};

struct PyVirtualThread {
    PyObject_HEAD
    struct ThreadState thread;
};

// Auxiliary frame data
struct FrameAux {
    uint8_t code;
    PyObject *frame;
    PyObject *locals;
    intptr_t frame_link;
};

// ceval2.c
PyObject *_PyEval_Fast(struct ThreadState *ts, Register acc, const uint8_t *pc);
PyObject *PyEval2_EvalGen(PyGenObject2 *gen, PyObject *opt_value);
PyObject *PyEval2_GetGlobals(void);
PyObject *PyEval2_GetLocals(void);

// Used by pstate.c
struct ThreadState *vm_new_threadstate(PyThreadState *tstate);

// used by genobject2.c
int vm_traverse_stack(struct ThreadState *ts, visitproc visit, void *arg);

// used by errors.c
PyObject *vm_traceback_here(struct ThreadState *ts);


// Used by funcobject2.c.
PyObject *vm_builtins_from_globals(PyObject *globals);

// private
PyObject *_PyFunc_Call(PyObject *func, PyObject *args, PyObject *kwds);
PyObject *_PyFunc_Vectorcall(PyObject *func, PyObject* const* stack,
                             size_t nargsf, PyObject *kwnames);
PyObject *_Py_method_call(PyObject *obj, PyObject *args, PyObject *kwds);


struct _PyCodeObject2;
typedef struct _PyCodeObject2 PyCodeObject2;

Register vm_unknown_opcode(intptr_t opcode);

int vm_raise(struct ThreadState *ts, PyObject *exc);
int vm_reraise(struct ThreadState *ts, Register exc);

Register vm_setup_with(struct ThreadState *ts, Py_ssize_t opA);
Register vm_setup_async_with(struct ThreadState *ts, Py_ssize_t opA);
int vm_setup_annotations(struct ThreadState *ts, PyObject *locals);
int vm_exit_with(struct ThreadState *ts, Py_ssize_t opA);
int vm_exit_async_with(struct ThreadState *ts, Py_ssize_t opA);
int vm_exit_with_res(struct ThreadState *ts, Py_ssize_t opA, PyObject *exit_res);

PyObject *
vm_handled_exc(struct ThreadState *ts);

const uint8_t *vm_exception_unwind(struct ThreadState *ts, bool skip_first_frame);

// decrefs x!
Register vm_to_bool(Register x);

int vm_unpack(struct ThreadState *ts, PyObject *v, Py_ssize_t base,
              Py_ssize_t argcnt, Py_ssize_t argcntafter);

typedef PyObject* (*intrinsic1)(PyObject *arg);
typedef PyObject* (*intrinsicN)(PyObject *const *args, Py_ssize_t n);

extern union intrinsic {
    intrinsic1 intrinsic1;
    intrinsicN intrinsicN;
} intrinsics_table[];

PyObject *
vm_call_intrinsic(struct ThreadState *ts, Py_ssize_t id, Py_ssize_t opA, Py_ssize_t nargs);

PyObject *vm_load_name(struct ThreadState *ts, PyObject *locals, PyObject *name);
PyObject *vm_load_global(struct ThreadState *ts, PyObject *key, intptr_t *meta);
Register vm_load_class_deref(struct ThreadState *ts, Py_ssize_t opA, PyObject *name);
PyObject *vm_name_error(struct ThreadState *ts, PyObject *name);
int vm_delete_name(struct ThreadState *ts, PyObject *name);
int vm_load_method(struct ThreadState *ts, PyObject *owner, PyObject *name, int opA);

void vm_err_non_iterator(struct ThreadState *ts, PyObject *o);
void vm_err_awaitable(struct ThreadState *ts, Register acc);
void vm_err_coroutine_awaited(struct ThreadState *ts);
void vm_err_unbound(struct ThreadState *ts, Py_ssize_t idx);
void vm_err_async_for_aiter(struct ThreadState *ts, PyTypeObject *type);
void vm_err_async_for_no_anext(struct ThreadState *ts, PyTypeObject *type);
void vm_err_async_for_anext_invalid(struct ThreadState *ts, Register res);
void vm_err_dict_update(struct ThreadState *ts, Register acc);

PyObject *vm_import_name(struct ThreadState *ts, PyFunc *this_func, PyObject *arg);
PyObject *vm_import_from(struct ThreadState *ts, PyObject *v, PyObject *name);
int vm_import_star(struct ThreadState *ts, PyObject *module, PyObject *locals);


Register vm_build_set(struct ThreadState *ts, Py_ssize_t base, Py_ssize_t n);
Register vm_tuple_prepend(PyObject *tuple, PyObject *obj);
PyObject *vm_build_slice(struct ThreadState *ts, Py_ssize_t base);

int vm_callargs_to_tuple(struct ThreadState *ts);
int vm_kwargs_to_dict(struct ThreadState *ts);

PyObject *vm_call_cfunction(struct ThreadState *ts, Register acc);
PyObject *vm_call_function(struct ThreadState *ts, Register acc);
PyObject *vm_tpcall_function(struct ThreadState *ts, Register acc);

intptr_t vm_frame_clear_aux(intptr_t frame_link);

Register
vm_make_function(struct ThreadState *ts, PyCodeObject2 *code);

int duplicate_keyword_argument(struct ThreadState *ts, PyCodeObject2 *co, PyObject *keyword);
int missing_arguments(struct ThreadState *ts);
int too_many_positional(struct ThreadState *ts, Py_ssize_t posargcount);


int vm_setup_ex(struct ThreadState *ts, PyCodeObject2 *co, Register acc);
int vm_setup_varargs(struct ThreadState *ts, PyCodeObject2 *co, Register acc);
int vm_setup_kwargs(struct ThreadState *ts, PyCodeObject2 *co, Register acc, PyObject **kwnames);
int vm_setup_cells(struct ThreadState *ts, PyCodeObject2 *code);
void vm_setup_err(struct ThreadState *ts, Register acc);

Register
vm_load_build_class(struct ThreadState *ts, PyObject *builtins);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

const uint8_t *
vm_exc_match(struct ThreadState *ts, PyObject *tp, PyObject *exc, const uint8_t *pc, int opD);

void vm_free_threadstate(struct ThreadState *ts);
int vm_for_iter_exc(struct ThreadState *ts);
PyObject *vm_get_iter(PyObject *obj);
int vm_end_async_for(struct ThreadState *ts, Py_ssize_t opA);

int
vm_init_thread_state(struct ThreadState *old, struct ThreadState *ts);

#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL_H */
