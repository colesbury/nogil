/* Definitions for bytecode */

#ifndef Py_LIMITED_API
#ifndef Py_CODE2_H
#define Py_CODE2_H
#ifdef __cplusplus
extern "C" {
#endif


PyAPI_DATA(PyTypeObject) PyCode2_Type;

#define PyCode2_Check(op) (Py_TYPE(op) == &PyCode2_Type)

enum {
    // number of arguments excluding keyword-only args, *args, and **kwargs
    // if more than 255 arguments, this value is zero and the overflow bit
    // is set.
    CODE_MASK_ARGS          = 0x0000ff, // bits 0-7

    // bits 8-15 are always zero in code (keyword arguments in acc)
    CODE_FLAG_UNUSED_1      = 0x00ff00, // bits 8-15 always zero

    // set if the function has a *args parameter
    CODE_FLAG_VARARGS       = 0x010000, // bit  16

    CODE_FLAG_UNUSED_2      = 0x020000, // bit  17 always zero

    // set if the function has a **kwargs parameter
    CODE_FLAG_VARKEYWORDS   = 0x040000, // bit  18

    // set if the code has cell variables (i.e. captured by other functions)
    CODE_FLAG_HAS_CELLS     = 0x080000, // bit  19

    // set if the code has free (captured) variables
    CODE_FLAG_HAS_FREEVARS  = 0x100000, // bit  20

    // set if there are ANY keyword only arguments
    CODE_FLAG_KWD_ONLY_ARGS = 0x200000, // bit  21

    // set if there more than 255 arguments
    CODE_FLAG_OVERFLOW      = 0x400000, // bit  22
};

enum {
    /* number of positional arguments */
    ACC_MASK_ARGS           = 0x0000ff,  // bits 0-7

    /* number of keyword arguments in call */
    ACC_MASK_KWARGS         = 0x00ff00,  // bits 8-15

    ACC_SHIFT_KWARGS        = 8,

    /* set if the caller uses *args */
    ACC_FLAG_VARARGS        = 0x010000,  // bit  16

    /* set if the caller uses **kwargs */
    ACC_FLAG_VARKEYWORDS    = 0x020000,  // bit  17
};

#define ACC_KWCOUNT(acc) (((acc.as_int64) & ACC_MASK_KWARGS) >> ACC_SHIFT_KWARGS)
#define ACC_ARGCOUNT(acc) ((acc.as_int64) & ACC_MASK_ARGS)

struct _PyHandlerTable;

typedef struct _PyCodeObject2 {
    PyObject_HEAD
    uint32_t co_packed_flags;
    int co_flags; // unused???
    Py_ssize_t co_argcount;     /* number of arguments excluding kwd-only, *args, and **kwargs */
    Py_ssize_t co_nlocals;      /* number of local variables (including arguments) */

    Py_ssize_t co_ndefaultargs;
    Py_ssize_t co_posonlyargcount;
    Py_ssize_t co_totalargcount; /* number of arguments including kwd-only, but not *args and **kwargs */

    Py_ssize_t co_framesize;    /* maximum stack usage */
    Py_ssize_t co_size;         /* size of instructions (in bytes) */
    Py_ssize_t co_nconsts;      /* number of constants */
    Py_ssize_t co_niconsts;     /* number of integer constants */
    Py_ssize_t co_ncells;
    Py_ssize_t co_nfreevars;    /* number of captured free variables (including default args) */

    PyObject **co_constants;    /* pointer to constants array */
    Py_ssize_t *co_iconstants;  /* integer constants */
    Py_ssize_t *co_cell2reg;
    Py_ssize_t *co_free2reg;

    struct _PyHandlerTable *co_exc_handlers;

    PyObject *co_weakreflist;

    Py_ssize_t co_nmeta;
    int co_firstlineno;
    PyObject *co_varnames;      /* tuple of strings (local variable names) */
    PyObject *co_freevars;      /* tuple of strings (free variable names) */
    PyObject *co_cellvars;      /* tuple of strings (cell variable names) */
    PyObject *co_filename;      /* unicode (where it was loaded from) */
    PyObject *co_name;          /* unicode (name, for reference) */
    PyObject *co_lnotab;        /* string (encoding addr<->lineno mapping) See */
} PyCodeObject2;

// PyAPI_FUNC(PyCodeObject2 *) PyCode2_New(PyObject *bytecode, PyObject *consts);
PyAPI_FUNC(PyCodeObject2 *)
PyCode2_New(Py_ssize_t instr_size, Py_ssize_t nconsts, Py_ssize_t niconsts,
            Py_ssize_t nmeta, Py_ssize_t ncells, Py_ssize_t ncaptured,
            Py_ssize_t nexc_handlers);


/* Return the line number associated with the specified bytecode index
   in this code object.  If you just need the line number of a frame,
   use PyFrame_GetLineNumber() instead. */
PyAPI_FUNC(int) PyCode2_Addr2Line(PyCodeObject2 *, int);

#define PyCode2_GET_CODE(co) (PyCode2_Code((PyCodeObject2 *)(co)))
#define PyCode2_FROM_FUNC(func) (PyCode2_FromInstr(((PyFuncBase *)func)->first_instr))

static inline uint32_t *
PyCode2_Code(PyCodeObject2 *code)
{
    return (uint32_t *)((char *)code + sizeof(PyCodeObject2));
}

static inline PyCodeObject2 *
PyCode2_FromInstr(const uint32_t *first_instr)
{
    return (PyCodeObject2 *)((char *)first_instr - sizeof(PyCodeObject2));
}

static inline PyCodeObject2 *
PyCode2_FromFunc(PyFunc *func)
{
    return PyCode2_FromInstr(func->func_base.first_instr);
}


#ifdef __cplusplus
}
#endif
#endif /* !Py_CODE2_H */
#endif /* Py_LIMITED_API */
