#include <stdbool.h>

#include "Python.h"
#include "code2.h"
#include "opcode2.h"
#include "structmember.h"
#include "pycore_code.h"
#include "pycore_object.h"
#include "pycore_gc.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"

// An individual register can have an owning or non-owning reference
// Deferred and immortal objects always have non-owning references (immortal for correctness, deferred for perf, helps)
// A regular object *may* have a non-owning reference for aliases

// alias       unowned
// not-alias   unowned|owned

// Function arguments may or may not be aliases
// Function return values are *never* aliases (?)

// x = object()
// y = x       # must not be an alias (!) (because x might change)
// foo(x, x)   # can be aliases!


// The debugger needs to make all aliases into non-aliases (?)

// So temporaries can be aliases
// Arguments can be aliases
// Assignemnts to named variables should not be aliases
// Return values should not be aliases: this allows:

//  y = foo()

// CALL(...)
// MOV(y, acc)  # transfer ownership



// Error handling
// Walk the stack
// Free variables
// Find nearest exception handler
// Jump?

// WebKit returns PC from every op
// There's an error handler PC for every instruction size inline so no matter the advance still reading/jumping to error handler
// WebKit writes result directly to destination register


// In Python, any function that can call arbitrary code (most):
// - can resize stack (ts->regs)
// - can raise an exception

static int code_kwonlyargcount(PyCodeObject2 *co);

/*[clinic input]
module _code2
class code "PyCodeObject2 *" "&PyCode2_Type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=96be18a1b345cf3c]*/

#include "clinic/codeobject2.c.h"

/* align size to a multiple of a power-of-2 alignment */
static Py_ssize_t
align_up(Py_ssize_t size, Py_ssize_t align)
{
    assert((align & (align - 1)) == 0 && "align must be power of two");
    return (size + (align - 1)) & -align;
}

PyCodeObject2 *
PyCode2_New(Py_ssize_t instr_size, Py_ssize_t nconsts,
            Py_ssize_t nmeta, Py_ssize_t ncells, Py_ssize_t nfreevars,
            Py_ssize_t nexc_handlers)
{
    assert(sizeof(PyCodeObject2) % sizeof(void*) == 0);
    Py_ssize_t instr_aligned_size = align_up(instr_size, sizeof(void*));
    Py_ssize_t total_size = (
        sizeof(PyCodeObject2) +
        instr_aligned_size +
        nmeta * sizeof(intptr_t) +
        nconsts * sizeof(PyObject *) +
        ncells * sizeof(Py_ssize_t) +
        nfreevars * 2 * sizeof(Py_ssize_t) +
        sizeof(struct _PyHandlerTable) +
        nexc_handlers * sizeof(ExceptionHandler));

    PyCodeObject2 *co = (PyCodeObject2 *)_PyObject_GC_Malloc(total_size);
    if (co == NULL) {
        return NULL;
    }
    memset(co, 0, sizeof(PyCodeObject2));
    PyObject_INIT(co, &PyCode2_Type);

    char *ptr = (char *)co + sizeof(PyCodeObject2);

    ptr += instr_aligned_size;
    co->co_size = instr_size;

    co->co_nmeta = nmeta;
    memset(ptr, -1, nmeta * sizeof(intptr_t));
    ptr += nmeta * sizeof(intptr_t);

    co->co_nconsts = nconsts;
    co->co_constants = (PyObject **)ptr;
    ptr += nconsts * sizeof(PyObject *);
    memset(co->co_constants, 0, nconsts * sizeof(PyObject*));

    co->co_ncells = ncells;
    co->co_cell2reg = (ncells == 0 ? NULL : (Py_ssize_t *)ptr);
    ptr += ncells * sizeof(Py_ssize_t);

    co->co_nfreevars = nfreevars;
    co->co_free2reg = (nfreevars == 0 ? NULL : (Py_ssize_t *)ptr);
    ptr += nfreevars * 2 * sizeof(Py_ssize_t);

    co->co_exc_handlers = (struct _PyHandlerTable *)ptr;
    co->co_exc_handlers->size = nexc_handlers;
    return co;
}

PyDoc_STRVAR(code_doc,
"code(???, argcount, posonlyargcount, kwonlyargcount, nlocals, stacksize,\n\
      flags, codestring, constants, names, varnames, filename, name,\n\
      firstlineno, lnotab[, freevars[, cellvars]])\n\
\n\
Create a code object.  Not for the faint of heart.");

/*[clinic input]
@classmethod
code.__new__ as code_new
    bytecode: object(subclass_of="&PyBytes_Type")
    constants as consts: object(subclass_of="&PyTuple_Type")
    argcount: int = 0
    posonlyargcount: int = 0
    kwonlyargcount: int = 0
    ndefaultargs: int = 0
    nlocals: int = 0
    framesize: int = 0
    nmeta: int = 0
    flags: int = 0
    varnames: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    filename: unicode = None
    name: unicode = None
    firstlineno: int = 0
    linetable: object(subclass_of="&PyBytes_Type") = None
    eh_table: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    freevars: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    cellvars: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    free2reg: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    cell2reg: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()

Create a code object.  Not for the faint of heart.
[clinic start generated code]*/

static PyObject *
code_new_impl(PyTypeObject *type, PyObject *bytecode, PyObject *consts,
              int argcount, int posonlyargcount, int kwonlyargcount,
              int ndefaultargs, int nlocals, int framesize, int nmeta,
              int flags, PyObject *varnames, PyObject *filename,
              PyObject *name, int firstlineno, PyObject *linetable,
              PyObject *eh_table, PyObject *freevars, PyObject *cellvars,
              PyObject *free2reg, PyObject *cell2reg)
/*[clinic end generated code: output=1479fe53549304cb input=8a495d20172a274b]*/
{
    Py_ssize_t ncells = cell2reg ? PyTuple_GET_SIZE(cell2reg) : 0;
    Py_ssize_t ncaptured = free2reg ? PyTuple_GET_SIZE(free2reg) : 0;
    Py_ssize_t nexc_handlers = eh_table ? PyTuple_GET_SIZE(eh_table) : 0;

    PyCodeObject2 *co = PyCode2_New(
        PyBytes_GET_SIZE(bytecode),
        PyTuple_GET_SIZE(consts),
        nmeta,
        ncells,
        ncaptured,
        nexc_handlers);
    if (co == NULL) {
        return NULL;
    }
    co->co_argcount = argcount;
    co->co_posonlyargcount = posonlyargcount;
    co->co_totalargcount = argcount + kwonlyargcount;
    co->co_nlocals = nlocals;
    co->co_ncells = ncells;
    co->co_nfreevars = ncaptured;
    co->co_ndefaultargs = ndefaultargs;
    co->co_flags = flags;
    co->co_framesize = framesize;
    Py_XINCREF(varnames);
    co->co_varnames = varnames;
    Py_XINCREF(freevars);
    co->co_freevars = freevars;
    Py_XINCREF(cellvars);
    co->co_cellvars = cellvars;
    Py_INCREF(filename);
    co->co_filename = filename;
    Py_INCREF(name);
    co->co_name = name;
    co->co_firstlineno = firstlineno;
    Py_INCREF(linetable);
    co->co_lnotab = linetable;

    assert(co->co_size == PyBytes_GET_SIZE(bytecode));
    memcpy(PyCode2_GET_CODE(co), PyBytes_AS_STRING(bytecode), co->co_size);

    for (Py_ssize_t i = 0, n = co->co_nconsts; i != n; i++) {
        PyObject *c = PyTuple_GET_ITEM(consts, i);
        Py_INCREF(c);
        if (PyUnicode_CheckExact(c)) {
            PyUnicode_InternInPlace(&c);
        }
        co->co_constants[i] = c;
    }
    for (Py_ssize_t i = 0; i < ncells; i++) {
        co->co_cell2reg[i] = PyLong_AsSsize_t(PyTuple_GET_ITEM(cell2reg, i));
    }
    for (Py_ssize_t i = 0; i < ncaptured; i++) {
        PyObject *pair = PyTuple_GET_ITEM(free2reg, i);
        co->co_free2reg[i*2+0] = PyLong_AsSsize_t(PyTuple_GET_ITEM(pair, 0));
        co->co_free2reg[i*2+1] = PyLong_AsSsize_t(PyTuple_GET_ITEM(pair, 1));
    }

    struct _PyHandlerTable *exc_handlers = co->co_exc_handlers;
    exc_handlers->size = nexc_handlers;
    for (Py_ssize_t i = 0; i < nexc_handlers; i++) {
        PyObject *entry = PyTuple_GET_ITEM(eh_table, i);
        ExceptionHandler *handler = &exc_handlers->entries[i];
        handler->start = PyLong_AsSsize_t(PyTuple_GET_ITEM(entry, 0));
        handler->handler = PyLong_AsSsize_t(PyTuple_GET_ITEM(entry, 1));
        handler->handler_end = PyLong_AsSsize_t(PyTuple_GET_ITEM(entry, 2));
        handler->reg = PyLong_AsSsize_t(PyTuple_GET_ITEM(entry, 3));
    }

    PyCode2_UpdateFlags(co);
    return (PyObject *)co;
}

void
PyCode2_UpdateFlags(PyCodeObject2 *co)
{
    co->co_packed_flags = 0;
    co->co_packed_flags |= (co->co_argcount < 256 ? co->co_argcount : CODE_FLAG_OVERFLOW);
    co->co_packed_flags |= (co->co_ncells > 0 ? CODE_FLAG_HAS_CELLS : 0);
    co->co_packed_flags |= (co->co_nfreevars > co->co_ndefaultargs ? CODE_FLAG_HAS_FREEVARS : 0);
    co->co_packed_flags |= (co->co_flags & CO_VARARGS) ? CODE_FLAG_VARARGS : 0;
    co->co_packed_flags |= (co->co_flags & CO_VARKEYWORDS) ? CODE_FLAG_VARKEYWORDS : 0;
    co->co_packed_flags |= (co->co_totalargcount > co->co_argcount ? CODE_FLAG_KWD_ONLY_ARGS : 0);
    co->co_packed_flags |= (co->co_flags & CO_NEWLOCALS) ? 0 : CODE_FLAG_LOCALS_DICT;
}

static void
code_dealloc(PyCodeObject2 *co)
{
    PyObject_GC_UnTrack(co);

    PyObject **consts = co->co_constants;
    Py_ssize_t nconsts = co->co_nconsts;
    for (Py_ssize_t i = 0; i != nconsts; i++) {
        Py_XDECREF(consts[i]);
    }

    Py_XDECREF(co->co_varnames);
    Py_XDECREF(co->co_freevars);
    Py_XDECREF(co->co_cellvars);
    Py_XDECREF(co->co_filename);
    Py_XDECREF(co->co_name);
    Py_XDECREF(co->co_lnotab);
}

static PyObject *
code_repr(PyCodeObject2 *co)
{
    int lineno;
    if (co->co_firstlineno != 0)
        lineno = co->co_firstlineno;
    else
        lineno = -1;
    if (co->co_filename && PyUnicode_Check(co->co_filename)) {
        return PyUnicode_FromFormat(
            "<code object %U at %p, file \"%U\", line %d>",
            co->co_name, co, co->co_filename, lineno);
    } else {
        return PyUnicode_FromFormat(
            "<code object %U at %p, file ???, line %d>",
            co->co_name, co, lineno);
    }
}

static Py_hash_t
code_hash(PyCodeObject2 *co)
{
    return 7;
}

static int
code_traverse(PyCodeObject2 *co, visitproc visit, void *arg)
{
    return 0;
}

static PyObject *
code_richcompare(PyObject *self, PyObject *other, int op)
{
    if ((op != Py_EQ && op != Py_NE) ||
        !PyCode2_Check(self) ||
        !PyCode2_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    PyCodeObject2 *a = (PyCodeObject2 *)self;
    PyCodeObject2 *b = (PyCodeObject2 *)other;

    int eq = 1;

    #define COMPARE_INT(name) if (a->name != b->name) goto unequal;
    COMPARE_INT(co_packed_flags);
    COMPARE_INT(co_flags);
    COMPARE_INT(co_argcount);
    COMPARE_INT(co_nlocals);
    COMPARE_INT(co_ndefaultargs);
    COMPARE_INT(co_posonlyargcount);
    COMPARE_INT(co_totalargcount);
    COMPARE_INT(co_framesize);
    COMPARE_INT(co_size);
    COMPARE_INT(co_nconsts);
    COMPARE_INT(co_ncells);
    COMPARE_INT(co_nmeta);
    COMPARE_INT(co_firstlineno);
    COMPARE_INT(co_exc_handlers->size);
    #undef COMPARE_INT

    #define RICH_COMPARE(name) do { \
        eq = PyObject_RichCompareBool(a->name, b->name, Py_EQ); \
        if (eq <= 0) goto done; \
    } while (0)
    RICH_COMPARE(co_name);
    RICH_COMPARE(co_varnames);
    RICH_COMPARE(co_freevars);
    RICH_COMPARE(co_cellvars);
    // NOTE: we don't compare co_filename!
    #undef RICH_COMPARE

    // compare constants
    for (int i = 0, n = a->co_nconsts; i < n; i++) {
        PyObject *const1 = _PyCode_ConstantKey(a->co_constants[i]);
        if (const1 == NULL) {
            return NULL;
        }
        PyObject *const2 = _PyCode_ConstantKey(b->co_constants[i]);
        if (const2 == NULL) {
            Py_DECREF(const1);
            return NULL;
        }
        eq = PyObject_RichCompareBool(const1, const2, Py_EQ);
        Py_DECREF(const1);
        Py_DECREF(const2);
        if (eq <= 0) goto done;
    }

    // compare bytecode
    eq = (0 == memcmp(PyCode2_GET_CODE(a),
                      PyCode2_GET_CODE(b),
                      a->co_size));
    if (eq) goto done;

    // compare exception handler entries
    eq = (0 == memcmp(a->co_exc_handlers->entries,
                      b->co_exc_handlers->entries,
                      a->co_exc_handlers->size * sizeof(ExceptionHandler)));
    goto done;

unequal:
    eq = 0;

done:
    if (eq == -1) return NULL;
    return (eq ^ (op == Py_NE)) ? Py_True : Py_False;
}

static PyObject *
code_sizeof(PyCodeObject2 *co, PyObject *Py_UNUSED(args))
{
    Py_ssize_t size = sizeof(PyCodeObject);
    size += co->co_size;
    size = co->co_nconsts * sizeof(PyObject *);
    return PyLong_FromSsize_t(size);
}

static int
code_kwonlyargcount(PyCodeObject2 *co)
{
    return co->co_totalargcount - co->co_argcount;
}

static PyObject *
code_getkwonlyargcount(PyCodeObject2 *co, PyObject *Py_UNUSED(args))
{
    return PyLong_FromLong(code_kwonlyargcount(co));
}

static PyObject *
code_getcode(PyCodeObject2 *co, PyObject *Py_UNUSED(args))
{
    uint8_t *bytecode = PyCode2_Code(co);
    return PyBytes_FromStringAndSize((char *)bytecode, co->co_size);
}

static PyObject *
code_getconsts(PyCodeObject2 *co, PyObject *Py_UNUSED(args))
{
    PyObject *t = PyTuple_New(co->co_nconsts);
    if (t == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i != co->co_nconsts; i++) {
        PyObject *c = co->co_constants[i];
        Py_INCREF(c);
        PyTuple_SET_ITEM(t, i, c);
    }
    return t;
}

static PyObject *
code_getexc_handlers(PyCodeObject2 *co, PyObject *Py_UNUSED(args))
{
    Py_ssize_t size = co->co_exc_handlers->size;
    PyObject *t = PyTuple_New(size);
    if (t == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i != size; i++) {
        ExceptionHandler *h = &co->co_exc_handlers->entries[i];
        PyObject *entry = Py_BuildValue("(nnnn)",
            h->start,
            h->handler,
            h->handler_end,
            h->reg);
        if (entry == NULL) {
            Py_DECREF(t);
            return NULL;
        }
        PyTuple_SET_ITEM(t, i, entry);
    }
    return t;
}

static PyObject *
code_getcell2reg(PyCodeObject2 *co, PyObject *Py_UNUSED(args))
{
    PyObject *t = PyTuple_New(co->co_ncells);
    if (t == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i != co->co_ncells; i++) {
        PyObject *c = PyLong_FromSsize_t(co->co_cell2reg[i]);
        if (c == NULL) {
            Py_DECREF(t);
            return NULL;
        }
        PyTuple_SET_ITEM(t, i, c);
    }
    return t;
}

static PyObject *
code_getfree2reg(PyCodeObject2 *co, PyObject *Py_UNUSED(args))
{
    Py_ssize_t size = co->co_nfreevars;
    PyObject *t = PyTuple_New(size);
    if (t == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i != size; i++) {
        PyObject *value = Py_BuildValue("(nn)",
            co->co_free2reg[2 * i],
            co->co_free2reg[2 * i + 1]);
        if (value == NULL) {
            Py_DECREF(t);
            return NULL;
        }
        PyTuple_SET_ITEM(t, i, value);
    }
    return t;
}

/*[clinic input]
code.replace

    *
    co_argcount: int(c_default="self->co_argcount") = -1
    co_posonlyargcount: int(c_default="self->co_posonlyargcount") = -1
    co_kwonlyargcount: int(c_default="code_kwonlyargcount(self)") = -1
    co_ndefaultargs: int(c_default="self->co_ndefaultargs") = -1
    co_nlocals: int(c_default="self->co_nlocals") = -1
    co_framesize: int(c_default="self->co_framesize") = -1
    co_nmeta: int(c_default="self->co_nmeta") = -1
    co_flags: int(c_default="self->co_flags") = -1
    co_firstlineno: int(c_default="self->co_firstlineno") = -1
    co_code: object(subclass_of="&PyBytes_Type", c_default="NULL") = None
    co_consts: object(subclass_of="&PyTuple_Type", c_default="NULL") = None
    co_varnames: object(subclass_of="&PyTuple_Type", c_default="self->co_varnames") = None
    co_freevars: object(subclass_of="&PyTuple_Type", c_default="self->co_freevars") = None
    co_cellvars: object(subclass_of="&PyTuple_Type", c_default="self->co_cellvars") = None
    co_filename: unicode(c_default="self->co_filename") = None
    co_name: unicode(c_default="self->co_name") = None
    co_lnotab: object(subclass_of="&PyBytes_Type", c_default="self->co_lnotab") = None

Return a copy of the code object with new values for the specified fields.
[clinic start generated code]*/

static PyObject *
code_replace_impl(PyCodeObject2 *self, int co_argcount,
                  int co_posonlyargcount, int co_kwonlyargcount,
                  int co_ndefaultargs, int co_nlocals, int co_framesize,
                  int co_nmeta, int co_flags, int co_firstlineno,
                  PyObject *co_code, PyObject *co_consts,
                  PyObject *co_varnames, PyObject *co_freevars,
                  PyObject *co_cellvars, PyObject *co_filename,
                  PyObject *co_name, PyObject *co_lnotab)
/*[clinic end generated code: output=86146079e706c9f4 input=5cb6d203cfc9397b]*/
{
    PyObject *co = NULL;
    PyObject *eh_table = NULL;
    PyObject *free2reg = NULL, *cell2reg = NULL;
    
    #define DEFAULT(arg, value) do {                    \
        arg = (!arg) ? (value) : (Py_INCREF(arg), arg); \
        if (!arg) goto cleanup;                         \
    } while (0)

    DEFAULT(co_code, code_getcode(self, NULL));
    DEFAULT(co_consts, code_getconsts(self, NULL));
    #undef DEFAULT

    eh_table = code_getexc_handlers(self, NULL);
    if (eh_table == NULL) goto cleanup;

    free2reg = code_getfree2reg(self, NULL);
    if (free2reg == NULL) goto cleanup;

    cell2reg = code_getcell2reg(self, NULL);
    if (cell2reg == NULL) goto cleanup;

    co = code_new_impl(
        &PyCode2_Type,
        co_code,
        co_consts,
        co_argcount,
        co_posonlyargcount,
        co_kwonlyargcount,
        co_ndefaultargs,
        co_nlocals,
        co_framesize,
        co_nmeta,
        co_flags,
        co_varnames,
        co_filename,
        co_name,
        co_firstlineno,
        co_lnotab,
        eh_table,
        co_freevars,
        co_cellvars,
        free2reg,
        cell2reg);

cleanup:
    Py_XDECREF(cell2reg);
    Py_XDECREF(free2reg);
    Py_XDECREF(co_code);
    Py_XDECREF(co_consts);
    Py_XDECREF(eh_table);
    return co;
}

static struct PyMethodDef code_methods[] = {
    {"__sizeof__", (PyCFunction)code_sizeof, METH_NOARGS},
    CODE_REPLACE_METHODDEF
    {NULL, NULL}                /* sentinel */
};

#define OFF(x) offsetof(PyCodeObject2, x)

static PyMemberDef code_memberlist[] = {
    {"co_argcount",             T_PYSSIZET,  OFF(co_argcount),        READONLY},
    {"co_posonlyargcount",      T_PYSSIZET,  OFF(co_posonlyargcount), READONLY},
    {"co_totalargcount",        T_PYSSIZET,  OFF(co_totalargcount),   READONLY},
    {"co_nlocals",      T_PYSSIZET,     OFF(co_nlocals),         READONLY},
    {"co_framesize",    T_PYSSIZET,     OFF(co_framesize),       READONLY},
    {"co_flags",        T_INT,          OFF(co_flags),           READONLY},
    {"co_packed_flags", T_INT,          OFF(co_packed_flags),    READONLY},
    {"co_varnames",     T_OBJECT,       OFF(co_varnames),        READONLY},
    {"co_freevars",     T_OBJECT,       OFF(co_freevars),        READONLY},
    {"co_cellvars",     T_OBJECT,       OFF(co_cellvars),        READONLY},
    {"co_filename",     T_OBJECT,       OFF(co_filename),        READONLY},
    {"co_name",         T_OBJECT,       OFF(co_name),            READONLY},
    {"co_firstlineno",  T_INT,          OFF(co_firstlineno),     READONLY},
    {"co_lnotab",       T_OBJECT,       OFF(co_lnotab),          READONLY},
    {NULL}      /* Sentinel */
};

static PyGetSetDef code_getset[] = {
    {"co_kwonlyargcount", (getter)code_getkwonlyargcount, (setter)NULL, NULL, NULL},
    {"co_code", (getter)code_getcode, (setter)NULL, "code bytes", NULL},
    {"co_consts", (getter)code_getconsts, (setter)NULL, "constants", NULL},
    {"co_exc_handlers", (getter)code_getexc_handlers, (setter)NULL, "exception handlers", NULL},
    {"co_cell2reg", (getter)code_getcell2reg, (setter)NULL, "cell variables", NULL},
    {"co_free2reg", (getter)code_getfree2reg, (setter)NULL, "free variables", NULL},
    {NULL} /* sentinel */
};

PyTypeObject PyCode2_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "code",
    sizeof(PyCodeObject2),
    0,
    (destructor)code_dealloc,           /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    (reprfunc)code_repr,                /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    (hashfunc)code_hash,                /* tp_hash */
    0,                                  /* tp_call */
    0,                                  /* tp_str */
    PyObject_GenericGetAttr,            /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    code_doc,                           /* tp_doc */
    (traverseproc)code_traverse,        /* tp_traverse */
    0,                                  /* tp_clear */
    code_richcompare,                   /* tp_richcompare */
    offsetof(PyCodeObject2, co_weakreflist),     /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    code_methods,                       /* tp_methods */
    code_memberlist,                    /* tp_members */
    code_getset,                        /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    0,                                  /* tp_init */
    0,                                  /* tp_alloc */
    code_new,                           /* tp_new */
};

int
PyCode2_Addr2Line(PyCodeObject2 *co, int addrq)
{
    Py_ssize_t size = PyBytes_Size(co->co_lnotab) / 2;
    unsigned char *p = (unsigned char*)PyBytes_AsString(co->co_lnotab);
    int line = co->co_firstlineno;
    int addr = 0;
    while (--size >= 0) {
        addr += *p++;
        if (addr > addrq)
            break;
        line += (signed char)*p;
        p++;
    }
    return line;
}