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

#include <setjmp.h>

struct compiler {
    PyObject *filename;
    struct symtable *st;
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

    PyObject *const_cache;     /* Python dict holding all constants,
                                    including names tuple */
    struct compiler_unit *u; /* compiler state for current block */
    PyObject *stack;           /* Python list holding compiler_unit ptrs */
    PyArena *arena;            /* pointer to memory allocation arena */
    jmp_buf jb;
};

static void compiler_init(struct compiler *c);
static void compiler_free(struct compiler *c);

static PyCodeObject2 *compiler_mod(struct compiler *, mod_ty);

#define COMPILER_ERROR(c) (longjmp(c->jb, 1))

static PyCodeObject2 *
compile_object(struct compiler *c, mod_ty mod, PyObject *filename,
               PyCompilerFlags *flags, int optimize, PyArena *arena)
{
    if (setjmp(c->jb) != 0) {
        assert(PyErr_Occurred());
        compiler_free(c);
        return NULL;
    }

    compiler_init(c);
    Py_INCREF(filename);
    c->filename = filename;
    c->arena = arena;
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
    c->optimize = optimize;
    c->nestlevel = 0;
    c->do_not_emit_bytecode = 0;

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

static PyCodeObject2 *
compiler_mod(struct compiler *c, mod_ty mod)
{
    static _Py_Identifier module_ident = _Py_static_string_init("<module>");
    PyObject *module_str = _PyUnicode_FromId(&module_ident);
    if (!module_str) {
        COMPILER_ERROR(c);
    }

    PyErr_Format(PyExc_RuntimeError, "compiler_mod NYI");
    COMPILER_ERROR(c);

    // compiler_enter_scope(c, module_str, COMPILER_SCOPE_MODULE, mod, /*lineno=*/0);
    // switch (mod->kind) {
    // case Module_kind:
    //     compiler_body(c, mod->v.Module.body);
    //     break;
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
    // default:
    //     PyErr_Format(PyExc_SystemError,
    //                  "module kind %d should not be possible",
    //                  mod->kind);
    //     COMPILER_ERROR(c);
    // }
    // co = assemble(c, addNone);
    // compiler_exit_scope(c);
    // return co;
}

static void
compiler_init(struct compiler *c)
{
    c->const_cache = PyDict_New();
    if (!c->const_cache) {
        COMPILER_ERROR(c);
    }

    c->stack = PyList_New(0);
    if (!c->stack) {
        COMPILER_ERROR(c);
    }
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
    Py_DECREF(c->stack);
}