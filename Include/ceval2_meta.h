#ifndef Py_CEVAL2_META_H
#define Py_CEVAL2_META_H

#ifdef __cplusplus
extern "C" {
#endif

typedef union _Register {
    int64_t as_int64;
} Register;

#define REFCOUNT_TAG 0x0
#define NO_REFCOUNT_TAG 0x1
#define REFCOUNT_MASK 0x1

#define PRI_TAG 0x4
#define PRI_TRUE 0x2

enum {
    FRAME_PYTHON = 0,
    FRAME_C = 1,
    FRAME_GENERATOR = 3,
    FRAME_TAG_MASK = 3
};

#define FRAME_EXTRA     3
#define CFRAME_EXTRA    4

/*

  idx      Python frame
       +-------------------+
  -4   |   [frame delta]   |
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

static inline Register
PACK_BOOL(bool value)
{
    Register r;
    r.as_int64 = PRI_TAG | (((int64_t)value + 1) << 32);
    return r;
}

static inline bool
IS_PRI(Register r)
{
    return (r.as_int64 & PRI_TAG) != 0;
}

static inline int32_t
AS_PRI(Register r)
{
    return (r.as_int64 >> 32);
}

static inline bool
IS_OBJ(Register r)
{
    return (r.as_int64 & 0x2) == 0;
}

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

#define PACK(o, tag) ((Register){(intptr_t)o | tag})

static inline Register
PACK_OBJ(PyObject *o)
{
    Register r;
    r.as_int64 = (intptr_t)o | _PyObject_IS_IMMORTAL(o);
    return r;
}

static inline Register
PACK_INCREF(PyObject *obj)
{
    Register r;
    r.as_int64 = (intptr_t)obj;
    if ((obj->ob_ref_local & 0x3) == 0) {
        _Py_INCREF_TOTAL
        if (LIKELY(_Py_ThreadLocal(obj))) {
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
    const uint32_t *next_instr;

    // true bottom of stack
    Register *stack;

    // top of stack
    Register *maxstack;

    // Currently handled exception
    PyObject *handled_exc;

    // current metadata ???
    uint16_t *metadata;

    char thread_type;

    struct _ts *ts;
};

struct PyVirtualThread {
    PyObject_HEAD
    struct ThreadState thread;
};

// ceval2.c
PyObject* _PyEval_Fast(struct ThreadState *ts, Py_ssize_t nargs, const uint32_t *pc);

struct _PyCodeObject2;
typedef struct _PyCodeObject2 PyCodeObject2;

PyObject *
exec_code2(PyCodeObject2 *code, PyObject *globals);


Register vm_compare(Register a, Register b);

Register vm_unknown_opcode(intptr_t opcode);

int
vm_raise(struct ThreadState *ts, PyObject *err);

void
vm_reraise(struct ThreadState *ts, PyObject *err);

Register
vm_setup_with(struct ThreadState *ts, Py_ssize_t opA);
int
vm_exit_with(struct ThreadState *ts, Py_ssize_t opA);

const uint32_t *
vm_exception_unwind(struct ThreadState *ts, const uint32_t *next_instr);

// decrefs x!
Register vm_to_bool(Register x);
const uint32_t *
vm_is_true(Register acc, const uint32_t *next_instr, intptr_t opD);
const uint32_t *
vm_is_false(Register acc, const uint32_t *next_instr, intptr_t opD);

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

Register vm_load_name(Register *regs, PyObject *name);
Register vm_load_class_deref(struct ThreadState *ts, Py_ssize_t opA, PyObject *name);
int vm_delete_name(struct ThreadState *ts, PyObject *name);
int vm_store_global(PyObject *dict, PyObject *name, Register value);
int vm_load_method(struct ThreadState *ts, PyObject *owner, PyObject *name, int opA);

Register
vm_import_name(struct ThreadState *ts, PyFunc *this_func, PyObject *arg);
PyObject *
vm_import_from(struct ThreadState *ts, PyObject *v, PyObject *name);

Register vm_build_list(Register *regs, Py_ssize_t n);
Register vm_build_set(struct ThreadState *ts, Py_ssize_t base, Py_ssize_t n);
Register vm_build_tuple(struct ThreadState *ts, Py_ssize_t base, Py_ssize_t n);
Register vm_build_slice(Register *regs);

PyObject *vm_call_cfunction(struct ThreadState *ts, Py_ssize_t nargs);
PyObject *vm_tpcall_function(struct ThreadState *ts, Py_ssize_t nargs);
PyObject *vm_call_function(struct ThreadState *ts, Py_ssize_t nargs);

Register
vm_make_function(struct ThreadState *ts, PyCodeObject2 *code);
int vm_setup_cells_freevars(struct ThreadState *ts, PyCodeObject2 *code);
int vm_setup_kwargs(struct ThreadState *ts, PyCodeObject2 *code, int64_t flags);
int vm_setup_default_args(struct ThreadState *ts, PyCodeObject2 *co, Register acc);

Register
vm_load_build_class(struct ThreadState *ts, PyObject *builtins, int opA);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

PyObject *vm_args_error(struct ThreadState *ts);

PyObject *vm_error_not_callable(struct ThreadState *ts);
void vm_handle_error(struct ThreadState *ts);
const uint32_t *
vm_exc_match(struct ThreadState *ts, PyObject *tp, const uint32_t *next_instr, int opD);

// void vm_zero_refcount(PyObject *op);
void vm_decref_shared(PyObject *op);
void vm_incref_shared(PyObject *op);

PyObject *vm_new_func(void);

struct ThreadState *
new_threadstate(void);

void vm_free_threadstate(struct ThreadState *ts);
int vm_for_iter_exc(struct ThreadState *ts);

int
vm_init_thread_state(struct ThreadState *old, struct ThreadState *ts);

#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL_H */
