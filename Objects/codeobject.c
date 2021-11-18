#include <stdbool.h>

#include "Python.h"
#include "opcode.h"
#include "structmember.h"
#include "pycore_code.h"
#include "pycore_object.h"
#include "pycore_gc.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"
#include "pycore_hashtable.h"

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

/* Holder for co_extra information */
typedef struct {
    Py_ssize_t ce_size;
    void *ce_extras[1];
} _PyCodeObjectExtra;

/*[clinic input]
class code "PyCodeObject *" "&PyCode_Type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=78aa5d576683bb4b]*/

#include "clinic/codeobject.c.h"

/* align size to a multiple of a power-of-2 alignment */
static Py_ssize_t
align_up(Py_ssize_t size, Py_ssize_t align)
{
    assert((align & (align - 1)) == 0 && "align must be power of two");
    return (size + (align - 1)) & -align;
}

PyCodeObject *
PyCode_New2(Py_ssize_t instr_size, Py_ssize_t nconsts,
            Py_ssize_t nmeta, Py_ssize_t ncells, Py_ssize_t nfreevars,
            Py_ssize_t nexc_handlers, Py_ssize_t jump_table_size)
{
    assert(sizeof(PyCodeObject) % sizeof(void*) == 0);
    Py_ssize_t instr_aligned_size = align_up(instr_size, sizeof(void*));
    Py_ssize_t total_size = (
        sizeof(PyCodeObject) +
        instr_aligned_size +
        nmeta * sizeof(intptr_t) +
        nconsts * sizeof(PyObject *) +
        ncells * sizeof(Py_ssize_t) +
        nfreevars * 2 * sizeof(Py_ssize_t) +
        sizeof(struct _PyHandlerTable) +
        nexc_handlers * sizeof(ExceptionHandler) +
        sizeof (struct _PyJumpSideTable) +
        jump_table_size * sizeof(JumpEntry));

    PyCodeObject *co = (PyCodeObject *)_PyObject_GC_Malloc(total_size);
    if (co == NULL) {
        return NULL;
    }
    memset(co, 0, sizeof(PyCodeObject));
    PyObject_INIT(co, &PyCode_Type);

    char *ptr = (char *)co + sizeof(PyCodeObject);

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
    ptr += sizeof(*co->co_exc_handlers) + nexc_handlers * sizeof(ExceptionHandler);

    co->co_jump_table = (struct _PyJumpSideTable *)ptr;
    co->co_jump_table->size = jump_table_size;
    _PyObject_GC_TRACK(co);
    _PyObject_SET_DEFERRED_RC(co);
    return co;
}


PyDoc_STRVAR(code_doc,
"code(argcount, posonlyargcount, kwonlyargcount, nlocals, stacksize,\n\
      flags, codestring, constants, names, varnames, filename, name,\n\
      firstlineno, lnotab[, freevars[, cellvars]])\n\
\n\
Create a code object.  Not for the faint of heart.");

/*[clinic input]
@classmethod
code.__new__ as code_new
    argcount: int = 0
    posonlyargcount: int = 0
    kwonlyargcount: int = 0
    nlocals: int = 0
    framesize: int = 0
    ndefaultargs: int = 0
    nmeta: int = 0
    flags: int = 0
    code: object(subclass_of="&PyBytes_Type") = None
    constants as consts: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    varnames: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    filename: unicode = None
    name: unicode = None
    firstlineno: int = 0
    linetable: object(subclass_of="&PyBytes_Type") = None
    eh_table: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    jump_table: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    freevars: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    cellvars: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    free2reg: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    cell2reg: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()

Create a code object.  Not for the faint of heart.
[clinic start generated code]*/

static PyObject *
code_new_impl(PyTypeObject *type, int argcount, int posonlyargcount,
              int kwonlyargcount, int nlocals, int framesize,
              int ndefaultargs, int nmeta, int flags, PyObject *code,
              PyObject *consts, PyObject *varnames, PyObject *filename,
              PyObject *name, int firstlineno, PyObject *linetable,
              PyObject *eh_table, PyObject *jump_table, PyObject *freevars,
              PyObject *cellvars, PyObject *free2reg, PyObject *cell2reg)
/*[clinic end generated code: output=81c120af9984b30c input=ef688bd1f889035e]*/
{
    Py_ssize_t ncells = cell2reg ? PyTuple_GET_SIZE(cell2reg) : 0;
    Py_ssize_t ncaptured = free2reg ? PyTuple_GET_SIZE(free2reg) : 0;
    Py_ssize_t nexc_handlers = eh_table ? PyTuple_GET_SIZE(eh_table) : 0;
    Py_ssize_t jump_table_size = jump_table ? PyTuple_GET_SIZE(jump_table) : 0;

    PyCodeObject *co = PyCode_New2(
        code ? PyBytes_GET_SIZE(code) : 0,
        consts ? PyTuple_GET_SIZE(consts) : 0,
        nmeta,
        ncells,
        ncaptured,
        nexc_handlers,
        jump_table_size);
    if (co == NULL) {
        return NULL;
    }
    co->co_argcount = argcount;
    co->co_posonlyargcount = posonlyargcount;
    co->co_kwonlyargcount = kwonlyargcount;
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

    if (co->co_size != 0) {
        memcpy(PyCode_FirstInstr(co), PyBytes_AS_STRING(code), co->co_size);
    }
    for (Py_ssize_t i = 0, n = co->co_nconsts; i != n; i++) {
        PyObject *c = PyTuple_GET_ITEM(consts, i);
        Py_INCREF(c);
        co->co_constants[i] = c;
    }
    if (_PyCode_InternConstants(co) != 0) {
        Py_DECREF(co);
        return NULL;
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

    _PyCode_UpdateFlags(co);
    return (PyObject *)co;
}

PyCodeObject *
PyCode_NewEmpty(const char *filename, const char *funcname, int firstlineno)
{
    PyObject *funcname_ob = NULL;
    PyObject *filename_ob = NULL;
    PyObject *co = NULL;

    funcname_ob = PyUnicode_FromString(funcname);
    if (funcname_ob == NULL) {
        goto done;
    }
    filename_ob = PyUnicode_DecodeFSDefault(filename);
    if (filename_ob == NULL) {
        goto done;
    }
    co = code_new_impl(
        &PyCode_Type,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        NULL, /*code*/
        NULL, /*consts*/
        NULL, /*varnames*/
        filename_ob,
        funcname_ob,
        firstlineno,
        Py_None, /*linetable*/
        NULL, /*eh_table*/
        NULL, /*jump_table*/
        NULL, /*freevars*/
        NULL, /*cellvars*/
        NULL, /*free2reg*/
        NULL /*cell2reg*/
    );
done:
    Py_XDECREF(funcname_ob);
    Py_XDECREF(filename_ob);
    return (PyCodeObject *)co;
}



PyCodeObject *
PyCode_NewWithPosOnlyArgs(int argcount, int posonlyargcount, int kwonlyargcount,
                          int nlocals, int stacksize, int flags,
                          PyObject *code, PyObject *consts, PyObject *names,
                          PyObject *varnames, PyObject *freevars, PyObject *cellvars,
                          PyObject *filename, PyObject *name, int firstlineno,
                          PyObject *lnotab)
{
    /* Check argument types */
    if (argcount < posonlyargcount || posonlyargcount < 0 ||
        kwonlyargcount < 0 || nlocals < 0 ||
        stacksize < 0 || flags < 0 ||
        code == NULL || !PyBytes_Check(code) ||
        consts == NULL || !PyTuple_Check(consts) ||
        names == NULL || !PyTuple_Check(names) ||
        varnames == NULL || !PyTuple_Check(varnames) ||
        freevars == NULL || !PyTuple_Check(freevars) ||
        cellvars == NULL || !PyTuple_Check(cellvars) ||
        name == NULL || !PyUnicode_Check(name) ||
        filename == NULL || !PyUnicode_Check(filename) ||
        lnotab == NULL || !PyBytes_Check(lnotab)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    /* Make sure that code is indexable with an int, this is
       a long running assumption in ceval.c and many parts of
       the interpreter. */
    if (PyBytes_GET_SIZE(code) > INT_MAX) {
        PyErr_SetString(PyExc_OverflowError, "co_code larger than INT_MAX");
        return NULL;
    }

    /* Check for any inner or outer closure references */
    if (!PyTuple_GET_SIZE(cellvars) && !PyTuple_GET_SIZE(freevars)) {
        flags |= CO_NOFREE;
    } else {
        flags &= ~CO_NOFREE;
    }

    return (PyCodeObject *)code_new_impl(
        &PyCode_Type,
        argcount,
        posonlyargcount,
        kwonlyargcount,
        nlocals,
        stacksize,
        0, /*co_ndefaultargs*/
        0, /*co_nmeta*/
        flags,
        code,
        consts,
        varnames,
        filename,
        name,
        firstlineno,
        lnotab,
        NULL, /*eh_table*/
        NULL, /*jump_table*/
        freevars, /*co_freevars*/
        cellvars, /*co_cellvars*/
        NULL, /*free2reg*/
        NULL /*cell2reg*/
    );
}

PyCodeObject *
PyCode_New(int argcount, int kwonlyargcount,
           int nlocals, int stacksize, int flags,
           PyObject *code, PyObject *consts, PyObject *names,
           PyObject *varnames, PyObject *freevars, PyObject *cellvars,
           PyObject *filename, PyObject *name, int firstlineno,
           PyObject *lnotab)
{
    return PyCode_NewWithPosOnlyArgs(argcount, 0, kwonlyargcount, nlocals,
                                     stacksize, flags, code, consts, names,
                                     varnames, freevars, cellvars, filename,
                                     name, firstlineno, lnotab);
}

void
_PyCode_UpdateFlags(PyCodeObject *co)
{
    co->co_packed_flags = 0;
    co->co_packed_flags |= (co->co_argcount < 256 ? co->co_argcount : CODE_FLAG_OVERFLOW);
    co->co_packed_flags |= (co->co_ncells > 0 ? CODE_FLAG_HAS_CELLS : 0);
    co->co_packed_flags |= (co->co_nfreevars > co->co_ndefaultargs ? CODE_FLAG_HAS_FREEVARS : 0);
    co->co_packed_flags |= (co->co_flags & CO_VARARGS) ? CODE_FLAG_VARARGS : 0;
    if (co->co_flags & CO_VARKEYWORDS) {
        co->co_packed_flags |= CODE_FLAG_VARKEYWORDS;
    }
    if (co->co_totalargcount > co->co_argcount) {
        co->co_packed_flags |= CODE_FLAG_KWD_ONLY_ARGS;
    }
    if (!(co->co_flags & CO_NEWLOCALS)) {
        co->co_packed_flags |= CODE_FLAG_LOCALS_DICT;
    }
    if (co->co_flags & (CO_GENERATOR|CO_COROUTINE|CO_ASYNC_GENERATOR)) {
        co->co_packed_flags |= CODE_FLAG_GENERATOR;
    }
}

static void
code_dealloc(PyCodeObject *co)
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

    if (co->co_extra != NULL) {
        PyInterpreterState *interp = _PyInterpreterState_GET();
        _PyCodeObjectExtra *co_extra = co->co_extra;

        for (Py_ssize_t i = 0; i < co_extra->ce_size; i++) {
            freefunc free_extra = interp->co_extra_freefuncs[i];

            if (free_extra != NULL) {
                free_extra(co_extra->ce_extras[i]);
            }
        }

        PyMem_Free(co_extra);
    }

    if (co->co_weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject*)co);
    }
    PyObject_GC_Del(co);
}

static Py_uhash_t
hash_const(const void *key)
{
    PyObject *op = (PyObject *)key;
    if (PySlice_Check(op)) {
        PySliceObject *s = (PySliceObject *)op;
        PyObject *data[3] = { s->start, s->stop, s->step };
        return _Py_HashBytes(&data, sizeof(data));
    }
    else if (PyTuple_CheckExact(op)) {
        Py_ssize_t size = PyTuple_GET_SIZE(op);
        PyObject **data = _PyTuple_ITEMS(op);
        return _Py_HashBytes(data, sizeof(PyObject *) * size);
    }
    Py_hash_t h = PyObject_Hash(op);
    if (h == -1) {
        Py_FatalError("hash failed");
    }
    return (Py_uhash_t)h;
}

static int
compare_constants(PyObject *op1, PyObject *op2)
{
    if (op1 == op2) {
        return 1;
    }
    if (Py_TYPE(op1) != Py_TYPE(op2)) {
        return 0;
    }
    if (PyTuple_CheckExact(op1)) {
        Py_ssize_t size = PyTuple_GET_SIZE(op1);
        if (size != PyTuple_GET_SIZE(op2)) {
            return 0;
        }
        for (Py_ssize_t i = 0; i < size; i++) {
            if (PyTuple_GET_ITEM(op1, i) != PyTuple_GET_ITEM(op2, i)) {
                return 0;
            }
        }
        return 1;
    }
    else if (PyBytes_CheckExact(op1)) {
        return PyObject_RichCompareBool(op1, op2, Py_EQ);
    }
    else if (PyLong_CheckExact(op1)) {
        return PyObject_RichCompareBool(op1, op2, Py_EQ);
    }
    else if (PySlice_Check(op1)) {
        PySliceObject *s1 = (PySliceObject *)op1;
        PySliceObject *s2 = (PySliceObject *)op2;
        return (s1->start == s2->start &&
                s1->stop  == s2->stop  &&
                s1->step  == s2->step);
    }
    else if (PyFloat_CheckExact(op1)) {
        double f1 = PyFloat_AS_DOUBLE(op1);
        double f2 = PyFloat_AS_DOUBLE(op2);
        return (memcmp(&f1, &f2, sizeof(double)) == 0);
    }
    else if (PyComplex_CheckExact(op1)) {
        Py_complex c1 = ((PyComplexObject *)op1)->cval;
        Py_complex c2 = ((PyComplexObject *)op2)->cval;
        return (memcmp(&c1, &c2, sizeof(Py_complex)) == 0);
    }
    else {
        Py_FatalError("unexpected type in compare_constants");
        return 0;
    }
}

static int
compare_const(const void *key1, const void *key2)
{
    return compare_constants((PyObject *)key1, (PyObject *)key2);
}

static int
intern_immortal(_Py_hashtable_t *ht, PyObject *key, PyObject **ptr)
{
    // TODO: only intern + immortalize when in nogil mode.

    assert(!PyUnicode_CheckExact(key));
    _Py_hashtable_entry_t *entry;
    PyObject *op = *ptr;

    entry = _Py_hashtable_get_entry(ht, op);
    if (entry == NULL) {
        if (_Py_hashtable_set(ht, op, op) != 0) {
            return -1;
        }
        if (PyType_HasFeature(Py_TYPE(op), Py_TPFLAGS_HAVE_GC)) {
            PyObject_GC_UnTrack(op);
        }
        op->ob_ref_local |= _Py_REF_IMMORTAL_MASK;
        op->ob_tid = 0;
    }
    else {
        PyObject *value = (PyObject *)entry->value;
        Py_INCREF(value);
        Py_SETREF(*ptr, value);
    }
    return 0;
}

static int
intern_constant(_Py_hashtable_t *ht, PyObject **ptr)
{
    PyObject *op = *ptr;
    if (_PyObject_IS_IMMORTAL(op)) {
        // already interned
        return 0;
    }
    else if (PyUnicode_CheckExact(op)) {
        PyUnicode_InternInPlace(ptr);
        return 0;
    }
    else if (PyFloat_CheckExact(op) ||
             PyLong_CheckExact(op) ||
             PyComplex_CheckExact(op) ||
             PyBytes_CheckExact(op))
    {
        return intern_immortal(ht, op, ptr);
    }
    else if (PySlice_Check(op)) {
        PySliceObject *s = (PySliceObject *)op;
        if (intern_constant(ht, &s->start) != 0) {
            return -1;
        }
        if (intern_constant(ht, &s->stop) != 0) {
            return -1;
        }
        if (intern_constant(ht, &s->step) != 0) {
            return -1;
        }
        return intern_immortal(ht, op, ptr);
    }
    else if (PyTuple_CheckExact(op)) {
        PyObject **items = _PyTuple_ITEMS(op);
        for (Py_ssize_t i = 0, n = PyTuple_GET_SIZE(op); i < n; i++) {
            if (intern_constant(ht, &items[i]) != 0) {
                return -1;
            }
        }
        return intern_immortal(ht, op, ptr);
    }
    else if (PyFrozenSet_CheckExact(op)) {
        // intern and immortalize set contents, but don't bother
        // with the set itself for now.
        PySetObject *set = (PySetObject *)op;
        for (Py_ssize_t i = 0; i <= set->mask; i++) {
            if (set->table[i].key != NULL) {
                if (intern_constant(ht, &set->table[i].key) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }

    // don't bother immortalizing code objects
    assert(PyCode_Check(op));
    return 0;
}

int
_PyCode_InternConstants(PyCodeObject *co)
{
    PyInterpreterState *is = PyThreadState_GET()->interp;
    _PyRecursiveMutex_lock(&is->consts_mutex);

    _Py_hashtable_t *consts = is->consts;
    if (consts == NULL) {
        consts = _Py_hashtable_new(&hash_const, &compare_const);
        if (consts == NULL) {
            goto error;
        }
        is->consts = consts;
    }

    for (Py_ssize_t i = 0; i < co->co_nconsts; i++) {
        if (intern_constant(consts, &co->co_constants[i]) != 0) {
            goto error;
        }
    }

    _PyRecursiveMutex_unlock(&is->consts_mutex);
    return 0;

error:
    _PyRecursiveMutex_unlock(&is->consts_mutex);
    return -1;
}

static PyObject *
code_repr(PyCodeObject *co)
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
code_hash(PyCodeObject *co)
{
    return 7;
}

static int
code_traverse(PyCodeObject *co, visitproc visit, void *arg)
{
    return 0;
}

PyObject*
_PyCode_ConstantKey(PyObject *op)
{
    PyObject *key;

    /* Py_None and Py_Ellipsis are singletons. */
    if (op == Py_None || op == Py_Ellipsis
       || PyLong_CheckExact(op)
       || PyUnicode_CheckExact(op)
          /* code_richcompare() uses _PyCode_ConstantKey() internally */
       || PyCode_Check(op)
       || PyCode_Check(op))
    {
        /* Objects of these types are always different from object of other
         * type and from tuples. */
        Py_INCREF(op);
        key = op;
    }
    else if (PyBool_Check(op) || PyBytes_CheckExact(op)) {
        /* Make booleans different from integers 0 and 1.
         * Avoid BytesWarning from comparing bytes with strings. */
        key = PyTuple_Pack(2, Py_TYPE(op), op);
    }
    else if (PyFloat_CheckExact(op)) {
        double d = PyFloat_AS_DOUBLE(op);
        /* all we need is to make the tuple different in either the 0.0
         * or -0.0 case from all others, just to avoid the "coercion".
         */
        if (d == 0.0 && copysign(1.0, d) < 0.0)
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_None);
        else
            key = PyTuple_Pack(2, Py_TYPE(op), op);
    }
    else if (PyComplex_CheckExact(op)) {
        Py_complex z;
        int real_negzero, imag_negzero;
        /* For the complex case we must make complex(x, 0.)
           different from complex(x, -0.) and complex(0., y)
           different from complex(-0., y), for any x and y.
           All four complex zeros must be distinguished.*/
        z = PyComplex_AsCComplex(op);
        real_negzero = z.real == 0.0 && copysign(1.0, z.real) < 0.0;
        imag_negzero = z.imag == 0.0 && copysign(1.0, z.imag) < 0.0;
        /* use True, False and None singleton as tags for the real and imag
         * sign, to make tuples different */
        if (real_negzero && imag_negzero) {
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_True);
        }
        else if (imag_negzero) {
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_False);
        }
        else if (real_negzero) {
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_None);
        }
        else {
            key = PyTuple_Pack(2, Py_TYPE(op), op);
        }
    }
    else if (PyTuple_CheckExact(op)) {
        Py_ssize_t i, len;
        PyObject *tuple;

        len = PyTuple_GET_SIZE(op);
        tuple = PyTuple_New(len);
        if (tuple == NULL)
            return NULL;

        for (i=0; i < len; i++) {
            PyObject *item, *item_key;

            item = PyTuple_GET_ITEM(op, i);
            item_key = _PyCode_ConstantKey(item);
            if (item_key == NULL) {
                Py_DECREF(tuple);
                return NULL;
            }

            PyTuple_SET_ITEM(tuple, i, item_key);
        }

        key = PyTuple_Pack(2, tuple, op);
        Py_DECREF(tuple);
    }
    else if (PyFrozenSet_CheckExact(op)) {
        Py_ssize_t pos = 0;
        PyObject *item;
        Py_hash_t hash;
        Py_ssize_t i, len;
        PyObject *tuple, *set;

        len = PySet_GET_SIZE(op);
        tuple = PyTuple_New(len);
        if (tuple == NULL)
            return NULL;

        i = 0;
        while (_PySet_NextEntry(op, &pos, &item, &hash)) {
            PyObject *item_key;

            item_key = _PyCode_ConstantKey(item);
            if (item_key == NULL) {
                Py_DECREF(tuple);
                return NULL;
            }

            assert(i < len);
            PyTuple_SET_ITEM(tuple, i, item_key);
            i++;
        }
        set = PyFrozenSet_New(tuple);
        Py_DECREF(tuple);
        if (set == NULL)
            return NULL;

        key = PyTuple_Pack(2, set, op);
        Py_DECREF(set);
        return key;
    }
    else if (PySlice_Check(op)) {
        PySliceObject *slice = (PySliceObject *)op;
        return Py_BuildValue("(O(OOO))", Py_TYPE(op), slice->start, slice->stop, slice->step);
    }
    else {
        /* for other types, use the object identifier as a unique identifier
         * to ensure that they are seen as unequal. */
        PyObject *obj_id = PyLong_FromVoidPtr(op);
        if (obj_id == NULL)
            return NULL;

        key = PyTuple_Pack(2, obj_id, op);
        Py_DECREF(obj_id);
    }
    return key;
}

static PyObject *
code_richcompare(PyObject *self, PyObject *other, int op)
{
    if ((op != Py_EQ && op != Py_NE) ||
        !PyCode_Check(self) ||
        !PyCode_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    PyCodeObject *a = (PyCodeObject *)self;
    PyCodeObject *b = (PyCodeObject *)other;

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
    eq = (0 == memcmp(PyCode_FirstInstr(a),
                      PyCode_FirstInstr(b),
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
code_sizeof(PyCodeObject *co, PyObject *Py_UNUSED(args))
{
    Py_ssize_t size = sizeof(PyCodeObject);
    size += co->co_size;
    size += co->co_nconsts * sizeof(PyObject *);
    return PyLong_FromSsize_t(size);
}

static PyObject *
code_getcode(PyCodeObject *co, PyObject *Py_UNUSED(args))
{
    uint8_t *bytecode = PyCode_FirstInstr(co);
    return PyBytes_FromStringAndSize((char *)bytecode, co->co_size);
}

static PyObject *
code_getnames(PyCodeObject *co, PyObject *Py_UNUSED(args))
{
    // TODO: traverse bytecode looking for LOAD_GLOBAL/LOAD_NAME ??
    return PyTuple_New(0);
}

static PyObject *
code_getconsts(PyCodeObject *co, PyObject *Py_UNUSED(args))
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
code_getexc_handlers(PyCodeObject *co, PyObject *Py_UNUSED(args))
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
code_get_jump_table(PyCodeObject *co, PyObject *Py_UNUSED(args))
{
    Py_ssize_t size = co->co_jump_table->size;
    PyObject *t = PyTuple_New(size);
    if (t == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i != size; i++) {
        JumpEntry *j = &co->co_jump_table->entries[i];
        PyObject *entry = Py_BuildValue("(nn)",
            (Py_ssize_t)j->from,
            (Py_ssize_t)j->delta);
        if (entry == NULL) {
            Py_DECREF(t);
            return NULL;
        }
        PyTuple_SET_ITEM(t, i, entry);
    }
    return t;
}

static PyObject *
code_getcell2reg(PyCodeObject *co, PyObject *Py_UNUSED(args))
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
code_getfree2reg(PyCodeObject *co, PyObject *Py_UNUSED(args))
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
    co_kwonlyargcount: int(c_default="self->co_kwonlyargcount") = -1
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
code_replace_impl(PyCodeObject *self, int co_argcount,
                  int co_posonlyargcount, int co_kwonlyargcount,
                  int co_ndefaultargs, int co_nlocals, int co_framesize,
                  int co_nmeta, int co_flags, int co_firstlineno,
                  PyObject *co_code, PyObject *co_consts,
                  PyObject *co_varnames, PyObject *co_freevars,
                  PyObject *co_cellvars, PyObject *co_filename,
                  PyObject *co_name, PyObject *co_lnotab)
/*[clinic end generated code: output=96a0f34eb63827c7 input=55640c6349f94397]*/
{
    PyObject *co = NULL;
    PyObject *eh_table = NULL;
    PyObject *jump_table = NULL;
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

    jump_table = code_get_jump_table(self, NULL);
    if (jump_table == NULL) goto cleanup;

    free2reg = code_getfree2reg(self, NULL);
    if (free2reg == NULL) goto cleanup;

    cell2reg = code_getcell2reg(self, NULL);
    if (cell2reg == NULL) goto cleanup;

    co = code_new_impl(
        &PyCode_Type,
        co_argcount,
        co_posonlyargcount,
        co_kwonlyargcount,
        co_nlocals,
        co_framesize,
        co_ndefaultargs,
        co_nmeta,
        co_flags,
        co_code,
        co_consts,
        co_varnames,
        co_filename,
        co_name,
        co_firstlineno,
        co_lnotab,
        eh_table,
        jump_table,
        co_freevars,
        co_cellvars,
        free2reg,
        cell2reg);

cleanup:
    Py_XDECREF(cell2reg);
    Py_XDECREF(free2reg);
    Py_XDECREF(co_code);
    Py_XDECREF(co_consts);
    Py_XDECREF(jump_table);
    Py_XDECREF(eh_table);
    return co;
}

static struct PyMethodDef code_methods[] = {
    {"__sizeof__", (PyCFunction)code_sizeof, METH_NOARGS},
    CODE_REPLACE_METHODDEF
    {NULL, NULL}                /* sentinel */
};

#define OFF(x) offsetof(PyCodeObject, x)

static PyMemberDef code_memberlist[] = {
    {"co_argcount",             T_PYSSIZET,  OFF(co_argcount),        READONLY},
    {"co_posonlyargcount",      T_PYSSIZET,  OFF(co_posonlyargcount), READONLY},
    {"co_kwonlyargcount",       T_PYSSIZET,  OFF(co_kwonlyargcount),  READONLY},
    {"co_totalargcount",        T_PYSSIZET,  OFF(co_totalargcount),   READONLY},
    {"co_nlocals",      T_PYSSIZET,     OFF(co_nlocals),         READONLY},
    {"co_framesize",    T_PYSSIZET,     OFF(co_framesize),       READONLY},
    {"co_stacksize",    T_PYSSIZET,     OFF(co_framesize),       READONLY},
    {"co_ndefaultargs", T_PYSSIZET,     OFF(co_ndefaultargs),    READONLY},
    {"co_nmeta",        T_PYSSIZET,     OFF(co_nmeta),           READONLY},
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
    {"co_code", (getter)code_getcode, (setter)NULL, "code bytes", NULL},
    {"co_consts", (getter)code_getconsts, (setter)NULL, "constants", NULL},
    {"co_names", (getter)code_getnames, (setter)NULL, "names", NULL},
    {"co_exc_handlers", (getter)code_getexc_handlers, (setter)NULL, "exception handlers", NULL},
    {"co_jump_table", (getter)code_get_jump_table, (setter)NULL, "jump side table", NULL},
    {"co_cell2reg", (getter)code_getcell2reg, (setter)NULL, "cell variables", NULL},
    {"co_free2reg", (getter)code_getfree2reg, (setter)NULL, "free variables", NULL},
    {NULL} /* sentinel */
};

PyTypeObject PyCode_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "code",
    sizeof(PyCodeObject),
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
    offsetof(PyCodeObject, co_weakreflist),     /* tp_weaklistoffset */
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
PyCode_Addr2Line(PyCodeObject *co, int addrq)
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

/* Update *bounds to describe the first and one-past-the-last instructions in
   the same line as lasti.  Return the number of that line. */
int
_PyCode_CheckLineNumber(PyCodeObject* co, int lasti, PyAddrPair *bounds)
{
    Py_ssize_t size;
    int addr, line;
    unsigned char* p;

    p = (unsigned char*)PyBytes_AS_STRING(co->co_lnotab);
    size = PyBytes_GET_SIZE(co->co_lnotab) / 2;

    addr = 0;
    line = co->co_firstlineno;
    assert(line > 0);

    /* possible optimization: if f->f_lasti == instr_ub
       (likely to be a common case) then we already know
       instr_lb -- if we stored the matching value of p
       somewhere we could skip the first while loop. */

    /* See lnotab_notes.txt for the description of
       co_lnotab.  A point to remember: increments to p
       come in (addr, line) pairs. */

    bounds->ap_lower = 0;
    while (size > 0) {
        if (addr + *p > lasti)
            break;
        addr += *p++;
        if ((signed char)*p)
            bounds->ap_lower = addr;
        line += (signed char)*p;
        p++;
        --size;
    }

    if (size > 0) {
        while (--size >= 0) {
            addr += *p++;
            if ((signed char)*p)
                break;
            p++;
        }
        bounds->ap_upper = addr;
    }
    else {
        bounds->ap_upper = INT_MAX;
    }

    return line;
}

int
_PyCode_GetExtra(PyObject *code, Py_ssize_t index, void **extra)
{
    if (!PyCode_Check(code)) {
        PyErr_BadInternalCall();
        return -1;
    }

    PyCodeObject *o = (PyCodeObject *) code;
    _PyCodeObjectExtra *co_extra = (_PyCodeObjectExtra*) o->co_extra;

    if (co_extra == NULL || co_extra->ce_size <= index) {
        *extra = NULL;
        return 0;
    }

    *extra = co_extra->ce_extras[index];
    return 0;
}


int
_PyCode_SetExtra(PyObject *code, Py_ssize_t index, void *extra)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();

    if (!PyCode_Check(code) || index < 0 ||
            index >= interp->co_extra_user_count) {
        PyErr_BadInternalCall();
        return -1;
    }

    PyCodeObject *o = (PyCodeObject *) code;
    _PyCodeObjectExtra *co_extra = (_PyCodeObjectExtra *) o->co_extra;

    if (co_extra == NULL || co_extra->ce_size <= index) {
        Py_ssize_t i = (co_extra == NULL ? 0 : co_extra->ce_size);
        co_extra = PyMem_Realloc(
                co_extra,
                sizeof(_PyCodeObjectExtra) +
                (interp->co_extra_user_count-1) * sizeof(void*));
        if (co_extra == NULL) {
            return -1;
        }
        for (; i < interp->co_extra_user_count; i++) {
            co_extra->ce_extras[i] = NULL;
        }
        co_extra->ce_size = interp->co_extra_user_count;
        o->co_extra = co_extra;
    }

    if (co_extra->ce_extras[index] != NULL) {
        freefunc free = interp->co_extra_freefuncs[index];
        if (free != NULL) {
            free(co_extra->ce_extras[index]);
        }
    }

    co_extra->ce_extras[index] = extra;
    return 0;
}
