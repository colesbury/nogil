#ifndef Py_INTERNAL_CODE_H
#define Py_INTERNAL_CODE_H
#ifdef __cplusplus
extern "C" {
#endif

enum {
    // number of arguments excluding keyword-only args, *args, and **kwargs
    // if more than 255 arguments, this value is zero and the overflow bit
    // is set.
    CODE_MASK_ARGS          = 0x000000ff, // bits 0-7

    // bits 8-15 are always zero in code (keyword arguments in acc)
    CODE_FLAG_UNUSED_1      = 0x0000ff00, // bits 8-15 always zero

    // set if the function has a *args parameter
    CODE_FLAG_VARARGS       = 0x00010000, // bit  16

    CODE_FLAG_UNUSED_2      = 0x00020000, // bit  17 always zero

    // set if the function has a **kwargs parameter
    CODE_FLAG_VARKEYWORDS   = 0x00040000, // bit  18

    // set if the code has cell variables (i.e. captured by other functions)
    CODE_FLAG_HAS_CELLS     = 0x00080000, // bit  19

    // set if the code has free (captured) variables
    CODE_FLAG_HAS_FREEVARS  = 0x00100000, // bit  20

    // set if there are ANY keyword only arguments
    CODE_FLAG_KWD_ONLY_ARGS = 0x00200000, // bit  21

    // set if there more than 255 arguments
    CODE_FLAG_OVERFLOW      = 0x00400000, // bit  22

    // set if the function uses a locals dict (in regs[0])
    CODE_FLAG_LOCALS_DICT   = 0x00800000, // bit  23

    // set if the function is a generator, coroutine, or async generator
    CODE_FLAG_GENERATOR     = 0x01000000, // bit  24
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

typedef struct {
    Py_ssize_t start;   /* start instr for try block range */
    Py_ssize_t handler; /* end instr try block AND start of handler range */
    Py_ssize_t handler_end; /* end of handler block */
    Py_ssize_t reg;     /* temporary register to store active exception */
} ExceptionHandler;

struct _PyHandlerTable {
    Py_ssize_t size;
    ExceptionHandler entries[];
};

typedef struct {
    uint32_t from;  /* address of JUMP_SIDE_TABLE instruction */
    int32_t delta;  /* jump delta */
} JumpEntry;

struct _PyJumpSideTable {
    Py_ssize_t size;
    JumpEntry entries[];
};

PyCodeObject *
PyCode_NewInternal(
        int, int, int, int, int, int, int, int, PyObject *, PyObject *,
        PyObject *, PyObject *, PyObject *, PyObject *,
        PyObject *, PyObject *, int, PyObject *);

void _PyCode_UpdateFlags(PyCodeObject *);
int _PyCode_InternConstants(PyCodeObject *);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CODE_H */
