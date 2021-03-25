/*
 * This file compiles an abstract syntax tree (AST) into Python bytecode.
 *
 * The primary entry point is PyAST_Compile(), which returns a
 * PyCodeObject.  The compiler makes several passes to build the code
 * object:
 *   1. Checks for future statements.  See future.c
 *   2. Builds a symbol table.  See symtable.c.
 *   3. Generate code for basic blocks.  See compiler_mod() in this file.
 *   4. Assemble the basic blocks into final code.  See assemble() in
 *      this file.
 *   5. Optimize the byte code (peephole optimizations).  See peephole.c
 *
 * Note that compiler_mod() suggests module, but the module ast type
 * (mod_ty) has cases for expressions and interactive statements.
 *
 * CAUTION: The VISIT_* macros abort the current function when they
 * encounter a problem. So don't invoke them when there is memory
 * which needs to be released. Code blocks are OK, as the compiler
 * structure takes care of releasing those.  Use the arena to manage
 * objects.
 */

#include "Python.h"

#include "pycore_pystate.h"   /* _PyInterpreterState_GET_UNSAFE() */
#include "pycore_code.h"
#include "Python-ast.h"
#include "ast.h"
#include "symtable.h"
#include "opcode2.h"
// #include "wordcode_helpers.h"
#include "code2.h"
#include "mimalloc.h"

#include <setjmp.h>

enum {
    DEFAULT_INSTR_SIZE = 32
};

#define COMPILER_ERROR(c) (longjmp(c->jb, 1))
#define MAX_IMMEDIATES 3

struct instr {
    uint8_t opcode;
    int32_t imm[MAX_IMMEDIATES];
    int32_t lineno;
};

struct instr_array {
    uint8_t *arr;
    uint32_t offset;
    uint32_t allocated;
};

enum {
    COMPILER_SCOPE_MODULE,
    COMPILER_SCOPE_CLASS,
    COMPILER_SCOPE_FUNCTION,
    COMPILER_SCOPE_ASYNC_FUNCTION,
    COMPILER_SCOPE_LAMBDA,
    COMPILER_SCOPE_COMPREHENSION,
};

/* The following items change on entry and exit of code blocks.
   They must be saved and restored when returning to a block.
*/
struct compiler_unit {
    struct compiler_unit *prev;

    struct instr_array instr;

    struct lineno_table {
        uint8_t *arr;
        uint32_t offset;
        uint32_t allocated;
    } lineno_table;

    PySTEntryObject *ste;

    PyObject *name;
    PyObject *qualname;  /* dot-separated qualified name (lazy) */
    int scope_type;

    /* The following fields are dicts that map objects to
       the index of them in co_XXX.      The index is used as
       the argument for opcodes that refer to those collections.
    */
    PyObject *consts;    /* all constants */
    PyObject *varnames;  /* local variables */
    PyObject *cellvars;  /* cell variables */
    PyObject *freevars;  /* free variables */

    PyObject *private;        /* for private name mangling */

    Py_ssize_t argcount;        /* number of arguments for block */
    Py_ssize_t posonlyargcount;        /* number of positional only arguments for block */
    Py_ssize_t kwonlyargcount; /* number of keyword only arguments for block */
    Py_ssize_t nlocals;
    Py_ssize_t max_registers;
    Py_ssize_t next_register;

    int reachable;
    int firstlineno; /* the first lineno of the block */
    int lineno;          /* the lineno for the current stmt */
    int col_offset;      /* the offset of the current stmt */
    int lineno_set;  /* boolean to indicate whether instr
                          has been generated with current lineno */
};

struct compiler {
    struct compiler_unit *unit;   /* compiler state for current block */
    struct symtable *st;
    PyObject *const_cache;     /* dict holding all constants */

    PyObject *filename;
    PyFutureFeatures *future; /* pointer to module's __future__ */
    PyCompilerFlags flags;

    int optimize;              /* optimization level */
    int interactive;           /* true if in interactive mode */
    int nestlevel;
    int do_not_emit_bytecode;  /* The compiler won't emit any bytecode
                                    if this value is different from zero.
                                    This can be used to temporarily visit
                                    nodes without emitting bytecode to
                                    check only errors. */
    PyArena *arena;            /* pointer to memory allocation arena */
    jmp_buf jb;
};

static int compiler_enter_scope(struct compiler *, PyObject *, int, void *, int);
static void compiler_exit_scope(struct compiler *c);
static void compiler_free(struct compiler *c);
static void compiler_unit_free(struct compiler_unit *u);

static PyCodeObject2 *compiler_mod(struct compiler *, mod_ty);
static void compiler_visit_stmts(struct compiler *, asdl_seq *stmts);
static void compiler_visit_stmt(struct compiler *, stmt_ty);
static void compiler_visit_expr(struct compiler *, expr_ty);

static PyCodeObject2 *assemble(struct compiler *, int addNone);

static PyCodeObject2 *
compile_object(struct compiler *c, mod_ty mod, PyObject *filename,
               PyCompilerFlags *flags, int optimize, PyArena *arena)
{
    if (setjmp(c->jb) != 0) {
        assert(PyErr_Occurred());
        compiler_free(c);
        return NULL;
    }

    c->const_cache = PyDict_New();
    if (!c->const_cache) {
        COMPILER_ERROR(c);
    }
    Py_INCREF(filename);
    c->filename = filename;
    c->arena = arena;
    c->optimize = optimize;
    c->nestlevel = 0;
    c->do_not_emit_bytecode = 0;
    c->future = PyFuture_FromASTObject(mod, filename);
    if (c->future == NULL) {
        COMPILER_ERROR(c);
    }
    if (flags != NULL) {
        int merged = flags->cf_flags | c->future->ff_features;
        flags->cf_flags = merged;
        c->future->ff_features = merged;
        c->flags = *flags;
    }
    else {
        c->flags = _PyCompilerFlags_INIT;
        c->flags.cf_flags = c->future->ff_features;
    }

    if (!_PyAST_Optimize(mod, arena, c->optimize)) {
        COMPILER_ERROR(c);
    }

    c->st = PySymtable_BuildObject(mod, filename, c->future);
    if (c->st == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_SystemError, "no symtable");
        }
        COMPILER_ERROR(c);
    }

    PyCodeObject2 *co = compiler_mod(c, mod);
    compiler_free(c);
    return co;
}

PyObject *
PyAST_CompileObject2(mod_ty mod, PyObject *filename, PyCompilerFlags *flags,
                     int optimize, PyArena *arena)
{
    struct compiler c;
    memset(&c, 0, sizeof(c));
    return (PyObject *)compile_object(&c, mod, filename, flags, optimize, arena);
}

static uint8_t *
next_instr(struct compiler *c, int size)
{
    struct instr_array *instr = &c->unit->instr;
    if (instr->offset + size >= instr->allocated) {
        if (instr->allocated > INT32_MAX / 2) {
            PyErr_NoMemory();
            COMPILER_ERROR(c);
        }
        Py_ssize_t new_size = instr->allocated * 2;
        if (new_size < DEFAULT_INSTR_SIZE) {
            new_size = DEFAULT_INSTR_SIZE;
        }

        uint8_t *arr = mi_rezalloc(instr->arr, new_size);
        if (arr == NULL) {
            PyErr_NoMemory();
            COMPILER_ERROR(c);
        }
        instr->arr = arr;
        instr->allocated = new_size;
    }
    uint8_t *ptr = &instr->arr[instr->offset];
    instr->offset += size;
    return ptr;
}

static Py_ssize_t
compiler_add_o(struct compiler *c, PyObject *dict, PyObject *o)
{
    PyObject *v;
    Py_ssize_t arg;

    v = PyDict_GetItemWithError(dict, o);
    if (!v) {
        if (PyErr_Occurred()) {
            return -1;
        }
        arg = PyDict_GET_SIZE(dict);
        v = PyLong_FromSsize_t(arg);
        if (!v) {
            return -1;
        }
        if (PyDict_SetItem(dict, o, v) < 0) {
            Py_DECREF(v);
            return -1;
        }
        Py_DECREF(v);
    }
    else
        arg = PyLong_AsLong(v);
    return arg;
}

// Merge const *o* recursively and return constant key object.
static PyObject*
merge_consts_recursive(struct compiler *c, PyObject *o)
{
    // None and Ellipsis are singleton, and key is the singleton.
    // No need to merge object and key.
    if (o == Py_None || o == Py_Ellipsis) {
        Py_INCREF(o);
        return o;
    }

    PyObject *key = _PyCode_ConstantKey(o);
    if (key == NULL) {
        return NULL;
    }

    // t is borrowed reference
    PyObject *t = PyDict_SetDefault(c->const_cache, key, key);
    if (t != key) {
        // o is registered in const_cache.  Just use it.
        Py_XINCREF(t);
        Py_DECREF(key);
        return t;
    }

    // We registered o in const_cache.
    // When o is a tuple or frozenset, we want to merge its
    // items too.
    if (PyTuple_CheckExact(o)) {
        Py_ssize_t len = PyTuple_GET_SIZE(o);
        for (Py_ssize_t i = 0; i < len; i++) {
            PyObject *item = PyTuple_GET_ITEM(o, i);
            PyObject *u = merge_consts_recursive(c, item);
            if (u == NULL) {
                Py_DECREF(key);
                return NULL;
            }

            // See _PyCode_ConstantKey()
            PyObject *v;  // borrowed
            if (PyTuple_CheckExact(u)) {
                v = PyTuple_GET_ITEM(u, 1);
            }
            else {
                v = u;
            }
            if (v != item) {
                Py_INCREF(v);
                PyTuple_SET_ITEM(o, i, v);
                Py_DECREF(item);
            }

            Py_DECREF(u);
        }
    }
    else if (PyFrozenSet_CheckExact(o)) {
        // *key* is tuple. And its first item is frozenset of
        // constant keys.
        // See _PyCode_ConstantKey() for detail.
        assert(PyTuple_CheckExact(key));
        assert(PyTuple_GET_SIZE(key) == 2);

        Py_ssize_t len = PySet_GET_SIZE(o);
        if (len == 0) {  // empty frozenset should not be re-created.
            return key;
        }
        PyObject *tuple = PyTuple_New(len);
        if (tuple == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        Py_ssize_t i = 0, pos = 0;
        PyObject *item;
        Py_hash_t hash;
        while (_PySet_NextEntry(o, &pos, &item, &hash)) {
            PyObject *k = merge_consts_recursive(c, item);
            if (k == NULL) {
                Py_DECREF(tuple);
                Py_DECREF(key);
                return NULL;
            }
            PyObject *u;
            if (PyTuple_CheckExact(k)) {
                u = PyTuple_GET_ITEM(k, 1);
                Py_INCREF(u);
                Py_DECREF(k);
            }
            else {
                u = k;
            }
            PyTuple_SET_ITEM(tuple, i, u);  // Steals reference of u.
            i++;
        }

        // Instead of rewriting o, we create new frozenset and embed in the
        // key tuple.  Caller should get merged frozenset from the key tuple.
        PyObject *new = PyFrozenSet_New(tuple);
        Py_DECREF(tuple);
        if (new == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        assert(PyTuple_GET_ITEM(key, 1) == o);
        Py_DECREF(o);
        PyTuple_SET_ITEM(key, 1, new);
    }

    return key;
}


static Py_ssize_t
compiler_add_const(struct compiler *c, PyObject *o)
{
    if (c->do_not_emit_bytecode) {
        return 0;
    }

    PyObject *key = merge_consts_recursive(c, o);
    if (key == NULL) {
        return -1;
    }

    Py_ssize_t arg = compiler_add_o(c, c->unit->consts, key);
    Py_DECREF(key);
    return arg;
}

static int32_t
const_none(struct compiler *c)
{
    return compiler_add_const(c, Py_None);
}

static void
write_uint32(uint8_t *pc, int imm)
{
    uint32_t value = (uint32_t)imm;
    memcpy(pc, &value, sizeof(uint32_t));
}

static void
emit0(struct compiler *c, int opcode)
{
    uint8_t *pc = next_instr(c, 1);
    pc[0] = opcode;
}

static void
emit1(struct compiler *c, int opcode, int imm0)
{
    int wide = (imm0 > 255);
    if (wide) {
        uint8_t *pc = next_instr(c, 6);
        pc[0] = WIDE;
        pc[1] = opcode;
        write_uint32(&pc[2], imm0);
    }
    else {
        uint8_t *pc = next_instr(c, 2);
        pc[0] = opcode;
        pc[1] = (uint8_t)imm0;
    }
}

/* Compile a sequence of statements, checking for a docstring
   and for annotations. */
static void
compiler_body(struct compiler *c, asdl_seq *stmts)
{
    emit1(c, FUNC_HEADER, 0);
    compiler_visit_stmts(c, stmts);
    if (c->unit->reachable) {
        emit1(c, LOAD_CONST, const_none(c));
        emit0(c, RETURN_VALUE);
    }
}

static void
compiler_visit_stmt_expr(struct compiler *c, expr_ty value)
{
    if (c->interactive && c->nestlevel <= 1) {
        return;
    }

    // if (value->kind == Constant_kind) {
    //     /* ignore constant statement */
    //     return 1;
    // }

    // VISIT(c, expr, value);
    // ADDOP(c, POP_TOP);
    // return 1;
}

static void
compiler_visit_stmt(struct compiler *c, stmt_ty s)
{
    /* Always assign a lineno to the next instruction for a stmt. */
    c->unit->lineno = s->lineno;
    c->unit->col_offset = s->col_offset;
    c->unit->lineno_set = 0;

    switch (s->kind) {
    // case FunctionDef_kind:
    //     return compiler_function(c, s, 0);
    // case ClassDef_kind:
    //     return compiler_class(c, s);
    // case Return_kind:
    //     return compiler_return(c, s);
    // case Delete_kind:
    //     VISIT_SEQ(c, expr, s->v.Delete.targets)
    //     break;
    // case Assign_kind:
    //     n = asdl_seq_LEN(s->v.Assign.targets);
    //     VISIT(c, expr, s->v.Assign.value);
    //     for (i = 0; i < n; i++) {
    //         if (i < n - 1)
    //             ADDOP(c, DUP_TOP);
    //         VISIT(c, expr,
    //               (expr_ty)asdl_seq_GET(s->v.Assign.targets, i));
    //     }
    //     break;
    // case AugAssign_kind:
    //     return compiler_augassign(c, s);
    // case AnnAssign_kind:
    //     return compiler_annassign(c, s);
    // case For_kind:
    //     return compiler_for(c, s);
    // case While_kind:
    //     return compiler_while(c, s);
    // case If_kind:
    //     return compiler_if(c, s);
    // case Raise_kind:
    //     n = 0;
    //     if (s->v.Raise.exc) {
    //         VISIT(c, expr, s->v.Raise.exc);
    //         n++;
    //         if (s->v.Raise.cause) {
    //             VISIT(c, expr, s->v.Raise.cause);
    //             n++;
    //         }
    //     }
    //     ADDOP_I(c, RAISE_VARARGS, (int)n);
    //     break;
    // case Try_kind:
    //     return compiler_try(c, s);
    // case Assert_kind:
    //     return compiler_assert(c, s);
    // case Import_kind:
    //     return compiler_import(c, s);
    // case ImportFrom_kind:
    //     return compiler_from_import(c, s);
    // case Global_kind:
    // case Nonlocal_kind:
    //     break;
    case Expr_kind:
        return compiler_visit_stmt_expr(c, s->v.Expr.value);
    // case Pass_kind:
    //     break;
    // case Break_kind:
    //     return compiler_break(c);
    // case Continue_kind:
    //     return compiler_continue(c);
    // case With_kind:
    //     return compiler_with(c, s, 0);
    // case AsyncFunctionDef_kind:
    //     return compiler_function(c, s, 1);
    // case AsyncWith_kind:
    //     return compiler_async_with(c, s, 0);
    // case AsyncFor_kind:
    //     return compiler_async_for(c, s);
    default:
        PyErr_Format(PyExc_RuntimeError, "unhandled stmt type %d", s->kind);
        COMPILER_ERROR(c);
    }
}

static void
compiler_visit_stmts(struct compiler *c, asdl_seq *stmts)
{
    for (int i = 0, n = asdl_seq_LEN(stmts); i != n; i++) {
        stmt_ty elt = asdl_seq_GET(stmts, i);
        compiler_visit_stmt(c, elt);
    }
}

static PyCodeObject2 *
compiler_mod(struct compiler *c, mod_ty mod)
{
    static _Py_Identifier module_ident = _Py_static_string_init("<module>");
    PyObject *module_str = _PyUnicode_FromId(&module_ident);
    if (!module_str) {
        COMPILER_ERROR(c);
    }

    compiler_enter_scope(c, module_str, COMPILER_SCOPE_MODULE, mod, /*lineno=*/0);
    switch (mod->kind) {
    case Module_kind:
        compiler_body(c, mod->v.Module.body);
        break;
    // case Interactive_kind:
    //     if (find_ann(mod->v.Interactive.body)) {
    //         ADDOP(c, SETUP_ANNOTATIONS);
    //     }
    //     c->c_interactive = 1;
    //     VISIT_SEQ_IN_SCOPE(c, stmt, mod->v.Interactive.body);
    //     break;
    // case Expression_kind:
    //     VISIT_IN_SCOPE(c, expr, mod->v.Expression.body);
    //     addNone = 0;
    //     break;
    // case Suite_kind:
    //     PyErr_SetString(PyExc_SystemError,
    //                     "suite should not be possible");
    //     COMPILER_ERROR(c);
    default:
        PyErr_Format(PyExc_SystemError,
                     "module kind %d should not be possible",
                     mod->kind);
        COMPILER_ERROR(c);
    }

    PyCodeObject2 *co = assemble(c, /*addNOne???*/0);
    compiler_exit_scope(c);
    return co;
    // PyErr_Format(PyExc_RuntimeError, "nyi");
    // COMPILER_ERROR(c);
    // co = assemble(c, addNone);
    // return co;
}

static void
compiler_free(struct compiler *c)
{
    if (c->st)
        PySymtable_Free(c->st);
    if (c->future)
        PyObject_Free(c->future);
    Py_XDECREF(c->filename);
    Py_DECREF(c->const_cache);
    while(c->unit != NULL) {
        struct compiler_unit *u = c->unit;
        c->unit = u->prev;
        compiler_unit_free(u);
    }
}


static PyObject *
list2dict(PyObject *list)
{
    Py_ssize_t i, n;
    PyObject *v, *k;
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    n = PyList_Size(list);
    for (i = 0; i < n; i++) {
        v = PyLong_FromSsize_t(i);
        if (!v) {
            Py_DECREF(dict);
            return NULL;
        }
        k = PyList_GET_ITEM(list, i);
        if (PyDict_SetItem(dict, k, v) < 0) {
            Py_DECREF(v);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(v);
    }
    return dict;
}

/* Return new dict containing names from src that match scope(s).

src is a symbol table dictionary.  If the scope of a name matches
either scope_type or flag is set, insert it into the new dict.  The
values are integers, starting at offset and increasing by one for
each key.
*/

static PyObject *
dictbytype(PyObject *src, int scope_type, int flag, Py_ssize_t offset)
{
    Py_ssize_t i = offset, scope, num_keys, key_i;
    PyObject *k, *v, *dest = PyDict_New();
    PyObject *sorted_keys;

    assert(offset >= 0);
    if (dest == NULL)
        return NULL;

    /* Sort the keys so that we have a deterministic order on the indexes
       saved in the returned dictionary.  These indexes are used as indexes
       into the free and cell var storage.  Therefore if they aren't
       deterministic, then the generated bytecode is not deterministic.
    */
    sorted_keys = PyDict_Keys(src);
    if (sorted_keys == NULL)
        return NULL;
    if (PyList_Sort(sorted_keys) != 0) {
        Py_DECREF(sorted_keys);
        return NULL;
    }
    num_keys = PyList_GET_SIZE(sorted_keys);

    for (key_i = 0; key_i < num_keys; key_i++) {
        /* XXX this should probably be a macro in symtable.h */
        long vi;
        k = PyList_GET_ITEM(sorted_keys, key_i);
        v = PyDict_GetItem(src, k);
        assert(PyLong_Check(v));
        vi = PyLong_AS_LONG(v);
        scope = (vi >> SCOPE_OFFSET) & SCOPE_MASK;

        if (scope == scope_type || vi & flag) {
            PyObject *item = PyLong_FromSsize_t(i);
            if (item == NULL) {
                Py_DECREF(sorted_keys);
                Py_DECREF(dest);
                return NULL;
            }
            i++;
            if (PyDict_SetItem(dest, k, item) < 0) {
                Py_DECREF(sorted_keys);
                Py_DECREF(item);
                Py_DECREF(dest);
                return NULL;
            }
            Py_DECREF(item);
        }
    }
    Py_DECREF(sorted_keys);
    return dest;
}

static int
compiler_enter_scope(struct compiler *c, PyObject *name,
                     int scope_type, void *key, int lineno)
{
    struct compiler_unit *u;

    u = PyObject_Malloc(sizeof(struct compiler_unit));
    if (u == NULL) {
        PyErr_NoMemory();
        COMPILER_ERROR(c);
    }

    memset(u, 0, sizeof(struct compiler_unit));

    // Push onto stack
    u->prev = c->unit;
    if (c->unit) {
        u->private = c->unit->private;
        Py_XINCREF(u->private);
    }
    c->unit = u;

    u->reachable = true;
    u->scope_type = scope_type;
    u->argcount = 0;
    u->posonlyargcount = 0;
    u->kwonlyargcount = 0;
    u->ste = PySymtable_Lookup(c->st, key);
    if (!u->ste) {
        COMPILER_ERROR(c);
    }
    Py_INCREF(name);
    u->name = name;
    u->varnames = list2dict(u->ste->ste_varnames);
    if (u->varnames == NULL) {
        COMPILER_ERROR(c);
    }
    u->nlocals = PyDict_GET_SIZE(u->varnames);
    u->max_registers = u->next_register = u->nlocals;
    u->cellvars = dictbytype(u->ste->ste_symbols, CELL, 0, 0);
    if (u->cellvars == NULL) {
        COMPILER_ERROR(c);
    }
    if (u->ste->ste_needs_class_closure) {
        /* Cook up an implicit __class__ cell. */
        _Py_IDENTIFIER(__class__);
        PyObject *name;
        int res;
        assert(u->scope_type == COMPILER_SCOPE_CLASS);
        assert(PyDict_GET_SIZE(u->cellvars) == 0);
        name = _PyUnicode_FromId(&PyId___class__);
        if (!name) {
            COMPILER_ERROR(c);
        }
        res = PyDict_SetItem(u->cellvars, name, _PyLong_Zero);
        if (res < 0) {
            COMPILER_ERROR(c);
        }
    }

    u->freevars = dictbytype(u->ste->ste_symbols, FREE, DEF_FREE_CLASS,
                               PyDict_GET_SIZE(u->cellvars));
    if (!u->freevars) {
        COMPILER_ERROR(c);
    }

    u->firstlineno = lineno;
    u->lineno = 0;
    u->col_offset = 0;
    u->lineno_set = 0;
    u->consts = PyDict_New();
    if (!u->consts) {
        COMPILER_ERROR(c);
    }

    u->private = NULL;
    c->unit = u;
    c->nestlevel++;

    // if (u->u_scope_type != COMPILER_SCOPE_MODULE) {
    //     if (!compiler_set_qualname(c))
    //         return 0;
    // }

    return 1;
}

static void
compiler_exit_scope(struct compiler *c)
{
    // Py_ssize_t n;
    // PyObject *capsule;

    // c->nestlevel--;
    // compiler_unit_free(c->u);
    // /* Restore c->u to the parent unit. */
    // n = PyList_GET_SIZE(c->c_stack) - 1;
    // if (n >= 0) {
    //     capsule = PyList_GET_ITEM(c->c_stack, n);
    //     c->u = (struct compiler_unit *)PyCapsule_GetPointer(capsule, CAPSULE_NAME);
    //     assert(c->u);
    //     /* we are deleting from a list so this really shouldn't fail */
    //     if (PySequence_DelItem(c->c_stack, n) < 0)
    //         Py_FatalError("compiler_exit_scope()");
    //     compiler_unit_check(c->u);
    // }
    // else
    //     c->u = NULL;
}

static void
compiler_unit_free(struct compiler_unit *u)
{
    // basicblock *b, *next;

    // compiler_unit_check(u);
    // b = u->u_blocks;
    // while (b != NULL) {
    //     if (b->b_instr)
    //         PyObject_Free((void *)b->b_instr);
    //     next = b->b_list;
    //     PyObject_Free((void *)b);
    //     b = next;
    // }
    // Py_CLEAR(u->u_ste);
    // Py_CLEAR(u->u_name);
    // Py_CLEAR(u->u_qualname);
    // Py_CLEAR(u->u_consts);
    // Py_CLEAR(u->u_names);
    // Py_CLEAR(u->u_varnames);
    // Py_CLEAR(u->u_freevars);
    // Py_CLEAR(u->u_cellvars);
    // Py_CLEAR(u->u_private);
    // PyObject_Free(u);
}

static int
compiler_call(struct compiler *c, expr_ty e)
{
    // int ret;
    // ret = maybe_optimize_method_call(c, e);
    // if (ret >= 0) {
    //     return ret;
    // }
    // expr_ty func = e->v.Call.func;
    // if (!check_caller(c, func)) {
    //     return 0;
    // }
    // if (func->kind == Name_kind) {
    //     if (!compiler_nameop(c, func->v.Name.id, FuncLoad)) {
    //         return 0;
    //     }
    // }
    // else {
    //     VISIT(c, expr, func);
    //     ADDOP(c, DEFER_REFCOUNT);
    // }
    // return compiler_call_helper(c, 0,
    //                             e->v.Call.args,
    //                             e->v.Call.keywords);
}

static void
compiler_visit_expr(struct compiler *c, expr_ty e)
{
    c->unit->lineno = e->lineno;
    c->unit->col_offset = e->col_offset;


    switch (e->kind) {
    // case NamedExpr_kind:
    //     VISIT(c, expr, e->v.NamedExpr.value);
    //     ADDOP(c, DUP_TOP);
    //     VISIT(c, expr, e->v.NamedExpr.target);
    //     break;
    // case BoolOp_kind:
    //     return compiler_boolop(c, e);
    // case BinOp_kind:
    //     VISIT(c, expr, e->v.BinOp.left);
    //     VISIT(c, expr, e->v.BinOp.right);
    //     ADDOP(c, binop(c, e->v.BinOp.op));
    //     break;
    // case UnaryOp_kind:
    //     VISIT(c, expr, e->v.UnaryOp.operand);
    //     ADDOP(c, unaryop(e->v.UnaryOp.op));
    //     break;
    // case Lambda_kind:
    //     return compiler_lambda(c, e);
    // case IfExp_kind:
    //     return compiler_ifexp(c, e);
    // case Dict_kind:
    //     return compiler_dict(c, e);
    // case Set_kind:
    //     return compiler_set(c, e);
    // case GeneratorExp_kind:
    //     return compiler_genexp(c, e);
    // case ListComp_kind:
    //     return compiler_listcomp(c, e);
    // case SetComp_kind:
    //     return compiler_setcomp(c, e);
    // case DictComp_kind:
    //     return compiler_dictcomp(c, e);
    // case Yield_kind:
    //     if (c->u->u_ste->ste_type != FunctionBlock)
    //         return compiler_error(c, "'yield' outside function");
    //     if (e->v.Yield.value) {
    //         VISIT(c, expr, e->v.Yield.value);
    //     }
    //     else {
    //         ADDOP_LOAD_CONST(c, Py_None);
    //     }
    //     ADDOP(c, YIELD_VALUE);
    //     break;
    // case YieldFrom_kind:
    //     if (c->u->u_ste->ste_type != FunctionBlock)
    //         return compiler_error(c, "'yield' outside function");

    //     if (c->u->u_scope_type == COMPILER_SCOPE_ASYNC_FUNCTION)
    //         return compiler_error(c, "'yield from' inside async function");

    //     VISIT(c, expr, e->v.YieldFrom.value);
    //     ADDOP(c, GET_YIELD_FROM_ITER);
    //     ADDOP_LOAD_CONST(c, Py_None);
    //     ADDOP(c, YIELD_FROM);
    //     break;
    // case Await_kind:
    //     if (!(c->c_flags->cf_flags & PyCF_ALLOW_TOP_LEVEL_AWAIT)){
    //         if (c->u->u_ste->ste_type != FunctionBlock){
    //             return compiler_error(c, "'await' outside function");
    //         }

    //         if (c->u->u_scope_type != COMPILER_SCOPE_ASYNC_FUNCTION &&
    //                 c->u->u_scope_type != COMPILER_SCOPE_COMPREHENSION){
    //             return compiler_error(c, "'await' outside async function");
    //         }
    //     }

    //     VISIT(c, expr, e->v.Await.value);
    //     ADDOP(c, GET_AWAITABLE);
    //     ADDOP_LOAD_CONST(c, Py_None);
    //     ADDOP(c, YIELD_FROM);
    //     break;
    // case Compare_kind:
    //     return compiler_compare(c, e);
    case Call_kind:
        return compiler_call(c, e);
    // case Constant_kind:
    //     ADDOP_LOAD_CONST(c, e->v.Constant.value);
    //     break;
    // case JoinedStr_kind:
    //     return compiler_joined_str(c, e);
    // case FormattedValue_kind:
    //     return compiler_formatted_value(c, e);
    // /* The following exprs can be assignment targets. */
    // case Attribute_kind:
    //     if (e->v.Attribute.ctx != AugStore)
    //         VISIT(c, expr, e->v.Attribute.value);
    //     switch (e->v.Attribute.ctx) {
    //     case AugLoad:
    //         ADDOP(c, DUP_TOP);
    //         /* Fall through */
    //     case Load:
    //         ADDOP_NAME(c, LOAD_ATTR, e->v.Attribute.attr, names);
    //         break;
    //     case AugStore:
    //         ADDOP(c, ROT_TWO);
    //         /* Fall through */
    //     case Store:
    //         ADDOP_NAME(c, STORE_ATTR, e->v.Attribute.attr, names);
    //         break;
    //     case Del:
    //         ADDOP_NAME(c, DELETE_ATTR, e->v.Attribute.attr, names);
    //         break;
    //     case Param:
    //     default:
    //         PyErr_SetString(PyExc_SystemError,
    //                         "param invalid in attribute expression");
    //         return 0;
    //     }
    //     break;
    // case Subscript_kind:
    //     switch (e->v.Subscript.ctx) {
    //     case AugLoad:
    //         VISIT(c, expr, e->v.Subscript.value);
    //         VISIT_SLICE(c, e->v.Subscript.slice, AugLoad);
    //         break;
    //     case Load:
    //         if (!check_subscripter(c, e->v.Subscript.value)) {
    //             return 0;
    //         }
    //         if (!check_index(c, e->v.Subscript.value, e->v.Subscript.slice)) {
    //             return 0;
    //         }
    //         VISIT(c, expr, e->v.Subscript.value);
    //         VISIT_SLICE(c, e->v.Subscript.slice, Load);
    //         break;
    //     case AugStore:
    //         VISIT_SLICE(c, e->v.Subscript.slice, AugStore);
    //         break;
    //     case Store:
    //         VISIT(c, expr, e->v.Subscript.value);
    //         VISIT_SLICE(c, e->v.Subscript.slice, Store);
    //         break;
    //     case Del:
    //         VISIT(c, expr, e->v.Subscript.value);
    //         VISIT_SLICE(c, e->v.Subscript.slice, Del);
    //         break;
    //     case Param:
    //     default:
    //         PyErr_SetString(PyExc_SystemError,
    //             "param invalid in subscript expression");
    //         return 0;
    //     }
    //     break;
    // case Starred_kind:
    //     switch (e->v.Starred.ctx) {
    //     case Store:
    //         /* In all legitimate cases, the Starred node was already replaced
    //          * by compiler_list/compiler_tuple. XXX: is that okay? */
    //         return compiler_error(c,
    //             "starred assignment target must be in a list or tuple");
    //     default:
    //         return compiler_error(c,
    //             "can't use starred expression here");
    //     }
    // case Name_kind:
    //     return compiler_nameop(c, e->v.Name.id, e->v.Name.ctx);
    // /* child nodes of List and Tuple will have expr_context set */
    // case List_kind:
    //     return compiler_list(c, e);
    // case Tuple_kind:
    //     return compiler_tuple(c, e);
    }
}

// struct assembler {
//     PyObject *a_bytecode;  /* string containing bytecode */
//     int a_offset;              /* offset into bytecode */
//     int a_nblocks;             /* number of reachable blocks */
//     PyObject *a_lnotab;    /* string containing lnotab */
//     int a_lnotab_off;      /* offset into lnotab */
//     int a_lineno;              /* last lineno of emitted instruction */
//     int a_lineno_off;      /* bytecode offset of last lineno */
// };

// static void
// assemble_init(struct compiler *c, struct assembler *a, int firstlineno)
// {
//     memset(a, 0, sizeof(struct assembler));
//     a->a_lineno = firstlineno;
//     a->a_bytecode = PyBytes_FromStringAndSize(NULL, DEFAULT_CODE_SIZE);
//     if (!a->a_bytecode) {
//         COMPILER_ERROR(c);
//     }
//     a->a_lnotab = PyBytes_FromStringAndSize(NULL, DEFAULT_LNOTAB_SIZE);
//     if (!a->a_lnotab) {
//         COMPILER_ERROR(c);
//     }
//     return 1;
// }

// static void
// assemble_free(struct assembler *a)
// {
//     Py_XDECREF(a->a_bytecode);
//     Py_XDECREF(a->a_lnotab);
//     // if (a->a_postorder)
//     //     PyObject_Free(a->a_postorder);
// }

static PyCodeObject2 *
makecode(struct compiler *c)
{
    Py_ssize_t instr_size = c->unit->instr.offset;
    Py_ssize_t nconsts = PyDict_GET_SIZE(c->unit->consts);
    Py_ssize_t niconsts = 0;
    Py_ssize_t nmeta = 0;
    Py_ssize_t ncells = 0;
    Py_ssize_t ncaptures = 0;
    Py_ssize_t nexc_handlers = 0;

    PyCodeObject2 *co = PyCode2_New(instr_size, nconsts, niconsts, nmeta, ncells, ncaptures, nexc_handlers);
    if (co == NULL) {
        COMPILER_ERROR(c);
    }

    co->co_argcount = c->unit->argcount;
    co->co_posonlyargcount = c->unit->posonlyargcount;
    co->co_totalargcount = c->unit->kwonlyargcount + c->unit->argcount;
    co->co_nlocals = c->unit->nlocals;
    // co->co_ndefaultargs = ndefaultargs;
    // co->co_flags = flags;
    // co->co_framesize = framesize;
    co->co_varnames = PyTuple_New(0);
    co->co_freevars = PyTuple_New(0);
    co->co_cellvars = PyTuple_New(0);
    co->co_filename = c->filename;
    Py_INCREF(co->co_filename);
    co->co_name = c->unit->name;
    Py_INCREF(co->co_name);
    co->co_firstlineno = c->unit->firstlineno;
    co->co_lnotab = PyBytes_FromStringAndSize("", 0);
    memcpy(PyCode2_GET_CODE(co), c->unit->instr.arr, c->unit->instr.offset);

    PyObject *consts = c->unit->consts;
    Py_ssize_t pos = 0, i = 0;
    PyObject *key, *value;
    while (PyDict_Next(consts, &pos, &key, &value)) {
        Py_INCREF(key);
        if (PyUnicode_CheckExact(key)) {
            PyUnicode_InternInPlace(&key);
        }
        co->co_constants[i] = key;
        i++;
    }

    return co;
}

static PyCodeObject2 *
assemble(struct compiler *c, int addNone)
{
    PyCodeObject2 *co;
    co = makecode(c);
    // assemble_free(&a);
    return co;
    // TODO: add missing reutrn
}

/* Emits a SyntaxWarning and returns 1 on success.
   If a SyntaxWarning raised as error, replaces it with a SyntaxError
   and returns 0.
*/
// static int
// compiler_warn(struct compiler *c, const char *format, ...)
// {
//     va_list vargs;
// #ifdef HAVE_STDARG_PROTOTYPES
//     va_start(vargs, format);
// #else
//     va_start(vargs);
// #endif
//     PyObject *msg = PyUnicode_FromFormatV(format, vargs);
//     va_end(vargs);
//     if (msg == NULL) {
//         return 0;
//     }
//     if (PyErr_WarnExplicitObject(PyExc_SyntaxWarning, msg, c->c_filename,
//                                  c->u->u_lineno, NULL, NULL) < 0)
//     {
//         if (PyErr_ExceptionMatches(PyExc_SyntaxWarning)) {
//             /* Replace the SyntaxWarning exception with a SyntaxError
//                to get a more accurate error report */
//             PyErr_Clear();
//             assert(PyUnicode_AsUTF8(msg) != NULL);
//             compiler_error(c, PyUnicode_AsUTF8(msg));
//         }
//         Py_DECREF(msg);
//         return 0;
//     }
//     Py_DECREF(msg);
//     return 1;
// }