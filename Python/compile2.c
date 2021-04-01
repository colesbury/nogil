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
#include "code2.h"
#include "mimalloc.h"

#include <setjmp.h>

enum {
    FRAME_EXTRA = 4, // FIXME get from ceval2_meta.h
    REG_ACCUMULATOR = -1
};

enum {
    DEFAULT_INSTR_SIZE = 32,
    DEFAULT_LNOTAB_SIZE = 16,
    MAX_IMMEDIATES = 3
};

#define COMPILER_ERROR(c) (longjmp(c->jb, 1))

// #define DEFAULT_BLOCK_SIZE 16
// #define DEFAULT_BLOCKS 8
// #define DEFAULT_CODE_SIZE 128
// #define DEFAULT_LNOTAB_SIZE 16

// #define COMP_GENEXP   0
// #define COMP_LISTCOMP 1
// #define COMP_SETCOMP  2
// #define COMP_DICTCOMP 3

enum {
    COMPILER_SCOPE_MODULE,
    COMPILER_SCOPE_CLASS,
    COMPILER_SCOPE_FUNCTION,
    COMPILER_SCOPE_ASYNC_FUNCTION,
    COMPILER_SCOPE_LAMBDA,
    COMPILER_SCOPE_COMPREHENSION,
};

enum {
    ACCESS_FAST = 0,
    ACCESS_DEREF = 1,
    ACCESS_CLASSDEREF = 2,
    ACCESS_NAME = 3,
    ACCESS_GLOBAL = 4
};

struct instr_array {
    uint8_t *arr;
    uint32_t offset;
    uint32_t allocated;
};

struct bc_label {
    int bound;
    uint32_t offset;
};

struct multi_label {
    struct bc_label *arr;
    Py_ssize_t n;
    Py_ssize_t allocated;
};

#define MULTI_LABEL_INIT {NULL, 0, 0}

enum frame_block_type {
    WHILE_LOOP,
    FOR_LOOP,
    TRY_FINALLY,
    EXCEPT,
    WITH,
    ASYNC_WITH
};

struct fblock {
    enum frame_block_type type;
    union {
        struct {
            Py_ssize_t reg;
            struct multi_label *break_label;
            struct multi_label *continue_label;
        } ForLoop;

        struct {
            struct multi_label *break_label;
            struct multi_label *continue_label;
        } WhileLoop;

        struct {
            struct multi_label *label;
            Py_ssize_t reg;
        } TryFinally;

        struct {
            Py_ssize_t reg;
        } Except;

        struct {
            Py_ssize_t reg;
        } With;

        struct {
            Py_ssize_t reg;
        } AsyncWith;
    };
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

    struct block_table {
        struct fblock *arr;
        uint32_t offset;
        uint32_t allocated;
    } blocks;

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
    PyObject *metadata;  /* hints for global loads */

    PyObject *private;        /* for private name mangling */

    Py_ssize_t argcount;        /* number of arguments for block */
    Py_ssize_t posonlyargcount; /* number of positional only arguments for block */
    Py_ssize_t kwonlyargcount;  /* number of keyword only arguments for block */
    Py_ssize_t nlocals;
    Py_ssize_t max_registers;
    Py_ssize_t next_register;

    int reachable;
    int firstlineno;    /* the first lineno of the block */
    int lineno;         /* the lineno for the current stmt */
    int col_offset;     /* the offset of the current stmt */
    int lineno_set;     /* boolean to indicate whether instr
                           has been generated with current lineno */
};

/* This struct captures the global state of a compilation.

FIXME: out of date
The unit pointer points to the current compilation unit, while units
for enclosing blocks are stored in c_stack.     The u and c_stack are
managed by compiler_enter_scope() and compiler_exit_scope().

Note that we don't track recursion levels during compilation - the
task of detecting and rejecting excessive levels of nesting is
handled by the symbol analysis pass.

*/

struct compiler {
    struct compiler_unit *unit;   /* compiler state for current block */
    struct symtable *st;
    PyObject *const_cache;     /* dict holding all constants */

    PyCodeObject2 *code;
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
    mi_heap_t *heap;
    jmp_buf jb;
};

static int compiler_enter_scope(struct compiler *, identifier, int, void *, int);
static void compiler_free(struct compiler *);
static void compiler_unit_free(struct compiler_unit *u);
// static basicblock *compiler_new_block(struct compiler *);
// static int compiler_next_instr(struct compiler *, basicblock *);
// static int compiler_addop(struct compiler *, int);
// static int compiler_addop_i(struct compiler *, int, Py_ssize_t);
// static int compiler_addop_j(struct compiler *, int, basicblock *, int);
static void compiler_error(struct compiler *, const char *);
static void compiler_warn(struct compiler *, const char *, ...);
static void compiler_nameop(struct compiler *, identifier, expr_context_ty);
static void compiler_store(struct compiler *, identifier);
static int compiler_access(struct compiler *c, PyObject *mangled_name);
static int32_t compiler_varname(struct compiler *c, PyObject *mangled_name);
static int32_t const_none(struct compiler *c);

static PyCodeObject2 *compiler_mod(struct compiler *, mod_ty);
static void compiler_visit_stmts(struct compiler *, asdl_seq *stmts);
static void compiler_visit_stmt(struct compiler *, stmt_ty);
// static int compiler_visit_keyword(struct compiler *, keyword_ty);
static void compiler_visit_expr(struct compiler *, expr_ty);
static void compiler_augassign(struct compiler *, stmt_ty);
static void compiler_annassign(struct compiler *, stmt_ty);
static void compiler_slice(struct compiler *, slice_ty);
static void compiler_assign_reg(struct compiler *c, expr_ty target, Py_ssize_t value);

static int inplace_binop(struct compiler *, operator_ty);
static int are_all_items_const(asdl_seq *, Py_ssize_t, Py_ssize_t);
static int expr_constant(expr_ty);

// static int compiler_with(struct compiler *, stmt_ty, int);
// static int compiler_async_with(struct compiler *, stmt_ty, int);
// static int compiler_async_for(struct compiler *, stmt_ty);
// static void compiler_call_helper(struct compiler *c, int n,
//                                  asdl_seq *args,
//                                  asdl_seq *keywords);
static void compiler_call(struct compiler *c, expr_ty e);
// static int compiler_try_except(struct compiler *, stmt_ty);
static void compiler_set_qualname(struct compiler *, struct compiler_unit *);

// static int compiler_sync_comprehension_generator(
//                                       struct compiler *c,
//                                       asdl_seq *generators, int gen_index,
//                                       expr_ty elt, expr_ty val, int type);

// static int compiler_async_comprehension_generator(
//                                       struct compiler *c,
//                                       asdl_seq *generators, int gen_index,
//                                       expr_ty elt, expr_ty val, int type);

static void assemble(struct compiler *, int addNone);
// static PyObject *__doc__, *__annotations__;

// #define CAPSULE_NAME "compile.c compiler unit"
_Py_IDENTIFIER(__name__);
_Py_IDENTIFIER(__module__);
_Py_IDENTIFIER(__qualname__);
_Py_IDENTIFIER(__class__);
_Py_IDENTIFIER(__annotations__);

// TODO: copy _Py_Mangle from compile.c
PyAPI_FUNC(PyObject*) _Py_Mangle(PyObject *p, PyObject *name);

static PyObject *
mangle(struct compiler *c, PyObject *name)
{
    PyObject *mangled = _Py_Mangle(c->unit->private, name);
    if (!mangled) {
        COMPILER_ERROR(c);
    }
    PyObject *t = PyDict_SetDefault(c->const_cache, mangled, mangled);
    Py_DECREF(mangled);
    if (t == NULL) {
        COMPILER_ERROR(c);
    }
    return t;
}

// static int
// compiler_init(struct compiler *c)
// {
//     memset(c, 0, sizeof(struct compiler));

//     c->c_const_cache = PyDict_New();
//     if (!c->c_const_cache) {
//         return 0;
//     }

//     c->c_stack = PyList_New(0);
//     if (!c->c_stack) {
//         Py_CLEAR(c->c_const_cache);
//         return 0;
//     }

//     return 1;
// }

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
    c->heap = mi_heap_new();
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

// PyCodeObject *
// PyAST_CompileEx(mod_ty mod, const char *filename_str, PyCompilerFlags *flags,
//                 int optimize, PyArena *arena)
// {
//     PyObject *filename;
//     PyCodeObject *co;
//     filename = PyUnicode_DecodeFSDefault(filename_str);
//     if (filename == NULL)
//         return NULL;
//     co = PyAST_CompileObject(mod, filename, flags, optimize, arena);
//     Py_DECREF(filename);
//     return co;

// }

// PyCodeObject *
// PyNode_Compile(struct _node *n, const char *filename)
// {
//     PyCodeObject *co = NULL;
//     mod_ty mod;
//     PyArena *arena = PyArena_New();
//     if (!arena)
//         return NULL;
//     mod = PyAST_FromNode(n, NULL, filename, arena);
//     if (mod)
//         co = PyAST_Compile(mod, filename, NULL, arena);
//     PyArena_Free(arena);
//     return co;
// }

static void
compiler_free(struct compiler *c)
{
    if (c->st)
        PySymtable_Free(c->st);
    if (c->future)
        PyObject_Free(c->future);
    Py_XDECREF(c->filename);
    Py_DECREF(c->const_cache);
    Py_CLEAR(c->code);
    while(c->unit != NULL) {
        struct compiler_unit *u = c->unit;
        c->unit = u->prev;
        compiler_unit_free(u);
    }
    if (c->heap) {
        mi_heap_destroy(c->heap);
        c->heap = NULL;
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

static PyObject *
unicode_from_id(struct compiler *c, _Py_Identifier *id)
{
    PyObject *s = _PyUnicode_FromId(id);
    if (s == NULL) {
        COMPILER_ERROR(c);
    } 
    return s;
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

static void
add_local_variables(struct compiler *c, PyObject *varnames, PyObject *symbols)
{
    Py_ssize_t pos = 0;
    PyObject *key;
    PyObject *value;
    while (PyDict_Next(symbols, &pos, &key, &value)) {
        long vi = PyLong_AS_LONG(value);
        Py_ssize_t scope = (vi >> SCOPE_OFFSET) & SCOPE_MASK;
        printf("symbol %s scope %zd\n", PyUnicode_AsUTF8(key), scope);
        if (scope != LOCAL) {
            continue;
        }
        if (PyDict_Contains(varnames, key)) {
            continue;
        }
        PyObject *idx = PyLong_FromLong(PyDict_GET_SIZE(varnames));
        if (idx == NULL) {
            COMPILER_ERROR(c);
        }
        if (PyDict_SetItem(varnames, key, idx) < 0) {
            Py_DECREF(idx);
            COMPILER_ERROR(c);
        }
    }
}

static void
add_locals(struct compiler *c, PyObject *varnames)
{
    _Py_static_string(PyId_locals, "<locals>");
    PyObject *idx = PyLong_FromLong(PyDict_GET_SIZE(varnames));
    assert(PyDict_GET_SIZE(varnames) == 0);
    if (idx == NULL) {
        COMPILER_ERROR(c);
    }
    if (_PyDict_SetItemId(varnames, &PyId_locals, idx) < 0) {
        Py_DECREF(idx);
        COMPILER_ERROR(c);
    }
}

// static void
// compiler_unit_check(struct compiler_unit *u)
// {
//     basicblock *block;
//     for (block = u->u_blocks; block != NULL; block = block->b_list) {
//         assert((uintptr_t)block != 0xcbcbcbcbU);
//         assert((uintptr_t)block != 0xfbfbfbfbU);
//         assert((uintptr_t)block != 0xdbdbdbdbU);
//         if (block->b_instr != NULL) {
//             assert(block->b_ialloc > 0);
//             assert(block->b_iused > 0);
//             assert(block->b_ialloc >= block->b_iused);
//         }
//         else {
//             assert (block->b_iused == 0);
//             assert (block->b_ialloc == 0);
//         }
//     }
// }

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
    Py_CLEAR(u->ste);
    Py_CLEAR(u->name);
    Py_CLEAR(u->qualname);
    Py_CLEAR(u->consts);
    Py_CLEAR(u->varnames);
    Py_CLEAR(u->cellvars);
    Py_CLEAR(u->freevars);
    Py_CLEAR(u->metadata);
    Py_CLEAR(u->private);
    PyObject_Free(u);
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
    if (u->ste->ste_type == FunctionBlock) {
        add_local_variables(c, u->varnames, u->ste->ste_symbols);
    }
    else {
        add_locals(c, u->varnames);
    }
    u->nlocals = PyDict_GET_SIZE(u->varnames);
    u->max_registers = u->next_register = u->nlocals;
    u->cellvars = dictbytype(u->ste->ste_symbols, CELL, 0, 0);
    if (u->cellvars == NULL) {
        COMPILER_ERROR(c);
    }
    if (u->ste->ste_needs_class_closure) {
        /* Cook up an implicit __class__ cell. */
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
    u->metadata = PyDict_New();
    if (u->metadata == NULL) {
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
    if (u->scope_type != COMPILER_SCOPE_MODULE) {
        compiler_set_qualname(c, u);
    }
    c->unit = u;
    c->nestlevel++;

    //     if (!compiler_set_qualname(c))
    //         return 0;
    // }

    return 1;
}

static void
compiler_exit_scope(struct compiler *c)
{
    struct compiler_unit *unit = c->unit;
    c->unit = unit->prev;
    compiler_unit_free(unit);
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
compiler_set_qualname(struct compiler *c, struct compiler_unit *u)
{
    _Py_static_string(dot, ".");
    _Py_static_string(dot_locals, ".<locals>");
    PyObject *dot_str, *dot_locals_str;

    dot_str = unicode_from_id(c, &dot);
    dot_locals_str = unicode_from_id(c, &dot_locals);

    Py_INCREF(u->name);    
    u->qualname = u->name;

    if (c->unit != NULL) {
        int scope;
        struct compiler_unit *parent = c->unit;
        PyObject *mangled;

        assert(parent);

        if (u->scope_type == COMPILER_SCOPE_FUNCTION
            || u->scope_type == COMPILER_SCOPE_ASYNC_FUNCTION
            || u->scope_type == COMPILER_SCOPE_CLASS) {
            assert(u->name);
            mangled = mangle(c, u->name);
            scope = PyST_GetScope(parent->ste, mangled);
            assert(scope != GLOBAL_IMPLICIT);
            if (scope == GLOBAL_EXPLICIT) {
                return;
            }
        }

        PyObject *base;
        if (parent->scope_type == COMPILER_SCOPE_FUNCTION
            || parent->scope_type == COMPILER_SCOPE_ASYNC_FUNCTION
            || parent->scope_type == COMPILER_SCOPE_LAMBDA) {
            base = PyUnicode_Concat(parent->qualname, dot_locals_str);
            if (base == NULL) {
                COMPILER_ERROR(c);
            }
        }
        else {
             base = parent->qualname;
             Py_INCREF(base);
        }

        PyObject *name = PyUnicode_Concat(base, dot_str);
        Py_DECREF(base);
        if (name == NULL) {
            COMPILER_ERROR(c);
        }

        PyUnicode_Append(&name, u->name);
        if (name == NULL) {
            COMPILER_ERROR(c);
        }
        Py_SETREF(u->qualname, name);
    }
}

static bool
is_local(struct compiler *c, Py_ssize_t reg)
{
    assert(reg >= 0 && reg < c->unit->next_register);
    return reg < c->unit->nlocals;
}

static bool
is_temporary(struct compiler *c, Py_ssize_t reg)
{
    return !is_local(c, reg);
}

// /* Allocate a new block and return a pointer to it.
//    Returns NULL on error.
// */

// static basicblock *
// compiler_new_block(struct compiler *c)
// {
//     basicblock *b;
//     struct compiler_unit *u;

//     u = c->u;
//     b = (basicblock *)PyObject_Malloc(sizeof(basicblock));
//     if (b == NULL) {
//         PyErr_NoMemory();
//         return NULL;
//     }
//     memset((void *)b, 0, sizeof(basicblock));
//     /* Extend the singly linked list of blocks with new block. */
//     b->b_list = u->u_blocks;
//     u->u_blocks = b;
//     return b;
// }

// static basicblock *
// compiler_next_block(struct compiler *c)
// {
//     basicblock *block = compiler_new_block(c);
//     if (block == NULL)
//         return NULL;
//     c->u->u_curblock->b_next = block;
//     c->u->u_curblock = block;
//     return block;
// }

// static basicblock *
// compiler_use_next_block(struct compiler *c, basicblock *block)
// {
//     assert(block != NULL);
//     c->u->u_curblock->b_next = block;
//     c->u->u_curblock = block;
//     return block;
// }

// /* Returns the offset of the next instruction in the current block's
//    b_instr array.  Resizes the b_instr as necessary.
//    Returns -1 on failure.
// */

// static int
// compiler_next_instr(struct compiler *c, basicblock *b)
// {
//     assert(b != NULL);
//     if (b->b_instr == NULL) {
//         b->b_instr = (struct instr *)PyObject_Malloc(
//                          sizeof(struct instr) * DEFAULT_BLOCK_SIZE);
//         if (b->b_instr == NULL) {
//             PyErr_NoMemory();
//             return -1;
//         }
//         b->b_ialloc = DEFAULT_BLOCK_SIZE;
//         memset((char *)b->b_instr, 0,
//                sizeof(struct instr) * DEFAULT_BLOCK_SIZE);
//     }
//     else if (b->b_iused == b->b_ialloc) {
//         struct instr *tmp;
//         size_t oldsize, newsize;
//         oldsize = b->b_ialloc * sizeof(struct instr);
//         newsize = oldsize << 1;

//         if (oldsize > (SIZE_MAX >> 1)) {
//             PyErr_NoMemory();
//             return -1;
//         }

//         if (newsize == 0) {
//             PyErr_NoMemory();
//             return -1;
//         }
//         b->b_ialloc <<= 1;
//         tmp = (struct instr *)PyObject_Realloc(
//                                         (void *)b->b_instr, newsize);
//         if (tmp == NULL) {
//             PyErr_NoMemory();
//             return -1;
//         }
//         b->b_instr = tmp;
//         memset((char *)b->b_instr + oldsize, 0, newsize - oldsize);
//     }
//     return b->b_iused++;
// }

// /* Set the i_lineno member of the instruction at offset off if the
//    line number for the current expression/statement has not
//    already been set.  If it has been set, the call has no effect.

//    The line number is reset in the following cases:
//    - when entering a new scope
//    - on each statement
//    - on each expression that start a new line
//    - before the "except" and "finally" clauses
//    - before the "for" and "while" expressions
// */

// static void
// compiler_set_lineno(struct compiler *c, int off)
// {
//     basicblock *b;
//     if (c->u->u_lineno_set)
//         return;
//     c->u->u_lineno_set = 1;
//     b = c->u->u_curblock;
//     b->b_instr[off].i_lineno = c->u->u_lineno;
// }

// /* Return the stack effect of opcode with argument oparg.

//    Some opcodes have different stack effect when jump to the target and
//    when not jump. The 'jump' parameter specifies the case:

//    * 0 -- when not jump
//    * 1 -- when jump
//    * -1 -- maximal
//  */
// /* XXX Make the stack effect of WITH_CLEANUP_START and
//    WITH_CLEANUP_FINISH deterministic. */
// static int
// stack_effect(int opcode, int oparg, int jump)
// {
//     switch (opcode) {
//         case NOP:
//         case EXTENDED_ARG:
//             return 0;

//         /* Stack manipulation */
//         case POP_TOP:
//             return -1;
//         case ROT_TWO:
//         case ROT_THREE:
//         case ROT_FOUR:
//             return 0;
//         case DUP_TOP:
//             return 1;
//         case DUP_TOP_TWO:
//             return 2;

//         case DEFER_REFCOUNT:
//             return -1;
//         case LOAD_GLOBAL_FOR_CALL:
//         case LOAD_FAST_FOR_CALL:
//             return 0;

//         /* Unary operators */
//         case UNARY_POSITIVE:
//         case UNARY_NEGATIVE:
//         case UNARY_NOT:
//         case UNARY_INVERT:
//             return 0;

//         case SET_ADD:
//         case LIST_APPEND:
//             return -1;
//         case MAP_ADD:
//             return -2;

//         /* Binary operators */
//         case BINARY_POWER:
//         case BINARY_MULTIPLY:
//         case BINARY_MATRIX_MULTIPLY:
//         case BINARY_MODULO:
//         case BINARY_ADD:
//         case BINARY_SUBTRACT:
//         case BINARY_SUBSCR:
//         case BINARY_FLOOR_DIVIDE:
//         case BINARY_TRUE_DIVIDE:
//             return -1;
//         case INPLACE_FLOOR_DIVIDE:
//         case INPLACE_TRUE_DIVIDE:
//             return -1;

//         case INPLACE_ADD:
//         case INPLACE_SUBTRACT:
//         case INPLACE_MULTIPLY:
//         case INPLACE_MATRIX_MULTIPLY:
//         case INPLACE_MODULO:
//             return -1;
//         case STORE_SUBSCR:
//             return -3;
//         case DELETE_SUBSCR:
//             return -2;

//         case BINARY_LSHIFT:
//         case BINARY_RSHIFT:
//         case BINARY_AND:
//         case BINARY_XOR:
//         case BINARY_OR:
//             return -1;
//         case INPLACE_POWER:
//             return -1;
//         case GET_ITER:
//             return 0;

//         case PRINT_EXPR:
//             return -1;
//         case LOAD_BUILD_CLASS:
//             return 1;
//         case INPLACE_LSHIFT:
//         case INPLACE_RSHIFT:
//         case INPLACE_AND:
//         case INPLACE_XOR:
//         case INPLACE_OR:
//             return -1;

//         case SETUP_WITH:
//             /* 1 in the normal flow.
//              * Restore the stack position and push 6 values before jumping to
//              * the handler if an exception be raised. */
//             return jump ? 6 : 1;
//         case RETURN_VALUE:
//             return -1;
//         case IMPORT_STAR:
//             return -1;
//         case SETUP_ANNOTATIONS:
//             return 0;
//         case YIELD_VALUE:
//             return 0;
//         case YIELD_FROM:
//             return -1;
//         case POP_BLOCK:
//             return 0;
//         case POP_EXCEPT:
//             return -3;

//         case STORE_NAME:
//             return -1;
//         case DELETE_NAME:
//             return 0;
//         case UNPACK_SEQUENCE:
//             return oparg-1;
//         case UNPACK_EX:
//             return (oparg&0xFF) + (oparg>>8);
//         case FOR_ITER:
//             /* -1 at end of iterator, 1 if continue iterating. */
//             return jump > 0 ? -1 : 1;

//         case STORE_ATTR:
//             return -2;
//         case DELETE_ATTR:
//             return -1;
//         case STORE_GLOBAL:
//             return -1;
//         case DELETE_GLOBAL:
//             return 0;
//         case LOAD_CONST:
//             return 1;
//         case LOAD_NAME:
//             return 1;
//         case BUILD_TUPLE:
//         case BUILD_LIST:
//         case BUILD_SET:
//         case BUILD_STRING:
//             return 1-oparg;
//         case BUILD_MAP:
//             return 1 - 2*oparg;
//         case BUILD_CONST_KEY_MAP:
//             return -oparg;
//         case LOAD_ATTR:
//             return 0;
//         case COMPARE_OP:
//         case IS_OP:
//         case CONTAINS_OP:
//             return -1;
//         case JUMP_IF_NOT_EXC_MATCH:
//             return -2;
//         case IMPORT_NAME:
//             return -1;
//         case IMPORT_FROM:
//             return 1;

//         /* Jumps */
//         case JUMP_FORWARD:
//         case JUMP_ABSOLUTE:
//             return 0;

//         case JUMP_IF_TRUE_OR_POP:
//         case JUMP_IF_FALSE_OR_POP:
//             return jump ? 0 : -1;

//         case POP_JUMP_IF_FALSE:
//         case POP_JUMP_IF_TRUE:
//             return -1;

//         case LOAD_GLOBAL:
//             return 1;

//         /* Exception handling */
//         case SETUP_FINALLY:
//             /* 0 in the normal flow.
//              * Restore the stack position and push 6 values before jumping to
//              * the handler if an exception be raised. */
//             return jump ? 6 : 0;
//         case RERAISE:
//             return -3;

//         case WITH_EXCEPT_START:
//             return 1;

//         case LOAD_FAST:
//             return 1;
//         case STORE_FAST:
//             return -1;
//         case DELETE_FAST:
//             return 0;

//         case RAISE_VARARGS:
//             return -oparg;

//         /* Functions and calls */
//         case CALL_FUNCTION:
//             return -oparg + 1;
//         case CALL_METHOD:
//             return -oparg;
//         case CALL_FUNCTION_KW:
//             return -oparg;
//         case CALL_FUNCTION_EX:
//             return -((oparg & 0x01) != 0);
//         case MAKE_FUNCTION:
//             return -1 - ((oparg & 0x01) != 0) - ((oparg & 0x02) != 0) -
//                 ((oparg & 0x04) != 0) - ((oparg & 0x08) != 0);
//         case BUILD_SLICE:
//             if (oparg == 3)
//                 return -2;
//             else
//                 return -1;

//         /* Closures */
//         case LOAD_CLOSURE:
//             return 1;
//         case LOAD_DEREF:
//         case LOAD_CLASSDEREF:
//             return 1;
//         case STORE_DEREF:
//             return -1;
//         case DELETE_DEREF:
//             return 0;

//         /* Iterators and generators */
//         case GET_AWAITABLE:
//             return 0;
//         case SETUP_ASYNC_WITH:
//             /* 0 in the normal flow.
//              * Restore the stack position to the position before the result
//              * of __aenter__ and push 6 values before jumping to the handler
//              * if an exception be raised. */
//             return jump ? -1 + 6 : 0;
//         case BEFORE_ASYNC_WITH:
//             return 1;
//         case GET_AITER:
//             return 0;
//         case GET_ANEXT:
//             return 1;
//         case GET_YIELD_FROM_ITER:
//             return 0;
//         case END_ASYNC_FOR:
//             return -7;
//         case FORMAT_VALUE:
//             /* If there's a fmt_spec on the stack, we go from 2->1,
//                else 1->1. */
//             return (oparg & FVS_MASK) == FVS_HAVE_SPEC ? -1 : 0;
//         case LOAD_METHOD:
//             return 0;
//         case LOAD_ASSERTION_ERROR:
//             return 1;
//         case LIST_TO_TUPLE:
//             return 0;
//         case LIST_EXTEND:
//         case SET_UPDATE:
//         case DICT_MERGE:
//         case DICT_UPDATE:
//             return -1;
//         default:
//             return PY_INVALID_STACK_EFFECT;
//     }
//     return PY_INVALID_STACK_EFFECT; /* not reachable */
// }

// int
// PyCompile_OpcodeStackEffectWithJump(int opcode, int oparg, int jump)
// {
//     return stack_effect(opcode, oparg, jump);
// }

// int
// PyCompile_OpcodeStackEffect(int opcode, int oparg)
// {
//     return stack_effect(opcode, oparg, -1);
// }

// int
// PyCompile_CallableStackSize(PyObject *bytecode)
// {
//     assert(PyBytes_Check(bytecode));
//     Py_ssize_t size = Py_SIZE(bytecode) / sizeof(_Py_CODEUNIT);
//     _Py_CODEUNIT *code = (_Py_CODEUNIT *)PyBytes_AS_STRING(bytecode);

//     static int8_t fn_stack_effect[256];

//     // initialize table of callable stack effects
//     static _PyOnceFlag once;
//     if (_PyBeginOnce(&once)) {
//         fn_stack_effect[DEFER_REFCOUNT] = 1;
//         fn_stack_effect[LOAD_FAST_FOR_CALL] = 1;
//         fn_stack_effect[LOAD_GLOBAL_FOR_CALL] = 1;
//         fn_stack_effect[LOAD_METHOD] = 1;
//         fn_stack_effect[CALL_METHOD] = -1;
//         fn_stack_effect[CALL_FUNCTION] = -1;
//         fn_stack_effect[CALL_FUNCTION_KW] = -1;
//         fn_stack_effect[CALL_FUNCTION_EX] = -1;
//         _PyEndOnce(&once);
//     }

//     // The callable stack effect is easier to compute than the normal stack
//     // effect. The relevant instructions are only generated from function
//     // call experssions. We don't even need to take into account jumps because
//     // expressions can't contain statements in Python.
//     int curdepth = 0;
//     int maxdepth = 0;
//     for (Py_ssize_t i = 0; i < size; i++) {
//         int opcode = _Py_OPCODE(code[i]);
//         curdepth += fn_stack_effect[opcode];
//         if (curdepth > maxdepth) {
//             maxdepth = curdepth;
//         }
//     }

//     return maxdepth;
// }

// struct BlockEffect {
//     signed block_stack : 2;
//     unsigned jabs : 1;
//     unsigned jrel : 1;
//     unsigned uncond_jmp : 1;
// };

// int
// PyCompile_BlockDepth(PyObject *bytecode)
// {
//     assert(PyBytes_Check(bytecode));
//     Py_ssize_t size = Py_SIZE(bytecode) / sizeof(_Py_CODEUNIT);
//     _Py_CODEUNIT *code = (_Py_CODEUNIT *)PyBytes_AS_STRING(bytecode);

//     static struct BlockEffect block_effect[256];

//     // initialize table of callable stack effects
//     static _PyOnceFlag once;
//     if (_PyBeginOnce(&once)) {
//         // These opcodes push onto the block stack
//         block_effect[SETUP_FINALLY].block_stack = 1;
//         block_effect[SETUP_ASYNC_WITH].block_stack = 1;
//         block_effect[SETUP_WITH].block_stack = 1;

//         // These opcodes pop from the block stack
//         block_effect[POP_EXCEPT].block_stack = -1;
//         block_effect[POP_BLOCK].block_stack = -1;
//         block_effect[END_ASYNC_FOR].block_stack = -1;

//         // These opcodes may jump to an absolute address
//         block_effect[POP_JUMP_IF_FALSE].jabs = 1;
//         block_effect[POP_JUMP_IF_TRUE].jabs = 1;
//         block_effect[JUMP_IF_FALSE_OR_POP].jabs = 1;
//         block_effect[JUMP_IF_TRUE_OR_POP].jabs = 1;
//         block_effect[JUMP_ABSOLUTE].jabs = 1;
//         block_effect[JUMP_IF_NOT_EXC_MATCH].jabs = 1;

//         // These opcodes may jump to a relative address
//         block_effect[JUMP_FORWARD].jrel = 1;
//         block_effect[FOR_ITER].jrel = 1;
//         block_effect[SETUP_FINALLY].jrel = 1;
//         block_effect[SETUP_ASYNC_WITH].jrel = 1;
//         block_effect[SETUP_WITH].jrel = 1;

//         // These opcodes unconditionally jump or return; i.e.
//         // the subsequent opcode is not directly reached from here.
//         block_effect[JUMP_ABSOLUTE].uncond_jmp = 1;
//         block_effect[JUMP_FORWARD].uncond_jmp = 1;
//         block_effect[RETURN_VALUE].uncond_jmp = 1;
//         block_effect[RAISE_VARARGS].uncond_jmp = 1;
//         block_effect[RERAISE].uncond_jmp = 1;

//         _PyEndOnce(&once);
//     }

//     int *entry_depth = (int *)PyMem_RawMalloc(size * sizeof(int));
//     if (!entry_depth) {
//         PyErr_NoMemory();
//         return -1;
//     }
//     memset(entry_depth, 0, size * sizeof(int));

//     int curdepth = 0;
//     int maxdepth = 0;
//     for (Py_ssize_t i = 0; i < size; i++) {
//         int opcode = _Py_OPCODE(code[i]);
//         int oparg = _Py_OPARG(code[i]);
//         while (opcode == EXTENDED_ARG) {
//             i++;
//             opcode = _Py_OPCODE(code[i]);
//             oparg = (oparg << 8) | _Py_OPARG(code[i]);
//         }

//         if (entry_depth[i] > curdepth) {
//             curdepth = entry_depth[i];
//         }

//         struct BlockEffect effect = block_effect[opcode];
//         curdepth += effect.block_stack;

//         // assert(curdepth >= 0);
//         if (curdepth > maxdepth) {
//             maxdepth = curdepth;
//         }

//         if (effect.jabs | effect.jrel) {
//             int target = oparg / sizeof(_Py_CODEUNIT);
//             if (effect.jrel) {
//                 // relative jumps are relative to next instruction
//                 target += i + 1;
//             }
//             if (curdepth > entry_depth[target]) {
//                 entry_depth[target] = curdepth;
//             }
//         }
//         if (effect.uncond_jmp) {
//             curdepth = 0;
//         }
//     }

//     PyMem_RawFree(entry_depth);
//     return maxdepth;
// }

static void
resize_array(struct compiler *c, void **arr, uint32_t *size, Py_ssize_t min_size, Py_ssize_t unit)
{
    Py_ssize_t old_size = *size;
    if (old_size > INT32_MAX / 2) {
        PyErr_NoMemory();
        COMPILER_ERROR(c);
    }

    Py_ssize_t new_size = old_size * 2;
    if (new_size < min_size) {
        new_size = min_size;
    }

    void *ptr = mi_recalloc(*arr, new_size, unit);
    if (ptr == NULL) {
        PyErr_NoMemory();
        COMPILER_ERROR(c);
    }
    *arr = ptr;
    *size = new_size;
}

/* Add an opcode with no argument.
   Returns 0 on failure, 1 on success.
*/

static uint8_t *
next_instr(struct compiler *c, int size)
{
    struct instr_array *instr = &c->unit->instr;
    if (instr->offset + size >= instr->allocated) {
        resize_array(
            c,
            (void**)&instr->arr, 
            &instr->allocated,
            DEFAULT_INSTR_SIZE,
            sizeof(*instr->arr));
    }
    uint8_t *ptr = &instr->arr[instr->offset];
    instr->offset += size;
    return ptr;
}

static void
write_uint32(uint8_t *pc, int imm)
{
    uint32_t value = (uint32_t)imm;
    memcpy(pc, &value, sizeof(uint32_t));
}

static void
write_uint16(uint8_t *pc, int imm)
{
    uint16_t value = (uint16_t)imm;
    memcpy(pc, &value, sizeof(uint16_t));
}

static void
write_int16(uint8_t *pc, int imm)
{
    int16_t value = (int16_t)imm;
    memcpy(pc, &value, sizeof(int16_t));
}

static void
emit0(struct compiler *c, int opcode)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    uint8_t *pc = next_instr(c, 1);
    pc[0] = opcode;
}

static void
emit1(struct compiler *c, int opcode, int imm0)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
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

static void
emit2(struct compiler *c, int opcode, int imm0, int imm1)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    int wide = (imm0 > 255 || imm1 > 255);
    if (wide) {
        uint8_t *pc = next_instr(c, 10);
        pc[0] = WIDE;
        pc[1] = opcode;
        write_uint32(&pc[2], imm0);
        write_uint32(&pc[6], imm1);
    }
    else {
        uint8_t *pc = next_instr(c, 3);
        pc[0] = opcode;
        pc[1] = (uint8_t)imm0;
        pc[2] = (uint8_t)imm1;
    }
}

static void
emit3(struct compiler *c, int opcode, int imm0, int imm1, int imm2)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    int wide = (imm0 > 255 || imm1 > 255 || imm2 > 255);
    if (wide) {
        uint8_t *pc = next_instr(c, 14);
        pc[0] = WIDE;
        pc[1] = opcode;
        write_uint32(&pc[2], imm0);
        write_uint32(&pc[6], imm1);
        write_uint32(&pc[10], imm2);
    }
    else {
        uint8_t *pc = next_instr(c, 4);
        pc[0] = opcode;
        pc[1] = (uint8_t)imm0;
        pc[2] = (uint8_t)imm1;
        pc[3] = (uint8_t)imm2;
    }
}

static void
emit_call(struct compiler *c, int opcode, int base, int flags)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    int wide = (base > 255);
    if (wide) {
        uint8_t *pc = next_instr(c, 8);
        pc[0] = WIDE;
        pc[1] = opcode;
        write_uint32(&pc[2], base);
        write_uint16(&pc[6], flags);
    }
    else {
        uint8_t *pc = next_instr(c, 4);
        pc[0] = opcode;
        pc[1] = base;
        write_uint16(&pc[2], flags);
    }
}

static void
emit_jump(struct compiler *c, int opcode, struct bc_label *label)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    uint8_t *pc = next_instr(c, 3);
    pc[0] = opcode;
    write_uint16(&pc[1], 0);
    label->bound = 0;
    label->offset = pc - c->unit->instr.arr;
}

static void
emit_bwd_jump(struct compiler *c, int opcode, uint32_t target)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    Py_ssize_t offset = (Py_ssize_t)target - (Py_ssize_t)c->unit->instr.offset;
    assert(offset < 0 && offset >= INT32_MIN);
    if (offset > INT16_MIN) {
        uint8_t *pc = next_instr(c, 3);
        pc[0] = opcode;
        write_uint16(&pc[1], (uint16_t)offset);
    }
    else {
        uint8_t *pc = next_instr(c, 6);
        pc[0] = WIDE;
        pc[1] = opcode;
        write_uint32(&pc[2], (uint32_t)offset);
    }
}

static void
emit_for(struct compiler *c, Py_ssize_t reg, uint32_t target)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    Py_ssize_t offset = (Py_ssize_t)target - (Py_ssize_t)c->unit->instr.offset;
    assert(offset < 0 && offset >= INT32_MIN);
    if (offset > INT16_MIN && reg < 256) {
        uint8_t *pc = next_instr(c, 4);
        pc[0] = FOR_ITER;
        pc[1] = reg;
        write_uint16(&pc[2], (uint16_t)offset);
    }
    else {
        uint8_t *pc = next_instr(c, 10);
        pc[0] = WIDE;
        pc[1] = FOR_ITER;
        write_uint32(&pc[2], reg);
        write_uint32(&pc[6], (uint32_t)offset);
    }
}

static void
emit_label(struct compiler *c, struct bc_label *label)
{
    if (c->do_not_emit_bytecode) {
        return;
    }
    assert(!label->bound);
    uint32_t pos = c->unit->instr.offset;
    Py_ssize_t delta = (Py_ssize_t)pos - (Py_ssize_t)label->offset;
    if (delta > INT16_MAX) {
        PyErr_Format(PyExc_RuntimeError, "jump too big: %d", (int)delta);
        COMPILER_ERROR(c);
    }
    if (delta <= 0) {
        // forward jumps should go forward
        PyErr_Format(PyExc_RuntimeError, "negative jmp: %d", (int)delta);
        COMPILER_ERROR(c);
    }
    assert(delta >= 0);
    uint8_t *jmp =  &c->unit->instr.arr[label->offset];
    write_int16(&jmp[1], delta);
    label->bound = 1;
    c->unit->reachable = true;
}

static void
emit_compare(struct compiler *c, Py_ssize_t reg, cmpop_ty cmp)
{
    switch (cmp) {
    case Eq:    emit2(c, COMPARE_OP, Py_EQ, reg); break;
    case NotEq: emit2(c, COMPARE_OP, Py_NE, reg); break;
    case Lt:    emit2(c, COMPARE_OP, Py_LT, reg); break;
    case LtE:   emit2(c, COMPARE_OP, Py_LE, reg); break;
    case Gt:    emit2(c, COMPARE_OP, Py_GT, reg); break;
    case GtE:   emit2(c, COMPARE_OP, Py_GE, reg); break;

    case Is:    emit1(c, IS_OP, reg); break;
    case IsNot: emit1(c, IS_OP, reg); 
                emit0(c, UNARY_NOT_FAST); break;

    case In:    emit1(c, CONTAINS_OP, reg); break;
    case NotIn: emit1(c, CONTAINS_OP, reg);
                emit0(c, UNARY_NOT_FAST); break;
    }
}

static struct bc_label *
multi_label_next(struct compiler *c, struct multi_label *labels)
{
    if (labels->n == labels->allocated) {
        Py_ssize_t newsize = (labels->allocated * 2) + 1;
        struct bc_label *arr;
        arr = mi_heap_recalloc(c->heap, labels->arr, newsize, sizeof(*labels->arr));
        if (arr == NULL) {
            COMPILER_ERROR(c);
        }
        labels->arr = arr;
        labels->allocated = newsize;
    }
    return &labels->arr[labels->n++];
}

static void
emit_multi_label(struct compiler *c, struct multi_label *labels)
{
    for (Py_ssize_t i = 0, n = labels->n; i != n; i++) {
        emit_label(c, &labels->arr[i]);
    }
    mi_free(labels->arr);
    memset(labels, 0, sizeof(*labels));
}

static int
reserve_regs(struct compiler *c, int n)
{
    int r = c->unit->next_register;
    c->unit->next_register += n;
    if (c->unit->next_register > c->unit->max_registers) {
        c->unit->max_registers = c->unit->next_register;
    }
    return r;
}

static void
free_reg(struct compiler *c, Py_ssize_t reg)
{
    if (is_temporary(c, reg)) {
        c->unit->next_register -= 1;
        assert(c->unit->next_register == reg);
    }
}

static void
free_regs_above(struct compiler *c, Py_ssize_t base)
{
    if (base < c->unit->next_register) {
        c->unit->next_register = base;
    }
}

static void
clear_reg(struct compiler *c, Py_ssize_t reg)
{
    if (is_temporary(c, reg)) {
        emit1(c, CLEAR_FAST, reg);
        free_reg(c, reg);
    }
}

static void
expr_to_reg(struct compiler *c, expr_ty e, Py_ssize_t reg)
{
    if (e == NULL) {
        emit1(c, LOAD_CONST, const_none(c));
    }
    else {
        compiler_visit_expr(c, e);
    }
    emit1(c, STORE_FAST, reg);
    if (reg >= c->unit->next_register) {
        reserve_regs(c, reg - c->unit->next_register + 1);
    }
}

static Py_ssize_t
expr_discharge(struct compiler *c, expr_ty e)
{
    if (e->kind == Name_kind) {
        PyObject *mangled = mangle(c, e->v.Name.id);
        int access = compiler_access(c, mangled);
        if (access == ACCESS_FAST) {
            return compiler_varname(c, mangled);
        }
    }

    compiler_visit_expr(c, e);
    return REG_ACCUMULATOR;
}

static Py_ssize_t
expr_to_any_reg(struct compiler *c, expr_ty e)
{
    Py_ssize_t reg = expr_discharge(c, e);
    if (reg == REG_ACCUMULATOR) {
        reg = reserve_regs(c, 1);
        emit1(c, STORE_FAST, reg);
    }
    return reg;
}

static void
to_accumulator(struct compiler *c, Py_ssize_t reg)
{
    if (reg != REG_ACCUMULATOR) {
        assert(reg >= 0 && reg < c->unit->max_registers);
        emit1(c, LOAD_FAST, reg);
    }
}

// static int
// compiler_addop(struct compiler *c, int opcode)
// {
//     basicblock *b;
//     struct instr *i;
//     int off;
//     assert(!HAS_ARG(opcode));
//     if (c->c_do_not_emit_bytecode) {
//         return 1;
//     }
//     off = compiler_next_instr(c, c->u->u_curblock);
//     if (off < 0)
//         return 0;
//     b = c->u->u_curblock;
//     i = &b->b_instr[off];
//     i->i_opcode = opcode;
//     i->i_oparg = 0;
//     if (opcode == RETURN_VALUE)
//         b->b_return = 1;
//     compiler_set_lineno(c, off);
//     return 1;
// }

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

static int32_t
compiler_varname(struct compiler *c, PyObject *mangled_name)
{
    PyObject *v = PyDict_GetItemWithError(c->unit->varnames, mangled_name);
    if (v == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_Format(PyExc_RuntimeError, "missing name %U", mangled_name);
        }
        COMPILER_ERROR(c);
    }
    return PyLong_AsLong(v);
}

static int32_t
compiler_metaslot(struct compiler *c, PyObject *name)
{
    return compiler_add_o(c, c->unit->metadata, name);
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
        // o is registered in c_const_cache.  Just use it.
        Py_XINCREF(t);
        Py_DECREF(key);
        return t;
    }

    // We registered o in c_const_cache.
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
        Py_DECREF(o);
        return 0;
    }

    PyObject *key = merge_consts_recursive(c, o);
    Py_DECREF(o);
    if (key == NULL) {
        COMPILER_ERROR(c);
    }

    Py_ssize_t arg = compiler_add_o(c, c->unit->consts, key);
    Py_DECREF(key);
    if (arg < 0) {
        COMPILER_ERROR(c);
    }
    return arg;
}

static int32_t
compiler_const(struct compiler *c, PyObject *value)
{
    Py_INCREF(value);
    return compiler_add_const(c, value);
}

static int32_t
compiler_new_const(struct compiler *c, PyObject *value)
{
    return compiler_add_const(c, value);
}

static int32_t
const_none(struct compiler *c)
{
    return compiler_const(c, Py_None);
}

// static int
// compiler_addop_load_const(struct compiler *c, PyObject *o)
// {
//     if (c->c_do_not_emit_bytecode) {
//         return 1;
//     }

//     Py_ssize_t arg = compiler_add_const(c, o);
//     if (arg < 0)
//         return 0;
//     return compiler_addop_i(c, LOAD_CONST, arg);
// }

// static int
// compiler_addop_o(struct compiler *c, int opcode, PyObject *dict,
//                      PyObject *o)
// {
//     if (c->c_do_not_emit_bytecode) {
//         return 1;
//     }

//     Py_ssize_t arg = compiler_add_o(c, dict, o);
//     if (arg < 0)
//         return 0;
//     return compiler_addop_i(c, opcode, arg);
// }

// static int
// compiler_addop_name(struct compiler *c, int opcode, PyObject *dict,
//                     PyObject *o)
// {
//     Py_ssize_t arg;

//     if (c->c_do_not_emit_bytecode) {
//         return 1;
//     }

//     PyObject *mangled = _Py_Mangle(c->u->u_private, o);
//     if (!mangled)
//         return 0;
//     arg = compiler_add_o(c, dict, mangled);
//     Py_DECREF(mangled);
//     if (arg < 0)
//         return 0;
//     return compiler_addop_i(c, opcode, arg);
// }

// /* Add an opcode with an integer argument.
//    Returns 0 on failure, 1 on success.
// */

// static int
// compiler_addop_i(struct compiler *c, int opcode, Py_ssize_t oparg)
// {
//     struct instr *i;
//     int off;

//     if (c->c_do_not_emit_bytecode) {
//         return 1;
//     }

//     /* oparg value is unsigned, but a signed C int is usually used to store
//        it in the C code (like Python/ceval.c).

//        Limit to 32-bit signed C int (rather than INT_MAX) for portability.

//        The argument of a concrete bytecode instruction is limited to 8-bit.
//        EXTENDED_ARG is used for 16, 24, and 32-bit arguments. */
//     assert(HAS_ARG(opcode));
//     assert(0 <= oparg && oparg <= 2147483647);

//     off = compiler_next_instr(c, c->u->u_curblock);
//     if (off < 0)
//         return 0;
//     i = &c->u->u_curblock->b_instr[off];
//     i->i_opcode = opcode;
//     i->i_oparg = Py_SAFE_DOWNCAST(oparg, Py_ssize_t, int);
//     compiler_set_lineno(c, off);
//     return 1;
// }

// static int
// compiler_addop_j(struct compiler *c, int opcode, basicblock *b, int absolute)
// {
//     struct instr *i;
//     int off;

//     if (c->c_do_not_emit_bytecode) {
//         return 1;
//     }

//     assert(HAS_ARG(opcode));
//     assert(b != NULL);
//     off = compiler_next_instr(c, c->u->u_curblock);
//     if (off < 0)
//         return 0;
//     i = &c->u->u_curblock->b_instr[off];
//     i->i_opcode = opcode;
//     i->i_target = b;
//     if (absolute)
//         i->i_jabs = 1;
//     else
//         i->i_jrel = 1;
//     compiler_set_lineno(c, off);
//     return 1;
// }

// MACROS were here

/* These macros allows to check only for errors and not emmit bytecode
 * while visiting nodes.
*/

#define BEGIN_DO_NOT_EMIT_BYTECODE { \
    c->do_not_emit_bytecode++;

#define END_DO_NOT_EMIT_BYTECODE \
    c->do_not_emit_bytecode--; \
}

// /* Search if variable annotations are present statically in a block. */

// static int
// find_ann(asdl_seq *stmts)
// {
//     int i, j, res = 0;
//     stmt_ty st;

//     for (i = 0; i < asdl_seq_LEN(stmts); i++) {
//         st = (stmt_ty)asdl_seq_GET(stmts, i);
//         switch (st->kind) {
//         case AnnAssign_kind:
//             return 1;
//         case For_kind:
//             res = find_ann(st->v.For.body) ||
//                   find_ann(st->v.For.orelse);
//             break;
//         case AsyncFor_kind:
//             res = find_ann(st->v.AsyncFor.body) ||
//                   find_ann(st->v.AsyncFor.orelse);
//             break;
//         case While_kind:
//             res = find_ann(st->v.While.body) ||
//                   find_ann(st->v.While.orelse);
//             break;
//         case If_kind:
//             res = find_ann(st->v.If.body) ||
//                   find_ann(st->v.If.orelse);
//             break;
//         case With_kind:
//             res = find_ann(st->v.With.body);
//             break;
//         case AsyncWith_kind:
//             res = find_ann(st->v.AsyncWith.body);
//             break;
//         case Try_kind:
//             for (j = 0; j < asdl_seq_LEN(st->v.Try.handlers); j++) {
//                 excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
//                     st->v.Try.handlers, j);
//                 if (find_ann(handler->v.ExceptHandler.body)) {
//                     return 1;
//                 }
//             }
//             res = find_ann(st->v.Try.body) ||
//                   find_ann(st->v.Try.finalbody) ||
//                   find_ann(st->v.Try.orelse);
//             break;
//         default:
//             res = 0;
//         }
//         if (res) {
//             break;
//         }
//     }
//     return res;
// }

// /*
//  * Frame block handling functions
//  */

// static int
// compiler_push_fblock(struct compiler *c, enum fblocktype t, basicblock *b,
//                      basicblock *exit, void *datum)
// {
//     struct fblockinfo *f;
//     if (c->u->u_nfblocks >= CO_MAXBLOCKS) {
//         PyErr_SetString(PyExc_SyntaxError,
//                         "too many statically nested blocks");
//         return 0;
//     }
//     f = &c->u->u_fblock[c->u->u_nfblocks++];
//     f->fb_type = t;
//     f->fb_block = b;
//     f->fb_exit = exit;
//     f->fb_datum = datum;
//     return 1;
// }

// static void
// compiler_pop_fblock(struct compiler *c, enum fblocktype t, basicblock *b)
// {
//     struct compiler_unit *u = c->u;
//     assert(u->u_nfblocks > 0);
//     u->u_nfblocks--;
//     assert(u->u_fblock[u->u_nfblocks].fb_type == t);
//     assert(u->u_fblock[u->u_nfblocks].fb_block == b);
// }

// static int
// compiler_call_exit_with_nones(struct compiler *c) {
//     ADDOP(c, DEFER_REFCOUNT);
//     ADDOP_O(c, LOAD_CONST, Py_None, consts);
//     ADDOP(c, DUP_TOP);
//     ADDOP(c, DUP_TOP);
//     ADDOP_I(c, CALL_FUNCTION, 3);
//     return 1;
// }

/* Unwind a frame block.  If preserve_tos is true, the TOS before
 * popping the blocks will be restored afterwards, unless another
 * return, break or continue is found. In which case, the TOS will
 * be popped.
 * FIXME: docs
 */
static void
compiler_unwind_block(struct compiler *c, struct fblock *block)
{
    switch (block->type) {
    case WHILE_LOOP:
        return;

    case FOR_LOOP:
        emit1(c, CLEAR_FAST, block->ForLoop.reg);
        return;

    case TRY_FINALLY:
        assert(false && "NYI: TryFinally");
        break;

    case EXCEPT:
        emit1(c, END_EXCEPT, block->Except.reg);
        return;

    case WITH:
        emit1(c, END_WITH, block->With.reg);
        return;

    case ASYNC_WITH:
        emit1(c, END_ASYNC_WITH, block->AsyncWith.reg);
        return;
    }
    Py_UNREACHABLE();
}

// /** Unwind block stack. If loop is not NULL, then stop when the first loop is encountered. */
// static int
// compiler_unwind_fblock_stack(struct compiler *c, int preserve_tos, struct fblockinfo **loop) {
//     if (c->u->u_nfblocks == 0) {
//         return 1;
//     }
//     struct fblockinfo *top = &c->u->u_fblock[c->u->u_nfblocks-1];
//     if (loop != NULL && (top->fb_type == WHILE_LOOP || top->fb_type == FOR_LOOP)) {
//         *loop = top;
//         return 1;
//     }
//     struct fblockinfo copy = *top;
//     c->u->u_nfblocks--;
//     if (!compiler_unwind_fblock(c, &copy, preserve_tos)) {
//         return 0;
//     }
//     if (!compiler_unwind_fblock_stack(c, preserve_tos, loop)) {
//         return 0;
//     }
//     c->u->u_fblock[c->u->u_nfblocks] = copy;
//     c->u->u_nfblocks++;
//     return 1;
// }

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
    // int i = 0;
    // stmt_ty st;
    // PyObject *docstring;

    // /* Set current line number to the line number of first statement.
    //    This way line number for SETUP_ANNOTATIONS will always
    //    coincide with the line number of first "real" statement in module.
    //    If body is empty, then lineno will be set later in assemble. */
    // if (c->u->u_scope_type == COMPILER_SCOPE_MODULE &&
    //     !c->u->u_lineno && asdl_seq_LEN(stmts)) {
    //     st = (stmt_ty)asdl_seq_GET(stmts, 0);
    //     c->u->u_lineno = st->lineno;
    // }
    // /* Every annotated class and module should have __annotations__. */
    // if (find_ann(stmts)) {
    //     ADDOP(c, SETUP_ANNOTATIONS);
    // }
    // if (!asdl_seq_LEN(stmts))
    //     return 1;
    // /* if not -OO mode, set docstring */
    // if (c->c_optimize < 2) {
    //     docstring = _PyAST_GetDocString(stmts);
    //     if (docstring) {
    //         i = 1;
    //         st = (stmt_ty)asdl_seq_GET(stmts, 0);
    //         assert(st->kind == Expr_kind);
    //         VISIT(c, expr, st->v.Expr.value);
    //         if (!compiler_nameop(c, __doc__, Store))
    //             return 0;
    //     }
    // }
    // for (; i < asdl_seq_LEN(stmts); i++)
    //     VISIT(c, stmt, (stmt_ty)asdl_seq_GET(stmts, i));
    // return 1;
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

    assemble(c, /*addNOne???*/0);
    compiler_exit_scope(c);
    PyCodeObject2 *co = c->code;
    c->code = NULL;
    return co;
}

// /* The test for LOCAL must come before the test for FREE in order to
//    handle classes where name is both local and free.  The local var is
//    a method and the free var is a free var referenced within a method.
// */

// static int
// get_ref_type(struct compiler *c, PyObject *name)
// {
//     int scope;
//     if (c->u->u_scope_type == COMPILER_SCOPE_CLASS &&
//         _PyUnicode_EqualToASCIIString(name, "__class__"))
//         return CELL;
//     scope = PyST_GetScope(c->u->u_ste, name);
//     if (scope == 0) {
//         char buf[350];
//         PyOS_snprintf(buf, sizeof(buf),
//                       "unknown scope for %.100s in %.100s(%s)\n"
//                       "symbols: %s\nlocals: %s\nglobals: %s",
//                       PyUnicode_AsUTF8(name),
//                       PyUnicode_AsUTF8(c->u->u_name),
//                       PyUnicode_AsUTF8(PyObject_Repr(c->u->u_ste->ste_id)),
//                       PyUnicode_AsUTF8(PyObject_Repr(c->u->u_ste->ste_symbols)),
//                       PyUnicode_AsUTF8(PyObject_Repr(c->u->u_varnames)),
//                       PyUnicode_AsUTF8(PyObject_Repr(c->u->u_names))
//         );
//         Py_FatalError(buf);
//     }

//     return scope;
// }

// static int
// compiler_lookup_arg(PyObject *dict, PyObject *name)
// {
//     PyObject *v;
//     v = PyDict_GetItem(dict, name);
//     if (v == NULL)
//         return -1;
//     return PyLong_AS_LONG(v);
// }

// static int
// compiler_make_closure(struct compiler *c, PyCodeObject *co, Py_ssize_t flags, PyObject *qualname)
// {
//     Py_ssize_t i, free = PyCode_GetNumFree(co);
//     if (qualname == NULL)
//         qualname = co->co_name;

//     if (free) {
//         for (i = 0; i < free; ++i) {
//             /* Bypass com_addop_varname because it will generate
//                LOAD_DEREF but LOAD_CLOSURE is needed.
//             */
//             PyObject *name = PyTuple_GET_ITEM(co->co_freevars, i);
//             int arg, reftype;

//             /* Special case: If a class contains a method with a
//                free variable that has the same name as a method,
//                the name will be considered free *and* local in the
//                class.  It should be handled by the closure, as
//                well as by the normal name lookup logic.
//             */
//             reftype = get_ref_type(c, name);
//             if (reftype == CELL)
//                 arg = compiler_lookup_arg(c->u->u_cellvars, name);
//             else /* (reftype == FREE) */
//                 arg = compiler_lookup_arg(c->u->u_freevars, name);
//             if (arg == -1) {
//                 fprintf(stderr,
//                     "lookup %s in %s %d %d\n"
//                     "freevars of %s: %s\n",
//                     PyUnicode_AsUTF8(PyObject_Repr(name)),
//                     PyUnicode_AsUTF8(c->u->u_name),
//                     reftype, arg,
//                     PyUnicode_AsUTF8(co->co_name),
//                     PyUnicode_AsUTF8(PyObject_Repr(co->co_freevars)));
//                 Py_FatalError("compiler_make_closure()");
//             }
//             ADDOP_I(c, LOAD_CLOSURE, arg);
//         }
//         flags |= 0x08;
//         ADDOP_I(c, BUILD_TUPLE, free);
//     }
//     ADDOP_LOAD_CONST(c, (PyObject*)co);
//     ADDOP_LOAD_CONST(c, qualname);
//     ADDOP_I(c, MAKE_FUNCTION, flags);
//     return 1;
// }


static Py_ssize_t
compiler_decorators(struct compiler *c, asdl_seq* decos)
{
    Py_ssize_t base = -1;
    for (Py_ssize_t i = 0; i < asdl_seq_LEN(decos); i++) {
        base = c->unit->next_register + FRAME_EXTRA;
        expr_to_reg(c, asdl_seq_GET(decos, i), base - 1);
    }
    return base;
}

// static int
// compiler_visit_kwonlydefaults(struct compiler *c, asdl_seq *kwonlyargs,
//                               asdl_seq *kw_defaults)
// {
//     /* Push a dict of keyword-only default values.

//        Return 0 on error, -1 if no dict pushed, 1 if a dict is pushed.
//        */
//     int i;
//     PyObject *keys = NULL;

//     for (i = 0; i < asdl_seq_LEN(kwonlyargs); i++) {
//         arg_ty arg = asdl_seq_GET(kwonlyargs, i);
//         expr_ty default_ = asdl_seq_GET(kw_defaults, i);
//         if (default_) {
//             PyObject *mangled = _Py_Mangle(c->u->u_private, arg->arg);
//             if (!mangled) {
//                 goto error;
//             }
//             if (keys == NULL) {
//                 keys = PyList_New(1);
//                 if (keys == NULL) {
//                     Py_DECREF(mangled);
//                     return 0;
//                 }
//                 PyList_SET_ITEM(keys, 0, mangled);
//             }
//             else {
//                 int res = PyList_Append(keys, mangled);
//                 Py_DECREF(mangled);
//                 if (res == -1) {
//                     goto error;
//                 }
//             }
//             if (!compiler_visit_expr(c, default_)) {
//                 goto error;
//             }
//         }
//     }
//     if (keys != NULL) {
//         Py_ssize_t default_count = PyList_GET_SIZE(keys);
//         PyObject *keys_tuple = PyList_AsTuple(keys);
//         Py_DECREF(keys);
//         ADDOP_LOAD_CONST_NEW(c, keys_tuple);
//         ADDOP_I(c, BUILD_CONST_KEY_MAP, default_count);
//         assert(default_count > 0);
//         return 1;
//     }
//     else {
//         return -1;
//     }

// error:
//     Py_XDECREF(keys);
//     return 0;
// }

static void
compiler_visit_annexpr(struct compiler *c, expr_ty annotation)
{
    PyObject *str = _PyAST_ExprAsUnicode(annotation);
    if (str == NULL) {
        COMPILER_ERROR(c);
    }
    emit1(c, LOAD_CONST, compiler_new_const(c, str));
}

// static int
// compiler_visit_argannotation(struct compiler *c, identifier id,
//     expr_ty annotation, PyObject *names)
// {
//     if (annotation) {
//         PyObject *mangled;
//         if (c->c_future->ff_features & CO_FUTURE_ANNOTATIONS) {
//             VISIT(c, annexpr, annotation)
//         }
//         else {
//             VISIT(c, expr, annotation);
//         }
//         mangled = _Py_Mangle(c->u->u_private, id);
//         if (!mangled)
//             return 0;
//         if (PyList_Append(names, mangled) < 0) {
//             Py_DECREF(mangled);
//             return 0;
//         }
//         Py_DECREF(mangled);
//     }
//     return 1;
// }

// static int
// compiler_visit_argannotations(struct compiler *c, asdl_seq* args,
//                               PyObject *names)
// {
//     int i;
//     for (i = 0; i < asdl_seq_LEN(args); i++) {
//         arg_ty arg = (arg_ty)asdl_seq_GET(args, i);
//         if (!compiler_visit_argannotation(
//                         c,
//                         arg->arg,
//                         arg->annotation,
//                         names))
//             return 0;
//     }
//     return 1;
// }

// static int
// compiler_visit_annotations(struct compiler *c, arguments_ty args,
//                            expr_ty returns)
// {
//     /* Push arg annotation dict.
//        The expressions are evaluated out-of-order wrt the source code.

//        Return 0 on error, -1 if no dict pushed, 1 if a dict is pushed.
//        */
//     static identifier return_str;
//     PyObject *names;
//     Py_ssize_t len;
//     names = PyList_New(0);
//     if (!names)
//         return 0;

//     if (!compiler_visit_argannotations(c, args->args, names))
//         goto error;
//     if (!compiler_visit_argannotations(c, args->posonlyargs, names))
//         goto error;
//     if (args->vararg && args->vararg->annotation &&
//         !compiler_visit_argannotation(c, args->vararg->arg,
//                                      args->vararg->annotation, names))
//         goto error;
//     if (!compiler_visit_argannotations(c, args->kwonlyargs, names))
//         goto error;
//     if (args->kwarg && args->kwarg->annotation &&
//         !compiler_visit_argannotation(c, args->kwarg->arg,
//                                      args->kwarg->annotation, names))
//         goto error;

//     if (!return_str) {
//         return_str = PyUnicode_InternFromString("return");
//         if (!return_str)
//             goto error;
//     }
//     if (!compiler_visit_argannotation(c, return_str, returns, names)) {
//         goto error;
//     }

//     len = PyList_GET_SIZE(names);
//     if (len) {
//         PyObject *keytuple = PyList_AsTuple(names);
//         Py_DECREF(names);
//         ADDOP_LOAD_CONST_NEW(c, keytuple);
//         ADDOP_I(c, BUILD_CONST_KEY_MAP, len);
//         return 1;
//     }
//     else {
//         Py_DECREF(names);
//         return -1;
//     }

// error:
//     Py_DECREF(names);
//     return 0;
// }

// static void
// compiler_visit_defaults(struct compiler *c, arguments_ty args)
// {
//     VISIT_SEQ(c, expr, args->defaults);
//     ADDOP_I(c, BUILD_TUPLE, asdl_seq_LEN(args->defaults));
//     return 1;
// }

static Py_ssize_t
compiler_default_arguments(struct compiler *c, arguments_ty args)
{
    Py_ssize_t funcflags = 0;
    if (args->defaults && asdl_seq_LEN(args->defaults) > 0) {
        PyErr_Format(PyExc_RuntimeError, "compiler_visit_defaults NYI");
        COMPILER_ERROR(c);
        // compiler_visit_defaults(c, args);
        // funcflags |= 0x01;
    }
    if (args->kwonlyargs) {
        PyErr_Format(PyExc_RuntimeError, "compiler_visit_kwonlydefaults NYI");
        COMPILER_ERROR(c);
        // int res = compiler_visit_kwonlydefaults(c, args->kwonlyargs,
        //                                         args->kw_defaults);
        // if (res > 0) {
        //     funcflags |= 0x02;
        // }
    }
    return funcflags;
}

static void
compiler_function(struct compiler *c, stmt_ty s, int is_async)
{
    PyObject *qualname, *docstring = NULL;
    arguments_ty args;
    expr_ty returns;
    identifier name;
    asdl_seq* decos;
    asdl_seq *body;
    Py_ssize_t i, funcflags, deco_base;
    // int annotations;
    int scope_type;
    int firstlineno;

    if (is_async) {
        assert(s->kind == AsyncFunctionDef_kind);

        args = s->v.AsyncFunctionDef.args;
        returns = s->v.AsyncFunctionDef.returns;
        decos = s->v.AsyncFunctionDef.decorator_list;
        name = s->v.AsyncFunctionDef.name;
        body = s->v.AsyncFunctionDef.body;

        scope_type = COMPILER_SCOPE_ASYNC_FUNCTION;
    } else {
        assert(s->kind == FunctionDef_kind);

        args = s->v.FunctionDef.args;
        returns = s->v.FunctionDef.returns;
        decos = s->v.FunctionDef.decorator_list;
        name = s->v.FunctionDef.name;
        body = s->v.FunctionDef.body;

        scope_type = COMPILER_SCOPE_FUNCTION;
    }

    deco_base = compiler_decorators(c, decos);

    firstlineno = s->lineno;
    if (asdl_seq_LEN(decos)) {
        firstlineno = ((expr_ty)asdl_seq_GET(decos, 0))->lineno;
    }

    funcflags = compiler_default_arguments(c, args); // FIXME
    (void)funcflags; (void)qualname; (void)returns;
    // annotations = compiler_visit_annotations(c, args, returns);
    // if (annotations == 0) {
    //     return 0;
    // }
    // else if (annotations > 0) {
    //     funcflags |= 0x04;
    // }

    compiler_enter_scope(c, name, scope_type, (void *)s, firstlineno);

    emit1(c, FUNC_HEADER, 0);

    /* if not -OO mode, add docstring */
    if (c->optimize < 2) {
        docstring = _PyAST_GetDocString(body);
    }
    compiler_const(c, docstring ? docstring : Py_None); //?????

    c->unit->argcount = asdl_seq_LEN(args->args);
    c->unit->posonlyargcount = asdl_seq_LEN(args->posonlyargs);
    c->unit->kwonlyargcount = asdl_seq_LEN(args->kwonlyargs);
    compiler_visit_stmts(c, body);
    assemble(c, 1);
    compiler_exit_scope(c);

    emit1(c, MAKE_FUNCTION, compiler_const(c, (PyObject *)c->code));

    qualname = c->unit->qualname;
    // Py_INCREF(qualname);
    // compiler_exit_scope(c);
    // if (co == NULL) {
    //     Py_XDECREF(qualname);
    //     Py_XDECREF(co);
    //     return 0;
    // }

    // compiler_make_closure(c, co, funcflags, qualname);
    // Py_DECREF(qualname);
    // Py_DECREF(co);

    /* decorators */
    for (i = 0; i < asdl_seq_LEN(decos); i++) {
        emit1(c, STORE_FAST, deco_base);
        emit_call(c, CALL_FUNCTION, deco_base, 1);
        deco_base -= FRAME_EXTRA - 1;
    }

    compiler_store(c, name);
    // return compiler_nameop(c, name, Store);
}

static void
compiler_class(struct compiler *c, stmt_ty s)
{
    int firstlineno;
    Py_ssize_t deco_base;
    asdl_seq* decos = s->v.ClassDef.decorator_list;

    deco_base = compiler_decorators(c, decos);

    firstlineno = s->lineno;
    if (asdl_seq_LEN(decos)) {
        firstlineno = ((expr_ty)asdl_seq_GET(decos, 0))->lineno;
    }

    /* ultimately generate code for:
         <name> = __build_class__(<func>, <name>, *<bases>, **<keywords>)
       where:
         <func> is a function/closure created from the class body;
            it has a single argument (__locals__) where the dict
            (or MutableSequence) representing the locals is passed
         <name> is the class name
         <bases> is the positional arguments and *varargs argument
         <keywords> is the keyword arguments and **kwds argument
       This borrows from compiler_call.
    */

    /* 1. compile the class body into a code object */
    compiler_enter_scope(c, s->v.ClassDef.name,
                         COMPILER_SCOPE_CLASS, (void *)s, firstlineno);
    /* this block represents what we do in the new scope */
    {
        /* use the class name for name mangling */
        Py_INCREF(s->v.ClassDef.name);
        Py_XSETREF(c->unit->private, s->v.ClassDef.name);
        /* load (global) __name__ ... */
        compiler_nameop(c, _PyUnicode_FromId(&PyId___name__), Load);
        /* ... and store it as __module__ */
        compiler_nameop(c, _PyUnicode_FromId(&PyId___module__), Store);
        assert(c->unit->qualname);
        /* store the qualified name */
        emit1(c, LOAD_CONST, compiler_const(c, c->unit->qualname));
        compiler_nameop(c, _PyUnicode_FromId(&PyId___qualname__), Store);

        /* compile the body proper */
        compiler_body(c, s->v.ClassDef.body);
        /* Return __classcell__ if it is referenced, otherwise return None */
        if (c->unit->ste->ste_needs_class_closure) {
            assert(false && "ste_needs_class_closure NYI");
            // /* Store __classcell__ into class namespace & return it */
            // str = PyUnicode_InternFromString("__class__");
            // if (str == NULL) {
            //     compiler_exit_scope(c);
            //     return 0;
            // }
            // i = compiler_lookup_arg(c->unit->cellvars, str);
            // Py_DECREF(str);
            // if (i < 0) {
            //     compiler_exit_scope(c);
            //     return 0;
            // }
            // assert(i == 0);

            // ADDOP_I(c, LOAD_CLOSURE, i);
            // ADDOP(c, DUP_TOP);
            // str = PyUnicode_InternFromString("__classcell__");
            // if (!str || !compiler_nameop(c, str, Store)) {
            //     Py_XDECREF(str);
            //     compiler_exit_scope(c);
            //     return 0;
            // }
            // Py_DECREF(str);
        }
        else {
            /* No methods referenced __class__, so just return None */
            assert(PyDict_GET_SIZE(c->unit->cellvars) == 0);
            emit1(c, LOAD_CONST, const_none(c));
        }
        emit0(c, RETURN_VALUE);
        /* create the code object */
        assemble(c, 1);
    }

    /* leave the new scope */
    compiler_exit_scope(c);

    Py_ssize_t base = c->unit->next_register + FRAME_EXTRA;
    reserve_regs(c, FRAME_EXTRA + 2);
    emit0(c, LOAD_BUILD_CLASS);
    emit1(c, STORE_FAST, base - 1);
    emit1(c, LOAD_CONST, compiler_const(c, (PyObject *)c->code));
    emit1(c, STORE_FAST, base);
    emit1(c, LOAD_CONST, compiler_const(c, s->v.ClassDef.name));
    emit1(c, STORE_FAST, base + 1);

    Py_ssize_t n = asdl_seq_LEN(s->v.ClassDef.bases);
    assert(n <= 253);
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty b = asdl_seq_GET(s->v.ClassDef.bases, i);
        expr_to_reg(c, b, base + i + 2);
    }

    assert(asdl_seq_LEN(s->v.ClassDef.keywords) == 0);
    emit_call(c, CALL_FUNCTION, base, n + 2);
    c->unit->next_register = base - FRAME_EXTRA;

//     /* 6. apply decorators */
//     for (i = 0; i < asdl_seq_LEN(decos); i++) {
//         ADDOP_I(c, CALL_FUNCTION, 1);
//     }


//     /* 3. load a function (or closure) made from the code object */
//     compiler_make_closure(c, co, 0, NULL);
//     Py_DECREF(co);

//     /* 4. load class name */
//     ADDOP_LOAD_CONST(c, s->v.ClassDef.name);

//     /* 5. generate the rest of the code for the call */
//     if (!compiler_call_helper(c, 2,
//                               s->v.ClassDef.bases,
//                               s->v.ClassDef.keywords))
//         return 0;


//     /* 7. store into <name> */
//     if (!compiler_nameop(c, s->v.ClassDef.name, Store))
//         return 0;
//     return 1;
}

/* Return 0 if the expression is a constant value except named singletons.
   Return 1 otherwise. */
static int
check_is_arg(expr_ty e)
{
    if (e->kind != Constant_kind) {
        return 1;
    }
    PyObject *value = e->v.Constant.value;
    return (value == Py_None
         || value == Py_False
         || value == Py_True
         || value == Py_Ellipsis);
}

/* Check operands of identity chacks ("is" and "is not").
   Emit a warning if any operand is a constant except named singletons.
   Return 0 on error.
 */
static void
check_compare(struct compiler *c, expr_ty e)
{
    Py_ssize_t i, n;
    int left = check_is_arg(e->v.Compare.left);
    n = asdl_seq_LEN(e->v.Compare.ops);
    for (i = 0; i < n; i++) {
        cmpop_ty op = (cmpop_ty)asdl_seq_GET(e->v.Compare.ops, i);
        int right = check_is_arg((expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
        if (op == Is || op == IsNot) {
            if (!right || !left) {
                const char *msg = (op == Is)
                        ? "\"is\" with a literal. Did you mean \"==\"?"
                        : "\"is not\" with a literal. Did you mean \"!=\"?";
                compiler_warn(c, msg);
                return;
            }
        }
        left = right;
    }
}



// static int
// compiler_jump_if(struct compiler *c, expr_ty e, basicblock *next, int cond)
// {
//     switch (e->kind) {
//     case UnaryOp_kind:
//         if (e->v.UnaryOp.op == Not)
//             return compiler_jump_if(c, e->v.UnaryOp.operand, next, !cond);
//         /* fallback to general implementation */
//         break;
//     case BoolOp_kind: {
//         asdl_seq *s = e->v.BoolOp.values;
//         Py_ssize_t i, n = asdl_seq_LEN(s) - 1;
//         assert(n >= 0);
//         int cond2 = e->v.BoolOp.op == Or;
//         basicblock *next2 = next;
//         if (!cond2 != !cond) {
//             next2 = compiler_new_block(c);
//             if (next2 == NULL)
//                 return 0;
//         }
//         for (i = 0; i < n; ++i) {
//             if (!compiler_jump_if(c, (expr_ty)asdl_seq_GET(s, i), next2, cond2))
//                 return 0;
//         }
//         if (!compiler_jump_if(c, (expr_ty)asdl_seq_GET(s, n), next, cond))
//             return 0;
//         if (next2 != next)
//             compiler_use_next_block(c, next2);
//         return 1;
//     }
//     case IfExp_kind: {
//         basicblock *end, *next2;
//         end = compiler_new_block(c);
//         if (end == NULL)
//             return 0;
//         next2 = compiler_new_block(c);
//         if (next2 == NULL)
//             return 0;
//         if (!compiler_jump_if(c, e->v.IfExp.test, next2, 0))
//             return 0;
//         if (!compiler_jump_if(c, e->v.IfExp.body, next, cond))
//             return 0;
//         ADDOP_JREL(c, JUMP_FORWARD, end);
//         compiler_use_next_block(c, next2);
//         if (!compiler_jump_if(c, e->v.IfExp.orelse, next, cond))
//             return 0;
//         compiler_use_next_block(c, end);
//         return 1;
//     }
//     case Compare_kind: {
//         Py_ssize_t i, n = asdl_seq_LEN(e->v.Compare.ops) - 1;
//         if (n > 0) {
//             if (!check_compare(c, e)) {
//                 return 0;
//             }
//             basicblock *cleanup = compiler_new_block(c);
//             if (cleanup == NULL)
//                 return 0;
//             VISIT(c, expr, e->v.Compare.left);
//             for (i = 0; i < n; i++) {
//                 VISIT(c, expr,
//                     (expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
//                 ADDOP(c, DUP_TOP);
//                 ADDOP(c, ROT_THREE);
//                 ADDOP_COMPARE(c, asdl_seq_GET(e->v.Compare.ops, i));
//                 ADDOP_JABS(c, POP_JUMP_IF_FALSE, cleanup);
//                 NEXT_BLOCK(c);
//             }
//             VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, n));
//             ADDOP_COMPARE(c, asdl_seq_GET(e->v.Compare.ops, n));
//             ADDOP_JABS(c, cond ? POP_JUMP_IF_TRUE : POP_JUMP_IF_FALSE, next);
//             basicblock *end = compiler_new_block(c);
//             if (end == NULL)
//                 return 0;
//             ADDOP_JREL(c, JUMP_FORWARD, end);
//             compiler_use_next_block(c, cleanup);
//             ADDOP(c, POP_TOP);
//             if (!cond) {
//                 ADDOP_JREL(c, JUMP_FORWARD, next);
//             }
//             compiler_use_next_block(c, end);
//             return 1;
//         }
//         /* fallback to general implementation */
//         break;
//     }
//     default:
//         /* fallback to general implementation */
//         break;
//     }

//     /* general implementation */
//     VISIT(c, expr, e);
//     ADDOP_JABS(c, cond ? POP_JUMP_IF_TRUE : POP_JUMP_IF_FALSE, next);
//     return 1;
// }

static void
compiler_ifexp(struct compiler *c, expr_ty e)
{
    struct bc_label end, next;
    assert(e->kind == IfExp_kind);

    compiler_visit_expr(c, e->v.IfExp.test);
    emit_jump(c, POP_JUMP_IF_FALSE, &next);
    compiler_visit_expr(c, e->v.IfExp.body);
    emit_jump(c, JUMP, &end);
    emit_label(c, &next);
    compiler_visit_expr(c, e->v.IfExp.orelse);
    emit_label(c, &end);
}

// static int
// compiler_lambda(struct compiler *c, expr_ty e)
// {
//     PyCodeObject *co;
//     PyObject *qualname;
//     static identifier name;
//     Py_ssize_t funcflags;
//     arguments_ty args = e->v.Lambda.args;
//     assert(e->kind == Lambda_kind);

//     if (!name) {
//         name = PyUnicode_InternFromString("<lambda>");
//         if (!name)
//             return 0;
//     }

//     funcflags = compiler_default_arguments(c, args);
//     if (funcflags == -1) {
//         return 0;
//     }

//     if (!compiler_enter_scope(c, name, COMPILER_SCOPE_LAMBDA,
//                               (void *)e, e->lineno))
//         return 0;

//     /* Make None the first constant, so the lambda can't have a
//        docstring. */
//     if (compiler_add_const(c, Py_None) < 0)
//         return 0;

//     c->u->u_argcount = asdl_seq_LEN(args->args);
//     c->u->u_posonlyargcount = asdl_seq_LEN(args->posonlyargs);
//     c->u->u_kwonlyargcount = asdl_seq_LEN(args->kwonlyargs);
//     VISIT_IN_SCOPE(c, expr, e->v.Lambda.body);
//     if (c->u->u_ste->ste_generator) {
//         co = assemble(c, 0);
//     }
//     else {
//         ADDOP_IN_SCOPE(c, RETURN_VALUE);
//         co = assemble(c, 1);
//     }
//     qualname = c->u->u_qualname;
//     Py_INCREF(qualname);
//     compiler_exit_scope(c);
//     if (co == NULL)
//         return 0;

//     compiler_make_closure(c, co, funcflags, qualname);
//     Py_DECREF(qualname);
//     Py_DECREF(co);

//     return 1;
// }

static void
compiler_if(struct compiler *c, stmt_ty s)
{
    int constant;
    assert(s->kind == If_kind);

    constant = expr_constant(s->v.If.test);
    /* constant = 0: "if 0"
     * constant = 1: "if 1", "if 2", ...
     * constant = -1: rest */
    if (constant == 0) {
        BEGIN_DO_NOT_EMIT_BYTECODE
        compiler_visit_stmts(c, s->v.If.body);
        END_DO_NOT_EMIT_BYTECODE
        if (s->v.If.orelse) {
            compiler_visit_stmts(c, s->v.If.orelse);
        }
    } else if (constant == 1) {
        compiler_visit_stmts(c, s->v.If.body);
        if (s->v.If.orelse) {
            BEGIN_DO_NOT_EMIT_BYTECODE
            compiler_visit_stmts(c, s->v.If.orelse);
            END_DO_NOT_EMIT_BYTECODE
        }
    } else {
        struct bc_label next;
        compiler_visit_expr(c, s->v.If.test);
        emit_jump(c, POP_JUMP_IF_FALSE, &next);
        compiler_visit_stmts(c, s->v.If.body);
        if (asdl_seq_LEN(s->v.If.orelse)) {
            struct bc_label after;
            emit_jump(c, JUMP, &after);
            emit_label(c, &next);
            compiler_visit_stmts(c, s->v.If.orelse);
            emit_label(c, &after);
        }
        else {
            emit_label(c, &next);
        }
    }
}

static struct fblock *
compiler_push_loop(struct compiler *c, int loop_type)
{
    struct block_table *blocks = &c->unit->blocks;
    if (blocks->offset == blocks->allocated) {
        resize_array(
            c,
            (void**)&blocks->arr,
            &blocks->allocated,
            /*min_size=*/8,
            sizeof(*blocks->arr));
    }
    struct fblock *loop = &blocks->arr[blocks->offset++];
    loop->type = loop_type;
    return loop;
}

static void
compiler_pop_loop(struct compiler *c, struct fblock *loop)
{
    struct block_table *blocks = &c->unit->blocks;
    assert(loop == &blocks->arr[blocks->offset - 1]);
    blocks->offset--;
    memset(&blocks->arr[blocks->offset], 0, sizeof(*loop));
}

static void
compiler_for(struct compiler *c, stmt_ty s)
{
    struct multi_label break_label = MULTI_LABEL_INIT;
    struct multi_label continue_label = MULTI_LABEL_INIT;
    struct fblock *loop;
    Py_ssize_t reg;
    uint32_t top_offset;

    compiler_visit_expr(c, s->v.For.iter);
    reg = reserve_regs(c, 1);

    emit1(c, GET_ITER, reg);
    emit_jump(c, JUMP, multi_label_next(c, &continue_label));
    top_offset = c->unit->instr.offset;

    loop = compiler_push_loop(c, FOR_LOOP);
    loop->ForLoop.reg = reg;
    loop->ForLoop.break_label = &break_label;
    loop->ForLoop.continue_label = &continue_label;

    compiler_assign_reg(c, s->v.For.target, REG_ACCUMULATOR);
    compiler_visit_stmts(c, s->v.For.body);
    emit_multi_label(c, &continue_label);
    emit_for(c, reg, top_offset);
    free_reg(c, reg);

    compiler_pop_loop(c, loop);

    if (s->v.For.orelse) {
        compiler_visit_stmts(c, s->v.For.orelse);
    }

    emit_multi_label(c, &break_label);
}


// static int
// compiler_async_for(struct compiler *c, stmt_ty s)
// {
//     basicblock *start, *except, *end;
//     if (c->c_flags->cf_flags & PyCF_ALLOW_TOP_LEVEL_AWAIT){
//         c->u->u_ste->ste_coroutine = 1;
//     } else if (c->u->u_scope_type != COMPILER_SCOPE_ASYNC_FUNCTION) {
//         return compiler_error(c, "'async for' outside async function");
//     }

//     start = compiler_new_block(c);
//     except = compiler_new_block(c);
//     end = compiler_new_block(c);

//     if (start == NULL || except == NULL || end == NULL) {
//         return 0;
//     }
//     VISIT(c, expr, s->v.AsyncFor.iter);
//     ADDOP(c, GET_AITER);

//     compiler_use_next_block(c, start);
//     if (!compiler_push_fblock(c, FOR_LOOP, start, end, NULL)) {
//         return 0;
//     }
//     /* SETUP_FINALLY to guard the __anext__ call */
//     ADDOP_JREL(c, SETUP_FINALLY, except);
//     ADDOP(c, GET_ANEXT);
//     ADDOP_LOAD_CONST(c, Py_None);
//     ADDOP(c, YIELD_FROM);
//     ADDOP(c, POP_BLOCK);  /* for SETUP_FINALLY */

//     /* Success block for __anext__ */
//     VISIT(c, expr, s->v.AsyncFor.target);
//     VISIT_SEQ(c, stmt, s->v.AsyncFor.body);
//     ADDOP_JABS(c, JUMP_ABSOLUTE, start);

//     compiler_pop_fblock(c, FOR_LOOP, start);

//     /* Except block for __anext__ */
//     compiler_use_next_block(c, except);
//     ADDOP(c, END_ASYNC_FOR);

//     /* `else` block */
//     VISIT_SEQ(c, stmt, s->v.For.orelse);

//     compiler_use_next_block(c, end);

//     return 1;
// }

static void
compiler_while(struct compiler *c, stmt_ty s)
{
//     basicblock *loop, *orelse, *end, *anchor = NULL;
    int constant = expr_constant(s->v.While.test);

    if (constant == 0) {
//         BEGIN_DO_NOT_EMIT_BYTECODE
//         // Push a dummy block so the VISIT_SEQ knows that we are
//         // inside a while loop so it can correctly evaluate syntax
//         // errors.
//         if (!compiler_push_fblock(c, WHILE_LOOP, NULL, NULL, NULL)) {
//             return 0;
//         }
//         VISIT_SEQ(c, stmt, s->v.While.body);
//         // Remove the dummy block now that is not needed.
//         compiler_pop_fblock(c, WHILE_LOOP, NULL);
//         END_DO_NOT_EMIT_BYTECODE
//         if (s->v.While.orelse) {
//             VISIT_SEQ(c, stmt, s->v.While.orelse);
//         }
//         return 1;
    }
//     loop = compiler_new_block(c);
//     end = compiler_new_block(c);

    struct multi_label break_label = MULTI_LABEL_INIT;
    struct multi_label continue_label = MULTI_LABEL_INIT;
    struct fblock *loop;
    uint32_t top_offset;
    
    emit_jump(c, JUMP, multi_label_next(c, &continue_label));
    top_offset = c->unit->instr.offset;

    loop = compiler_push_loop(c, WHILE_LOOP);
    loop->WhileLoop.break_label = &break_label;
    loop->WhileLoop.continue_label = &continue_label;

    compiler_visit_stmts(c, s->v.While.body);
    emit_multi_label(c, &continue_label);
    compiler_visit_expr(c, s->v.While.test);
    emit_bwd_jump(c, POP_JUMP_IF_TRUE, top_offset);

    compiler_pop_loop(c, loop);

    if (s->v.While.orelse) {
        compiler_visit_stmts(c, s->v.While.orelse);
    }

    emit_multi_label(c, &break_label);


//     if (loop == NULL || end == NULL)
//         return 0;
//     if (s->v.While.orelse) {
//         orelse = compiler_new_block(c);
//         if (orelse == NULL)
//             return 0;
//     }
//     else
//         orelse = NULL;

//     compiler_use_next_block(c, loop);
//     if (!compiler_push_fblock(c, WHILE_LOOP, loop, end, NULL))
//         return 0;
//     if (constant == -1) {
//         if (!compiler_jump_if(c, s->v.While.test, anchor, 0))
//             return 0;
//     }
//     VISIT_SEQ(c, stmt, s->v.While.body);
//     ADDOP_JABS(c, JUMP_ABSOLUTE, loop);

//     /* XXX should the two POP instructions be in a separate block
//        if there is no else clause ?
//     */

//     if (constant == -1)
//         compiler_use_next_block(c, anchor);
//     compiler_pop_fblock(c, WHILE_LOOP, loop);

//     if (orelse != NULL) /* what if orelse is just pass? */
//         VISIT_SEQ(c, stmt, s->v.While.orelse);
//     compiler_use_next_block(c, end);

//     return 1;
}

static void
compiler_return(struct compiler *c, stmt_ty s)
{
    if (c->unit->ste->ste_type != FunctionBlock)
        return compiler_error(c, "'return' outside function");
    if (s->v.Return.value != NULL &&
        c->unit->ste->ste_coroutine && c->unit->ste->ste_generator)
    {
            return compiler_error(
                c, "'return' with value in async generator");
    }
    if (s->v.Return.value == NULL) {
        emit1(c, LOAD_CONST, const_none(c));
    }
    else {
        compiler_visit_expr(c, s->v.Return.value);
    }
    struct block_table *blocks = &c->unit->blocks;
    for (Py_ssize_t i = blocks->offset - 1; i >= 0; i--) {
        struct fblock *block = &blocks->arr[i];
        if (block->type == TRY_FINALLY) {
            emit1(c, STORE_FAST, block->TryFinally.reg + 1);
        }
        compiler_unwind_block(c, block);
    }
    emit0(c, RETURN_VALUE);
    c->unit->reachable = false;
}

static void
compiler_break(struct compiler *c)
{
    struct block_table *blocks = &c->unit->blocks;
    for (Py_ssize_t i = blocks->offset - 1; i >= 0; i--) {
        struct fblock *block = &blocks->arr[i];
        compiler_unwind_block(c, block);
        if (block->type == FOR_LOOP) {
            emit_jump(c, JUMP, multi_label_next(c, block->ForLoop.break_label));
            return;
        }
        if (block->type == WHILE_LOOP) {
            emit_jump(c, JUMP, multi_label_next(c, block->WhileLoop.break_label));
            return;
        }
    }
    compiler_error(c, "'break' outside loop");
}

static void
compiler_continue(struct compiler *c)
{
    struct block_table *blocks = &c->unit->blocks;
    for (Py_ssize_t i = blocks->offset - 1; i >= 0; i--) {
        struct fblock *block = &blocks->arr[i];
        compiler_unwind_block(c, block);
        if (block->type == FOR_LOOP) {
            emit_jump(c, JUMP, multi_label_next(c, block->ForLoop.continue_label));
            return;
        }
        if (block->type == WHILE_LOOP) {
            emit_jump(c, JUMP, multi_label_next(c, block->WhileLoop.continue_label));
            return;
        }
    }
    compiler_error(c, "'continue' not properly in loop");
}


static void
compiler_raise(struct compiler *c, stmt_ty s)
{
    if (s->v.Raise.cause) {
        Py_ssize_t base = c->unit->next_register;
        expr_to_reg(c, s->v.Raise.exc, base);
        expr_to_reg(c, s->v.Raise.cause, base + 1);
        emit3(c, CALL_INTRINSIC_N, Intrinsic_vm_exc_set_cause, base, 2);
        free_regs_above(c, base);
    }
    else if (s->v.Raise.exc) {
        compiler_visit_expr(c, s->v.Raise.exc);
    }
    emit0(c, RAISE);
}


// /* Code generated for "try: <body> finally: <finalbody>" is as follows:

//         SETUP_FINALLY           L
//         <code for body>
//         POP_BLOCK
//         <code for finalbody>
//         JUMP E
//     L:
//         <code for finalbody>
//     E:

//    The special instructions use the block stack.  Each block
//    stack entry contains the instruction that created it (here
//    SETUP_FINALLY), the level of the value stack at the time the
//    block stack entry was created, and a label (here L).

//    SETUP_FINALLY:
//     Pushes the current value stack level and the label
//     onto the block stack.
//    POP_BLOCK:
//     Pops en entry from the block stack.

//    The block stack is unwound when an exception is raised:
//    when a SETUP_FINALLY entry is found, the raised and the caught
//    exceptions are pushed onto the value stack (and the exception
//    condition is cleared), and the interpreter jumps to the label
//    gotten from the block stack.
// */

// static int
// compiler_try_finally(struct compiler *c, stmt_ty s)
// {
//     basicblock *body, *end, *exit;

//     body = compiler_new_block(c);
//     end = compiler_new_block(c);
//     exit = compiler_new_block(c);
//     if (body == NULL || end == NULL || exit == NULL)
//         return 0;

//     /* `try` block */
//     ADDOP_JREL(c, SETUP_FINALLY, end);
//     compiler_use_next_block(c, body);
//     if (!compiler_push_fblock(c, FINALLY_TRY, body, end, s->v.Try.finalbody))
//         return 0;
//     if (s->v.Try.handlers && asdl_seq_LEN(s->v.Try.handlers)) {
//         if (!compiler_try_except(c, s))
//             return 0;
//     }
//     else {
//         VISIT_SEQ(c, stmt, s->v.Try.body);
//     }
//     ADDOP(c, POP_BLOCK);
//     compiler_pop_fblock(c, FINALLY_TRY, body);
//     VISIT_SEQ(c, stmt, s->v.Try.finalbody);
//     ADDOP_JREL(c, JUMP_FORWARD, exit);
//     /* `finally` block */
//     compiler_use_next_block(c, end);
//     if (!compiler_push_fblock(c, FINALLY_END, end, NULL, NULL))
//         return 0;
//     VISIT_SEQ(c, stmt, s->v.Try.finalbody);
//     compiler_pop_fblock(c, FINALLY_END, end);
//     ADDOP(c, RERAISE);
//     compiler_use_next_block(c, exit);
//     return 1;
// }

// /*
//    Code generated for "try: S except E1 as V1: S1 except E2 as V2: S2 ...":
//    (The contents of the value stack is shown in [], with the top
//    at the right; 'tb' is trace-back info, 'val' the exception's
//    associated value, and 'exc' the exception.)

//    Value stack          Label   Instruction     Argument
//    []                           SETUP_FINALLY   L1
//    []                           <code for S>
//    []                           POP_BLOCK
//    []                           JUMP_FORWARD    L0

//    [tb, val, exc]       L1:     DUP                             )
//    [tb, val, exc, exc]          <evaluate E1>                   )
//    [tb, val, exc, exc, E1]      JUMP_IF_NOT_EXC_MATCH L2        ) only if E1
//    [tb, val, exc]               POP
//    [tb, val]                    <assign to V1>  (or POP if no V1)
//    [tb]                         POP
//    []                           <code for S1>
//                                 JUMP_FORWARD    L0

//    [tb, val, exc]       L2:     DUP
//    .............................etc.......................

//    [tb, val, exc]       Ln+1:   RERAISE     # re-raise exception

//    []                   L0:     <next statement>

//    Of course, parts are not generated if Vi or Ei is not present.
// */
// static int
// compiler_try_except(struct compiler *c, stmt_ty s)
// {
//     basicblock *body, *orelse, *except, *end;
//     Py_ssize_t i, n;

//     body = compiler_new_block(c);
//     except = compiler_new_block(c);
//     orelse = compiler_new_block(c);
//     end = compiler_new_block(c);
//     if (body == NULL || except == NULL || orelse == NULL || end == NULL)
//         return 0;
//     ADDOP_JREL(c, SETUP_FINALLY, except);
//     compiler_use_next_block(c, body);
//     if (!compiler_push_fblock(c, EXCEPT, body, NULL, NULL))
//         return 0;
//     VISIT_SEQ(c, stmt, s->v.Try.body);
//     ADDOP(c, POP_BLOCK);
//     compiler_pop_fblock(c, EXCEPT, body);
//     ADDOP_JREL(c, JUMP_FORWARD, orelse);
//     n = asdl_seq_LEN(s->v.Try.handlers);
//     compiler_use_next_block(c, except);
//     for (i = 0; i < n; i++) {
//         excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
//             s->v.Try.handlers, i);
//         if (!handler->v.ExceptHandler.type && i < n-1)
//             return compiler_error(c, "default 'except:' must be last");
//         c->u->u_lineno_set = 0;
//         c->u->u_lineno = handler->lineno;
//         c->u->u_col_offset = handler->col_offset;
//         except = compiler_new_block(c);
//         if (except == NULL)
//             return 0;
//         if (handler->v.ExceptHandler.type) {
//             ADDOP(c, DUP_TOP);
//             VISIT(c, expr, handler->v.ExceptHandler.type);
//             ADDOP_JABS(c, JUMP_IF_NOT_EXC_MATCH, except);
//         }
//         ADDOP(c, POP_TOP);
//         if (handler->v.ExceptHandler.name) {
//             basicblock *cleanup_end, *cleanup_body;

//             cleanup_end = compiler_new_block(c);
//             cleanup_body = compiler_new_block(c);
//             if (cleanup_end == NULL || cleanup_body == NULL) {
//                 return 0;
//             }

//             compiler_nameop(c, handler->v.ExceptHandler.name, Store);
//             ADDOP(c, POP_TOP);

//             /*
//               try:
//                   # body
//               except type as name:
//                   try:
//                       # body
//                   finally:
//                       name = None # in case body contains "del name"
//                       del name
//             */

//             /* second try: */
//             ADDOP_JREL(c, SETUP_FINALLY, cleanup_end);
//             compiler_use_next_block(c, cleanup_body);
//             if (!compiler_push_fblock(c, HANDLER_CLEANUP, cleanup_body, NULL, handler->v.ExceptHandler.name))
//                 return 0;

//             /* second # body */
//             VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
//             compiler_pop_fblock(c, HANDLER_CLEANUP, cleanup_body);
//             ADDOP(c, POP_BLOCK);
//             ADDOP(c, POP_EXCEPT);
//             /* name = None; del name */
//             ADDOP_LOAD_CONST(c, Py_None);
//             compiler_nameop(c, handler->v.ExceptHandler.name, Store);
//             compiler_nameop(c, handler->v.ExceptHandler.name, Del);
//             ADDOP_JREL(c, JUMP_FORWARD, end);

//             /* except: */
//             compiler_use_next_block(c, cleanup_end);

//             /* name = None; del name */
//             ADDOP_LOAD_CONST(c, Py_None);
//             compiler_nameop(c, handler->v.ExceptHandler.name, Store);
//             compiler_nameop(c, handler->v.ExceptHandler.name, Del);

//             ADDOP(c, RERAISE);
//         }
//         else {
//             basicblock *cleanup_body;

//             cleanup_body = compiler_new_block(c);
//             if (!cleanup_body)
//                 return 0;

//             ADDOP(c, POP_TOP);
//             ADDOP(c, POP_TOP);
//             compiler_use_next_block(c, cleanup_body);
//             if (!compiler_push_fblock(c, HANDLER_CLEANUP, cleanup_body, NULL, NULL))
//                 return 0;
//             VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
//             compiler_pop_fblock(c, HANDLER_CLEANUP, cleanup_body);
//             ADDOP(c, POP_EXCEPT);
//             ADDOP_JREL(c, JUMP_FORWARD, end);
//         }
//         compiler_use_next_block(c, except);
//     }
//     ADDOP(c, RERAISE);
//     compiler_use_next_block(c, orelse);
//     VISIT_SEQ(c, stmt, s->v.Try.orelse);
//     compiler_use_next_block(c, end);
//     return 1;
// }

// static int
// compiler_try(struct compiler *c, stmt_ty s) {
//     if (s->v.Try.finalbody && asdl_seq_LEN(s->v.Try.finalbody))
//         return compiler_try_finally(c, s);
//     else
//         return compiler_try_except(c, s);
// }


static void
compiler_import_as(struct compiler *c, identifier name, identifier asname)
{
    /* The IMPORT_NAME opcode was already generated.  This function
       merely needs to bind the result to a name.

       If there is a dot in name, we need to split it and emit a
       IMPORT_FROM for each name.
    */
    Py_ssize_t len = PyUnicode_GET_LENGTH(name);
    Py_ssize_t dot = PyUnicode_FindChar(name, '.', 0, len, 1);
    if (dot == -2) {
        COMPILER_ERROR(c);
    }
    if (dot != -1) {
        /* Consume the base module name to get the first attribute */
        Py_ssize_t reg = reserve_regs(c, 1);
        while (1) {
            Py_ssize_t pos = dot + 1;
            PyObject *attr;
            Py_ssize_t const_slot;
            dot = PyUnicode_FindChar(name, '.', pos, len, 1);
            if (dot == -2) {
                COMPILER_ERROR(c);
            }
            attr = PyUnicode_Substring(name, pos, (dot != -1) ? dot : len);
            if (!attr) {
                COMPILER_ERROR(c);
            }
            const_slot = compiler_new_const(c, attr);

            emit1(c, STORE_FAST, reg);
            emit2(c, IMPORT_FROM, reg, const_slot);
        }
        clear_reg(c, reg);
    }
    compiler_store(c, asname);
}

static void
compiler_import(struct compiler *c, stmt_ty s)
{
    /* The Import node stores a module name like a.b.c as a single
       string.  This is convenient for all cases except
         import a.b.c as d
       where we need to parse that string to extract the individual
       module names.
       XXX Perhaps change the representation to make this case simpler?
     */
    Py_ssize_t i, n = asdl_seq_LEN(s->v.Import.names);

    for (i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.Import.names, i);

        PyObject *arg = Py_BuildValue("(OOi)", alias->name, Py_None, 0);
        if (arg == NULL) {
            COMPILER_ERROR(c);
        }
        emit1(c, IMPORT_NAME, compiler_new_const(c, arg));

        if (alias->asname) {
            compiler_import_as(c, alias->name, alias->asname);
        }
        else {
            identifier tmp = alias->name;
            Py_ssize_t dot = PyUnicode_FindChar(
                alias->name, '.', 0, PyUnicode_GET_LENGTH(alias->name), 1);
            if (dot != -1) {
                tmp = PyUnicode_Substring(alias->name, 0, dot);
                if (tmp == NULL) {
                    COMPILER_ERROR(c);
                }
                PyArena_AddPyObject(c->arena, tmp); // FIXME: wrong
            }
            compiler_store(c, tmp);
        }
    }
}

static void
compiler_from_import(struct compiler *c, stmt_ty s)
{
    _Py_static_string(PyId_empty_string, "");
    Py_ssize_t i, n = asdl_seq_LEN(s->v.ImportFrom.names);
    PyObject *fromlist, *arg;

    fromlist = PyTuple_New(n);
    if (fromlist == NULL) {
        COMPILER_ERROR(c);
    }

    /* build up the names */
    for (i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.ImportFrom.names, i);
        Py_INCREF(alias->name);
        PyTuple_SET_ITEM(fromlist, i, alias->name);
    }

    PyObject *module = s->v.ImportFrom.module;
    if (module == NULL) {
        module = _PyUnicode_FromId(&PyId_empty_string);
    }

    arg = Py_BuildValue("(ONi)", module, fromlist, s->v.ImportFrom.level);
    if (arg == NULL) {
        COMPILER_ERROR(c);
    }

    emit1(c, IMPORT_NAME, compiler_new_const(c, arg));

    if (s->lineno > c->future->ff_lineno && s->v.ImportFrom.module &&
        _PyUnicode_EqualToASCIIString(s->v.ImportFrom.module, "__future__")) {
        compiler_error(c, "from __future__ imports must occur "
                           "at the beginning of the file");
    }

    Py_ssize_t reg = reserve_regs(c, 1);
    emit1(c, STORE_FAST, reg);
    for (i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.ImportFrom.names, i);
        identifier store_name;

        if (i == 0 && PyUnicode_READ_CHAR(alias->name, 0) == '*') {
            assert(n == 1);
            emit1(c, IMPORT_STAR, reg); // TODO: make IMPORT_STAR operate on acc
        }
        else {
            emit2(c, IMPORT_FROM, reg, compiler_const(c, alias->name));

            store_name = alias->asname ? alias->asname : alias->name;
            compiler_store(c, store_name);
        }
    }
    /* remove imported module */
    clear_reg(c, reg);
}

struct var_info {
    int access;
    Py_ssize_t slot;
};

static struct var_info
resolve(struct compiler *c, PyObject *name)
{
    struct var_info r;
    PyObject *mangled = mangle(c, name);
    r.access = compiler_access(c, mangled);
    if (r.access == ACCESS_FAST || r.access == ACCESS_DEREF) {
        r.slot = compiler_varname(c, mangled);
    }
    else {
        r.slot = compiler_const(c, mangled);
    }
    return r;
}

static void
assign_name(struct compiler *c, PyObject *name, Py_ssize_t src)
{
    struct var_info a = resolve(c, name);
    int opcodes[] = {
        [ACCESS_FAST]   = STORE_FAST,
        [ACCESS_DEREF]  = STORE_DEREF,
        [ACCESS_NAME]   = STORE_NAME,
        [ACCESS_GLOBAL] = STORE_GLOBAL,
    };

    assert(a.access != ACCESS_CLASSDEREF);
    if (a.access == ACCESS_FAST &&
        src != REG_ACCUMULATOR &&
        is_temporary(c, src))
    {
        emit2(c, MOVE, a.slot, src);
        return;
    }

    to_accumulator(c, src);
    emit1(c, opcodes[a.access], a.slot);
}

static void
compiler_store(struct compiler *c, PyObject *name)
{
    // FIXME: just merge with assign_name ?
    assign_name(c, name, REG_ACCUMULATOR);
}

static void
assignment_helper(struct compiler *c, asdl_seq *elts)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    Py_ssize_t i;
    Py_ssize_t argcnt = n;
    Py_ssize_t after = -1; // FIXME: make non-negative
    int seen_star = 0;
    for (i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind != Starred_kind) {
            continue;
        }
        if (seen_star) {
            compiler_error(c,
                "two starred expressions in assignment");
        }
        seen_star = 1;
        argcnt = i;
        after = n - i - 1;
    }
    Py_ssize_t base = reserve_regs(c, n);
    emit3(c, UNPACK, base, argcnt, after);
    for (i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind) {
            elt = elt->v.Starred.value;
        }
        compiler_assign_reg(c, elt, base + i);
    }
    free_regs_above(c, base);
}

static void
compiler_assign_reg(struct compiler *c, expr_ty t, Py_ssize_t reg)
{
    // FIXME: when is reg preserved or cleared?
    switch (t->kind) {
    case Name_kind:
        assign_name(c, t->v.Name.id, reg);
        break;
    case Attribute_kind: {
        Py_ssize_t owner = expr_to_any_reg(c, t->v.Attribute.value);
        to_accumulator(c, reg);
        emit2(c, STORE_ATTR, owner, compiler_const(c, t->v.Attribute.attr));
        break;
    }
    case Subscript_kind: {
        Py_ssize_t container = expr_to_any_reg(c, t->v.Subscript.value);
        Py_ssize_t sub = PY_SSIZE_T_MAX; // FIXME: need slice to reg
        // Py_ssize_t sub = expr_to_any_reg(c, t->v.Subscript.slice);
        to_accumulator(c, reg);
        emit2(c, STORE_SUBSCR, container, sub);
        clear_reg(c, sub);
        clear_reg(c, container);
        break;
    }
    case List_kind:
        emit1(c, LOAD_FAST, reg);
        assignment_helper(c, t->v.List.elts);
        break;
    case Tuple_kind:
        emit1(c, LOAD_FAST, reg);
        assignment_helper(c, t->v.Tuple.elts);
        break;
    default:
        PyErr_Format(PyExc_SystemError, "unsupported assignment: %d", t->kind);
        COMPILER_ERROR(c);
    }
}

static void
compiler_assign_expr(struct compiler *c, expr_ty t, expr_ty value)
{
    switch (t->kind) {
    case Name_kind:
        assign_name(c, t->v.Name.id, expr_discharge(c, value));
        break;
    case Attribute_kind: {
        Py_ssize_t owner = expr_to_any_reg(c, t->v.Attribute.value);
        compiler_visit_expr(c, value);
        emit2(c, STORE_ATTR, owner, compiler_const(c, t->v.Attribute.attr));
        break;
    }
    case Subscript_kind: {
        Py_ssize_t container = expr_to_any_reg(c, t->v.Subscript.value);
        Py_ssize_t sub = PY_SSIZE_T_MAX; // FIXME: need slice to reg
        // Py_ssize_t sub = expr_to_any_reg(c, t->v.Subscript.slice);
        compiler_visit_expr(c, value);
        emit2(c, STORE_SUBSCR, container, sub);
        clear_reg(c, sub);
        clear_reg(c, container);
        break;
    }
    case List_kind:
        compiler_visit_expr(c, value);
        assignment_helper(c, t->v.List.elts);
        break;
    case Tuple_kind:
        compiler_visit_expr(c, value);
        assignment_helper(c, t->v.Tuple.elts);
        break;
    default:
        PyErr_Format(PyExc_SystemError, "unsupported assignment: %d", t->kind);
        COMPILER_ERROR(c);
    }
}

static void
compiler_assign(struct compiler *c, stmt_ty s)
{
    asdl_seq *targets = s->v.Assign.targets;
    Py_ssize_t n = asdl_seq_LEN(targets);

    if (n == 1) {
        expr_ty target = asdl_seq_GET(targets, 0);
        compiler_assign_expr(c, target, s->v.Assign.value);
        return;
    }

    Py_ssize_t value = expr_to_any_reg(c, s->v.Assign.value); 
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty target = asdl_seq_GET(targets, i);
        compiler_assign_reg(c, target, value);
    }
    clear_reg(c, value);
}

static void
compiler_delete_expr(struct compiler *c, expr_ty t)
{
    static int opcodes[] = {
        [ACCESS_FAST]   = DELETE_FAST,
        [ACCESS_DEREF]  = DELETE_DEREF,
        [ACCESS_NAME]   = DELETE_NAME,
        [ACCESS_GLOBAL] = DELETE_GLOBAL,
    };
    switch (t->kind) {
    case Name_kind: {
        struct var_info a = resolve(c, t->v.Name.id);
        assert(a.access != ACCESS_CLASSDEREF);
        emit1(c, opcodes[a.access], a.slot);
        break;
    }
    case Attribute_kind:
        compiler_visit_expr(c, t->v.Attribute.value);
        emit1(c, DELETE_ATTR, compiler_const(c, t->v.Attribute.attr));
        break;
    case Subscript_kind: {
        Py_ssize_t container = expr_to_any_reg(c, t->v.Subscript.value);
        compiler_slice(c, t->v.Subscript.slice);
        emit1(c, DELETE_SUBSCR, container);
        clear_reg(c, container);
        break;
    }
    default: Py_UNREACHABLE();
    }
}

static void
compiler_delete(struct compiler *c, stmt_ty s)
{
    asdl_seq *targets = s->v.Delete.targets;
    Py_ssize_t n = asdl_seq_LEN(targets);
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty target = asdl_seq_GET(targets, i);
        compiler_delete_expr(c, target);
    }
}

static void
compiler_assert(struct compiler *c, stmt_ty s)
{
    struct bc_label end;

    if (c->optimize) {
        return;
    }
    if (s->v.Assert.test->kind == Tuple_kind &&
        asdl_seq_LEN(s->v.Assert.test->v.Tuple.elts) > 0)
    {
        compiler_warn(c, "assertion is always true, "
                         "perhaps remove parentheses?");
    }

    compiler_visit_expr(c, s->v.Assert.test);
    emit_jump(c, POP_JUMP_IF_TRUE, &end);
    if (s->v.Assert.msg) {
        compiler_visit_expr(c, s->v.Assert.msg);
    }
    emit1(c, CALL_INTRINSIC_1, Intrinsic_vm_raise_assertion_error);
    emit_label(c, &end);
}

static void
compiler_visit_stmt_expr(struct compiler *c, expr_ty value)
{
    if (c->interactive && c->nestlevel <= 1) {
        // VISIT(c, expr, value);
        // ADDOP(c, PRINT_EXPR);
        return;
    }

    if (value->kind == Constant_kind) {
        /* ignore constant statement */
        return;
    }

    compiler_visit_expr(c, value);
    emit0(c, CLEAR_ACC);
}

static void
compiler_visit_stmt(struct compiler *c, stmt_ty s)
{
//     Py_ssize_t i, n;
    Py_ssize_t next_register = c->unit->next_register;

    /* Always assign a lineno to the next instruction for a stmt. */
    c->unit->lineno = s->lineno;
    c->unit->col_offset = s->col_offset;
    c->unit->lineno_set = 0;

    switch (s->kind) {
    case FunctionDef_kind:
        compiler_function(c, s, 0);
        break;
    case ClassDef_kind:
        compiler_class(c, s);
        break;
    case Return_kind:
        compiler_return(c, s);
        break;
    case Delete_kind:
        compiler_delete(c, s);
        break;
    case Assign_kind:
        compiler_assign(c, s);
        break;
    case AugAssign_kind:
        compiler_augassign(c, s);
        break;
    case AnnAssign_kind:
        compiler_annassign(c, s);
        break;
    case For_kind:
        compiler_for(c, s);
        break;
    case While_kind:
        compiler_while(c, s);
        break;
    case If_kind:
        compiler_if(c, s);
        break;
    case Raise_kind:
        compiler_raise(c, s);
        break;
//     case Try_kind:
//         return compiler_try(c, s);
    case Assert_kind:
        compiler_assert(c, s);
        break;
    case Import_kind:
        compiler_import(c, s);
        break;
    case ImportFrom_kind:
        compiler_from_import(c, s);
        break;
    case Global_kind:
    case Nonlocal_kind:
        break;
    case Expr_kind:
        return compiler_visit_stmt_expr(c, s->v.Expr.value);
    case Pass_kind:
        break;
    case Break_kind:
        compiler_break(c);
        break;
    case Continue_kind:
        compiler_continue(c);
        break;
//     case With_kind:
//         return compiler_with(c, s, 0);
//     case AsyncFunctionDef_kind:
//         return compiler_function(c, s, 1);
//     case AsyncWith_kind:
//         return compiler_async_with(c, s, 0);
//     case AsyncFor_kind:
//         return compiler_async_for(c, s);
        default:
            PyErr_Format(PyExc_RuntimeError, "unhandled stmt %d", s->kind);
            COMPILER_ERROR(c);
    }

    assert(next_register == c->unit->next_register);
}

static void
compiler_visit_stmts(struct compiler *c, asdl_seq *stmts)
{
    for (int i = 0, n = asdl_seq_LEN(stmts); i != n; i++) {
        stmt_ty elt = asdl_seq_GET(stmts, i);
        compiler_visit_stmt(c, elt);
    }
}

static int
unaryop(unaryop_ty op)
{
    switch (op) {
    case Invert:    return UNARY_INVERT;
    case Not:       return UNARY_NOT;
    case UAdd:      return UNARY_POSITIVE;
    case USub:      return UNARY_NEGATIVE;
    }
    Py_UNREACHABLE();
}

static int
binop(operator_ty op)
{
    switch (op) {
    case Add:       return BINARY_ADD;
    case Sub:       return BINARY_SUBTRACT;
    case Mult:      return BINARY_MULTIPLY;
    case MatMult:   return BINARY_MATRIX_MULTIPLY;
    case Div:       return BINARY_TRUE_DIVIDE;
    case Mod:       return BINARY_MODULO;
    case Pow:       return BINARY_POWER;
    case LShift:    return BINARY_LSHIFT;
    case RShift:    return BINARY_RSHIFT;
    case BitOr:     return BINARY_OR;
    case BitXor:    return BINARY_XOR;
    case BitAnd:    return BINARY_AND;
    case FloorDiv:  return BINARY_FLOOR_DIVIDE;
    }
    Py_UNREACHABLE();
}

static int
inplace_binop(struct compiler *c, operator_ty op)
{
    switch (op) {
    case Add:       return INPLACE_ADD;
    case Sub:       return INPLACE_SUBTRACT;
    case Mult:      return INPLACE_MULTIPLY;
    case MatMult:   return INPLACE_MATRIX_MULTIPLY;
    case Div:       return INPLACE_TRUE_DIVIDE;
    case Mod:       return INPLACE_MODULO;
    case Pow:       return INPLACE_POWER;
    case LShift:    return INPLACE_LSHIFT;
    case RShift:    return INPLACE_RSHIFT;
    case BitOr:     return INPLACE_OR;
    case BitXor:    return INPLACE_XOR;
    case BitAnd:    return INPLACE_AND;
    case FloorDiv:  return INPLACE_FLOOR_DIVIDE;
    default:
        PyErr_Format(PyExc_SystemError,
            "inplace binary op %d should not be possible", op);
        COMPILER_ERROR(c);
    }
}

static int
compiler_access(struct compiler *c, PyObject *mangled_name)
{
    int scope = PyST_GetScope(c->unit->ste, mangled_name);
    PySTEntryObject *ste = c->unit->ste;
    switch (scope) {
    case FREE:
    case CELL:
        return ste->ste_type == ClassBlock ? ACCESS_CLASSDEREF : ACCESS_DEREF;
    case LOCAL:
        return ste->ste_type == FunctionBlock ? ACCESS_FAST : ACCESS_NAME;
    case GLOBAL_IMPLICIT:
        return ste->ste_type == FunctionBlock ? ACCESS_GLOBAL : ACCESS_NAME;
    case GLOBAL_EXPLICIT:
        return ACCESS_GLOBAL;
    default:
        return ACCESS_NAME;
    }
}

static void
compiler_nameop(struct compiler *c, PyObject *name, expr_context_ty ctx)
{
    if (name == NULL) {
        COMPILER_ERROR(c);
    }
    // int op, scope;
    // Py_ssize_t arg;
    // enum { OP_FAST, OP_GLOBAL, OP_DEREF, OP_NAME } optype;

    PyObject *mangled;
    /* XXX AugStore isn't used anywhere! */

    assert(!_PyUnicode_EqualToASCIIString(name, "None") &&
           !_PyUnicode_EqualToASCIIString(name, "True") &&
           !_PyUnicode_EqualToASCIIString(name, "False"));

    mangled = mangle(c, name);
    int access = compiler_access(c, mangled);
    switch (access) {
    case ACCESS_FAST:
        emit1(c, LOAD_FAST, compiler_varname(c, mangled));
        break;
    case ACCESS_DEREF:
        emit1(c, LOAD_DEREF, compiler_varname(c, mangled));
        break;
    case ACCESS_CLASSDEREF:
        emit2(c,
              LOAD_CLASSDEREF,
              compiler_varname(c, mangled),
              compiler_const(c, mangled));
        break;
    case ACCESS_NAME:
        emit2(c, 
              LOAD_NAME,
              compiler_const(c, mangled),
              compiler_metaslot(c, mangled));
        break;
    case ACCESS_GLOBAL:
        emit2(c,
              LOAD_GLOBAL,
              compiler_const(c, mangled),
              compiler_metaslot(c, mangled));
        break;
    }
}

static void
load_name_id(struct compiler *c, _Py_Identifier *id)
{
    PyObject *name = _PyUnicode_FromId(id);
    if (name == NULL) {
        COMPILER_ERROR(c);
    }
    compiler_nameop(c, name, Load);
}

static void
compiler_boolop(struct compiler *c, expr_ty e)
{
    int jump_opcode;
    Py_ssize_t i, n;
    asdl_seq *s;

    if (e->v.BoolOp.op == And)
        jump_opcode = JUMP_IF_FALSE;
    else
        jump_opcode = JUMP_IF_TRUE;
    
    s = e->v.BoolOp.values;
    n = asdl_seq_LEN(s);

    struct multi_label labels;
    memset(&labels, 0, sizeof(labels));

    compiler_visit_expr(c, asdl_seq_GET(s, 0));
    for (i = 1; i < n; ++i) {
        emit_jump(c, jump_opcode, multi_label_next(c, &labels));
        emit0(c, CLEAR_ACC);
        compiler_visit_expr(c, asdl_seq_GET(s, i));
    }
    emit_multi_label(c, &labels);
}

static void
starunpack_helper(struct compiler *c, asdl_seq *elts, int kind)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    Py_ssize_t i, seen_star = 0;
//     if (n > 2 && are_all_items_const(elts, 0, n)) {
//         PyObject *folded = PyTuple_New(n);
//         if (folded == NULL) {
//             return 0;
//         }
//         PyObject *val;
//         for (i = 0; i < n; i++) {
//             val = ((expr_ty)asdl_seq_GET(elts, i))->v.Constant.value;
//             Py_INCREF(val);
//             PyTuple_SET_ITEM(folded, i, val);
//         }
//         if (tuple) {
//             ADDOP_LOAD_CONST_NEW(c, folded);
//         } else {
//             if (add == SET_ADD) {
//                 Py_SETREF(folded, PyFrozenSet_New(folded));
//                 if (folded == NULL) {
//                     return 0;
//                 }
//             }
//             ADDOP_I(c, build, pushed);
//             ADDOP_LOAD_CONST_NEW(c, folded);
//             ADDOP_I(c, extend, 1);
//         }
//         return 1;
//     }
    Py_ssize_t base = c->unit->next_register;
    for (i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind) {
            if (seen_star == 0) {
                emit2(c, Set_kind ? BUILD_SET : BUILD_LIST, base, i);
                emit1(c, STORE_FAST, base);
                c->unit->next_register = base + 1;
                seen_star = 1;
            }
            compiler_visit_expr(c, elt->v.Starred.value);
            emit1(c, Set_kind ? SET_UPDATE : LIST_EXTEND, base);
        }
        else if (seen_star) {
            compiler_visit_expr(c, elt);
            emit1(c, Set_kind ? SET_ADD : LIST_APPEND, base);
        }
        else {
            expr_to_reg(c, elt, base + i);
        }
    }
    if (!seen_star) {
        int opcode = (kind == Set_kind ? BUILD_SET :
                      kind == List_kind ? BUILD_LIST :
                      /*kind == Tuple_kind*/ BUILD_TUPLE);
        emit2(c, opcode, base, n);
        c->unit->next_register = base;
    }
    else {
        emit1(c, LOAD_FAST, base);
        emit1(c, CLEAR_FAST, base);
        free_reg(c, base);
        if (kind == Tuple_kind) {
            // FIXME
        }
    }
//     return 1;
}

static void
compiler_list(struct compiler *c, expr_ty e)
{
    assert(e->v.List.ctx == Load);
    starunpack_helper(c, e->v.List.elts, e->kind);
    // asdl_seq *elts = e->v.List.elts;
    // if (e->v.List.ctx == Store) {
    //     return assignment_helper(c, elts);
    // }
    // else if (e->v.List.ctx == Load) {
    //     return starunpack_helper(c, elts, 0, BUILD_LIST,
    //                              LIST_APPEND, LIST_EXTEND, 0);
    // }
    // else
    //     VISIT_SEQ(c, expr, elts);
    // return 1;
}

// static int
// compiler_tuple(struct compiler *c, expr_ty e)
// {
//     asdl_seq *elts = e->v.Tuple.elts;
//     if (e->v.Tuple.ctx == Store) {
//         return assignment_helper(c, elts);
//     }
//     else if (e->v.Tuple.ctx == Load) {
//         return starunpack_helper(c, elts, 0, BUILD_LIST,
//                                  LIST_APPEND, LIST_EXTEND, 1);
//     }
//     else
//         VISIT_SEQ(c, expr, elts);
//     return 1;
// }

// static int
// compiler_set(struct compiler *c, expr_ty e)
// {
//     return starunpack_helper(c, e->v.Set.elts, 0, BUILD_SET,
//                              SET_ADD, SET_UPDATE, 0);
// }

static int
are_all_items_const(asdl_seq *seq, Py_ssize_t begin, Py_ssize_t end)
{
    Py_ssize_t i;
    for (i = begin; i < end; i++) {
        expr_ty key = (expr_ty)asdl_seq_GET(seq, i);
        if (key == NULL || key->kind != Constant_kind)
            return 0;
    }
    return 1;
}

// static int
// compiler_subdict(struct compiler *c, expr_ty e, Py_ssize_t begin, Py_ssize_t end)
// {
//     Py_ssize_t i, n = end - begin;
//     PyObject *keys, *key;
//     if (n > 1 && are_all_items_const(e->v.Dict.keys, begin, end)) {
//         for (i = begin; i < end; i++) {
//             VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
//         }
//         keys = PyTuple_New(n);
//         if (keys == NULL) {
//             return 0;
//         }
//         for (i = begin; i < end; i++) {
//             key = ((expr_ty)asdl_seq_GET(e->v.Dict.keys, i))->v.Constant.value;
//             Py_INCREF(key);
//             PyTuple_SET_ITEM(keys, i - begin, key);
//         }
//         ADDOP_LOAD_CONST_NEW(c, keys);
//         ADDOP_I(c, BUILD_CONST_KEY_MAP, n);
//     }
//     else {
//         for (i = begin; i < end; i++) {
//             VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.keys, i));
//             VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
//         }
//         ADDOP_I(c, BUILD_MAP, n);
//     }
//     return 1;
// }

// static int
// compiler_dict(struct compiler *c, expr_ty e)
// {
//     Py_ssize_t i, n, elements;
//     int have_dict;
//     int is_unpacking = 0;
//     n = asdl_seq_LEN(e->v.Dict.values);
//     have_dict = 0;
//     elements = 0;
//     for (i = 0; i < n; i++) {
//         is_unpacking = (expr_ty)asdl_seq_GET(e->v.Dict.keys, i) == NULL;
//         if (is_unpacking) {
//             if (elements) {
//                 if (!compiler_subdict(c, e, i - elements, i)) {
//                     return 0;
//                 }
//                 if (have_dict) {
//                     ADDOP_I(c, DICT_UPDATE, 1);
//                 }
//                 have_dict = 1;
//                 elements = 0;
//             }
//             if (have_dict == 0) {
//                 ADDOP_I(c, BUILD_MAP, 0);
//                 have_dict = 1;
//             }
//             VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
//             ADDOP_I(c, DICT_UPDATE, 1);
//         }
//         else {
//             if (elements == 0xFFFF) {
//                 if (!compiler_subdict(c, e, i - elements, i)) {
//                     return 0;
//                 }
//                 if (have_dict) {
//                     ADDOP_I(c, DICT_UPDATE, 1);
//                 }
//                 have_dict = 1;
//                 elements = 0;
//             }
//             else {
//                 elements++;
//             }
//         }
//     }
//     if (elements) {
//         if (!compiler_subdict(c, e, n - elements, n)) {
//             return 0;
//         }
//         if (have_dict) {
//             ADDOP_I(c, DICT_UPDATE, 1);
//         }
//         have_dict = 1;
//     }
//     if (!have_dict) {
//         ADDOP_I(c, BUILD_MAP, 0);
//     }
//     return 1;
// }

static Py_ssize_t
shuffle_down(struct compiler *c, Py_ssize_t lhs, Py_ssize_t rhs)
{
    if (is_local(c, lhs)) {
        return rhs;
    }
    else if (is_local(c, rhs)) {
        emit1(c, CLEAR_FAST, lhs);
        free_reg(c, lhs);
        return rhs;
    }
    else {
        emit2(c, MOVE, lhs, rhs);
        free_reg(c, rhs);
        return lhs;
    }
}

static void
compiler_compare(struct compiler *c, expr_ty e)
{
    struct multi_label labels;
    Py_ssize_t n, lhs, rhs = -1;

    // warn for things like "x is 4"
    check_compare(c, e);

    memset(&labels, 0, sizeof(labels));

    assert(asdl_seq_LEN(e->v.Compare.ops) > 0);
    lhs = expr_to_any_reg(c, e->v.Compare.left);    

    n = asdl_seq_LEN(e->v.Compare.ops);
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty comparator = asdl_seq_GET(e->v.Compare.comparators, i);
        cmpop_ty op = asdl_seq_GET(e->v.Compare.ops, i);

        if (i > 0) {
            // After the first comparison, the previous right-hand-side of the
            // comparison is the new left-hand-side. We perform this "shuffle"
            // without re-evaluating the expression.
            lhs = shuffle_down(c, lhs, rhs);
            rhs = -1;

            emit_jump(c, JUMP_IF_FALSE, multi_label_next(c, &labels));
        }

        // Load the right-hand-side of the comparison into the accumulator.
        // If this is not the final comparison, also ensure that it's saved
        // in a register.
        if (i < n - 1) {
            rhs = expr_to_any_reg(c, comparator);
            emit1(c, LOAD_FAST, rhs);
        }
        else {
            compiler_visit_expr(c, comparator);
        }

        // emit: <reg> OP <acc>
        assert(lhs >= 0);
        emit_compare(c, lhs, op);
    }

    emit_multi_label(c, &labels);
    clear_reg(c, lhs);


//     VISIT(c, expr, e->v.Compare.left);
//     if (n == 0) {
//         VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, 0));
//         ADDOP_COMPARE(c, asdl_seq_GET(e->v.Compare.ops, 0));
//     }
//     else {
//         basicblock *cleanup = compiler_new_block(c);
//         if (cleanup == NULL)
//             return 0;
//         for (i = 0; i < n; i++) {
//             VISIT(c, expr,
//                 (expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
//             ADDOP(c, DUP_TOP);
//             ADDOP(c, ROT_THREE);
//             ADDOP_COMPARE(c, asdl_seq_GET(e->v.Compare.ops, i));
//             ADDOP_JABS(c, JUMP_IF_FALSE_OR_POP, cleanup);
//             NEXT_BLOCK(c);
//         }
//         VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, n));
//         ADDOP_COMPARE(c, asdl_seq_GET(e->v.Compare.ops, n));
//         basicblock *end = compiler_new_block(c);
//         if (end == NULL)
//             return 0;
//         ADDOP_JREL(c, JUMP_FORWARD, end);
//         compiler_use_next_block(c, cleanup);
//         ADDOP(c, ROT_TWO);
//         ADDOP(c, POP_TOP);
//         compiler_use_next_block(c, end);
//     }
//     return 1;
}

// static PyTypeObject *
// infer_type(expr_ty e)
// {
//     switch (e->kind) {
//     case Tuple_kind:
//         return &PyTuple_Type;
//     case List_kind:
//     case ListComp_kind:
//         return &PyList_Type;
//     case Dict_kind:
//     case DictComp_kind:
//         return &PyDict_Type;
//     case Set_kind:
//     case SetComp_kind:
//         return &PySet_Type;
//     case GeneratorExp_kind:
//         return &PyGen_Type;
//     case Lambda_kind:
//         return &PyFunction_Type;
//     case JoinedStr_kind:
//     case FormattedValue_kind:
//         return &PyUnicode_Type;
//     case Constant_kind:
//         return Py_TYPE(e->v.Constant.value);
//     default:
//         return NULL;
//     }
// }

// static int
// check_caller(struct compiler *c, expr_ty e)
// {
//     switch (e->kind) {
//     case Constant_kind:
//     case Tuple_kind:
//     case List_kind:
//     case ListComp_kind:
//     case Dict_kind:
//     case DictComp_kind:
//     case Set_kind:
//     case SetComp_kind:
//     case GeneratorExp_kind:
//     case JoinedStr_kind:
//     case FormattedValue_kind:
//         return compiler_warn(c, "'%.200s' object is not callable; "
//                                 "perhaps you missed a comma?",
//                                 infer_type(e)->tp_name);
//     default:
//         return 1;
//     }
// }

// static int
// check_subscripter(struct compiler *c, expr_ty e)
// {
//     PyObject *v;

//     switch (e->kind) {
//     case Constant_kind:
//         v = e->v.Constant.value;
//         if (!(v == Py_None || v == Py_Ellipsis ||
//               PyLong_Check(v) || PyFloat_Check(v) || PyComplex_Check(v) ||
//               PyAnySet_Check(v)))
//         {
//             return 1;
//         }
//         /* fall through */
//     case Set_kind:
//     case SetComp_kind:
//     case GeneratorExp_kind:
//     case Lambda_kind:
//         return compiler_warn(c, "'%.200s' object is not subscriptable; "
//                                 "perhaps you missed a comma?",
//                                 infer_type(e)->tp_name);
//     default:
//         return 1;
//     }
// }

// static int
// check_index(struct compiler *c, expr_ty e, slice_ty s)
// {
//     PyObject *v;

//     if (s->kind != Index_kind) {
//         return 1;
//     }
//     PyTypeObject *index_type = infer_type(s->v.Index.value);
//     if (index_type == NULL
//         || PyType_FastSubclass(index_type, Py_TPFLAGS_LONG_SUBCLASS)
//         || index_type == &PySlice_Type) {
//         return 1;
//     }

//     switch (e->kind) {
//     case Constant_kind:
//         v = e->v.Constant.value;
//         if (!(PyUnicode_Check(v) || PyBytes_Check(v) || PyTuple_Check(v))) {
//             return 1;
//         }
//         /* fall through */
//     case Tuple_kind:
//     case List_kind:
//     case ListComp_kind:
//     case JoinedStr_kind:
//     case FormattedValue_kind:
//         return compiler_warn(c, "%.200s indices must be integers or slices, "
//                                 "not %.200s; "
//                                 "perhaps you missed a comma?",
//                                 infer_type(e)->tp_name,
//                                 index_type->tp_name);
//     default:
//         return 1;
//     }
// }

// // Return 1 if the method call was optimized, -1 if not, and 0 on error.
// static int
// maybe_optimize_method_call(struct compiler *c, expr_ty e)
// {
//     Py_ssize_t argsl, i;
//     expr_ty meth = e->v.Call.func;
//     asdl_seq *args = e->v.Call.args;

//     /* Check that the call node is an attribute access, and that
//        the call doesn't have keyword parameters. */
//     if (meth->kind != Attribute_kind || meth->v.Attribute.ctx != Load ||
//             asdl_seq_LEN(e->v.Call.keywords))
//         return -1;

//     /* Check that there are no *varargs types of arguments. */
//     argsl = asdl_seq_LEN(args);
//     for (i = 0; i < argsl; i++) {
//         expr_ty elt = asdl_seq_GET(args, i);
//         if (elt->kind == Starred_kind) {
//             return -1;
//         }
//     }

//     /* Alright, we can optimize the code. */
//     VISIT(c, expr, meth->v.Attribute.value);
//     ADDOP_NAME(c, LOAD_METHOD, meth->v.Attribute.attr, names);
//     VISIT_SEQ(c, expr, e->v.Call.args);
//     ADDOP_I(c, CALL_METHOD, asdl_seq_LEN(e->v.Call.args));
//     return 1;
// }

static bool
has_starred(asdl_seq *seq)
{
    for (Py_ssize_t  i = 0, n = asdl_seq_LEN(seq); i < n; i++) {
        expr_ty elt = asdl_seq_GET(seq, i);
        if (elt->kind == Starred_kind) {
            return true;
        }
    }
    return false; 
}

static bool
has_varkeywords(asdl_seq *keywords)
{
    for (Py_ssize_t  i = 0, n = asdl_seq_LEN(keywords); i < n; i++) {
        keyword_ty kw = asdl_seq_GET(keywords, i);
        if (kw->arg == NULL) {
            return true;
        }
    }
    return false; 
}

static void
compiler_call(struct compiler *c, expr_ty e)
{
    // if (maybe_optimize_method_call(c, e)) {
    //     return;
    // }

    expr_ty func = e->v.Call.func;
    // if (!check_caller(c, func)) {
    //     return;
    // }

    asdl_seq *args = e->v.Call.args;
    asdl_seq *keywords = e->v.Call.keywords;
    Py_ssize_t nargs = asdl_seq_LEN(args);
    Py_ssize_t nkwds = asdl_seq_LEN(keywords);
    if (nargs > 255 || nkwds > 255 ||
        has_starred(args) ||
        has_varkeywords(keywords))
    {
            // oops
        PyErr_Format(PyExc_RuntimeError, "unsupported call");
        COMPILER_ERROR(c);
    }

    int flags = nargs | (nkwds << 8);
    Py_ssize_t base = c->unit->next_register + FRAME_EXTRA;
    expr_to_reg(c, func, base - 1);
    for (Py_ssize_t i = 0; i < nargs; i++) {
        expr_ty elt = asdl_seq_GET(args, i);
        assert(elt->kind != Starred_kind);
        expr_to_reg(c, elt, base + i);
    }
    emit_call(c, CALL_FUNCTION, base, flags);
    c->unit->next_register = base - FRAME_EXTRA;
}

// static int
// compiler_joined_str(struct compiler *c, expr_ty e)
// {
//     VISIT_SEQ(c, expr, e->v.JoinedStr.values);
//     if (asdl_seq_LEN(e->v.JoinedStr.values) != 1)
//         ADDOP_I(c, BUILD_STRING, asdl_seq_LEN(e->v.JoinedStr.values));
//     return 1;
// }

// /* Used to implement f-strings. Format a single value. */
// static int
// compiler_formatted_value(struct compiler *c, expr_ty e)
// {
//     /* Our oparg encodes 2 pieces of information: the conversion
//        character, and whether or not a format_spec was provided.

//        Convert the conversion char to 3 bits:
//            : 000  0x0  FVC_NONE   The default if nothing specified.
//        !s  : 001  0x1  FVC_STR
//        !r  : 010  0x2  FVC_REPR
//        !a  : 011  0x3  FVC_ASCII

//        next bit is whether or not we have a format spec:
//        yes : 100  0x4
//        no  : 000  0x0
//     */

//     int conversion = e->v.FormattedValue.conversion;
//     int oparg;

//     /* The expression to be formatted. */
//     VISIT(c, expr, e->v.FormattedValue.value);

//     switch (conversion) {
//     case 's': oparg = FVC_STR;   break;
//     case 'r': oparg = FVC_REPR;  break;
//     case 'a': oparg = FVC_ASCII; break;
//     case -1:  oparg = FVC_NONE;  break;
//     default:
//         PyErr_Format(PyExc_SystemError,
//                      "Unrecognized conversion character %d", conversion);
//         return 0;
//     }
//     if (e->v.FormattedValue.format_spec) {
//         /* Evaluate the format spec, and update our opcode arg. */
//         VISIT(c, expr, e->v.FormattedValue.format_spec);
//         oparg |= FVS_HAVE_SPEC;
//     }

//     /* And push our opcode and oparg */
//     ADDOP_I(c, FORMAT_VALUE, oparg);

//     return 1;
// }

// static int
// compiler_subkwargs(struct compiler *c, asdl_seq *keywords, Py_ssize_t begin, Py_ssize_t end)
// {
//     Py_ssize_t i, n = end - begin;
//     keyword_ty kw;
//     PyObject *keys, *key;
//     assert(n > 0);
//     if (n > 1) {
//         for (i = begin; i < end; i++) {
//             kw = asdl_seq_GET(keywords, i);
//             VISIT(c, expr, kw->value);
//         }
//         keys = PyTuple_New(n);
//         if (keys == NULL) {
//             return 0;
//         }
//         for (i = begin; i < end; i++) {
//             key = ((keyword_ty) asdl_seq_GET(keywords, i))->arg;
//             Py_INCREF(key);
//             PyTuple_SET_ITEM(keys, i - begin, key);
//         }
//         ADDOP_LOAD_CONST_NEW(c, keys);
//         ADDOP_I(c, BUILD_CONST_KEY_MAP, n);
//     }
//     else {
//         /* a for loop only executes once */
//         for (i = begin; i < end; i++) {
//             kw = asdl_seq_GET(keywords, i);
//             ADDOP_LOAD_CONST(c, kw->arg);
//             VISIT(c, expr, kw->value);
//         }
//         ADDOP_I(c, BUILD_MAP, n);
//     }
//     return 1;
// }

// /* shared code between compiler_call and compiler_class */
// static int
// compiler_call_helper(struct compiler *c,
//                      int n, /* Args already pushed */
//                      asdl_seq *args,
//                      asdl_seq *keywords)
// {
//     Py_ssize_t i, nseen, nelts, nkwelts;

//     nelts = asdl_seq_LEN(args);
//     nkwelts = asdl_seq_LEN(keywords);

//     for (i = 0; i < nelts; i++) {
//         expr_ty elt = asdl_seq_GET(args, i);
//         if (elt->kind == Starred_kind) {
//             goto ex_call;
//         }
//     }
//     for (i = 0; i < nkwelts; i++) {
//         keyword_ty kw = asdl_seq_GET(keywords, i);
//         if (kw->arg == NULL) {
//             goto ex_call;
//         }
//     }

//     /* No * or ** args, so can use faster calling sequence */
//     for (i = 0; i < nelts; i++) {
//         expr_ty elt = asdl_seq_GET(args, i);
//         assert(elt->kind != Starred_kind);
//         VISIT(c, expr, elt);
//     }
//     if (nkwelts) {
//         PyObject *names;
//         VISIT_SEQ(c, keyword, keywords);
//         names = PyTuple_New(nkwelts);
//         if (names == NULL) {
//             return 0;
//         }
//         for (i = 0; i < nkwelts; i++) {
//             keyword_ty kw = asdl_seq_GET(keywords, i);
//             Py_INCREF(kw->arg);
//             PyTuple_SET_ITEM(names, i, kw->arg);
//         }
//         ADDOP_LOAD_CONST_NEW(c, names);
//         ADDOP_I(c, CALL_FUNCTION_KW, n + nelts + nkwelts);
//         return 1;
//     }
//     else {
//         ADDOP_I(c, CALL_FUNCTION, n + nelts);
//         return 1;
//     }

// ex_call:

//     /* Do positional arguments. */
//     if (n ==0 && nelts == 1 && ((expr_ty)asdl_seq_GET(args, 0))->kind == Starred_kind) {
//         VISIT(c, expr, ((expr_ty)asdl_seq_GET(args, 0))->v.Starred.value);
//     }
//     else if (starunpack_helper(c, args, n, BUILD_LIST,
//                                  LIST_APPEND, LIST_EXTEND, 1) == 0) {
//         return 0;
//     }
//     /* Then keyword arguments */
//     if (nkwelts) {
//         /* Has a new dict been pushed */
//         int have_dict = 0;

//         nseen = 0;  /* the number of keyword arguments on the stack following */
//         for (i = 0; i < nkwelts; i++) {
//             keyword_ty kw = asdl_seq_GET(keywords, i);
//             if (kw->arg == NULL) {
//                 /* A keyword argument unpacking. */
//                 if (nseen) {
//                     if (!compiler_subkwargs(c, keywords, i - nseen, i)) {
//                         return 0;
//                     }
//                     have_dict = 1;
//                     nseen = 0;
//                 }
//                 if (!have_dict) {
//                     ADDOP_I(c, BUILD_MAP, 0);
//                     have_dict = 1;
//                 }
//                 VISIT(c, expr, kw->value);
//                 ADDOP_I(c, DICT_MERGE, 1);
//             }
//             else {
//                 nseen++;
//             }
//         }
//         if (nseen) {
//             /* Pack up any trailing keyword arguments. */
//             if (!compiler_subkwargs(c, keywords, nkwelts - nseen, nkwelts)) {
//                 return 0;
//             }
//             if (have_dict) {
//                 ADDOP_I(c, DICT_MERGE, 1);
//             }
//             have_dict = 1;
//         }
//         assert(have_dict);
//     }
//     ADDOP_I(c, CALL_FUNCTION_EX, nkwelts > 0);
//     return 1;
// }


// /* List and set comprehensions and generator expressions work by creating a
//   nested function to perform the actual iteration. This means that the
//   iteration variables don't leak into the current scope.
//   The defined function is called immediately following its definition, with the
//   result of that call being the result of the expression.
//   The LC/SC version returns the populated container, while the GE version is
//   flagged in symtable.c as a generator, so it returns the generator object
//   when the function is called.

//   Possible cleanups:
//     - iterate over the generator sequence instead of using recursion
// */


// static int
// compiler_comprehension_generator(struct compiler *c,
//                                  asdl_seq *generators, int gen_index,
//                                  expr_ty elt, expr_ty val, int type)
// {
//     comprehension_ty gen;
//     gen = (comprehension_ty)asdl_seq_GET(generators, gen_index);
//     if (gen->is_async) {
//         return compiler_async_comprehension_generator(
//             c, generators, gen_index, elt, val, type);
//     } else {
//         return compiler_sync_comprehension_generator(
//             c, generators, gen_index, elt, val, type);
//     }
// }

// static int
// compiler_sync_comprehension_generator(struct compiler *c,
//                                       asdl_seq *generators, int gen_index,
//                                       expr_ty elt, expr_ty val, int type)
// {
//     /* generate code for the iterator, then each of the ifs,
//        and then write to the element */

//     comprehension_ty gen;
//     basicblock *start, *anchor, *skip, *if_cleanup;
//     Py_ssize_t i, n;

//     start = compiler_new_block(c);
//     skip = compiler_new_block(c);
//     if_cleanup = compiler_new_block(c);
//     anchor = compiler_new_block(c);

//     if (start == NULL || skip == NULL || if_cleanup == NULL ||
//         anchor == NULL)
//         return 0;

//     gen = (comprehension_ty)asdl_seq_GET(generators, gen_index);

//     if (gen_index == 0) {
//         /* Receive outermost iter as an implicit argument */
//         c->u->u_argcount = 1;
//         ADDOP_I(c, LOAD_FAST, 0);
//     }
//     else {
//         /* Sub-iter - calculate on the fly */
//         VISIT(c, expr, gen->iter);
//         ADDOP(c, GET_ITER);
//     }
//     compiler_use_next_block(c, start);
//     ADDOP_JREL(c, FOR_ITER, anchor);
//     NEXT_BLOCK(c);
//     VISIT(c, expr, gen->target);

//     /* XXX this needs to be cleaned up...a lot! */
//     n = asdl_seq_LEN(gen->ifs);
//     for (i = 0; i < n; i++) {
//         expr_ty e = (expr_ty)asdl_seq_GET(gen->ifs, i);
//         if (!compiler_jump_if(c, e, if_cleanup, 0))
//             return 0;
//         NEXT_BLOCK(c);
//     }

//     if (++gen_index < asdl_seq_LEN(generators))
//         if (!compiler_comprehension_generator(c,
//                                               generators, gen_index,
//                                               elt, val, type))
//         return 0;

//     /* only append after the last for generator */
//     if (gen_index >= asdl_seq_LEN(generators)) {
//         /* comprehension specific code */
//         switch (type) {
//         case COMP_GENEXP:
//             VISIT(c, expr, elt);
//             ADDOP(c, YIELD_VALUE);
//             ADDOP(c, POP_TOP);
//             break;
//         case COMP_LISTCOMP:
//             VISIT(c, expr, elt);
//             ADDOP_I(c, LIST_APPEND, gen_index + 1);
//             break;
//         case COMP_SETCOMP:
//             VISIT(c, expr, elt);
//             ADDOP_I(c, SET_ADD, gen_index + 1);
//             break;
//         case COMP_DICTCOMP:
//             /* With '{k: v}', k is evaluated before v, so we do
//                the same. */
//             VISIT(c, expr, elt);
//             VISIT(c, expr, val);
//             ADDOP_I(c, MAP_ADD, gen_index + 1);
//             break;
//         default:
//             return 0;
//         }

//         compiler_use_next_block(c, skip);
//     }
//     compiler_use_next_block(c, if_cleanup);
//     ADDOP_JABS(c, JUMP_ABSOLUTE, start);
//     compiler_use_next_block(c, anchor);

//     return 1;
// }

// static int
// compiler_async_comprehension_generator(struct compiler *c,
//                                       asdl_seq *generators, int gen_index,
//                                       expr_ty elt, expr_ty val, int type)
// {
//     comprehension_ty gen;
//     basicblock *start, *if_cleanup, *except;
//     Py_ssize_t i, n;
//     start = compiler_new_block(c);
//     except = compiler_new_block(c);
//     if_cleanup = compiler_new_block(c);

//     if (start == NULL || if_cleanup == NULL || except == NULL) {
//         return 0;
//     }

//     gen = (comprehension_ty)asdl_seq_GET(generators, gen_index);

//     if (gen_index == 0) {
//         /* Receive outermost iter as an implicit argument */
//         c->u->u_argcount = 1;
//         ADDOP_I(c, LOAD_FAST, 0);
//     }
//     else {
//         /* Sub-iter - calculate on the fly */
//         VISIT(c, expr, gen->iter);
//         ADDOP(c, GET_AITER);
//     }

//     compiler_use_next_block(c, start);

//     ADDOP_JREL(c, SETUP_FINALLY, except);
//     ADDOP(c, GET_ANEXT);
//     ADDOP_LOAD_CONST(c, Py_None);
//     ADDOP(c, YIELD_FROM);
//     ADDOP(c, POP_BLOCK);
//     VISIT(c, expr, gen->target);

//     n = asdl_seq_LEN(gen->ifs);
//     for (i = 0; i < n; i++) {
//         expr_ty e = (expr_ty)asdl_seq_GET(gen->ifs, i);
//         if (!compiler_jump_if(c, e, if_cleanup, 0))
//             return 0;
//         NEXT_BLOCK(c);
//     }

//     if (++gen_index < asdl_seq_LEN(generators))
//         if (!compiler_comprehension_generator(c,
//                                               generators, gen_index,
//                                               elt, val, type))
//         return 0;

//     /* only append after the last for generator */
//     if (gen_index >= asdl_seq_LEN(generators)) {
//         /* comprehension specific code */
//         switch (type) {
//         case COMP_GENEXP:
//             VISIT(c, expr, elt);
//             ADDOP(c, YIELD_VALUE);
//             ADDOP(c, POP_TOP);
//             break;
//         case COMP_LISTCOMP:
//             VISIT(c, expr, elt);
//             ADDOP_I(c, LIST_APPEND, gen_index + 1);
//             break;
//         case COMP_SETCOMP:
//             VISIT(c, expr, elt);
//             ADDOP_I(c, SET_ADD, gen_index + 1);
//             break;
//         case COMP_DICTCOMP:
//             /* With '{k: v}', k is evaluated before v, so we do
//                the same. */
//             VISIT(c, expr, elt);
//             VISIT(c, expr, val);
//             ADDOP_I(c, MAP_ADD, gen_index + 1);
//             break;
//         default:
//             return 0;
//         }
//     }
//     compiler_use_next_block(c, if_cleanup);
//     ADDOP_JABS(c, JUMP_ABSOLUTE, start);

//     compiler_use_next_block(c, except);
//     ADDOP(c, END_ASYNC_FOR);

//     return 1;
// }

// static int
// compiler_comprehension(struct compiler *c, expr_ty e, int type,
//                        identifier name, asdl_seq *generators, expr_ty elt,
//                        expr_ty val)
// {
//     PyCodeObject *co = NULL;
//     comprehension_ty outermost;
//     PyObject *qualname = NULL;
//     int is_async_function = c->u->u_ste->ste_coroutine;
//     int is_async_generator = 0;

//     outermost = (comprehension_ty) asdl_seq_GET(generators, 0);

//     if (!compiler_enter_scope(c, name, COMPILER_SCOPE_COMPREHENSION,
//                               (void *)e, e->lineno))
//     {
//         goto error;
//     }

//     is_async_generator = c->u->u_ste->ste_coroutine;

//     if (is_async_generator && !is_async_function && type != COMP_GENEXP) {
//         compiler_error(c, "asynchronous comprehension outside of "
//                           "an asynchronous function");
//         goto error_in_scope;
//     }

//     if (type != COMP_GENEXP) {
//         int op;
//         switch (type) {
//         case COMP_LISTCOMP:
//             op = BUILD_LIST;
//             break;
//         case COMP_SETCOMP:
//             op = BUILD_SET;
//             break;
//         case COMP_DICTCOMP:
//             op = BUILD_MAP;
//             break;
//         default:
//             PyErr_Format(PyExc_SystemError,
//                          "unknown comprehension type %d", type);
//             goto error_in_scope;
//         }

//         ADDOP_I(c, op, 0);
//     }

//     if (!compiler_comprehension_generator(c, generators, 0, elt,
//                                           val, type))
//         goto error_in_scope;

//     if (type != COMP_GENEXP) {
//         ADDOP(c, RETURN_VALUE);
//     }

//     co = assemble(c, 1);
//     qualname = c->u->u_qualname;
//     Py_INCREF(qualname);
//     compiler_exit_scope(c);
//     if (co == NULL)
//         goto error;

//     if (!compiler_make_closure(c, co, 0, qualname))
//         goto error;

//     ADDOP(c, DEFER_REFCOUNT);
//     Py_DECREF(qualname);
//     Py_DECREF(co);

//     VISIT(c, expr, outermost->iter);

//     if (outermost->is_async) {
//         ADDOP(c, GET_AITER);
//     } else {
//         ADDOP(c, GET_ITER);
//     }

//     ADDOP_I(c, CALL_FUNCTION, 1);

//     if (is_async_generator && type != COMP_GENEXP) {
//         ADDOP(c, GET_AWAITABLE);
//         ADDOP_LOAD_CONST(c, Py_None);
//         ADDOP(c, YIELD_FROM);
//     }

//     return 1;
// error_in_scope:
//     compiler_exit_scope(c);
// error:
//     Py_XDECREF(qualname);
//     Py_XDECREF(co);
//     return 0;
// }

// static int
// compiler_genexp(struct compiler *c, expr_ty e)
// {
//     static identifier name;
//     if (!name) {
//         name = PyUnicode_InternFromString("<genexpr>");
//         if (!name)
//             return 0;
//     }
//     assert(e->kind == GeneratorExp_kind);
//     return compiler_comprehension(c, e, COMP_GENEXP, name,
//                                   e->v.GeneratorExp.generators,
//                                   e->v.GeneratorExp.elt, NULL);
// }

// static int
// compiler_listcomp(struct compiler *c, expr_ty e)
// {
//     static identifier name;
//     if (!name) {
//         name = PyUnicode_InternFromString("<listcomp>");
//         if (!name)
//             return 0;
//     }
//     assert(e->kind == ListComp_kind);
//     return compiler_comprehension(c, e, COMP_LISTCOMP, name,
//                                   e->v.ListComp.generators,
//                                   e->v.ListComp.elt, NULL);
// }

// static int
// compiler_setcomp(struct compiler *c, expr_ty e)
// {
//     static identifier name;
//     if (!name) {
//         name = PyUnicode_InternFromString("<setcomp>");
//         if (!name)
//             return 0;
//     }
//     assert(e->kind == SetComp_kind);
//     return compiler_comprehension(c, e, COMP_SETCOMP, name,
//                                   e->v.SetComp.generators,
//                                   e->v.SetComp.elt, NULL);
// }


// static int
// compiler_dictcomp(struct compiler *c, expr_ty e)
// {
//     static identifier name;
//     if (!name) {
//         name = PyUnicode_InternFromString("<dictcomp>");
//         if (!name)
//             return 0;
//     }
//     assert(e->kind == DictComp_kind);
//     return compiler_comprehension(c, e, COMP_DICTCOMP, name,
//                                   e->v.DictComp.generators,
//                                   e->v.DictComp.key, e->v.DictComp.value);
// }


// static int
// compiler_visit_keyword(struct compiler *c, keyword_ty k)
// {
//     VISIT(c, expr, k->value);
//     return 1;
// }

/* Test whether expression is constant.  For constants, report
   whether they are true or false.

   Return values: 1 for true, 0 for false, -1 for non-constant.
 */

static int
expr_constant(expr_ty e)
{
    if (e->kind == Constant_kind) {
        return PyObject_IsTrue(e->v.Constant.value);
    }
    return -1;
}

// static int
// compiler_with_except_finish(struct compiler *c) {
//     basicblock *exit;
//     exit = compiler_new_block(c);
//     if (exit == NULL)
//         return 0;
//     ADDOP_JABS(c, POP_JUMP_IF_TRUE, exit);
//     ADDOP(c, RERAISE);
//     compiler_use_next_block(c, exit);
//     ADDOP(c, POP_TOP);
//     ADDOP(c, POP_TOP);
//     ADDOP(c, POP_TOP);
//     ADDOP(c, POP_EXCEPT);
//     ADDOP(c, POP_TOP);
//     return 1;
// }

// /*
//    Implements the async with statement.

//    The semantics outlined in that PEP are as follows:

//    async with EXPR as VAR:
//        BLOCK

//    It is implemented roughly as:

//    context = EXPR
//    exit = context.__aexit__  # not calling it
//    value = await context.__aenter__()
//    try:
//        VAR = value  # if VAR present in the syntax
//        BLOCK
//    finally:
//        if an exception was raised:
//            exc = copy of (exception, instance, traceback)
//        else:
//            exc = (None, None, None)
//        if not (await exit(*exc)):
//            raise
//  */
// static int
// compiler_async_with(struct compiler *c, stmt_ty s, int pos)
// {
//     basicblock *block, *final, *exit;
//     withitem_ty item = asdl_seq_GET(s->v.AsyncWith.items, pos);

//     assert(s->kind == AsyncWith_kind);
//     if (c->c_flags->cf_flags & PyCF_ALLOW_TOP_LEVEL_AWAIT){
//         c->u->u_ste->ste_coroutine = 1;
//     } else if (c->u->u_scope_type != COMPILER_SCOPE_ASYNC_FUNCTION){
//         return compiler_error(c, "'async with' outside async function");
//     }

//     block = compiler_new_block(c);
//     final = compiler_new_block(c);
//     exit = compiler_new_block(c);
//     if (!block || !final || !exit)
//         return 0;

//     /* Evaluate EXPR */
//     VISIT(c, expr, item->context_expr);

//     ADDOP(c, BEFORE_ASYNC_WITH);
//     ADDOP(c, GET_AWAITABLE);
//     ADDOP_LOAD_CONST(c, Py_None);
//     ADDOP(c, YIELD_FROM);

//     ADDOP_JREL(c, SETUP_ASYNC_WITH, final);

//     /* SETUP_ASYNC_WITH pushes a finally block. */
//     compiler_use_next_block(c, block);
//     if (!compiler_push_fblock(c, ASYNC_WITH, block, final, NULL)) {
//         return 0;
//     }

//     if (item->optional_vars) {
//         VISIT(c, expr, item->optional_vars);
//     }
//     else {
//     /* Discard result from context.__aenter__() */
//         ADDOP(c, POP_TOP);
//     }

//     pos++;
//     if (pos == asdl_seq_LEN(s->v.AsyncWith.items))
//         /* BLOCK code */
//         VISIT_SEQ(c, stmt, s->v.AsyncWith.body)
//     else if (!compiler_async_with(c, s, pos))
//             return 0;

//     compiler_pop_fblock(c, ASYNC_WITH, block);
//     ADDOP(c, POP_BLOCK);
//     /* End of body; start the cleanup */

//     /* For successful outcome:
//      * call __exit__(None, None, None)
//      */
//     if(!compiler_call_exit_with_nones(c))
//         return 0;
//     ADDOP(c, GET_AWAITABLE);
//     ADDOP_O(c, LOAD_CONST, Py_None, consts);
//     ADDOP(c, YIELD_FROM);

//     ADDOP(c, POP_TOP);

//     ADDOP_JABS(c, JUMP_ABSOLUTE, exit);

//     /* For exceptional outcome: */
//     compiler_use_next_block(c, final);

//     ADDOP(c, WITH_EXCEPT_START);
//     ADDOP(c, GET_AWAITABLE);
//     ADDOP_LOAD_CONST(c, Py_None);
//     ADDOP(c, YIELD_FROM);
//     compiler_with_except_finish(c);

// compiler_use_next_block(c, exit);
//     return 1;
// }


// /*
//    Implements the with statement from PEP 343.
//    with EXPR as VAR:
//        BLOCK
//    is implemented as:
//         <code for EXPR>
//         SETUP_WITH  E
//         <code to store to VAR> or POP_TOP
//         <code for BLOCK>
//         LOAD_CONST (None, None, None)
//         CALL_FUNCTION_EX 0
//         JUMP_FORWARD  EXIT
//     E:  WITH_EXCEPT_START (calls EXPR.__exit__)
//         POP_JUMP_IF_TRUE T:
//         RERAISE
//     T:  POP_TOP * 3 (remove exception from stack)
//         POP_EXCEPT
//         POP_TOP
//     EXIT:
//  */

// static int
// compiler_with(struct compiler *c, stmt_ty s, int pos)
// {
//     basicblock *block, *final, *exit;
//     withitem_ty item = asdl_seq_GET(s->v.With.items, pos);

//     assert(s->kind == With_kind);

//     block = compiler_new_block(c);
//     final = compiler_new_block(c);
//     exit = compiler_new_block(c);
//     if (!block || !final || !exit)
//         return 0;

//     /* Evaluate EXPR */
//     VISIT(c, expr, item->context_expr);
//     /* Will push bound __exit__ */
//     ADDOP_JREL(c, SETUP_WITH, final);

//     /* SETUP_WITH pushes a finally block. */
//     compiler_use_next_block(c, block);
//     if (!compiler_push_fblock(c, WITH, block, final, NULL)) {
//         return 0;
//     }

//     if (item->optional_vars) {
//         VISIT(c, expr, item->optional_vars);
//     }
//     else {
//     /* Discard result from context.__enter__() */
//         ADDOP(c, POP_TOP);
//     }

//     pos++;
//     if (pos == asdl_seq_LEN(s->v.With.items))
//         /* BLOCK code */
//         VISIT_SEQ(c, stmt, s->v.With.body)
//     else if (!compiler_with(c, s, pos))
//             return 0;

//     ADDOP(c, POP_BLOCK);
//     compiler_pop_fblock(c, WITH, block);

//     /* End of body; start the cleanup. */

//     /* For successful outcome:
//      * call __exit__(None, None, None)
//      */
//     if (!compiler_call_exit_with_nones(c))
//         return 0;
//     ADDOP(c, POP_TOP);
//     ADDOP_JREL(c, JUMP_FORWARD, exit);

//     /* For exceptional outcome: */
//     compiler_use_next_block(c, final);

//     ADDOP(c, WITH_EXCEPT_START);
//     compiler_with_except_finish(c);

//     compiler_use_next_block(c, exit);
//     return 1;
// }

static void
compiler_visit_expr1(struct compiler *c, expr_ty e)
{
    Py_ssize_t reg;
    switch (e->kind) {
//     case NamedExpr_kind:
//         VISIT(c, expr, e->v.NamedExpr.value);
//         ADDOP(c, DUP_TOP);
//         VISIT(c, expr, e->v.NamedExpr.target);
//         break;
    case BoolOp_kind:
        compiler_boolop(c, e);
        break;
    case BinOp_kind:
        reg = expr_to_any_reg(c, e->v.BinOp.left);
        compiler_visit_expr(c, e->v.BinOp.right);
        emit1(c, binop(e->v.BinOp.op), reg);
        clear_reg(c, reg);
        break;
    case UnaryOp_kind:
        compiler_visit_expr(c, e->v.UnaryOp.operand);
        emit0(c, unaryop(e->v.UnaryOp.op));
        break;
//     case Lambda_kind:
//         return compiler_lambda(c, e);
    case IfExp_kind:
        compiler_ifexp(c, e);
        break;
//     case Dict_kind:
//         return compiler_dict(c, e);
//     case Set_kind:
//         return compiler_set(c, e);
//     case GeneratorExp_kind:
//         return compiler_genexp(c, e);
//     case ListComp_kind:
//         return compiler_listcomp(c, e);
//     case SetComp_kind:
//         return compiler_setcomp(c, e);
//     case DictComp_kind:
//         return compiler_dictcomp(c, e);
//     case Yield_kind:
//         if (c->u->u_ste->ste_type != FunctionBlock)
//             return compiler_error(c, "'yield' outside function");
//         if (e->v.Yield.value) {
//             VISIT(c, expr, e->v.Yield.value);
//         }
//         else {
//             ADDOP_LOAD_CONST(c, Py_None);
//         }
//         ADDOP(c, YIELD_VALUE);
//         break;
//     case YieldFrom_kind:
//         if (c->u->u_ste->ste_type != FunctionBlock)
//             return compiler_error(c, "'yield' outside function");

//         if (c->u->u_scope_type == COMPILER_SCOPE_ASYNC_FUNCTION)
//             return compiler_error(c, "'yield from' inside async function");

//         VISIT(c, expr, e->v.YieldFrom.value);
//         ADDOP(c, GET_YIELD_FROM_ITER);
//         ADDOP_LOAD_CONST(c, Py_None);
//         ADDOP(c, YIELD_FROM);
//         break;
//     case Await_kind:
//         if (!(c->c_flags->cf_flags & PyCF_ALLOW_TOP_LEVEL_AWAIT)){
//             if (c->u->u_ste->ste_type != FunctionBlock){
//                 return compiler_error(c, "'await' outside function");
//             }

//             if (c->u->u_scope_type != COMPILER_SCOPE_ASYNC_FUNCTION &&
//                     c->u->u_scope_type != COMPILER_SCOPE_COMPREHENSION){
//                 return compiler_error(c, "'await' outside async function");
//             }
//         }

//         VISIT(c, expr, e->v.Await.value);
//         ADDOP(c, GET_AWAITABLE);
//         ADDOP_LOAD_CONST(c, Py_None);
//         ADDOP(c, YIELD_FROM);
//         break;
    case Compare_kind:
        compiler_compare(c, e); break;
    case Call_kind:
        compiler_call(c, e);
        break;
    case Constant_kind:
        emit1(c, LOAD_CONST, compiler_const(c, e->v.Constant.value));
        break;
//     case JoinedStr_kind:
//         return compiler_joined_str(c, e);
//     case FormattedValue_kind:
//         return compiler_formatted_value(c, e);
//     /* The following exprs can be assignment targets. */
    case Attribute_kind: {
        assert(e->v.Attribute.ctx == Load);
        Py_ssize_t reg = expr_to_any_reg(c, e->v.Attribute.value);
        emit2(c, LOAD_ATTR, reg, compiler_const(c, e->v.Attribute.attr));
        clear_reg(c, reg);
        break;
    }
//         if (e->v.Attribute.ctx != AugStore)
//             VISIT(c, expr, e->v.Attribute.value);
//         switch (e->v.Attribute.ctx) {
//         case AugLoad:
//             ADDOP(c, DUP_TOP);
//             /* Fall through */
//         case Load:
//             ADDOP_NAME(c, LOAD_ATTR, e->v.Attribute.attr, names);
//             break;
//         case AugStore:
//             ADDOP(c, ROT_TWO);
//             /* Fall through */
//         case Store:
//             ADDOP_NAME(c, STORE_ATTR, e->v.Attribute.attr, names);
//             break;
//         case Del:
//             ADDOP_NAME(c, DELETE_ATTR, e->v.Attribute.attr, names);
//             break;
//         case Param:
//         default:
//             PyErr_SetString(PyExc_SystemError,
//                             "param invalid in attribute expression");
//             return 0;
//         }
//         break;
    case Subscript_kind: {
        assert(e->v.Subscript.ctx == Load);
        Py_ssize_t reg = expr_to_any_reg(c, e->v.Subscript.value);
        compiler_slice(c, e->v.Subscript.slice);
        emit1(c, BINARY_SUBSCR, reg);
        clear_reg(c, reg);
        break;
    }
//         switch (e->v.Subscript.ctx) {
//         case AugLoad:
//             VISIT(c, expr, e->v.Subscript.value);
//             VISIT_SLICE(c, e->v.Subscript.slice, AugLoad);
//             break;
//         case Load:
//             if (!check_subscripter(c, e->v.Subscript.value)) {
//                 return 0;
//             }
//             if (!check_index(c, e->v.Subscript.value, e->v.Subscript.slice)) {
//                 return 0;
//             }
//             VISIT(c, expr, e->v.Subscript.value);
//             VISIT_SLICE(c, e->v.Subscript.slice, Load);
//             break;
//         case AugStore:
//             VISIT_SLICE(c, e->v.Subscript.slice, AugStore);
//             break;
//         case Store:
//             VISIT(c, expr, e->v.Subscript.value);
//             VISIT_SLICE(c, e->v.Subscript.slice, Store);
//             break;
//         case Del:
//             VISIT(c, expr, e->v.Subscript.value);
//             VISIT_SLICE(c, e->v.Subscript.slice, Del);
//             break;
//         case Param:
//         default:
//             PyErr_SetString(PyExc_SystemError,
//                 "param invalid in subscript expression");
//             return 0;
//         }
//         break;
//     case Starred_kind:
//         switch (e->v.Starred.ctx) {
//         case Store:
//             /* In all legitimate cases, the Starred node was already replaced
//              * by compiler_list/compiler_tuple. XXX: is that okay? */
//             return compiler_error(c,
//                 "starred assignment target must be in a list or tuple");
//         default:
//             return compiler_error(c,
//                 "can't use starred expression here");
//         }
    case Name_kind:
        compiler_nameop(c, e->v.Name.id, e->v.Name.ctx);
        break;
//     /* child nodes of List and Tuple will have expr_context set */
    case List_kind:
        compiler_list(c, e);
        break;
//     case Tuple_kind:
//         return compiler_tuple(c, e);
        default:
            PyErr_Format(PyExc_RuntimeError, "unhandled expr %d", e->kind);
            COMPILER_ERROR(c);
    }
}

static void
compiler_visit_expr(struct compiler *c, expr_ty e)
{
    /* If expr e has a different line number than the last expr/stmt,
       set a new line number for the next instruction.
    */
    int old_lineno = c->unit->lineno;
    int old_col_offset = c->unit->col_offset;
    if (e->lineno != c->unit->lineno) {
        c->unit->lineno = e->lineno;
        c->unit->lineno_set = 0;
    }
    /* Updating the column offset is always harmless. */
    c->unit->col_offset = e->col_offset;

    Py_ssize_t base = c->unit->next_register;
    compiler_visit_expr1(c, e);
    assert(c->unit->next_register == base);

    if (old_lineno != c->unit->lineno) {
        c->unit->lineno = old_lineno;
        c->unit->lineno_set = 0;
    }
    c->unit->col_offset = old_col_offset;
}

static void
compiler_augassign(struct compiler *c, stmt_ty s)
{
    Py_ssize_t reg1, reg2, reg3, const_slot;
    expr_ty e = s->v.AugAssign.target;
    expr_ty name;

    assert(s->kind == AugAssign_kind);

    switch (e->kind) {
    case Attribute_kind:
        reg1 = expr_to_any_reg(c, e->v.Attribute.value);
        const_slot = compiler_const(c, e->v.Attribute.attr);
        emit2(c, LOAD_ATTR, reg1, const_slot);
        reg2 = reserve_regs(c, 1);
        emit1(c, STORE_FAST, reg2);
        compiler_visit_expr(c, s->v.AugAssign.value);
        emit1(c, inplace_binop(c, s->v.AugAssign.op), reg2);
        emit2(c, STORE_ATTR, reg1, const_slot);
        clear_reg(c, reg2);
        clear_reg(c, reg1);
        break;
    case Subscript_kind:
        reg1 = expr_to_any_reg(c, e->v.Subscript.value);
        assert(false && "NYI slice");
        // reg2 = expr_to_any_reg(c, e->v.Subscript.slice);
        emit1(c, LOAD_FAST, reg2);
        emit1(c, BINARY_SUBSCR, reg1);
        reg3 = reserve_regs(c, 1);
        emit1(c, STORE_FAST, reg3);
        compiler_visit_expr(c, s->v.AugAssign.value);
        emit1(c, inplace_binop(c, s->v.AugAssign.op), reg3);
        emit2(c, STORE_SUBSCR, reg1, reg2);
        clear_reg(c, reg3);
        clear_reg(c, reg2);
        clear_reg(c, reg1);
        break;
    case Name_kind:
        name = Name(e->v.Name.id, Load, e->lineno, e->col_offset,
                    e->end_lineno, e->end_col_offset, c->arena);
        reg1 = expr_to_any_reg(c, name);
        compiler_visit_expr(c, s->v.AugAssign.value);
        emit1(c, inplace_binop(c, s->v.AugAssign.op), reg1);
        compiler_store(c, e->v.Name.id);
        clear_reg(c, reg1);
        break;
    default:
        PyErr_Format(PyExc_SystemError,
            "invalid node type (%d) for augmented assignment",
            e->kind);
        COMPILER_ERROR(c);
    }
}

static void
check_ann_expr(struct compiler *c, expr_ty e)
{
    compiler_visit_expr(c, e);
    emit0(c, CLEAR_ACC);
}

static void
check_annotation(struct compiler *c, stmt_ty s)
{
    /* Annotations are only evaluated in a module or class. */
    if (c->unit->scope_type == COMPILER_SCOPE_MODULE ||
        c->unit->scope_type == COMPILER_SCOPE_CLASS) {
        check_ann_expr(c, s->v.AnnAssign.annotation);
    }
}

static void
check_ann_slice(struct compiler *c, slice_ty sl)
{
    switch(sl->kind) {
    case Index_kind:
        check_ann_expr(c, sl->v.Index.value);
        break;
    case Slice_kind:
        if (sl->v.Slice.lower) {
            check_ann_expr(c, sl->v.Slice.lower);
        }
        if (sl->v.Slice.upper) {
            check_ann_expr(c, sl->v.Slice.upper);
        }
        if (sl->v.Slice.step) {
            check_ann_expr(c, sl->v.Slice.step);
        }
        break;
    default:
        PyErr_SetString(PyExc_SystemError,
                        "unexpected slice kind");
        COMPILER_ERROR(c);
    }
}

static void
check_ann_subscr(struct compiler *c, slice_ty sl)
{
    /* We check that everything in a subscript is defined at runtime. */
    Py_ssize_t i, n;

    switch (sl->kind) {
    case Index_kind:
    case Slice_kind:
        check_ann_slice(c, sl);
        break;
    case ExtSlice_kind:
        n = asdl_seq_LEN(sl->v.ExtSlice.dims);
        for (i = 0; i < n; i++) {
            slice_ty subsl = (slice_ty)asdl_seq_GET(sl->v.ExtSlice.dims, i);
            switch (subsl->kind) {
            case Index_kind:
            case Slice_kind:
                check_ann_slice(c, subsl);
                break;
            case ExtSlice_kind:
            default:
                PyErr_SetString(PyExc_SystemError,
                                "extended slice invalid in nested slice");
                COMPILER_ERROR(c);
            }
        }
        break;
    default:
        PyErr_Format(PyExc_SystemError,
                     "invalid subscript kind %d", sl->kind);
        COMPILER_ERROR(c);
    }
}

static void
compiler_annassign(struct compiler *c, stmt_ty s)
{
    expr_ty targ = s->v.AnnAssign.target;

    assert(s->kind == AnnAssign_kind);

    /* We perform the actual assignment first. */
    if (s->v.AnnAssign.value) {
        compiler_assign_expr(c, targ, s->v.AnnAssign.value);
    }
    switch (targ->kind) {
    case Name_kind:
        /* If we have a simple name in a module or class, store annotation. */
        if (s->v.AnnAssign.simple &&
            (c->unit->scope_type == COMPILER_SCOPE_MODULE ||
             c->unit->scope_type == COMPILER_SCOPE_CLASS))
        {
            Py_ssize_t reg = reserve_regs(c, 2);
            load_name_id(c, &PyId___annotations__);
            emit1(c, STORE_FAST, reg);
            emit1(c, LOAD_CONST, compiler_const(c, mangle(c, targ->v.Name.id)));
            emit1(c, STORE_FAST, reg + 1);
            if (c->future->ff_features & CO_FUTURE_ANNOTATIONS) {
                compiler_visit_annexpr(c, s->v.AnnAssign.annotation);
            }
            else {
                compiler_visit_expr(c, s->v.AnnAssign.annotation);
            }
            emit2(c, STORE_SUBSCR, reg, reg + 1);
            clear_reg(c, reg + 1);
            clear_reg(c, reg);
        }
        break;
    case Attribute_kind:
        if (s->v.AnnAssign.value) {
            check_ann_expr(c, targ->v.Attribute.value);
        }
        break;
    case Subscript_kind:
        if (s->v.AnnAssign.value) {
            check_ann_expr(c, targ->v.Subscript.value);
            check_ann_subscr(c, targ->v.Subscript.slice);
        }
        break;
    default:
        PyErr_Format(PyExc_SystemError,
                     "invalid node type (%d) for annotated assignment",
                     targ->kind);
        COMPILER_ERROR(c);
    }
    /* Annotation is evaluated last. */
    if (!s->v.AnnAssign.simple) {
        check_annotation(c, s);
    }
}

/* Raises a SyntaxError and performs a non-local jump.
   If something goes wrong, a different exception may be raised.
*/

static void
compiler_error_u(struct compiler *c, PyObject *err)
{
    PyObject *loc;
    PyObject *u = NULL, *v = NULL;

    loc = PyErr_ProgramTextObject(c->filename, c->unit->lineno);
    if (!loc) {
        Py_INCREF(Py_None);
        loc = Py_None;
    }
    u = Py_BuildValue("(OiiO)", c->filename, c->unit->lineno,
                      c->unit->col_offset + 1, loc);
    if (!u)
        goto exit;
    v = Py_BuildValue("(OO)", err, u);
    if (!v)
        goto exit;
    PyErr_SetObject(PyExc_SyntaxError, v);
 exit:
    Py_DECREF(err);
    Py_DECREF(loc);
    Py_XDECREF(u);
    Py_XDECREF(v);
    COMPILER_ERROR(c);
}

static void
compiler_error(struct compiler *c, const char *errstr)
{
    PyObject *err = PyUnicode_FromString(errstr);
    if (err == NULL) {
        COMPILER_ERROR(c);
    }
    compiler_error_u(c, err);
}

/* Emits a SyntaxWarning and returns 1 on success.
   If a SyntaxWarning raised as error, replaces it with a SyntaxError
   and returns 0.
*/
static void
compiler_warn(struct compiler *c, const char *format, ...)
{
    va_list vargs;
#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, format);
#else
    va_start(vargs);
#endif
    PyObject *msg = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);
    if (msg == NULL) {
        COMPILER_ERROR(c);
    }
    if (PyErr_WarnExplicitObject(PyExc_SyntaxWarning, msg, c->filename,
                                 c->unit->lineno, NULL, NULL) < 0)
    {
        if (PyErr_ExceptionMatches(PyExc_SyntaxWarning)) {
            /* Replace the SyntaxWarning exception with a SyntaxError
               to get a more accurate error report */
            PyErr_Clear();
            compiler_error_u(c, msg);
            /* UNREACHABLE */;
        }
        Py_DECREF(msg);
        COMPILER_ERROR(c);
    }
    Py_DECREF(msg);
}

// static int
// compiler_handle_subscr(struct compiler *c, const char *kind,
//                        expr_context_ty ctx)
// {
//     int op = 0;

//     /* XXX this code is duplicated */
//     switch (ctx) {
//         case FuncLoad: /* fall through to Load */
//         case AugLoad: /* fall through to Load */
//         case Load:    op = BINARY_SUBSCR; break;
//         case AugStore:/* fall through to Store */
//         case Store:   op = STORE_SUBSCR; break;
//         case Del:     op = DELETE_SUBSCR; break;
//         case Param:
//             PyErr_Format(PyExc_SystemError,
//                          "invalid %s kind %d in subscript\n",
//                          kind, ctx);
//             return 0;
//     }
//     if (ctx == AugLoad) {
//         ADDOP(c, DUP_TOP_TWO);
//     }
//     else if (ctx == AugStore) {
//         ADDOP(c, ROT_THREE);
//     }
//     ADDOP(c, op);
//     return 1;
// }

static PyObject *
expr_as_const(expr_ty e)
{
    if (e == NULL) {
        return Py_None;
    }
    if (e->kind == Constant_kind) {
        return e->v.Constant.value;
    }
    return NULL;
}

static void
compiler_slice(struct compiler *c, slice_ty s)
{
    if (s->kind == Index_kind) {
        compiler_visit_expr(c, s->v.Index.value);
        return;
    }
    if (s->kind == ExtSlice_kind) {
        Py_ssize_t base = c->unit->next_register;
        Py_ssize_t i, n = asdl_seq_LEN(s->v.ExtSlice.dims);
        for (i = 0; i < n; i++) {
            slice_ty sub = (slice_ty)asdl_seq_GET(s->v.ExtSlice.dims, i);
            compiler_slice(c, sub);
            emit1(c, STORE_FAST, reserve_regs(c, 1));
        }
        emit2(c, BUILD_TUPLE, base, n);
        c->unit->next_register = base;
        return;
    }

    PyObject *lower, *upper, *step;
    lower = expr_as_const(s->v.Slice.lower);
    upper = expr_as_const(s->v.Slice.upper);
    step = expr_as_const(s->v.Slice.step);
    if (lower && upper && step) {
        PyObject *slice = PySlice_New(lower, upper, step);
        if (slice == NULL) {
            COMPILER_ERROR(c);
        }
        emit1(c, LOAD_CONST, compiler_new_const(c, slice));
        return;
    }

    Py_ssize_t base = c->unit->next_register;
    expr_to_reg(c, s->v.Slice.lower, base + 0);
    expr_to_reg(c, s->v.Slice.upper, base + 1);
    expr_to_reg(c, s->v.Slice.step,  base + 2);
    emit1(c, BUILD_SLICE, base);
    c->unit->next_register = base;
}

// /* End of the compiler section, beginning of the assembler section */

// /* do depth-first search of basic block graph, starting with block.
//    post records the block indices in post-order.

//    XXX must handle implicit jumps from one block to next
// */

// struct assembler {
//     PyObject *a_bytecode;  /* string containing bytecode */
//     int a_offset;              /* offset into bytecode */
//     int a_nblocks;             /* number of reachable blocks */
//     basicblock **a_postorder; /* list of blocks in dfs postorder */
//     PyObject *a_lnotab;    /* string containing lnotab */
//     int a_lnotab_off;      /* offset into lnotab */
//     int a_lineno;              /* last lineno of emitted instruction */
//     int a_lineno_off;      /* bytecode offset of last lineno */
// };

// static void
// dfs(struct compiler *c, basicblock *b, struct assembler *a, int end)
// {
//     int i, j;

//     /* Get rid of recursion for normal control flow.
//        Since the number of blocks is limited, unused space in a_postorder
//        (from a_nblocks to end) can be used as a stack for still not ordered
//        blocks. */
//     for (j = end; b && !b->b_seen; b = b->b_next) {
//         b->b_seen = 1;
//         assert(a->a_nblocks < j);
//         a->a_postorder[--j] = b;
//     }
//     while (j < end) {
//         b = a->a_postorder[j++];
//         for (i = 0; i < b->b_iused; i++) {
//             struct instr *instr = &b->b_instr[i];
//             if (instr->i_jrel || instr->i_jabs)
//                 dfs(c, instr->i_target, a, j);
//         }
//         assert(a->a_nblocks < j);
//         a->a_postorder[a->a_nblocks++] = b;
//     }
// }

// Py_LOCAL_INLINE(void)
// stackdepth_push(basicblock ***sp, basicblock *b, int depth)
// {
//     assert(b->b_startdepth < 0 || b->b_startdepth == depth);
//     if (b->b_startdepth < depth && b->b_startdepth < 100) {
//         assert(b->b_startdepth < 0);
//         b->b_startdepth = depth;
//         *(*sp)++ = b;
//     }
// }

// /* Find the flow path that needs the largest stack.  We assume that
//  * cycles in the flow graph have no net effect on the stack depth.
//  */
// static int
// stackdepth(struct compiler *c)
// {
//     basicblock *b, *entryblock = NULL;
//     basicblock **stack, **sp;
//     int nblocks = 0, maxdepth = 0;
//     for (b = c->u->u_blocks; b != NULL; b = b->b_list) {
//         b->b_startdepth = INT_MIN;
//         entryblock = b;
//         nblocks++;
//     }
//     if (!entryblock)
//         return 0;
//     stack = (basicblock **)PyObject_Malloc(sizeof(basicblock *) * nblocks);
//     if (!stack) {
//         PyErr_NoMemory();
//         return -1;
//     }

//     sp = stack;
//     stackdepth_push(&sp, entryblock, 0);
//     while (sp != stack) {
//         b = *--sp;
//         int depth = b->b_startdepth;
//         assert(depth >= 0);
//         basicblock *next = b->b_next;
//         for (int i = 0; i < b->b_iused; i++) {
//             struct instr *instr = &b->b_instr[i];
//             int effect = stack_effect(instr->i_opcode, instr->i_oparg, 0);
//             if (effect == PY_INVALID_STACK_EFFECT) {
//                 fprintf(stderr, "opcode = %d\n", instr->i_opcode);
//                 Py_FatalError("PyCompile_OpcodeStackEffect()");
//             }
//             int new_depth = depth + effect;
//             if (new_depth > maxdepth) {
//                 maxdepth = new_depth;
//             }
//             assert(depth >= 0); /* invalid code or bug in stackdepth() */
//             if (instr->i_jrel || instr->i_jabs) {
//                 effect = stack_effect(instr->i_opcode, instr->i_oparg, 1);
//                 assert(effect != PY_INVALID_STACK_EFFECT);
//                 int target_depth = depth + effect;
//                 if (target_depth > maxdepth) {
//                     maxdepth = target_depth;
//                 }
//                 assert(target_depth >= 0); /* invalid code or bug in stackdepth() */
//                 stackdepth_push(&sp, instr->i_target, target_depth);
//             }
//             depth = new_depth;
//             if (instr->i_opcode == JUMP_ABSOLUTE ||
//                 instr->i_opcode == JUMP_FORWARD ||
//                 instr->i_opcode == RETURN_VALUE ||
//                 instr->i_opcode == RAISE_VARARGS ||
//                 instr->i_opcode == RERAISE)
//             {
//                 /* remaining code is dead */
//                 next = NULL;
//                 break;
//             }
//         }
//         if (next != NULL) {
//             stackdepth_push(&sp, next, depth);
//         }
//     }
//     PyObject_Free(stack);
//     return maxdepth;
// }

// static int
// assemble_init(struct assembler *a, int nblocks, int firstlineno)
// {
//     memset(a, 0, sizeof(struct assembler));
//     a->a_lineno = firstlineno;
//     a->a_bytecode = PyBytes_FromStringAndSize(NULL, DEFAULT_CODE_SIZE);
//     if (!a->a_bytecode)
//         return 0;
//     a->a_lnotab = PyBytes_FromStringAndSize(NULL, DEFAULT_LNOTAB_SIZE);
//     if (!a->a_lnotab)
//         return 0;
//     if ((size_t)nblocks > SIZE_MAX / sizeof(basicblock *)) {
//         PyErr_NoMemory();
//         return 0;
//     }
//     a->a_postorder = (basicblock **)PyObject_Malloc(
//                                         sizeof(basicblock *) * nblocks);
//     if (!a->a_postorder) {
//         PyErr_NoMemory();
//         return 0;
//     }
//     return 1;
// }

// static void
// assemble_free(struct assembler *a)
// {
//     Py_XDECREF(a->a_bytecode);
//     Py_XDECREF(a->a_lnotab);
//     if (a->a_postorder)
//         PyObject_Free(a->a_postorder);
// }

// static int
// blocksize(basicblock *b)
// {
//     int i;
//     int size = 0;

//     for (i = 0; i < b->b_iused; i++)
//         size += instrsize(b->b_instr[i].i_oparg);
//     return size;
// }

// /* Appends a pair to the end of the line number table, a_lnotab, representing
//    the instruction's bytecode offset and line number.  See
//    Objects/lnotab_notes.txt for the description of the line number table. */

// static int
// assemble_lnotab(struct assembler *a, struct instr *i)
// {
//     int d_bytecode, d_lineno;
//     Py_ssize_t len;
//     unsigned char *lnotab;

//     d_bytecode = (a->a_offset - a->a_lineno_off) * sizeof(_Py_CODEUNIT);
//     d_lineno = i->i_lineno - a->a_lineno;

//     assert(d_bytecode >= 0);

//     if(d_bytecode == 0 && d_lineno == 0)
//         return 1;

//     if (d_bytecode > 255) {
//         int j, nbytes, ncodes = d_bytecode / 255;
//         nbytes = a->a_lnotab_off + 2 * ncodes;
//         len = PyBytes_GET_SIZE(a->a_lnotab);
//         if (nbytes >= len) {
//             if ((len <= INT_MAX / 2) && (len * 2 < nbytes))
//                 len = nbytes;
//             else if (len <= INT_MAX / 2)
//                 len *= 2;
//             else {
//                 PyErr_NoMemory();
//                 return 0;
//             }
//             if (_PyBytes_Resize(&a->a_lnotab, len) < 0)
//                 return 0;
//         }
//         lnotab = (unsigned char *)
//                    PyBytes_AS_STRING(a->a_lnotab) + a->a_lnotab_off;
//         for (j = 0; j < ncodes; j++) {
//             *lnotab++ = 255;
//             *lnotab++ = 0;
//         }
//         d_bytecode -= ncodes * 255;
//         a->a_lnotab_off += ncodes * 2;
//     }
//     assert(0 <= d_bytecode && d_bytecode <= 255);

//     if (d_lineno < -128 || 127 < d_lineno) {
//         int j, nbytes, ncodes, k;
//         if (d_lineno < 0) {
//             k = -128;
//             /* use division on positive numbers */
//             ncodes = (-d_lineno) / 128;
//         }
//         else {
//             k = 127;
//             ncodes = d_lineno / 127;
//         }
//         d_lineno -= ncodes * k;
//         assert(ncodes >= 1);
//         nbytes = a->a_lnotab_off + 2 * ncodes;
//         len = PyBytes_GET_SIZE(a->a_lnotab);
//         if (nbytes >= len) {
//             if ((len <= INT_MAX / 2) && len * 2 < nbytes)
//                 len = nbytes;
//             else if (len <= INT_MAX / 2)
//                 len *= 2;
//             else {
//                 PyErr_NoMemory();
//                 return 0;
//             }
//             if (_PyBytes_Resize(&a->a_lnotab, len) < 0)
//                 return 0;
//         }
//         lnotab = (unsigned char *)
//                    PyBytes_AS_STRING(a->a_lnotab) + a->a_lnotab_off;
//         *lnotab++ = d_bytecode;
//         *lnotab++ = k;
//         d_bytecode = 0;
//         for (j = 1; j < ncodes; j++) {
//             *lnotab++ = 0;
//             *lnotab++ = k;
//         }
//         a->a_lnotab_off += ncodes * 2;
//     }
//     assert(-128 <= d_lineno && d_lineno <= 127);

//     len = PyBytes_GET_SIZE(a->a_lnotab);
//     if (a->a_lnotab_off + 2 >= len) {
//         if (_PyBytes_Resize(&a->a_lnotab, len * 2) < 0)
//             return 0;
//     }
//     lnotab = (unsigned char *)
//                     PyBytes_AS_STRING(a->a_lnotab) + a->a_lnotab_off;

//     a->a_lnotab_off += 2;
//     if (d_bytecode) {
//         *lnotab++ = d_bytecode;
//         *lnotab++ = d_lineno;
//     }
//     else {      /* First line of a block; def stmt, etc. */
//         *lnotab++ = 0;
//         *lnotab++ = d_lineno;
//     }
//     a->a_lineno = i->i_lineno;
//     a->a_lineno_off = a->a_offset;
//     return 1;
// }

// /* assemble_emit()
//    Extend the bytecode with a new instruction.
//    Update lnotab if necessary.
// */

// static int
// assemble_emit(struct assembler *a, struct instr *i)
// {
//     int size, arg = 0;
//     Py_ssize_t len = PyBytes_GET_SIZE(a->a_bytecode);
//     _Py_CODEUNIT *code;

//     arg = i->i_oparg;
//     size = instrsize(arg);
//     if (i->i_lineno && !assemble_lnotab(a, i))
//         return 0;
//     if (a->a_offset + size >= len / (int)sizeof(_Py_CODEUNIT)) {
//         if (len > PY_SSIZE_T_MAX / 2)
//             return 0;
//         if (_PyBytes_Resize(&a->a_bytecode, len * 2) < 0)
//             return 0;
//     }
//     code = (_Py_CODEUNIT *)PyBytes_AS_STRING(a->a_bytecode) + a->a_offset;
//     a->a_offset += size;
//     write_op_arg(code, i->i_opcode, arg, size);
//     return 1;
// }

// static void
// assemble_jump_offsets(struct assembler *a, struct compiler *c)
// {
//     basicblock *b;
//     int bsize, totsize, extended_arg_recompile;
//     int i;

//     /* Compute the size of each block and fixup jump args.
//        Replace block pointer with position in bytecode. */
//     do {
//         totsize = 0;
//         for (i = a->a_nblocks - 1; i >= 0; i--) {
//             b = a->a_postorder[i];
//             bsize = blocksize(b);
//             b->b_offset = totsize;
//             totsize += bsize;
//         }
//         extended_arg_recompile = 0;
//         for (b = c->u->u_blocks; b != NULL; b = b->b_list) {
//             bsize = b->b_offset;
//             for (i = 0; i < b->b_iused; i++) {
//                 struct instr *instr = &b->b_instr[i];
//                 int isize = instrsize(instr->i_oparg);
//                 /* Relative jumps are computed relative to
//                    the instruction pointer after fetching
//                    the jump instruction.
//                 */
//                 bsize += isize;
//                 if (instr->i_jabs || instr->i_jrel) {
//                     instr->i_oparg = instr->i_target->b_offset;
//                     if (instr->i_jrel) {
//                         instr->i_oparg -= bsize;
//                     }
//                     instr->i_oparg *= sizeof(_Py_CODEUNIT);
//                     if (instrsize(instr->i_oparg) != isize) {
//                         extended_arg_recompile = 1;
//                     }
//                 }
//             }
//         }

//     /* XXX: This is an awful hack that could hurt performance, but
//         on the bright side it should work until we come up
//         with a better solution.

//         The issue is that in the first loop blocksize() is called
//         which calls instrsize() which requires i_oparg be set
//         appropriately. There is a bootstrap problem because
//         i_oparg is calculated in the second loop above.

//         So we loop until we stop seeing new EXTENDED_ARGs.
//         The only EXTENDED_ARGs that could be popping up are
//         ones in jump instructions.  So this should converge
//         fairly quickly.
//     */
//     } while (extended_arg_recompile);
// }

// static PyObject *
// dict_keys_inorder(PyObject *dict, Py_ssize_t offset)
// {
//     PyObject *tuple, *k, *v;
//     Py_ssize_t i, pos = 0, size = PyDict_GET_SIZE(dict);

//     tuple = PyTuple_New(size);
//     if (tuple == NULL)
//         return NULL;
//     while (PyDict_Next(dict, &pos, &k, &v)) {
//         i = PyLong_AS_LONG(v);
//         Py_INCREF(k);
//         assert((i - offset) < size);
//         assert((i - offset) >= 0);
//         PyTuple_SET_ITEM(tuple, i - offset, k);
//     }
//     return tuple;
// }

static PyObject *
unpack_const_key(PyObject *key)
{
    if (!PyTuple_CheckExact(key)) {
        Py_INCREF(key);
        return key;
    }
    PyObject *type = PyTuple_GET_ITEM(key, 0);
    PyObject *value = PyTuple_GET_ITEM(key, 1);
    if (type == (PyObject *)&PySlice_Type) {
        return PySlice_New(PyTuple_GET_ITEM(value, 0),
                           PyTuple_GET_ITEM(value, 1),
                           PyTuple_GET_ITEM(value, 2));
    }
    Py_INCREF(value);
    return value;
}

// static PyObject *
// consts_dict_keys_inorder(PyObject *dict)
// {
//     PyObject *consts, *k, *v;
//     Py_ssize_t i, pos = 0, size = PyDict_GET_SIZE(dict);

//     consts = PyList_New(size);   /* PyCode_Optimize() requires a list */
//     if (consts == NULL)
//         return NULL;
//     while (PyDict_Next(dict, &pos, &k, &v)) {
//         i = PyLong_AS_LONG(v);
//         /* The keys of the dictionary can be tuples wrapping a contant.
//          * (see compiler_add_o and _PyCode_ConstantKey). In that case
//          * the object we want is always second. */
//         if (PyTuple_CheckExact(k)) {
//             k = PyTuple_GET_ITEM(k, 1);
//         }
//         Py_INCREF(k);
//         assert(i < size);
//         assert(i >= 0);
//         PyList_SET_ITEM(consts, i, k);
//     }
//     return consts;
// }

// static int
// compute_code_flags(struct compiler *c)
// {
//     PySTEntryObject *ste = c->u->u_ste;
//     int flags = 0;
//     if (ste->ste_type == FunctionBlock) {
//         flags |= CO_NEWLOCALS | CO_OPTIMIZED;
//         if (ste->ste_nested)
//             flags |= CO_NESTED;
//         if (ste->ste_generator && !ste->ste_coroutine)
//             flags |= CO_GENERATOR;
//         if (!ste->ste_generator && ste->ste_coroutine)
//             flags |= CO_COROUTINE;
//         if (ste->ste_generator && ste->ste_coroutine)
//             flags |= CO_ASYNC_GENERATOR;
//         if (ste->ste_varargs)
//             flags |= CO_VARARGS;
//         if (ste->ste_varkeywords)
//             flags |= CO_VARKEYWORDS;
//     }

//     /* (Only) inherit compilerflags in PyCF_MASK */
//     flags |= (c->c_flags->cf_flags & PyCF_MASK);

//     if ((c->c_flags->cf_flags & PyCF_ALLOW_TOP_LEVEL_AWAIT) &&
//          ste->ste_coroutine &&
//          !ste->ste_generator) {
//         flags |= CO_COROUTINE;
//     }

//     return flags;
// }

// // Merge *tuple* with constant cache.
// // Unlike merge_consts_recursive(), this function doesn't work recursively.
// static int
// merge_const_tuple(struct compiler *c, PyObject **tuple)
// {
//     assert(PyTuple_CheckExact(*tuple));

//     PyObject *key = _PyCode_ConstantKey(*tuple);
//     if (key == NULL) {
//         return 0;
//     }

//     // t is borrowed reference
//     PyObject *t = PyDict_SetDefault(c->c_const_cache, key, key);
//     Py_DECREF(key);
//     if (t == NULL) {
//         return 0;
//     }
//     if (t == key) {  // tuple is new constant.
//         return 1;
//     }

//     PyObject *u = PyTuple_GET_ITEM(t, 1);
//     Py_INCREF(u);
//     Py_DECREF(*tuple);
//     *tuple = u;
//     return 1;
// }

static PyObject *
dict_keys_as_tuple(struct compiler *c, PyObject *dict)
{
    Py_ssize_t i = 0;
    Py_ssize_t pos = 0;
    PyObject *key, *value, *tuple;

    tuple = PyTuple_New(PyDict_GET_SIZE(dict));
    if (tuple == NULL) {
        COMPILER_ERROR(c);
    }  
    while (PyDict_Next(dict, &pos, &key, &value)) {
        Py_INCREF(key);
        PyTuple_SET_ITEM(tuple, i++, key);
    }
    return tuple;
}

static PyCodeObject2 *
makecode(struct compiler *c)
{
    Py_ssize_t instr_size = c->unit->instr.offset;
    Py_ssize_t nconsts = PyDict_GET_SIZE(c->unit->consts);
    Py_ssize_t niconsts = 0;
    Py_ssize_t nmeta = PyDict_GET_SIZE(c->unit->metadata);
    Py_ssize_t ncells = 0;
    Py_ssize_t ncaptures = 0;
    Py_ssize_t nexc_handlers = 0;

    PyCodeObject2 *co = PyCode2_New(instr_size, nconsts, niconsts, nmeta, ncells, ncaptures, nexc_handlers);
    if (co == NULL) {
        COMPILER_ERROR(c);
    }
    // FIXME: co leaked on error

    co->co_argcount = c->unit->argcount;
    co->co_posonlyargcount = c->unit->posonlyargcount;
    co->co_totalargcount = c->unit->kwonlyargcount + c->unit->argcount;
    co->co_nlocals = c->unit->nlocals;
    // co->co_ndefaultargs = ndefaultargs;
    // co->co_flags = flags;
    co->co_framesize = c->unit->max_registers;
    co->co_varnames = dict_keys_as_tuple(c, c->unit->varnames);
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
        key = unpack_const_key(key);
        if (key == NULL) {
            COMPILER_ERROR(c);
        }
        if (PyUnicode_CheckExact(key)) {
            PyUnicode_InternInPlace(&key);
        }
        co->co_constants[i] = key;
        i++;
    }

    return co;
}

// /* For debugging purposes only */
// #if 0
// static void
// dump_instr(const struct instr *i)
// {
//     const char *jrel = i->i_jrel ? "jrel " : "";
//     const char *jabs = i->i_jabs ? "jabs " : "";
//     char arg[128];

//     *arg = '\0';
//     if (HAS_ARG(i->i_opcode)) {
//         sprintf(arg, "arg: %d ", i->i_oparg);
//     }
//     fprintf(stderr, "line: %d, opcode: %d %s%s%s\n",
//                     i->i_lineno, i->i_opcode, arg, jabs, jrel);
// }

// static void
// dump_basicblock(const basicblock *b)
// {
//     const char *seen = b->b_seen ? "seen " : "";
//     const char *b_return = b->b_return ? "return " : "";
//     fprintf(stderr, "used: %d, depth: %d, offset: %d %s%s\n",
//         b->b_iused, b->b_startdepth, b->b_offset, seen, b_return);
//     if (b->b_instr) {
//         int i;
//         for (i = 0; i < b->b_iused; i++) {
//             fprintf(stderr, "  [%02d] ", i);
//             dump_instr(b->b_instr + i);
//         }
//     }
// }
// #endif

static void
assemble(struct compiler *c, int addNone)
{
    PyCodeObject2 *co;
    if (c->unit->reachable) {
        emit1(c, LOAD_CONST, const_none(c));
        emit0(c, RETURN_VALUE);
    }

    co = makecode(c);
    Py_XSETREF(c->code, co);
    // assemble_free(&a);
}

// static PyCodeObject *
// assemble(struct compiler *c, int addNone)
// {
//     basicblock *b, *entryblock;
//     struct assembler a;
//     int i, j, nblocks;
//     PyCodeObject *co = NULL;

//     /* Make sure every block that falls off the end returns None.
//        XXX NEXT_BLOCK() isn't quite right, because if the last
//        block ends with a jump or return b_next shouldn't set.
//      */
//     if (!c->u->u_curblock->b_return) {
//         NEXT_BLOCK(c);
//         if (addNone)
//             ADDOP_LOAD_CONST(c, Py_None);
//         ADDOP(c, RETURN_VALUE);
//     }

//     nblocks = 0;
//     entryblock = NULL;
//     for (b = c->u->u_blocks; b != NULL; b = b->b_list) {
//         nblocks++;
//         entryblock = b;
//     }

//     /* Set firstlineno if it wasn't explicitly set. */
//     if (!c->u->u_firstlineno) {
//         if (entryblock && entryblock->b_instr && entryblock->b_instr->i_lineno)
//             c->u->u_firstlineno = entryblock->b_instr->i_lineno;
//         else
//             c->u->u_firstlineno = 1;
//     }
//     if (!assemble_init(&a, nblocks, c->u->u_firstlineno))
//         goto error;
//     dfs(c, entryblock, &a, nblocks);

//     /* Can't modify the bytecode after computing jump offsets. */
//     assemble_jump_offsets(&a, c);

//     /* Emit code in reverse postorder from dfs. */
//     for (i = a.a_nblocks - 1; i >= 0; i--) {
//         b = a.a_postorder[i];
//         for (j = 0; j < b->b_iused; j++)
//             if (!assemble_emit(&a, &b->b_instr[j]))
//                 goto error;
//     }

//     if (_PyBytes_Resize(&a.a_lnotab, a.a_lnotab_off) < 0)
//         goto error;
//     if (_PyBytes_Resize(&a.a_bytecode, a.a_offset * sizeof(_Py_CODEUNIT)) < 0)
//         goto error;

//     co = makecode(c, &a);
//  error:
//     assemble_free(&a);
//     return co;
// }

// #undef PyAST_Compile
// PyCodeObject *
// PyAST_Compile(mod_ty mod, const char *filename, PyCompilerFlags *flags,
//               PyArena *arena)
// {
//     return PyAST_CompileEx(mod, filename, flags, -1, arena);
// }
