#ifndef Py_CEVAL2_META_H
#define Py_CEVAL2_META_H

#ifdef __cplusplus
extern "C" {
#endif

typedef union _Register {
    int64_t as_int64;
    PyObject *obj;
} Register;

PyObject *empty_tuple;

#define INT32_TAG 0x2
#define REFCOUNT_TAG 0x1
#define PRI_TAG 0x4
#define PRI_TRUE 0x2

#define FRAME_C 0x1

// number of extra words in a function frame
#define FRAME_EXTRA 2

static inline Register
PACK_INT32(int32_t value)
{
    Register r;
    r.as_int64 = INT32_TAG | ((int64_t)value << 32);
    return r;
}

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
    return (r.as_int64 & INT32_TAG) == 0;
}

static inline bool
IS_RC(Register r)
{
    return (r.as_int64 & REFCOUNT_TAG) != 0;
}

static inline bool
IS_INT32(Register r)
{
    return (r.as_int64 & INT32_TAG) != 0;
}

static inline int32_t
AS_INT32(Register r)
{
    return (r.as_int64 >> 32);
}

static inline PyObject *
AS_OBJ(Register r)
{
    return (PyObject *)(r.as_int64 & ~REFCOUNT_TAG);
}

static inline Register
PACK_OBJ(PyObject *o)
{
    Register r;
    r.obj = o;
    if (!_PyObject_IS_IMMORTAL(o)) {
        r.as_int64 |= REFCOUNT_TAG;
    }
    return r;
}

struct _ts;

struct ThreadState {
    // registers for current function (points within stack)
    Register *regs;

    const uint32_t *pc;

    // true bottom of stack
    Register *stack;

    // top of stack
    Register *maxstack;

    // Retained objects
    PyObject **maxrefs;

    // Base of retained object
    PyObject **refs;

    // Base of retained object
    PyObject **refs_base;

    Py_ssize_t nargs;

    // current metadata ???
    uint16_t *metadata;

    struct _ts *ts;

    void **opcode_targets;//[256];
};

// basically PyCodeObject ?
// typedef struct _PyFunc {
//     PyObject_HEAD;
//     // frame size
//     // number of parameters?
//     Code *code;
//     Register *constants;
//     intptr_t nargs;
//     Py_ssize_t framesize;
//     PyObject *globals;
//     // code
//     // constants
// } PyFunc;


// ceval2.c
PyObject* _PyEval_Fast(struct ThreadState *ts);

struct _PyCodeObject2;
typedef struct _PyCodeObject2 PyCodeObject2;

PyObject *
exec_code2(PyCodeObject2 *code, PyObject *globals);


Register vm_compare(Register a, Register b);

Register vm_unknown_opcode(intptr_t opcode);

// decrefs x!
Register vm_to_bool(Register x);

// decrefs acc!
Register vm_add(Register x, Register acc);
Register vm_add_slow(Register x, Register acc);
Register vm_inplace_add(Register x, Register acc);
Register vm_sub(Register x, Register acc);
Register vm_mul(Register a, Register acc);
Register vm_true_div(Register a, Register acc);
Register vm_floor_div(Register a, Register acc);

Register vm_load_name(PyObject *dict, PyObject *name);
Register vm_store_global(PyObject *dict, PyObject *name, Register value);

Register vm_build_list(Register *regs, Py_ssize_t n);
Register vm_build_tuple(Register *regs, Py_ssize_t n);
Register vm_list_append(Register list, Register item);

Register vm_call_cfunction(struct ThreadState *ts, Register *regs, int nargs);
Register vm_call_function(struct ThreadState *ts, int base, int nargs);

Register
vm_make_function(struct ThreadState *ts, PyCodeObject2 *code);
Register vm_setup_cells(struct ThreadState *ts, PyCodeObject2 *code);
Register vm_setup_freevars(struct ThreadState *ts, PyCodeObject2 *code);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

PyObject *vm_args_error(struct ThreadState *ts);

PyObject *vm_error_not_callable(struct ThreadState *ts);
void vm_handle_error(struct ThreadState *ts);

// void vm_zero_refcount(PyObject *op);
void vm_decref_shared(PyObject *op);
void vm_incref_shared(PyObject *op);

PyObject *vm_new_func(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL_H */
