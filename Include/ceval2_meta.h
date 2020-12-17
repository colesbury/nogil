#ifndef Py_CEVAL2_META_H
#define Py_CEVAL2_META_H

#ifdef __cplusplus
extern "C" {
#endif

typedef union _Register {
    int64_t as_int64;
    PyObject *obj;
} Register;

struct ThreadState {
    const uint32_t *pc;

    // true bottom of stack
    Register *stack;

    // top of stack
    Register *maxstack;

    // registers for current function (points within stack)
    Register *regs;

    // constants for current function
    // TODO: remove
    const Register *constants;
    Py_ssize_t nargs;


    // current metadata ???
    uint16_t *metadata;

    void **opcode_targets;//[256];
};

typedef uint32_t Code;

typedef struct {
    PyObject_HEAD
    Code *first_instr;  // can get PyCodeObject2 via offset
    PyObject *globals;
    // closure... LuaJit has closed-over variables as flexiable array member
} PyFunc;


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

Register vm_compare(Register a, Register b);

Register vm_unknown_opcode(intptr_t opcode);

// decrefs x!
Register vm_to_bool(Register x);

// decrefs acc!
Register vm_add(Register x, Register acc);

Register vm_load_name(PyObject *dict, PyObject *name);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

int vm_resize_stack(struct ThreadState *ts, Py_ssize_t needed);

PyObject *vm_args_error(struct ThreadState *ts);

PyObject *vm_error_not_callable(struct ThreadState *ts);

void vm_zero_refcount(PyObject *op);
void vm_decref_shared(PyObject *op);
void vm_incref_shared(PyObject *op);

PyObject *vm_new_func(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL_H */
