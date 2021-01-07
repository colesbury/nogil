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

#include <ctype.h>

#define UNLIKELY _PY_UNLIKELY
#define LIKELY _PY_LIKELY

// tt  = type
//   r = refcounted
//     .. [ttr]
// 000 ..  000 NULL
// aaa ..  000 non-RC OBJ
// aaa ..  001 RC obj
//     ..  010 int
//     ..  100 pri (True, False, None)


#define TARGET(name) \
    TARGET_##name: \
   __asm__ volatile(".TARGET_" #name ":");


#if defined(__clang__)
#define COLD_TARGET(name) TARGET_##name:
#elif defined(__GNUC__)
#define COLD_TARGET(name) TARGET_##name: __attribute__((cold));
#else
#define COLD_TARGET(name) TARGET_##name:
#endif

#define CALL_VM(call) \
    call; \
    regs = ts->regs;


#define DECREF(reg) do { \
    if (IS_RC(reg)) { \
        _Py_DECREF_TOTAL \
        PyObject *obj = AS_OBJ(reg); \
        if (LIKELY(_Py_ThreadLocal(obj))) { \
            uint32_t refcount = obj->ob_ref_local; \
            refcount -= 4; \
            obj->ob_ref_local = refcount; \
            if (UNLIKELY(refcount == 0)) { \
                CALL_VM(_Py_MergeZeroRefcount(obj)); \
            } \
        } \
        else { \
            CALL_VM(vm_decref_shared(obj)); \
        } \
    } \
} while (0)

#define USE_RC(obj) ((obj->ob_ref_local & 0x3) == 0)

#define INCREF(reg) do { \
    if (IS_RC(reg)) { \
        _Py_INCREF_TOTAL \
        PyObject *obj = AS_OBJ(reg); \
        if (LIKELY(_Py_ThreadLocal(obj))) { \
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
        if (_PY_LIKELY(_Py_ThreadLocal(op))) { \
            local += (1 << _Py_REF_LOCAL_SHIFT); \
            _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local); \
        } \
        else { \
            _Py_atomic_add_uint32(&op->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT)); \
        } \
    } \
} while(0)


// static inline void
// _Py_INCREF(PyObject *op)
// {
//     uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
//     if (_Py_REF_IS_IMMORTAL(local)) {
//         return;
//     }

//     _Py_INCREF_TOTAL
//     if (_PY_LIKELY(_Py_ThreadLocal(op))) {
//         local += (1 << _Py_REF_LOCAL_SHIFT);
//         _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local);
//     }
//     else {
//         vm_incref_shared(obj);
//     }
// }

static PyObject *primitives[3] = {
    Py_None,
    Py_False,
    Py_True
};

static inline Register
PACK_INCREF(PyObject *obj)
{
    Register r;
    r.obj = obj;
    if ((obj->ob_ref_local & 0x3) == 0) {
        _Py_INCREF_TOTAL
        r.as_int64 |= REFCOUNT_TAG;
        if (LIKELY(_Py_ThreadLocal(obj))) {
            uint32_t refcount = obj->ob_ref_local;
            refcount += 4;
            obj->ob_ref_local = refcount;
        }
        else {
            _Py_atomic_add_uint32(&obj->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT));
        }
    }
    return r;
}

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
_PyEval_Fast(struct ThreadState *ts)
{
    #include "opcode_targets2.h"
    if (!ts->opcode_targets[0]) {
        memcpy(ts->opcode_targets, opcode_targets_base, sizeof(opcode_targets_base));
    }

    const uint32_t *next_instr = ts->pc;
    intptr_t opcode;
    intptr_t opA;
    intptr_t opD;
    Register acc;
    // callee saved: rbx,rbp,r12,r13,r14,r15 6 usable registers + rsp

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
    // next_instr (pc) -- webkit splits this in two: PB/PC PB is saved, PC is returned on calls
    // constants -- webkit sticks this in frame?
    // opcode_targets
    // ts (?)
    // OBJ_MASK
    // INT32_TAG

    void **opcode_targets = ts->opcode_targets;
    uint16_t *metadata = ts->metadata;

    // NOTE: after memcpy call!
    Register *regs = ts->regs;
    acc = PACK_INT32(ts->nargs);
    DISPATCH(INITIAL);

    TARGET(LOAD_INT) {
        acc = PACK_INT32(opD);
        DISPATCH(LOAD_INT);
    }

    TARGET(LOAD_CONST) {
        acc.obj = CONSTANTS()[opA];
        DISPATCH(LOAD_CONST);
    }

    TARGET(TEST_LESS_THAN) {
        // is register less than accumulator
        Register r = regs[opA];
        if (IS_INT32(acc) && IS_INT32(r)) {
            // FIXME: to boolean?
            acc = PACK_BOOL(r.as_int64 < acc.as_int64);
        }
        else {
            CALL_VM(acc = vm_compare(r, acc));
        }
        DISPATCH(TEST_LESS_THAN);
    }

    TARGET(JUMP) {
        next_instr += opD - 0x8000;
        DISPATCH(JUMP);
    }

    TARGET(POP_JUMP_IF_FALSE) {
        if (acc.obj == Py_False) {
            next_instr += opD - 0x8000;
        }
        else if (LIKELY(acc.obj == Py_True)) {
            next_instr += 0;
        }
        else {
            int err;
            CALL_VM(err = PyObject_IsTrue(AS_OBJ(acc)));
            if (err == 0) {
                next_instr += opD - 0x8000;
            }
            DECREF(acc);
            acc.as_int64 = 0;
        }
        DISPATCH(POP_JUMP_IF_FALSE);
    }

    TARGET(JUMP_IF_FALSE) {
        if (_PY_LIKELY(IS_PRI(acc))) {
            if ((AS_PRI(acc) & PRI_TRUE) == 0) {
                next_instr += opD - 0x8000;
                DISPATCH(JUMP_IF_FALSE);
            }
            else {
                DISPATCH(JUMP_IF_FALSE);
            }
        }
        else {
            CALL_VM(acc = vm_to_bool(acc));
            if ((AS_PRI(acc) & PRI_TRUE) == 0) {
                opD = next_instr[-1] >> 16;
                next_instr += opD - 0x8000;
            }
            DISPATCH(JUMP_IF_FALSE);
        }
    }

    TARGET(FUNC_HEADER) {
        // opA contains framesize
        // acc contains nargs from call
        PyCodeObject2 *this_code = PyCode2_FromInstr(next_instr - 1);
        regs[-3].as_int64 = (intptr_t)this_code->co_constants;
        ts->regs = regs;
        if (UNLIKELY(regs + opA > ts->maxstack)) {
            // resize stack
            CALL_VM(vm_resize_stack(ts, opA));
            // todo: check for errors!
        }
        Py_ssize_t nargs = AS_INT32(acc);
        if (UNLIKELY(nargs != this_code->co_argcount)) {
            // error!
            // well... we might have set-up a try-catch, so we can't just return
            ts->regs = regs;
            return vm_args_error(ts);
        }
        if (this_code->co_ncells != 0) {
            CALL_VM(vm_setup_cells(ts, this_code));
        }
        if (this_code->co_nfreevars != 0) {
            CALL_VM(vm_setup_freevars(ts, this_code));
        }

        DISPATCH(FUNC_HEADER);
    }

    TARGET(CFUNC_HEADER) {
        Py_ssize_t nargs = AS_INT32(acc);
        CALL_VM(acc = vm_call_cfunction(ts, regs, nargs));
        next_instr = (const uint32_t *)regs[-2].as_int64;
        regs[-2].as_int64 = 0;
        regs[-3].as_int64 = 0;
        // this is the call that dispatched to us
        uint32_t call = next_instr[-1];
        intptr_t offset = (call >> 8) & 0xFF;
        regs -= offset;
        ts->regs = regs;
        DISPATCH(CFUNC_HEADER);
    }

    TARGET(MAKE_FUNCTION) {
        PyCodeObject2 *code = (PyCodeObject2 *)CONSTANTS()[opA];
        CALL_VM(acc = vm_make_function(ts, code));
        DISPATCH(MAKE_FUNCTION);
    }

    TARGET(CALL_FUNCTION) {
        // opsD = nargs
        // opsA - 2 = <empty> (frame link)
        // opsA - 1 = func
        // opsA + 0 = arg0
        // opsA + opsD = argsN
        PyObject *callable = AS_OBJ(regs[opA - 1]);
        if (UNLIKELY(!PyType_HasFeature(Py_TYPE(callable), Py_TPFLAGS_FUNC_INTERFACE))) {
            CALL_VM(acc = vm_call_function(ts, opA, opD));
            DISPATCH(CALL_FUNCTION2);
        }

        PyFuncBase *func = (PyFuncBase *)callable;
        regs = &regs[opA];
        ts->regs = regs;
        regs[-2].as_int64 = (intptr_t)next_instr;
        acc = PACK_INT32(opD);
        next_instr = func->first_instr;
        DISPATCH(CALL_FUNCTION);
    }

    TARGET(RETURN_VALUE) {
        Register *top = &regs[THIS_CODE()->co_nlocals];
        while (top != regs - 1) {
            top--;
            Register r = *top;
            top->as_int64 = 0;
            DECREF(r);
        }
        next_instr = (const uint32_t *)regs[-2].as_int64;
        regs[-2].as_int64 = 0;
        regs[-3].as_int64 = 0;
        if ((((uintptr_t)next_instr) & FRAME_C) != 0) {
            goto return_to_c;
        }
        // this is the call that dispatched to us
        uint32_t call = next_instr[-1];
        intptr_t offset = (call >> 8) & 0xFF;
        regs -= offset;
        ts->regs = regs;
        DISPATCH(RETURN_VALUE);
    }

    TARGET(LOAD_NAME) {
        PyObject *name = CONSTANTS()[opA];
        PyDictObject *globals = (PyDictObject *)THIS_FUNC()->globals;
        PyDictKeysObject *keys = globals->ma_keys;
        CALL_VM(acc = vm_load_name((PyObject *)globals, name));
        DISPATCH(LOAD_NAME);
    }

    TARGET(LOAD_GLOBAL) {
        PyObject *name = CONSTANTS()[opA];
        PyDictObject *globals = (PyDictObject *)THIS_FUNC()->globals;
        PyDictKeysObject *keys = globals->ma_keys;

        // intptr_t guess = metadata[opA];
        // guess &= keys->dk_size;
        // PyDictKeyEntry *entry = &keys->dk_entries[guess];
        // if (entry->me_key == name) {
        //     // printf("guess found\n");
        //     PyObject *value = entry->me_value;
        //     acc.obj = value;
        //     DISPATCH();
        // }

        CALL_VM(acc = vm_load_name((PyObject *)globals, name));
        DISPATCH(LOAD_GLOBAL);
    }

    TARGET(STORE_NAME) {
        PyObject *name = CONSTANTS()[opA];
        PyObject *globals = THIS_FUNC()->globals;
        CALL_VM(acc = vm_store_global(globals, name, acc));
        DISPATCH(STORE_NAME);
    }
    TARGET(STORE_GLOBAL) {
        PyObject *name = CONSTANTS()[opA];
        PyObject *globals = THIS_FUNC()->globals;
        CALL_VM(acc = vm_store_global(globals, name, acc));
        DISPATCH(STORE_GLOBAL);
    }

    TARGET(LOAD_FAST) {
        acc = regs[opA];
        INCREF(acc);
        DISPATCH(LOAD_FAST);
    }

    TARGET(STORE_FAST) {
        Register old = regs[opA];
        regs[opA] = acc;
        acc.as_int64 = 0;
        DECREF(old);
        DISPATCH(STORE_FAST);
    }

    TARGET(MOVE) {
        Register r = regs[opA];
        regs[opA] = regs[opD];
        regs[opD].as_int64 = 0;
        DECREF(r);
        DISPATCH(MOVE);
    }

    TARGET(COPY) {
        // FIXME: is this only used for aliases???
        regs[opA].as_int64 = regs[opD].as_int64 & ~REFCOUNT_TAG;
        DISPATCH(COPY);
    }

    TARGET(CLEAR_FAST) {
        Register r = regs[opA];
        regs[opA].as_int64 = 0;
        DECREF(r);
        DISPATCH(CLEAR_FAST);
    }

    TARGET(CLEAR_ACC) {
        Register r = acc;
        acc.as_int64 = 0;
        DECREF(r);
        DISPATCH(CLEAR_ACC);
    }

    TARGET(LOAD_DEREF) {
        PyObject *cell = AS_OBJ(regs[opA]);
        PyObject *value = PyCell_GET(cell);
        acc = PACK_INCREF(value);
        DISPATCH(LOAD_DEREF);
    }

    TARGET(STORE_DEREF) {
        PyObject *cell = AS_OBJ(regs[opA]);
        PyObject *value = AS_OBJ(acc);
        // FIXME: got to clear the old value
        if (!IS_RC(acc)) {
            _Py_INCREF(value);
        }
        PyCell_SET(cell, value);
        acc.as_int64 = 0;
        DISPATCH(STORE_DEREF);
    }

    TARGET(COMPARE_OP) {
        assert(opA <= Py_GE);
        PyObject *left = AS_OBJ(regs[opD]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyObject_RichCompare(left, right, opA));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(COMPARE_OP);
    }

    TARGET(BINARY_ADD) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Add(left, right));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(INPLACE_ADD);
    }

    TARGET(INPLACE_ADD) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_InPlaceAdd(left, right));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(INPLACE_ADD);
    }

    TARGET(BINARY_SUBTRACT) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Subtract(left, right));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(BINARY_SUBTRACT);
    }

    TARGET(BINARY_MULTIPLY) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_Multiply(left, right));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(BINARY_MULTIPLY);
    }

    TARGET(BINARY_TRUE_DIVIDE) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_TrueDivide(left, right));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(BINARY_TRUE_DIVIDE);
    }

    TARGET(BINARY_FLOOR_DIVIDE) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *left = AS_OBJ(regs[opA]);
        PyObject *right = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyNumber_FloorDivide(left, right));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(BINARY_FLOOR_DIVIDE);
    }

    TARGET(BINARY_SUBSCR) {
        assert(IS_OBJ(regs[opA]));
        assert(IS_OBJ(acc));
        PyObject *container = AS_OBJ(regs[opA]);
        PyObject *sub = AS_OBJ(acc);
        PyObject *res;
        CALL_VM(res = PyObject_GetItem(container, sub));
        DECREF(acc);
        acc = PACK_OBJ(res);
        DISPATCH(BINARY_SUBSCR);
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
            goto error;
    }

    TARGET(FOR_ITER) {
        PyObject *iter = AS_OBJ(regs[opA]);
        PyObject *next;
        CALL_VM(next = (*Py_TYPE(iter)->tp_iternext)(iter));
        if (next == NULL) {
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
        assert(acc.as_int64 != 0);
        DISPATCH(BUILD_SLICE);
    }

    TARGET(BUILD_LIST) {
        // opA = reg, opD = N
        CALL_VM(acc = vm_build_list(&regs[opA], opD));
        DISPATCH(BUILD_LIST);
    }

    TARGET(BUILD_TUPLE) {
        // opA = reg, opD = N
        CALL_VM(acc = vm_build_tuple(&regs[opA], opD));
        DISPATCH(BUILD_TUPLE);
    }

    TARGET(LIST_APPEND) {
        PyObject *list = AS_OBJ(regs[opA]);
        PyObject *item = AS_OBJ(acc);
        CALL_VM(PyList_Append(list, item));
        DECREF(acc);
        acc.as_int64 = 0;
        DISPATCH(LIST_APPEND);
    }

    TARGET(UNPACK_SEQUENCE) {
        // opA = reg, opD = N
        if (IS_OBJ(acc) && PyTuple_CheckExact(AS_OBJ(acc))) {
            PyObject *tuple = AS_OBJ(acc);
            for (Py_ssize_t i = 0; i < opD; i++) {
                PyObject *item = PyTuple_GET_ITEM(tuple, i);
                assert(regs[opA+i].as_int64 == 0);
                regs[opA+i] = PACK_INCREF(item);
            }
            DECREF(acc);
        }
        else {
            abort();
        }
        DISPATCH(UNPACK_SEQUENCE);
    }

    return_to_c: {
        if (IS_OBJ(acc)) {
            return AS_OBJ(acc);
        }
        else if (IS_INT32(acc)) {
            return PyLong_FromLong(AS_INT32(acc));
        }
        else if (IS_PRI(acc)) {
            return primitives[AS_PRI(acc)];
        }
        else {
            __builtin_unreachable();
        }
    }

    error: {
        CALL_VM(vm_handle_error(ts));
    }

    #include "unimplemented_opcodes.h"
    {
        CALL_VM(acc = vm_unknown_opcode(opcode));
        // opcode = 0;
        __builtin_unreachable();
    }
//

    COLD_TARGET(debug_regs) {
        // __asm__ volatile (
        //     "# REGISTER ASSIGNMENT \n\t"
        //     "# opcode = %0 \n\t"
        //     "# opA = %1 \n\t"
        //     "# opD = %2 \n\t"
        //     "# regs = %5 \n\t"
        //     "# acc = %3 \n\t"
        //     "# next_instr = %4 \n\t"
        //     "# constants = %6 \n\t"
        //     "# ts = %7 \n\t"
        //     "# opcode_targets = %8 \n\t"
        //     "# metadata = %9 \n\t"
        // ::
        //     "r" (opcode),
        //     "r" (opA),
        //     "r" (opD),
        //     "r" (acc),
        //     "r" (next_instr),
        //     "r" (regs),
        //     "r" (constants),
        //     "r" (ts),
        //     "r" (opcode_targets),
        //     "r" (foo));
        DISPATCH(debug_regs);
    }

    __builtin_unreachable();
    // Py_RETURN_NONE;
}
