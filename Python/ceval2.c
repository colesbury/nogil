#include "Python.h"
#include "pycore_call.h"
#include "pycore_ceval.h"
#include "pycore_code.h"
#include "pycore_object.h"
#include "pycore_refcnt.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"
#include "pycore_qsbr.h"
#include "pycore_dict.h"

#include "code2.h"
#include "dictobject.h"
#include "frameobject.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"
#include "opcode2.h"
#include "ceval2_meta.h"
#include "genobject2.h"

#include <ctype.h>

#if 1
#define DEBUG_LABEL(name) __asm__ volatile("." #name ":" :: "r"(reserved));
#else
#define DEBUG_LABEL(name)
#endif

#define DEBUG_REGS 0

#define TARGET(name) \
    TARGET_##name: \
    DEBUG_LABEL(TARGET_##name);


#if defined(__clang__)
#define COLD_TARGET(name) TARGET_##name:
#elif defined(__GNUC__)
#define COLD_TARGET(name) TARGET_##name: __attribute__((cold));
#else
#define COLD_TARGET(name) TARGET_##name:
#endif

#define CALL_VM(call) \
    ts->next_instr = next_instr; \
    call; \
    regs = ts->regs

#define CALL_VM_DONT_SAVE_NEXT_INSTR(call) \
    call; \
    regs = ts->regs

#define FUNC_CALL_VM(call) \
    call; \
    regs = ts->regs; \
    BREAK_LIVE_RANGE(next_instr); \
    this_code = PyCode2_FromInstr(next_instr - 1);

// I HAVE DEFEATED COMPILER.
#if defined(__GNUC__)
#define BREAK_LIVE_RANGE(a) __asm__ volatile ("" : "=r"(a) : "0"(a));
#else
#define BREAK_LIVE_RANGE(a) ((void)(a))
#endif

// Existing (c. 2021) compilers (gcc, clang, icc, msvc) generate poor
// code for the combination of assignment to the accumulator and decref
// of the previous value. This macro works around the issue by splitting
// the live ranges of the relevant variables. By inhibiting some
// optimizations, we improve the generated code (!).
#define SET_ACC(val) do {   \
    Register _old = acc;    \
    BREAK_LIVE_RANGE(_old); \
    acc = val;              \
    BREAK_LIVE_RANGE(acc);  \
    DECREF(_old);           \
} while (0)

#define IS_EMPTY(acc) (acc.as_int64 == 0 || !IS_RC(acc))

#define DECREF_X(reg, CALL) do { \
    if (IS_RC(reg)) { \
        _Py_DECREF_TOTAL \
        PyObject *obj = (PyObject *)reg.as_int64; \
        if (LIKELY(_Py_ThreadMatches(obj, tid))) { \
            uint32_t refcount = obj->ob_ref_local; \
            refcount -= 4; \
            obj->ob_ref_local = refcount; \
            if (UNLIKELY(refcount == 0)) { \
                CALL(_Py_MergeZeroRefcount(obj)); \
            } \
        } \
        else { \
            CALL(vm_decref_shared(obj)); \
        } \
    } \
} while (0)

#define DECREF(reg) DECREF_X(reg, CALL_VM)

#define INCREF(reg) do { \
    if (IS_RC(reg)) { \
        _Py_INCREF_TOTAL \
        PyObject *obj = (PyObject *)reg.as_int64; \
        if (LIKELY(_Py_ThreadMatches(obj, tid))) { \
            uint32_t refcount = obj->ob_ref_local; \
            refcount += 4; \
            obj->ob_ref_local = refcount; \
        } \
        else { \
            _Py_atomic_add_uint32(&obj->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT)); \
        } \
    } \
} while (0)

#define _Py_INCREF(op) do { \
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local); \
    if (!_Py_REF_IS_IMMORTAL(local)) { \
        _Py_INCREF_TOTAL \
        if (_PY_LIKELY(_Py_ThreadMatches(op, tid))) { \
            local += (1 << _Py_REF_LOCAL_SHIFT); \
            _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local); \
        } \
        else { \
            _Py_atomic_add_uint32(&op->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT)); \
        } \
    } \
} while(0)

#define _Py_DECREF(op) do { \
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local); \
    if (!_Py_REF_IS_IMMORTAL(local)) { \
        _Py_DECREF_TOTAL \
        if (LIKELY(_Py_ThreadMatches(op, tid))) { \
            uint32_t refcount = op->ob_ref_local; \
            refcount -= 4; \
            op->ob_ref_local = refcount; \
            if (UNLIKELY(refcount == 0)) { \
                CALL_VM(_Py_MergeZeroRefcount(op)); \
            } \
        } \
        else { \
            CALL_VM(vm_decref_shared(op)); \
        } \
    } \
} while(0)

// Clears and DECREFs the regsters from [-1, N) where N is usually
// the number of local variables.
// NOTE: The next_instr MUST be saved before calling CLEAR_REGISTERS.
// This allows us to skip saving it during the DECREF calls,
// which typically allows the compiler to re-use the register
// normally allocated to next_instr.
#define CLEAR_REGISTERS(N) do {                             \
    assert(ts->next_instr == next_instr);                   \
    Py_ssize_t _n = (N);                                    \
    do {                                                    \
        _n--;                                               \
        Register r = regs[_n];                              \
        regs[_n].as_int64 = 0;                              \
        if (r.as_int64 != 0) {                              \
            DECREF_X(r, CALL_VM_DONT_SAVE_NEXT_INSTR);      \
        }                                                   \
    } while (_n >= 0);                                      \
} while (0)


#define USE_REGISTER(value, reg) do { \
        register __typeof__(value) reg asm(#reg) = value; \
        __asm__ volatile("" :: "r"(reg)); \
    } while (0)


#define NEXTOPARG() do { \
        opD = *next_instr; \
        opcode = opD & 0xFF; \
        opA = (opD >> 8) & 0xFF; \
        opD >>= 16; \
        next_instr++; \
    } while (0)

#define RELOAD_OPD() (*(next_instr - 1) >> 16)
#define RELOAD_OPA() ((*(next_instr - 1) >> 8) & 0xFF)

#define DISPATCH(name) \
    NEXTOPARG(); \
    __asm__ volatile("# computed goto " #name); \
    goto *opcode_targets[opcode]

#define THIS_FUNC() \
    ((PyFunc *)AS_OBJ(regs[-1]))

#define THIS_CODE() \
    (PyCode2_FromInstr(THIS_FUNC()->func_base.first_instr))

#define CONSTANTS() \
    ((PyObject **)regs[-3].as_int64)

static const Register primitives[3] = {
    {(intptr_t)Py_False + NO_REFCOUNT_TAG},
    {(intptr_t)Py_True + NO_REFCOUNT_TAG},
    {(intptr_t)Py_None + NO_REFCOUNT_TAG},
};

// Frame layout:
// regs[-3] = constants
// regs[-2] = <frame_link>
// regs[-1] = <function>
// regs[0] = first local | locals dict

// LLINT (JSCORE)
// Call Frame points to regs
// regs[0] = caller
// regs[1] = returnPC
// regs[2] = code block
// regs[3] = nargs
// regs[4] = this
// regs[5] = arg0
// ...
// code block ->
//   ...
//   pointer to constants array
//
// so loading constant at index N
// tmp = load regs[2]
// tmp = load tmp[offsetof constnt]
// tmp = load tmp[N]

PyObject*
__attribute__((optimize("-fno-tree-loop-distribute-patterns")))
_PyEval_Fast(struct ThreadState *ts, Py_ssize_t nargs_, const uint32_t *pc)
{
    #include "opcode_targets2.h"
    if (UNLIKELY(!ts->ts->opcode_targets[0])) {
        memcpy(ts->ts->opcode_targets, opcode_targets_base, sizeof(opcode_targets_base));
    }

    const uint32_t *next_instr = pc;
    intptr_t opcode;
    intptr_t opA;
    intptr_t opD;
    Register acc = {nargs_};
    Register *regs = ts->regs;
    void **opcode_targets = ts->ts->opcode_targets;
    uintptr_t tid = _Py_ThreadId();
    intptr_t reserved = 0;
    // uint16_t *metadata = ts->metadata;
    // callee saved: rbx,rbp,r12,r13,r14,r15
    // 6 usable registers (+ rsp, which isn't directly usable)

    // WebKit Saved:
    // regs (cfr)
    // next_instr (PB, well half of it)
    // tagTypeNumber
    // tagMask
    // wasmInstance
    // metadataTable

    // LuaJIT saved:
    // constants (KBASE)
    // PC
    // opcode_targets

    // We want saved:
    // next_instr (pc)
    // opcode_targets
    // ts
    // acc
    // maybe: tid
    // maybe: constants/metadata
    // intptr_t tid = _Py_ThreadId(), tid2 = _Py_ThreadId() * 99;

    // NOTE: after memcpy call!
    DISPATCH(INITIAL);

    TARGET(LOAD_CONST) {
        acc = PACK(CONSTANTS()[opA], NO_REFCOUNT_TAG);
        DISPATCH(LOAD_CONST);
    }

    TARGET(JUMP) {
        next_instr += opD - 0x8000;
        DISPATCH(JUMP);
    }

    TARGET(POP_JUMP_IF_FALSE) {
        CALL_VM(next_instr = vm_is_false(acc, next_instr, opD));
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(POP_JUMP_IF_FALSE);
    }

    TARGET(POP_JUMP_IF_TRUE) {
        CALL_VM(next_instr = vm_is_true(acc, next_instr, opD));
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(POP_JUMP_IF_TRUE);
    }

    TARGET(JUMP_IF_FALSE) {
        CALL_VM(next_instr = vm_is_false(acc, next_instr, opD));
        DISPATCH(JUMP_IF_FALSE);
    }

    TARGET(JUMP_IF_TRUE) {
        CALL_VM(next_instr = vm_is_true(acc, next_instr, opD));
        DISPATCH(JUMP_IF_FALSE);
    }

    TARGET(FUNC_HEADER) {
        // opA contains framesize
        // acc contains nargs from call
        int err;
        assert(ts->regs == regs);

        if (UNLIKELY(regs + opA > ts->maxstack)) {
            // resize the virtual stack
            CALL_VM(err = vm_resize_stack(ts, opA));
            if (UNLIKELY(err != 0)) {
                acc.as_int64 = 0;
                goto error;
            }
        }

        PyCodeObject2 *this_code = PyCode2_FromInstr(next_instr - 1);
        regs[-3].as_int64 = (intptr_t)this_code->co_constants;

        // fast path if no keyword arguments, cells, or freevars and number
        // of positional arguments exactly matches.
        if (LIKELY((uint32_t)acc.as_int64 == this_code->co_packed_flags)) {
            goto dispatch_func_header;
        }

        ts->next_instr = next_instr;
        if ((acc.as_int64 & (ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS)) != 0) {
            // call passed arguments as tuple and keywords as dict
            // TODO: update acc to avoid checking all args for defaults
            FUNC_CALL_VM(err = vm_setup_ex(ts, this_code, acc));
            if (UNLIKELY(err != 0)) {
                acc.as_int64 = 0;
                goto error;
            }
            goto setup_default_args;
        }

        if (UNLIKELY((this_code->co_packed_flags & CODE_FLAG_VARARGS) != 0)) {
            FUNC_CALL_VM(err = vm_setup_varargs(ts, this_code, acc));
            if (UNLIKELY(err != 0)) {
                acc.as_int64 = 0;
                goto error;
            }
            Py_ssize_t posargs = (acc.as_int64 & ACC_MASK_ARGS);
            if (posargs > this_code->co_argcount) {
                acc.as_int64 -= posargs - this_code->co_argcount;
            }
        }
        else if ((acc.as_int64 & ACC_MASK_ARGS) > this_code->co_argcount) {
            FUNC_CALL_VM(too_many_positional(ts, acc.as_int64 & ACC_MASK_ARGS));
            acc.as_int64 = 0;
            goto error;
        }

        if (UNLIKELY((this_code->co_packed_flags & CODE_FLAG_VARKEYWORDS) != 0)) {
            // if the function uses **kwargs, create and store the dict
            PyObject *kwdict;
            FUNC_CALL_VM(kwdict = PyDict_New());
            if (UNLIKELY(kwdict == NULL)) {
                acc.as_int64 = 0;
                goto error;
            }
            Py_ssize_t pos = this_code->co_totalargcount;
            if ((this_code->co_packed_flags & CODE_FLAG_VARARGS) != 0) {
                pos += 1;
            }
            assert(regs[pos].as_int64 == 0);
            regs[pos] = PACK(kwdict, REFCOUNT_TAG);
        }

        if (acc.as_int64 & ACC_MASK_KWARGS) {
            assert(!IS_RC(regs[-FRAME_EXTRA - 1]));
            PyObject **kwnames = _PyTuple_ITEMS(AS_OBJ(regs[-FRAME_EXTRA - 1]));
            regs[-FRAME_EXTRA - 1].as_int64 = 0;

            Py_ssize_t total_args = this_code->co_totalargcount;
            while ((acc.as_int64 & ACC_MASK_KWARGS) != 0) {
                PyObject *keyword = *kwnames;

                /* Speed hack: do raw pointer compares. As names are
                   normally interned this should almost always hit. */
                Py_ssize_t j;
                for (j = this_code->co_posonlyargcount; j < total_args; j++) {
                    PyObject *name = PyTuple_GET_ITEM(this_code->co_varnames, j);
                    if (name == keyword) {
                        goto kw_found;
                    }
                }

                // keyword not found: might be missing or just not interned.
                // fall back to slower set-up path.
                FUNC_CALL_VM(err = vm_setup_kwargs(ts, this_code, acc, kwnames));
                if (UNLIKELY(err == -1)) {
                    acc.as_int64 = 0;
                    goto error;
                }
                break;

            kw_found:
                if (UNLIKELY(regs[j].as_int64 != 0)) {
                    FUNC_CALL_VM(duplicate_keyword_argument(ts, this_code, keyword));
                    acc.as_int64 = 0;
                    goto error;
                }

                Py_ssize_t kwdpos = -FRAME_EXTRA - ACC_KWCOUNT(acc) - 1;
                regs[j] = regs[kwdpos];
                regs[kwdpos].as_int64 = 0;
                acc.as_int64 -= (1 << ACC_SHIFT_KWARGS);
                kwnames++;
            }
        }

      setup_default_args: ;
        Py_ssize_t total_args = this_code->co_totalargcount;
        Py_ssize_t co_required_args = total_args - this_code->co_ndefaultargs;

        // Check for missing required arguments
        Py_ssize_t i = acc.as_int64 & ACC_MASK_ARGS;
        for (; i < co_required_args; i++) {
            if (UNLIKELY(regs[i].as_int64 == 0)) {
                FUNC_CALL_VM(missing_arguments(ts, acc.as_int64));
                acc.as_int64 = 0;
                goto error;
            }
        }

        // Fill in missing arguments with default values
        for (; i < total_args; i++) {
            if (regs[i].as_int64 == 0) {
                PyObject *deflt = THIS_FUNC()->freevars[i - co_required_args];
                if (UNLIKELY(deflt == NULL)) {
                    // Required keyword-only arguments can come after optional
                    // arguments. These arguments have NULL default values to
                    // signify that they are required.
                    FUNC_CALL_VM(missing_arguments(ts, acc.as_int64));
                    acc.as_int64 = 0;
                    goto error;
                }
                regs[i] = PACK(deflt, NO_REFCOUNT_TAG);
            }
        }

        // Convert variables to cells (if necessary) and load freevars from func
        if ((this_code->co_packed_flags & CODE_FLAG_HAS_CELLS) != 0) {
            int err;
            FUNC_CALL_VM(err = vm_setup_cells(ts, this_code));
            if (UNLIKELY(err != 0)) {
                acc.as_int64 = 0;
                goto error;
            }
        }
        if ((this_code->co_packed_flags & CODE_FLAG_HAS_FREEVARS) != 0) {
            PyFunc *this_func = THIS_FUNC();
            Py_ssize_t nfreevars = this_code->co_nfreevars;
            for (Py_ssize_t i = this_code->co_ndefaultargs; i < nfreevars; i++) {
                Py_ssize_t r = this_code->co_free2reg[i*2+1];
                PyObject *cell = this_func->freevars[i];
                assert(PyCell_Check(cell));
                regs[r] = PACK(cell, NO_REFCOUNT_TAG);
            }
        }

      dispatch_func_header:
        acc.as_int64 = 0;
        DISPATCH(FUNC_HEADER);
    }

    TARGET(METHOD_HEADER) {
        PyMethod *meth = (PyMethod *)AS_OBJ(regs[-1]);
        if ((acc.as_int64 & ACC_FLAG_VARARGS) != 0) {
            // TODO: would be nice to only use below case by handling hybrid call formats.
            PyObject *args = AS_OBJ(regs[-FRAME_EXTRA - 2]);
            assert(PyTuple_Check(args));
            Register res;
            CALL_VM(res = vm_tuple_prepend(args, meth->im_self));
            if (UNLIKELY(res.as_int64 == 0)) {
                goto error;
            }
            Register tmp = regs[-FRAME_EXTRA - 2];
            regs[-FRAME_EXTRA - 2] = res;
            DECREF(tmp);
            meth = (PyMethod *)AS_OBJ(regs[-1]);
        }
        else {
            // insert "self" as first argument
            Py_ssize_t n = ACC_ARGCOUNT(acc);
            while (n != 0) {
                regs[n] = regs[n - 1];
                n--;
            }
            regs[0] = PACK_INCREF(meth->im_self);
            acc.as_int64 += 1;
        }
        // tail call dispatch to underlying func
        PyObject *func = meth->im_func;
        next_instr = ((PyFuncBase *)func)->first_instr;
        Register tmp = regs[-1];
        regs[-1] = PACK_INCREF(func);
        DECREF(tmp);
        DISPATCH(METHOD_HEADER);
    }

    TARGET(CFUNC_HEADER) {
        PyObject *res;
        regs[-3].as_int64 = ACC_ARGCOUNT(acc);  // frame size
        CALL_VM(res = vm_call_cfunction(ts, acc));
        if (UNLIKELY(res == NULL)) {
            acc.as_int64 = 0;
            goto error;
        }
        acc = PACK_OBJ(res);
        CLEAR_REGISTERS(regs[-3].as_int64);
        next_instr = (const uint32_t *)regs[-2].as_int64;
        intptr_t frame_delta = regs[-4].as_int64;
        regs[-2].as_int64 = 0;
        regs[-3].as_int64 = 0;
        regs[-4].as_int64 = 0;
        regs -= frame_delta;
        ts->regs = regs;
        DISPATCH(CFUNC_HEADER);
    }

    TARGET(FUNC_TPCALL_HEADER) {
        PyObject *res;
        regs[-3].as_int64 = ACC_ARGCOUNT(acc);  // frame size
        // steals arguments
        CALL_VM(res = vm_tpcall_function(ts, acc));
        if (UNLIKELY(res == NULL)) {
            acc.as_int64 = 0;
            goto error;
        }
        acc = PACK_OBJ(res);
        CLEAR_REGISTERS(regs[-3].as_int64);
        next_instr = (const uint32_t *)regs[-2].as_int64;
        intptr_t frame_delta = regs[-4].as_int64;
        regs[-2].as_int64 = 0;
        regs[-3].as_int64 = 0;
        regs[-4].as_int64 = 0;
        regs -= frame_delta;
        ts->regs = regs;
        DISPATCH(FUNC_TPCALL_HEADER);
    }

    TARGET(GENERATOR_HEADER) {
        // setup generator?
        // copy arguments
        // return
        assert(IS_EMPTY(acc));
        PyGenObject2 *gen;
        CALL_VM(gen = PyGen2_NewWithSomething(ts));
        if (gen == NULL) {
            goto error;
        }
        PyGen2_SetNextInstr(gen, next_instr);
        acc = PACK_OBJ((PyObject *)gen);
        goto TARGET_RETURN_VALUE;
        DISPATCH(GENERATOR_HEADER);
    }

    TARGET(MAKE_FUNCTION) {
        PyCodeObject2 *code = (PyCodeObject2 *)CONSTANTS()[opA];
        CALL_VM(acc = vm_make_function(ts, code));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(MAKE_FUNCTION);
    }

    TARGET(CALL_METHOD) {
        assert(IS_EMPTY(acc));
        if (regs[opA].as_int64 == 0) {
            // If the LOAD_METHOD didn't provide us with a "self" then we
            // need to shift each argument down one. Note that opD >= 1.
            assert(opD >= 1);
            Register *low = &regs[opA];
            Register *hi = &regs[opA + opD];
            do {
                *low = *(low +1);
                low++;
            } while (low != hi);
            opD -= 1;
        }
        goto TARGET_CALL_FUNCTION;
    }

    TARGET(CALL_FUNCTION) {
        // opD = (kwargs << 8) | nargs
        // regs[opA - 4] = <empty> (frame delta)
        // regs[opA - 3] = <empty> (constants)
        // regs[opA - 2] = <empty> (frame link)
        // regs[opA - 1] = func
        // regs[opA + 0] = arg0
        // regs[opA + n] = argsN
        assert(IS_EMPTY(acc));
        PyObject *callable = AS_OBJ(regs[opA - 1]);
        regs = &regs[opA];
        ts->regs = regs;
        regs[-4].as_int64 = opA;    // frame delta
        regs[-2].as_int64 = (intptr_t)next_instr;
        acc.as_int64 = opD;
        if (!PyType_HasFeature(Py_TYPE(callable), Py_TPFLAGS_FUNC_INTERFACE)) {
            goto call_object;
        }
        next_instr = ((PyFuncBase *)callable)->first_instr;
        DISPATCH(CALL_FUNCTION);
    }

    call_object: {
        DEBUG_LABEL(call_object);
        regs[-3].as_int64 = ACC_ARGCOUNT(acc);  // frame size
        PyObject *res;
        CALL_VM(res = vm_call_function(ts, acc));
        if (UNLIKELY(res == NULL)) {
            // is this ok? do we need to adjust frame first?
            acc.as_int64 = 0;
            goto error;
        }
        acc = PACK_OBJ(res);
        CLEAR_REGISTERS(regs[-3].as_int64);
        next_instr = (const uint32_t *)regs[-2].as_int64;
        intptr_t frame_delta = regs[-4].as_int64;
        regs[-2].as_int64 = 0;
        regs[-3].as_int64 = 0; // should already be zero?
        regs[-4].as_int64 = 0;
        regs -= frame_delta;
        ts->regs = regs;
        DISPATCH(call_object);
    }

    TARGET(CALL_FUNCTION_EX) {
        // opA - 6 = *args
        // opA - 5 = **kwargs
        // opA - 4 = <empty> (frame delta)
        // opA - 3 = <empty> (constants/frame size)
        // opA - 2 = <empty> (frame link)
        // opA - 1 = func
        assert(IS_EMPTY(acc));
        PyObject *callable = AS_OBJ(regs[opA - 1]);
        regs = &regs[opA];
        ts->regs = regs;
        regs[-4].as_int64 = opA;  // frame delta
        regs[-2].as_int64 = (intptr_t)next_instr;
        if (!PyType_HasFeature(Py_TYPE(callable), Py_TPFLAGS_FUNC_INTERFACE)) {
            goto call_object_ex;
        }
        acc.as_int64 = ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS;
        next_instr = ((PyFuncBase *)callable)->first_instr;
        DISPATCH(CALL_FUNCTION_EX);
    }

    call_object_ex: {
        DEBUG_LABEL(call_object_ex);
        assert(regs[-3].as_int64 == 0 && "frame size not zero");
        PyObject *callable = AS_OBJ(regs[-1]);
        PyObject *args = AS_OBJ(regs[-FRAME_EXTRA - 2]);
        PyObject *kwargs = AS_OBJ(regs[-FRAME_EXTRA - 1]);
        PyObject *res;
        CALL_VM(res = PyObject_Call(callable, args, kwargs));
        if (UNLIKELY(res == NULL)) {
            // is this ok? do we need to adjust frame first?
            goto error;
        }
        acc = PACK_OBJ(res);
        DECREF(regs[-FRAME_EXTRA - 1]);  // clear **kwargs
        regs[-FRAME_EXTRA - 1].as_int64 = 0;
        DECREF(regs[-FRAME_EXTRA - 2]);  // clear *args
        regs[-FRAME_EXTRA - 2].as_int64 = 0;
        next_instr = (const uint32_t *)regs[-2].as_int64;
        intptr_t frame_delta = regs[-4].as_int64;
        regs[-2].as_int64 = 0;
        // regs[-3] is already zero
        regs[-4].as_int64 = 0;
        regs -= frame_delta;
        ts->regs = regs;
        DISPATCH(call_object_ex);
    }

    TARGET(YIELD_VALUE) {
        PyGenObject2 *gen = PyGen2_FromThread(ts);
        gen->status = GEN_YIELD;
        ts->next_instr = next_instr;
        // assert((regs[-2].as_int64 & FRAME_C) != 0);
        goto return_to_c;
    }

    TARGET(RETURN_VALUE) {
        ts->next_instr = next_instr;
        CLEAR_REGISTERS(THIS_CODE()->co_nlocals);
        intptr_t frame_link = regs[-2].as_int64;
        intptr_t frame_delta = regs[-4].as_int64;
        regs[-2].as_int64 = 0;
        regs[-3].as_int64 = 0;
        regs[-4].as_int64 = 0;
        regs -= frame_delta;
        ts->regs = regs;
        if (UNLIKELY((frame_link & FRAME_TAG_MASK) != FRAME_PYTHON)) {
            intptr_t tag = frame_link & FRAME_TAG_MASK;
            if (tag == FRAME_C) {
                ts->next_instr = (const uint32_t *)(frame_link & ~FRAME_TAG_MASK);
                goto return_to_c;
            }
            else if (tag == FRAME_GENERATOR) {
                goto generator_return_to_c;
            }
            else {
                __builtin_unreachable();
            }
        }
        next_instr = (const uint32_t *)frame_link;
        DISPATCH(RETURN_VALUE);
    }

    TARGET(LOAD_NAME) {
        assert(IS_EMPTY(acc));
        PyObject *name = CONSTANTS()[opA];
        CALL_VM(acc = vm_load_name(regs, name));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(LOAD_NAME);
    }

    TARGET(LOAD_GLOBAL) {
        assert(IS_EMPTY(acc));
        PyObject *name = CONSTANTS()[opA];
        PyObject *globals = THIS_FUNC()->globals;
        PyObject *value;
        // FIXME: need to check errors and that globals/builtins are exactly dicts
        CALL_VM(value = PyDict_GetItemWithError2(globals, name));
        if (value == NULL) {
            PyObject *builtins = THIS_FUNC()->builtins;
            CALL_VM(value = PyDict_GetItemWithError2(builtins, name));
            if (UNLIKELY(value == NULL)) {
                CALL_VM(vm_name_error(ts, name));
                // explicitly zero the accumulator value to allow the compiler
                // to re-use the acc register for "name" (because the acc isn't
                // used within LOAD_GLOBAL and overwritten at all exits.
                acc.as_int64 = 0;
                goto error;
            }
        }
        acc = PACK_OBJ(value);
        DISPATCH(LOAD_GLOBAL);
    }

    TARGET(LOAD_ATTR) {
        assert(IS_EMPTY(acc));
        PyObject *name = CONSTANTS()[opD];
        PyObject *owner = AS_OBJ(regs[opA]);
        PyObject *res;
        CALL_VM(res = _PyObject_GetAttrFast(owner, name));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        acc = PACK_OBJ(res);
        DISPATCH(LOAD_ATTR);
    }

    TARGET(LOAD_METHOD) {
        PyObject *name = CONSTANTS()[opD];
        PyObject *owner = AS_OBJ(acc);
        int err;
        CALL_VM(err = vm_load_method(ts, owner, name, opA));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(LOAD_METHOD);
    }

    TARGET(STORE_NAME) {
        PyObject *name = CONSTANTS()[opA];
        PyObject *locals = AS_OBJ(regs[0]);
        int err;
        CALL_VM(err = vm_store_global(locals, name, acc));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        acc.as_int64 = 0;
        DISPATCH(STORE_NAME);
    }

    TARGET(STORE_GLOBAL) {
        PyObject *name = CONSTANTS()[opA];
        PyObject *globals = THIS_FUNC()->globals;
        PyObject *value = AS_OBJ(acc);
        int err;
        CALL_VM(err = PyDict_SetItem(globals, name, value));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(STORE_GLOBAL);
    }

    TARGET(STORE_SUBSCR) {
        PyObject *container = AS_OBJ(regs[opA]);
        PyObject *sub = AS_OBJ(regs[opD]);
        PyObject *value = AS_OBJ(acc);
        int err;
        CALL_VM(err = PyObject_SetItem(container, sub, value));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(STORE_SUBSCR);
    }

    TARGET(STORE_ATTR) {
        PyObject *owner = AS_OBJ(regs[opA]);
        PyObject *name = CONSTANTS()[opD];
        PyObject *value = AS_OBJ(acc);
        int err;
        CALL_VM(err = PyObject_SetAttr(owner, name, value));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(STORE_ATTR);
    }

    TARGET(LOAD_FAST) {
        assert(IS_EMPTY(acc));
        acc = regs[opA];
        INCREF(acc);
        DISPATCH(LOAD_FAST);
    }

    TARGET(STORE_FAST) {
        Register old = regs[opA];
        regs[opA] = acc;
        acc.as_int64 = 0;
        if (old.as_int64) {
            DECREF(old);
        }
        DISPATCH(STORE_FAST);
    }

    TARGET(MOVE) {
        Register r = regs[opA];
        regs[opA] = regs[opD];
        regs[opD].as_int64 = 0;
        if (r.as_int64 != 0) {
            DECREF(r);
        }
        DISPATCH(MOVE);
    }

    TARGET(COPY) {
        assert(IS_EMPTY(regs[opA]));
        // FIXME: is this only used for aliases???
        regs[opA].as_int64 = regs[opD].as_int64 | NO_REFCOUNT_TAG;
        DISPATCH(COPY);
    }

    TARGET(CLEAR_FAST) {
        Register r = regs[opA];
        regs[opA].as_int64 = 0;
        if (r.as_int64 != 0) {
            DECREF(r);
        }
        DISPATCH(CLEAR_FAST);
    }

    TARGET(CLEAR_ACC) {
        Register r = acc;
        acc.as_int64 = 0;
        if (r.as_int64 != 0) {
            DECREF(r);
        }
        DISPATCH(CLEAR_ACC);
    }

    TARGET(LOAD_DEREF) {
        assert(IS_EMPTY(acc));
        PyObject *cell = AS_OBJ(regs[opA]);
        PyObject *value = PyCell_GET(cell);
        if (UNLIKELY(value == NULL)) {
            // FIXME: error
        }
        acc = PACK_INCREF(value);
        DISPATCH(LOAD_DEREF);
    }

    TARGET(STORE_DEREF) {
        PyObject *cell = AS_OBJ(regs[opA]);
        PyObject *value = AS_OBJ(acc);
        if (!IS_RC(acc)) {
            _Py_INCREF(value);
        }
        PyObject *prev = PyCell_GET(cell);
        PyCell_SET(cell, value);
        acc.as_int64 = 0;
        if (prev != NULL) {
            _Py_DECREF(prev);
        }
        DISPATCH(STORE_DEREF);
    }

    TARGET(LOAD_CLASSDEREF) {
        assert(IS_EMPTY(acc));
        PyObject *name = CONSTANTS()[opD];
        CALL_VM(acc = vm_load_class_deref(ts, opA, name));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(LOAD_CLASSDEREF);
    }

    TARGET(DELETE_FAST) {
        Register r = regs[opA];
        if (UNLIKELY(r.as_int64 == 0)) {
            // FIXME: name error
            goto error;
        }
        regs[opA].as_int64 = 0;
        DECREF(r);
        DISPATCH(DELETE_FAST);
    }

    TARGET(DELETE_NAME) {
        assert(IS_EMPTY(acc));
        PyObject *name = CONSTANTS()[opA];
        int err;
        CALL_VM(err = vm_delete_name(ts, name));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DISPATCH(DELETE_NAME);
    }

    TARGET(DELETE_GLOBAL) {
        PyObject *globals = THIS_FUNC()->globals;
        PyObject *name = CONSTANTS()[opA];
        int err;
        CALL_VM(err = PyDict_DelItem(globals, name));
        if (UNLIKELY(err != 0)) {
            // FIXME: convert KeyError to NameError
            goto error;
        }
        DISPATCH(DELETE_GLOBAL);
    }

    TARGET(DELETE_ATTR) {
        PyObject *owner = AS_OBJ(acc);
        PyObject *name = CONSTANTS()[opA];
        int err;
        CALL_VM(err = PyObject_SetAttr(owner, name, (PyObject *)NULL));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(DELETE_ATTR);
    }

    TARGET(DELETE_SUBSCR) {
        PyObject *container = AS_OBJ(regs[opA]);
        PyObject *sub = AS_OBJ(acc);
        int err;
        CALL_VM(err = PyObject_DelItem(container, sub));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(DELETE_SUBSCR);
    }

    TARGET(DELETE_DEREF) {
        PyObject *cell = AS_OBJ(regs[opA]);
        assert(PyCell_Check(cell));
        PyObject *old = PyCell_GET(cell);
        if (UNLIKELY(old == NULL)) {
            // TODO: name error
            goto error;
        }
        PyCell_SET(cell, NULL);
        _Py_DECREF(old);
        DISPATCH(DELETE_DEREF);
    }

    TARGET(COMPARE_OP) {
        assert(opA <= Py_GE);
        PyObject *left = AS_OBJ(regs[opD]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyObject_RichCompare(left, right, opA));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(COMPARE_OP);
    }

    TARGET(IS_OP) {
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        Register res = primitives[(left == right)];
        SET_ACC(res);
        DISPATCH(IS_OP);
    }

    TARGET(CONTAINS_OP) {
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        int cmp;
        CALL_VM(cmp = PySequence_Contains(right, left));
        if (UNLIKELY(cmp < 0)) {
            goto error;
        }
        SET_ACC(primitives[cmp]);
        DISPATCH(CONTAINS_OP);
    }

    TARGET(UNARY_POSITIVE) {
        PyObject *value = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Positive(value));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(UNARY_POSITIVE);
    }

    TARGET(UNARY_NEGATIVE) {
        PyObject *value = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Negative(value));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(UNARY_NEGATIVE);
    }

    TARGET(UNARY_INVERT) {
        PyObject *value = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Invert(value));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(UNARY_INVERT);
    }

    TARGET(UNARY_NOT) {
        PyObject *value = AS_OBJ(acc);
        int is_true;
        CALL_VM(is_true = PyObject_IsTrue(value));
        if (UNLIKELY(is_true < 0)) {
            goto error;
        }
        SET_ACC(primitives[!is_true]);
        DISPATCH(UNARY_NOT);
    }

    TARGET(UNARY_NOT_FAST) {
        assert(PyBool_Check(AS_OBJ(acc)) && !IS_RC(acc));
        int is_false = (acc.as_int64 == primitives[0].as_int64);
        acc = primitives[is_false];
        DISPATCH(UNARY_NOT_FAST);
    }

    TARGET(BINARY_ADD) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Add(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_ADD);
    }

    TARGET(BINARY_SUBTRACT) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Subtract(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_SUBTRACT);
    }

    TARGET(BINARY_MULTIPLY) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Multiply(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_MULTIPLY);
    }

    TARGET(BINARY_MODULO) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Remainder(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_MODULO);
    }

    TARGET(BINARY_TRUE_DIVIDE) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_TrueDivide(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_TRUE_DIVIDE);
    }

    TARGET(BINARY_FLOOR_DIVIDE) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_FloorDivide(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_FLOOR_DIVIDE);
    }

    TARGET(BINARY_POWER) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Power(left, right, Py_None));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_POWER);
    }

    TARGET(BINARY_MATRIX_MULTIPLY) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_MatrixMultiply(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_MATRIX_MULTIPLY);
    }

    TARGET(BINARY_LSHIFT) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Lshift(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_LSHIFT);
    }

    TARGET(BINARY_RSHIFT) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Rshift(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_RSHIFT);
    }

    TARGET(BINARY_AND) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_And(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_AND);
    }

    TARGET(BINARY_XOR) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Xor(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_XOR);
    }

    TARGET(BINARY_OR) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Or(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_OR);
    }

    TARGET(INPLACE_ADD) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceAdd(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_ADD);
    }

    TARGET(INPLACE_SUBTRACT) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceSubtract(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_SUBTRACT);
    }

    TARGET(INPLACE_MULTIPLY) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceMultiply(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_MULTIPLY);
    }

    TARGET(INPLACE_MODULO) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceRemainder(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_MODULO);
    }

    TARGET(INPLACE_TRUE_DIVIDE) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceTrueDivide(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_TRUE_DIVIDE);
    }

    TARGET(INPLACE_FLOOR_DIVIDE) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceFloorDivide(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_FLOOR_DIVIDE);
    }

    TARGET(INPLACE_POWER) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlacePower(left, right, Py_None));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_POWER);
    }

    TARGET(INPLACE_MATRIX_MULTIPLY) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceMatrixMultiply(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_MATRIX_MULTIPLY);
    }

    TARGET(INPLACE_LSHIFT) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceLshift(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_LSHIFT);
    }

    TARGET(INPLACE_RSHIFT) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceRshift(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_RSHIFT);
    }

    TARGET(INPLACE_AND) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceAnd(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_AND);
    }

    TARGET(INPLACE_XOR) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceXor(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_XOR);
    }

    TARGET(INPLACE_OR) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceOr(left, right));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(INPLACE_OR);
    }

    TARGET(BINARY_SUBSCR) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *container = AS_OBJ(regs[opA]);
        PyObject *sub = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyObject_GetItem(container, sub));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(BINARY_SUBSCR);
    }

    TARGET(IMPORT_NAME) {
        PyFunc *this_func = THIS_FUNC();
        PyObject *arg = CONSTANTS()[opA];
        CALL_VM(acc = vm_import_name(ts, this_func, arg));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(IMPORT_NAME);
    }

    TARGET(IMPORT_FROM) {
        PyObject *module = AS_OBJ(regs[opA]);
        PyObject *name = CONSTANTS()[opD];
        PyObject *res;
        CALL_VM(res = vm_import_from(ts, module, name));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        acc = PACK_OBJ(res);
        DISPATCH(IMPORT_FROM);
    }

    TARGET(GET_ITER) {
        assert(IS_OBJ(acc));
        PyObject *obj = AS_OBJ(acc);
        getiterfunc f = Py_TYPE(obj)->tp_iter;
        if (f == NULL) {
            goto get_iter_slow;
        }
        PyObject *iter;
        CALL_VM(iter = (*f)(obj));
        if (iter == NULL) {
            goto error;
        }
        if (Py_TYPE(iter)->tp_iternext == NULL) {
            goto error;
        }
        opA = RELOAD_OPA();
        assert(regs[opA].as_int64 == 0);
        regs[opA] = PACK_OBJ(iter);
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(GET_ITER);
        get_iter_slow:
            // FIXME
            goto error;
    }

    TARGET(FOR_ITER) {
        PyObject *iter = AS_OBJ(regs[opA]);
        PyObject *next;
        CALL_VM(next = (*Py_TYPE(iter)->tp_iternext)(iter));
        if (next == NULL) {
            if (UNLIKELY(ts->ts->curexc_type != NULL)) {
                int err;
                CALL_VM(err = vm_for_iter_exc(ts));
                if (err != 0) {
                    goto error;
                }
            }
            opA = RELOAD_OPA();
            Register r = regs[opA];
            regs[opA].as_int64 = 0;
            DECREF(r);
        }
        else {
            acc = PACK_OBJ(next);
            opD = RELOAD_OPD();
            next_instr += opD - 0x8000;
        }
        DISPATCH(FOR_ITER);
    }

    TARGET(BUILD_SLICE) {
        CALL_VM(acc = vm_build_slice(&regs[opA]));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(BUILD_SLICE);
    }

    TARGET(BUILD_LIST) {
        // opA = reg, opD = N
        CALL_VM(acc = vm_build_list(&regs[opA], opD));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(BUILD_LIST);
    }

    TARGET(BUILD_SET) {
        // opA = reg, opD = N
        CALL_VM(acc = vm_build_set(ts, opA, opD));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(BUILD_SET);
    }

    TARGET(BUILD_TUPLE) {
        // opA = reg, opD = N
        CALL_VM(acc = vm_build_tuple(ts, opA, opD));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(BUILD_TUPLE);
    }

    TARGET(BUILD_MAP) {
        assert(IS_EMPTY(acc));
        PyObject *res;
        CALL_VM(res = _PyDict_NewPresized(opA));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        acc = PACK(res, REFCOUNT_TAG);
        DISPATCH(BUILD_MAP);
    }

    TARGET(DICT_UPDATE) {
        PyObject *dict = AS_OBJ(regs[opA]);
        PyObject *update = AS_OBJ(acc);
        int err;
        CALL_VM(err = PyDict_Update(dict, update));
        if (UNLIKELY(err != 0)) {
            // TODO: update error message
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(DICT_UPDATE);
    }

    TARGET(DICT_MERGE) {
        PyObject *dict = AS_OBJ(regs[opA]);
        PyObject *update = AS_OBJ(acc);
        int err;
        CALL_VM(err = _PyDict_MergeEx(dict, update, 2));
        if (UNLIKELY(err != 0)) {
            // TODO: update error message
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(DICT_MERGE);
    }

    TARGET(LIST_APPEND) {
        PyObject *list = AS_OBJ(regs[opA]);
        PyObject *item = AS_OBJ(acc);
        int err;
        CALL_VM(err = PyList_Append(list, item));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(LIST_APPEND);
    }

    TARGET(LIST_EXTEND) {
        PyObject *list = AS_OBJ(regs[opA]);
        PyObject *iterable = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = _PyList_Extend((PyListObject *)list, iterable));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        assert(res == Py_None);
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(LIST_EXTEND);
    }

    TARGET(SET_ADD) {
        PyObject *set = AS_OBJ(regs[opA]);
        PyObject *item = AS_OBJ(acc);
        int err;
        CALL_VM(err = PySet_Add(set, item));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(SET_ADD);
    }

    TARGET(SET_UPDATE) {
        PyObject *set = AS_OBJ(regs[opA]);
        PyObject *iterable = AS_OBJ(acc);
        int err;
        CALL_VM(err = _PySet_Update(set, iterable));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(SET_UPDATE);
    }

    TARGET(UNPACK) {
        // iconstants[opA] = base, argcnt, argcntafter
        PyObject *seq = AS_OBJ(acc);
        Py_ssize_t *args = &THIS_CODE()->co_iconstants[opA];
        int err;
        CALL_VM(err = vm_unpack(ts, seq, args[0], args[1], args[2]));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(UNPACK);
    }

    TARGET(LOAD_BUILD_CLASS) {
        PyObject *builtins = THIS_FUNC()->builtins;
        CALL_VM(vm_load_build_class(ts, builtins, opA));
        // FIXME: error return
        DISPATCH(LOAD_BUILD_CLASS);
    }

    TARGET(LOAD_EXC) {
        assert(IS_EMPTY(acc));
        acc = PACK_INCREF(ts->handled_exc);
        DISPATCH(LOAD_EXC);
    }

    TARGET(RAISE) {
        PyObject *exc = AS_OBJ(acc);
        acc.as_int64 = 0;
        CALL_VM(vm_raise(ts, exc));
        goto exception_unwind;
    }

    TARGET(RERAISE) {
        PyObject *exc = ts->handled_exc;
        ts->handled_exc = AS_OBJ(regs[opA]);
        regs[opA].as_int64 = 0;
        CALL_VM(vm_raise(ts, exc));
        goto exception_unwind;
    }

    TARGET(JUMP_IF_NOT_EXC_MATCH) {
        PyObject *exc = AS_OBJ(acc);
        const uint32_t *target;
        CALL_VM(target = vm_exc_match(ts, exc, next_instr, opD));
        if (UNLIKELY(target == NULL)) {
            goto error;
        }
        next_instr = target;
        BREAK_LIVE_RANGE(next_instr);
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(JUMP_IF_NOT_EXC_MATCH);
    }

    TARGET(END_EXCEPT) {
        if (regs[opA].as_int64 == -1) {
            PyObject *prev = ts->handled_exc;
            ts->handled_exc = AS_OBJ(regs[opA + 1]);
            regs[opA].as_int64 = 0;
            if (prev != NULL) {
                _Py_DECREF(prev);
            }
        }
        DISPATCH(END_EXCEPT);
    }

    TARGET(CALL_FINALLY) {
        regs[opA] = PACK(next_instr, NO_REFCOUNT_TAG);
        next_instr += opD - 0x8000;
        DISPATCH(CALL_FINALLY);
    }

    TARGET(END_FINALLY) {
        int64_t link_addr = regs[opA].as_int64;
        acc = regs[opA + 1];
        regs[opA].as_int64 = 0;
        regs[opA + 1].as_int64 = 0;
        if (link_addr != 0) {
            if (UNLIKELY(link_addr == -1)) {
                // re-raise the exception that caused us to enter the
                // finally block.
                PyObject *exc = ts->handled_exc;
                ts->handled_exc = AS_OBJ(acc);
                acc.as_int64 = 0;
                CALL_VM(vm_reraise(ts, exc));
                goto exception_unwind;
            }
            else {
                next_instr = (const uint32_t *)(link_addr & ~REFCOUNT_MASK);
            }
        }
        DISPATCH(END_FINALLY);
    }

    TARGET(SETUP_WITH) {
        regs[opA] = acc;
        CALL_VM(acc = vm_setup_with(ts, opA));
        if (UNLIKELY(acc.as_int64 == 0)) {
            goto error;
        }
        DISPATCH(SETUP_WITH);
    }

    TARGET(END_WITH) {
        assert(IS_EMPTY(acc));
        int err;
        CALL_VM(err = vm_exit_with(ts, opA));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        DISPATCH(END_WITH);
    }

    TARGET(LOAD_INTRINSIC) {
        assert(IS_EMPTY(acc));
        acc = PACK((opA << 1), NO_REFCOUNT_TAG);
        DISPATCH(LOAD_INTRINSIC);
    }

    TARGET(CALL_INTRINSIC_1) {
        intrinsic1 fn = intrinsics_table[opA].intrinsic1;
        PyObject *value = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = fn(value));
        if (UNLIKELY(res == NULL)) {
            goto error;
        }
        SET_ACC(PACK_OBJ(res));
        DISPATCH(CALL_INTRINSIC_1);
    }

    TARGET(CALL_INTRINSIC_N) {
        PyObject *res;
        intptr_t id = (acc.as_int64 >> 1);
        CALL_VM(res = vm_call_intrinsic(ts, id, opA, opD));
        if (UNLIKELY(res == NULL)) {
            acc.as_int64 = 0;
            goto error;
        }
        acc = PACK_OBJ(res);
        DISPATCH(CALL_INTRINSIC_N);
    }

    generator_return_to_c: {
        PyObject *obj = AS_OBJ(acc);
        if (!IS_RC(acc)) {
            _Py_INCREF(obj);
        }
        PyGenObject2 *gen = PyGen2_FromThread(ts);
        assert(gen != NULL);
        gen->status = GEN_FINISHED;
        gen->return_value = obj;
        return NULL;
    }

    return_to_c: {
        PyObject *obj = AS_OBJ(acc);
        if (!IS_RC(acc)) {
            _Py_INCREF(obj);
        }
        return obj;
    }

    error: {
        // TODO: normalize exception and create traceback.
        // CALL_VM(vm_handle_error(ts));
        CALL_VM(vm_traceback_here(ts));
        goto exception_unwind;
    }

    exception_unwind: {
        CALL_VM(next_instr = vm_exception_unwind(ts, next_instr));
        if (next_instr == 0) {
            return NULL;
        }
        DISPATCH(exception_unwind);
    }

    #include "unimplemented_opcodes.h"
    {
        // CALL_VM(acc = vm_unknown_opcode(opcode));
        // opcode = 0;
#ifdef Py_DEBUG
        printf("unimplemented opcode: %zd\n", opcode);
#endif
        abort();
        __builtin_unreachable();
    }
//

    COLD_TARGET(debug_regs) {
#if DEBUG_REGS
        __asm__ volatile (
            "# REGISTER ASSIGNMENT \n\t"
            "# opcode = %0 \n\t"
            "# opA = %1 \n\t"
            "# opD = %2 \n\t"
            "# regs = %5 \n\t"
            "# acc = %3 \n\t"
            "# next_instr = %4 \n\t"
            "# ts = %6 \n\t"
            "# opcode_targets = %7 \n\t"
            "# tid = %8 \n\t"
            "# reserved = %9 \n\t"
        ::
            "r" (opcode),
            "r" (opA),
            "r" (opD),
            "r" (acc),
            "r" (next_instr),
            "r" (regs),
            "r" (ts),
            "r" (opcode_targets),
            "r" (tid),
            "r" (reserved));
#endif
        DISPATCH(debug_regs);
    }

    __builtin_unreachable();
    // Py_RETURN_NONE;
}
