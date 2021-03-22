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
#define DEBUG_LABEL(name) __asm__ volatile("." #name ":");
#else
#define DEBUG_LABEL(name)
#endif

#define DEBUG_REGS 0
#define DEBUG_FRAME 0

#if defined(__clang__)
#define COLD_TARGET(name) name:
#elif defined(__GNUC__)
#define COLD_TARGET(name) name: __attribute__((cold));
#else
#define COLD_TARGET(name) name:
#endif

#define CALL_VM(call) \
    ts->pc = pc; \
    call; \
    regs = ts->regs

#define CALL_VM_DONT_SAVE_NEXT_INSTR(call) \
    call; \
    regs = ts->regs

#define FUNC_CALL_VM(call) \
    call; \
    regs = ts->regs; \
    BREAK_LIVE_RANGE(pc); \
    this_code = PyCode2_FromInstr(pc);

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

#define XSET_ACC(val) do {   \
    Register _old = acc;    \
    BREAK_LIVE_RANGE(_old); \
    acc = val;              \
    BREAK_LIVE_RANGE(acc);  \
    if (_old.as_int64 != 0) \
        DECREF(_old);       \
} while (0)

#define SET_REG(dst, src) do {   \
    Register _old = dst;    \
    BREAK_LIVE_RANGE(_old); \
    dst = src;              \
    BREAK_LIVE_RANGE(dst);  \
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
            CALL(_Py_DecRefShared(obj)); \
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
            CALL_VM(_Py_DecRefShared(op)); \
        } \
    } \
} while(0)

#undef PACK_INCREF
#define PACK_INCREF(op) _PACK_INCREF(op, tid)


// Clears and DECREFs the regsters from [-1, N) where N is usually
// the number of local variables.
// NOTE: The CLEAR_REGISTERS macro saves pc to the thread state.
// This allows us to skip saving it during the DECREF calls,
// which typically allows the compiler to re-use the register
// normally allocated to pc.
#define CLEAR_REGISTERS(N) do {                             \
    ts->pc = pc;                                            \
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

#define XCONCAT(a, b, c) a ## b ## c
#define CONCAT(a, b, c) XCONCAT(a, b, c)

// OP_SIZE(LOAD_FAST) -> OP_SIZE_LOAD_FAST or OP_SIZE_WIDE_LOAD_FAST
#define OP_SIZE(name) CONCAT(OP_SIZE_, WIDTH_PREFIX, name)

// LABEL(ret) -> ret:  or wide_ret:
#define LABEL(name) CONCAT(WIDTH_PREFIX, name,)

#define NEXT_INSTRUCTION() \
    opcode = *pc; \
    /* __asm__ volatile("# computed goto " #name); */ \
    goto *opcode_targets[opcode]

#define JUMP_BY(arg) \
    pc += (arg); \
    NEXT_INSTRUCTION()

#define JUMP_TO(arg) \
    pc = (arg); \
    NEXT_INSTRUCTION()

#define DISPATCH(name) \
    pc += OP_SIZE(name); \
    NEXT_INSTRUCTION()

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

struct probe_result {
    Register acc;
    int found;
};

static inline struct probe_result
dict_probe(PyObject *op, PyObject *name, intptr_t guess, intptr_t tid);

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

_Py_ALWAYS_INLINE static inline uint16_t
load_uimm16(const uint8_t *addr)
{
    uint16_t r;
    memcpy(&r, addr, sizeof(r));
    return r;
}

_Py_ALWAYS_INLINE static inline uint32_t
load_uimm32(const uint8_t *addr)
{
    uint32_t r;
    memcpy(&r, addr, sizeof(r));
    return r;
}

PyObject*
__attribute__((optimize("-fno-tree-loop-distribute-patterns")))
_PyEval_Fast(struct ThreadState *ts, Py_ssize_t nargs_, const uint8_t *initial_pc)
{
    #include "opcode_targets2.h"
    if (UNLIKELY(!ts->ts->opcode_targets[0])) {
        memcpy(ts->ts->opcode_targets, opcode_targets_base, sizeof(opcode_targets_base));
        memcpy(ts->ts->opcode_targets + 128, wide_opcode_targets_base, 128 * sizeof(*wide_opcode_targets_base));
    }
    ts->ts->use_new_interp += 1;

    const uint8_t *pc = initial_pc;
    intptr_t opcode;
    Register acc = {nargs_};
    Register *regs = ts->regs;
    void **opcode_targets = ts->ts->opcode_targets;
    uintptr_t tid = _Py_ThreadId();

    // Dispatch to the first instruction
    NEXT_INSTRUCTION();

    #define WIDTH_PREFIX
    #define TARGET(name) name: DEBUG_LABEL(name);
    #define UImm(idx) (pc[idx + 1])
    #define UImm16(idx) (load_uimm16(&pc[idx + 1]))
    #define JumpImm(idx) ((int16_t)UImm16(idx))
    #include "ceval2_handlers.inc.c"
    TARGET(WIDE) {
        opcode = pc[1];
        goto *opcode_targets[128 + opcode];
    }
    #undef WIDTH_PREFIX
    #undef TARGET
    #undef UImm
    #undef UImm16
    #undef JumpImm

    #define WIDTH_PREFIX WIDE_
    #define WIDE_OP
    #define TARGET(name) COLD_TARGET(WIDE_##name) DEBUG_LABEL(WIDE_##name);
    #define UImm(idx) (load_uimm32(&pc[2 + 4 * idx]))
    #define UImm16(idx) (load_uimm16(&pc[2 + 4 * idx]))
    #define JumpImm(idx) ((int32_t)UImm(idx))
    #include "ceval2_handlers.inc.c"
    #undef WIDTH_PREFIX
    #undef TARGET
    #undef UImm
    #undef UImm16
    #undef JumpImm

    generator_return_to_c: {
        PyObject *obj = AS_OBJ(acc);
        if (!IS_RC(acc)) {
            _Py_INCREF(obj);
        }
        PyGenObject2 *gen = PyGen2_FromThread(ts);
        assert(gen != NULL);
        gen->status = GEN_FINISHED;
        gen->return_value = obj;
        ts->ts->use_new_interp -= 1;
        return NULL;
    }

    return_to_c: {
        PyObject *obj = AS_OBJ(acc);
        if (!IS_RC(acc)) {
            _Py_INCREF(obj);
        }
        ts->ts->use_new_interp -= 1;
        return obj;
    }

    error: {
        // TODO: normalize exception and create traceback.
        // CALL_VM(vm_handle_error(ts));
        CALL_VM(vm_traceback_here(ts));
        goto exception_unwind;
    }

    exception_unwind: {
        if (acc.as_int64 != 0) {
            DECREF(acc);
            acc.as_int64 = 0;
        }
        CALL_VM(pc = vm_exception_unwind(ts, pc));
        if (pc == 0) {
            ts->ts->use_new_interp -= 1;
            return NULL;
        }
        NEXT_INSTRUCTION();
    }

    _unknown_opcode:
    {
        // CALL_VM(acc = vm_unknown_opcode(opcode));
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
            "# opcode = %1 \n\t"
            "# regs = %4 \n\t"
            "# acc = %2 \n\t"
            "# pc = %3 \n\t"
            "# ts = %5 \n\t"
            "# opcode_targets = %6 \n\t"
            "# tid = %7 \n\t"
        ::
            "r" (opcode),
            "r" (opcode),
            "r" (acc),
            "r" (pc),
            "r" (regs),
            "r" (ts),
            "r" (opcode_targets),
            "r" (tid));
#endif
        NEXT_INSTRUCTION();
    }

    __builtin_unreachable();
    // Py_RETURN_NONE;
}

_Py_ALWAYS_INLINE static inline struct probe_result
dict_probe(PyObject *op, PyObject *name, intptr_t guess, intptr_t tid)
{
    assert(PyDict_CheckExact(op));
    struct probe_result result = {(Register){0}, 0};
    PyDictObject *dict = (PyDictObject *)op;
    PyDictKeysObject *keys = _Py_atomic_load_ptr_relaxed(&dict->ma_keys);
    guess = ((uintptr_t)guess) & keys->dk_size;
    PyDictKeyEntry *entry = &keys->dk_entries[(guess & keys->dk_size)];
    if (UNLIKELY(_Py_atomic_load_ptr(&entry->me_key) != name)) {
        return result;
    }
    PyObject *value = _Py_atomic_load_ptr(&entry->me_value);
    if (UNLIKELY(value == NULL)) {
        return result;
    }
    uint32_t refcount = _Py_atomic_load_uint32_relaxed(&value->ob_ref_local);
    if ((refcount & (_Py_REF_IMMORTAL_MASK)) != 0) {
        result.acc = PACK(value, NO_REFCOUNT_TAG);
        goto check_keys;
    }
    else if (LIKELY(_Py_ThreadMatches(value, tid))) {
        _Py_atomic_store_uint32_relaxed(&value->ob_ref_local, refcount + 4);
        result.acc = PACK(value, REFCOUNT_TAG);
        goto check_keys;
    }
    else {
        _Py_atomic_add_uint32(&value->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT));
        result.acc = PACK(value, REFCOUNT_TAG);
        if (value != _Py_atomic_load_ptr(&entry->me_value)) {
            result.found = 0;
            return result;
        }
    }
  check_keys:
    if (UNLIKELY(keys != _Py_atomic_load_ptr(&dict->ma_keys))) {
        result.found = 0;
        return result;
    }
    result.found = 1;
    return result;
}