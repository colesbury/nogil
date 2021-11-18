#ifndef Py_CPYTHON_CODE_H
#  error "this header file must not be included directly"
#endif

typedef uint16_t _Py_CODEUNIT;

#ifdef WORDS_BIGENDIAN
#  define _Py_OPCODE(word) ((word) >> 8)
#  define _Py_OPARG(word) ((word) & 255)
#else
#  define _Py_OPCODE(word) ((word) & 255)
#  define _Py_OPARG(word) ((word) >> 8)
#endif

struct _PyHandlerTable;
struct _PyJumpSideTable;

typedef struct PyCodeObject {
    PyObject_HEAD
    uint32_t co_packed_flags;
    int co_flags; // unused???
    Py_ssize_t co_argcount;     /* number of arguments excluding kwd-only, *args, and **kwargs */
    Py_ssize_t co_nlocals;      /* number of local variables (including arguments) */

    Py_ssize_t co_ndefaultargs;
    Py_ssize_t co_posonlyargcount;
    Py_ssize_t co_kwonlyargcount;
    Py_ssize_t co_totalargcount; /* number of arguments including kwd-only, but not *args and **kwargs */

    Py_ssize_t co_framesize;    /* maximum stack usage */
    Py_ssize_t co_size;         /* size of instructions (in bytes) */
    Py_ssize_t co_nconsts;      /* number of constants */
    Py_ssize_t co_ncells;
    Py_ssize_t co_nfreevars;    /* number of captured free variables (including default args) */

    PyObject **co_constants;    /* pointer to constants array */
    Py_ssize_t *co_cell2reg;
    Py_ssize_t *co_free2reg;

    struct _PyHandlerTable *co_exc_handlers;
    struct _PyJumpSideTable *co_jump_table;

    PyObject *co_weakreflist;
    /* Scratch space for extra data relating to the code object.
       Type is a void* to keep the format private in codeobject.c to force
       people to go through the proper APIs. */
    void *co_extra;

    Py_ssize_t co_nmeta;
    int co_firstlineno;
    PyObject *co_varnames;      /* tuple of strings (local variable names) */
    PyObject *co_freevars;      /* tuple of strings (free variable names) */
    PyObject *co_cellvars;      /* tuple of strings (cell variable names) */
    PyObject *co_filename;      /* unicode (where it was loaded from) */
    PyObject *co_name;          /* unicode (name, for reference) */
    PyObject *co_lnotab;        /* string (encoding addr<->lineno mapping) See */
} PyCodeObject;

/* Masks for co_flags above */
#define CO_OPTIMIZED    0x0001
#define CO_NEWLOCALS    0x0002
#define CO_VARARGS      0x0004
#define CO_VARKEYWORDS  0x0008
#define CO_NESTED       0x0010
#define CO_GENERATOR    0x0020
/* The CO_NOFREE flag is set if there are no free or cell variables.
   This information is redundant, but it allows a single flag test
   to determine whether there is any extra work to be done when the
   call frame it setup.
*/
#define CO_NOFREE       0x0040

/* The CO_COROUTINE flag is set for coroutine functions (defined with
   ``async def`` keywords) */
#define CO_COROUTINE            0x0080
#define CO_ITERABLE_COROUTINE   0x0100
#define CO_ASYNC_GENERATOR      0x0200

/* bpo-39562: These constant values are changed in Python 3.9
   to prevent collision with compiler flags. CO_FUTURE_ and PyCF_
   constants must be kept unique. PyCF_ constants can use bits from
   0x0100 to 0x10000. CO_FUTURE_ constants use bits starting at 0x20000. */
#define CO_FUTURE_DIVISION      0x20000
#define CO_FUTURE_ABSOLUTE_IMPORT 0x40000 /* do absolute imports by default */
#define CO_FUTURE_WITH_STATEMENT  0x80000
#define CO_FUTURE_PRINT_FUNCTION  0x100000
#define CO_FUTURE_UNICODE_LITERALS 0x200000

#define CO_FUTURE_BARRY_AS_BDFL  0x400000
#define CO_FUTURE_GENERATOR_STOP  0x800000
#define CO_FUTURE_ANNOTATIONS    0x1000000

/* This value is found in the co_cell2arg array when the associated cell
   variable does not correspond to an argument. */
#define CO_CELL_NOT_AN_ARG (-1)

/* This should be defined if a future statement modifies the syntax.
   For example, when a keyword is added.
*/
#define PY_PARSER_REQUIRES_FUTURE_KEYWORD

PyAPI_DATA(PyTypeObject) PyCode_Type;

#define PyCode_Check(op) Py_IS_TYPE(op, &PyCode_Type)
#define PyCode_GetNumFree(op) ((op)->co_nfreevars)

/* Public interface */
PyAPI_FUNC(PyCodeObject *) PyCode_New(
        int, int, int, int, int, PyObject *, PyObject *,
        PyObject *, PyObject *, PyObject *, PyObject *,
        PyObject *, PyObject *, int, PyObject *);

// FIXME(sgross): rename
PyAPI_FUNC(PyCodeObject *)
PyCode_New2(Py_ssize_t instr_size, Py_ssize_t nconsts,
            Py_ssize_t nmeta, Py_ssize_t ncells, Py_ssize_t ncaptured,
            Py_ssize_t nexc_handlers, Py_ssize_t jump_table_size);

PyAPI_FUNC(PyCodeObject *) PyCode_NewWithPosOnlyArgs(
        int, int, int, int, int, int, PyObject *, PyObject *,
        PyObject *, PyObject *, PyObject *, PyObject *,
        PyObject *, PyObject *, int, PyObject *);
        /* same as struct above */

/* Creates a new empty code object with the specified source location. */
PyAPI_FUNC(PyCodeObject *)
PyCode_NewEmpty(const char *filename, const char *funcname, int firstlineno);

/* Return the line number associated with the specified bytecode index
   in this code object.  If you just need the line number of a frame,
   use PyFrame_GetLineNumber() instead. */
PyAPI_FUNC(int) PyCode_Addr2Line(PyCodeObject *, int);

static inline PyCodeObject *
PyCode_FromFirstInstr(const uint8_t *first_instr)
{
    return (PyCodeObject *)((char *)first_instr - sizeof(PyCodeObject));
}

static inline uint8_t *
PyCode_FirstInstr(PyCodeObject *code)
{
    return (uint8_t *)((char *)code + sizeof(PyCodeObject));
}

static inline Py_ssize_t
PyCode_NumFreevars(PyCodeObject *code)
{
    return code->co_nfreevars - code->co_ndefaultargs;
}

static inline Py_ssize_t
PyCode_NumKwargs(PyCodeObject *code)
{
    return code->co_totalargcount - code->co_argcount;
}

/* for internal use only */
typedef struct _addr_pair {
        int ap_lower;
        int ap_upper;
} PyAddrPair;

/* Update *bounds to describe the first and one-past-the-last instructions in the
   same line as lasti.  Return the number of that line.
*/
PyAPI_FUNC(int) _PyCode_CheckLineNumber(PyCodeObject* co,
                                        int lasti, PyAddrPair *bounds);

/* Create a comparable key used to compare constants taking in account the
 * object type. It is used to make sure types are not coerced (e.g., float and
 * complex) _and_ to distinguish 0.0 from -0.0 e.g. on IEEE platforms
 *
 * Return (type(obj), obj, ...): a tuple with variable size (at least 2 items)
 * depending on the type and the value. The type is the first item to not
 * compare bytes and str which can raise a BytesWarning exception. */
PyAPI_FUNC(PyObject*) _PyCode_ConstantKey(PyObject *obj);

PyAPI_FUNC(PyObject*) PyCode_Optimize(PyObject *code, PyObject* consts,
                                      PyObject *names, PyObject *lnotab);


PyAPI_FUNC(int) _PyCode_GetExtra(PyObject *code, Py_ssize_t index,
                                 void **extra);
PyAPI_FUNC(int) _PyCode_SetExtra(PyObject *code, Py_ssize_t index,
                                 void *extra);
