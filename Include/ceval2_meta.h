#ifndef Py_CEVAL2_META_H
#define Py_CEVAL2_META_H

#ifdef __cplusplus
extern "C" {
#endif

typedef union _Register {
    int64_t as_int64;
} Register;

PyObject *empty_tuple;

#define REFCOUNT_TAG 0x0
#define NO_REFCOUNT_TAG 0x1
#define REFCOUNT_MASK 0x1

#define PRI_TAG 0x4
#define PRI_TRUE 0x2

#define FRAME_C 0x1

// number of extra words in a function frame
#define FRAME_EXTRA 3

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

#define PACK(o, tag) ((Register)((intptr_t)o | tag))

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

struct ThreadState {
    // registers for current function (points within stack)
    Register *regs;

    const uint32_t *pc;

    Py_ssize_t cframe_size;

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
const uint32_t *
vm_is_true(Register acc, const uint32_t *next_instr, intptr_t opD);
const uint32_t *
vm_is_false(Register acc, const uint32_t *next_instr, intptr_t opD);

void
vm_unpack_sequence(Register acc, Register *base, Py_ssize_t n);

Register vm_load_name(Register *regs, PyObject *name);
int vm_store_global(PyObject *dict, PyObject *name, Register value);
int vm_load_method(struct ThreadState *ts, PyObject *owner, PyObject *name, int opA);

Register
vm_import_name(struct ThreadState *ts, PyFunc *this_func, PyObject *arg);
Register vm_build_list(Register *regs, Py_ssize_t n);
Register vm_build_tuple(Register *regs, Py_ssize_t n);
Register vm_build_slice(Register *regs);
Register vm_list_append(Register list, Register item);

Register vm_call_cfunction(struct ThreadState *ts, Register *regs, int nargs);
Register vm_call_function(struct ThreadState *ts, int base, int nargs);

Register
vm_make_function(struct ThreadState *ts, PyCodeObject2 *code);
Register vm_setup_cells(struct ThreadState *ts, PyCodeObject2 *code);
Register vm_setup_freevars(struct ThreadState *ts, PyCodeObject2 *code);

Register
vm_load_build_class(struct ThreadState *ts, PyObject *builtins, int opA);

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
