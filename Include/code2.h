/* Definitions for bytecode */

#ifndef Py_LIMITED_API
#ifndef Py_CODE2_H
#define Py_CODE2_H
#ifdef __cplusplus
extern "C" {
#endif


PyAPI_DATA(PyTypeObject) PyCode2_Type;

typedef struct {
    PyObject_HEAD
    uint8_t co_argcount;
    uint8_t co_nlocals;
    uint8_t co_ncells;
    uint8_t co_nfreevars;
    Py_ssize_t co_size;         /* number of instructions */
    Py_ssize_t co_nconsts;      /* number of constants */

    PyObject **co_constants;    /* pointer to constants array */
    Py_ssize_t *co_cell2reg;
    Py_ssize_t *co_free2reg;

    PyObject *co_weakreflist;

    int co_firstlineno;
    PyObject *co_varnames;      /* tuple of strings (local variable names) */
    PyObject *co_freevars;      /* tuple of strings (free variable names) */
    PyObject *co_cellvars;      /* tuple of strings (cell variable names) */
    PyObject *co_filename;      /* unicode (where it was loaded from) */
    PyObject *co_name;          /* unicode (name, for reference) */
    PyObject *co_lnotab;        /* string (encoding addr<->lineno mapping) See */



    // constants
    // number of parameters
    // stack size
} PyCodeObject2;

// PyAPI_FUNC(PyCodeObject2 *) PyCode2_New(PyObject *bytecode, PyObject *consts);

#define PyCode2_GET_CODE(co) (PyCode2_Code((PyCodeObject2 *)(co)))

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



#ifdef __cplusplus
}
#endif
#endif /* !Py_CODE2_H */
#endif /* Py_LIMITED_API */
