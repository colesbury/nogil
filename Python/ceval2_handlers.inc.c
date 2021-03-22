TARGET(LOAD_CONST) {
    acc = PACK(CONSTANTS()[UImm(0)], NO_REFCOUNT_TAG);
    DISPATCH(LOAD_CONST);
}

TARGET(JUMP) {
    JUMP_BY(JumpImm(0));
}

TARGET(POP_JUMP_IF_FALSE) {
    PyObject *value = AS_OBJ(acc);
    if (value == Py_True) {
        acc.as_int64 = 0;
        DISPATCH(POP_JUMP_IF_FALSE);
    }
    else if (LIKELY(value == Py_False || value == Py_None)) {
        acc.as_int64 = 0;
        JUMP_BY(JumpImm(0));
    }
    else {
        int res;
        CALL_VM(res = PyObject_IsTrue(value));
        if (UNLIKELY(res < 0)) {
            goto error;
        }
        if (res == 0) {
            pc += JumpImm(0);
        }
        else {
            pc += OP_SIZE(POP_JUMP_IF_FALSE);
        }
        DECREF(acc);
        acc.as_int64 = 0;
        NEXT_INSTRUCTION();
    }
}

TARGET(POP_JUMP_IF_TRUE) {
    PyObject *value = AS_OBJ(acc);
    if (value == Py_True) {
        acc.as_int64 = 0;
        JUMP_BY(JumpImm(0));
    }
    else if (LIKELY(value == Py_False || value == Py_None)) {
        acc.as_int64 = 0;
        DISPATCH(POP_JUMP_IF_TRUE);
    }
    else {
        int res;
        CALL_VM(res = PyObject_IsTrue(value));
        if (UNLIKELY(res < 0)) {
            goto error;
        }
        if (res == 1) {
            pc += JumpImm(0);
        }
        else {
            pc += OP_SIZE(POP_JUMP_IF_TRUE);
        }
        DECREF(acc);
        acc.as_int64 = 0;
        NEXT_INSTRUCTION();
    }
}

TARGET(JUMP_IF_FALSE) {
    PyObject *value = AS_OBJ(acc);
    if (value == Py_True) {
        DISPATCH(JUMP_IF_FALSE);
    }
    else if (LIKELY(value == Py_False || value == Py_None)) {
        JUMP_BY(JumpImm(0));
    }
    else {
        int res;
        CALL_VM(res = PyObject_IsTrue(value));
        if (UNLIKELY(res < 0)) {
            goto error;
        }
        if (res == 0) {
            pc += JumpImm(0);
        }
        else {
            pc += OP_SIZE(JUMP_IF_FALSE);
        }
        NEXT_INSTRUCTION();
    }
}

TARGET(JUMP_IF_TRUE) {
    PyObject *value = AS_OBJ(acc);
    if (value == Py_True) {
        JUMP_BY(JumpImm(0));
    }
    else if (LIKELY(value == Py_False || value == Py_None)) {
        DISPATCH(JUMP_IF_TRUE);
    }
    else {
        int res;
        CALL_VM(res = PyObject_IsTrue(value));
        if (UNLIKELY(res < 0)) {
            goto error;
        }
        if (res == 1) {
            pc += JumpImm(0);
        }
        else {
            pc += OP_SIZE(JUMP_IF_TRUE);
        }
        NEXT_INSTRUCTION();
    }
}

TARGET(FUNC_HEADER) {
    // imm0 contains framesize
    // acc contains nargs from call
    int err;
    assert(ts->regs == regs);

    Py_ssize_t frame_size = UImm(0);
    if (UNLIKELY(regs + frame_size > ts->maxstack)) {
        // resize the virtual stack
        CALL_VM(err = vm_resize_stack(ts, frame_size));
        if (UNLIKELY(err != 0)) {
            acc.as_int64 = 0;
            goto error;
        }
    }

    PyCodeObject2 *this_code = PyCode2_FromInstr(pc);
    assert(Py_TYPE(this_code) == &PyCode2_Type);
    regs[-3].as_int64 = (intptr_t)this_code->co_constants;

    // fast path if no keyword arguments, cells, or freevars and number
    // of positional arguments exactly matches.
    if (LIKELY((uint32_t)acc.as_int64 == this_code->co_packed_flags)) {
        goto LABEL(dispatch_func_header);
    }

    ts->pc = pc;
    if ((acc.as_int64 & (ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS)) != 0) {
        // call passed arguments as tuple and keywords as dict
        // TODO: update acc to avoid checking all args for defaults
        FUNC_CALL_VM(err = vm_setup_ex(ts, this_code, acc));
        if (UNLIKELY(err != 0)) {
            acc.as_int64 = 0;
            goto error;
        }
        goto LABEL(setup_default_args);
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
                    goto LABEL(kw_found);
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

        LABEL(kw_found):
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

  LABEL(setup_default_args): ;
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
        Py_ssize_t n = this_code->co_nfreevars;
        for (Py_ssize_t i = this_code->co_ndefaultargs; i < n; i++) {
            Py_ssize_t r = this_code->co_free2reg[i*2+1];
            PyObject *cell = this_func->freevars[i];
            assert(PyCell_Check(cell));
            regs[r] = PACK(cell, NO_REFCOUNT_TAG);
        }
    }

  LABEL(dispatch_func_header):
    acc.as_int64 = 0;
    DISPATCH(FUNC_HEADER);
}

TARGET(METHOD_HEADER) {
    PyMethodObject *meth = (PyMethodObject *)AS_OBJ(regs[-1]);
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
        meth = (PyMethodObject *)AS_OBJ(regs[-1]);
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
    if (UNLIKELY(!PyFunc_Check(func))) {
        Register x = PACK_INCREF(func);
        SET_REG(regs[-1], x);
        goto call_object;
    }
    pc = ((PyFuncBase *)func)->first_instr;
    Register x = PACK_INCREF(func);
    SET_REG(regs[-1], x);
    NEXT_INSTRUCTION();
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
    pc = (const uint8_t *)regs[-2].as_int64;
    intptr_t frame_delta = regs[-4].as_int64;
    regs[-2].as_int64 = 0;
    regs[-3].as_int64 = 0;
    regs[-4].as_int64 = 0;
    regs -= frame_delta;
    ts->regs = regs;
    NEXT_INSTRUCTION();
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
    pc = (const uint8_t *)regs[-2].as_int64;
    intptr_t frame_delta = regs[-4].as_int64;
    regs[-2].as_int64 = 0;
    regs[-3].as_int64 = 0;
    regs[-4].as_int64 = 0;
    regs -= frame_delta;
    ts->regs = regs;
    NEXT_INSTRUCTION();
}

TARGET(COROGEN_HEADER) {
    // setup generator?
    // copy arguments
    // return
    assert(IS_EMPTY(acc));
    PyGenObject2 *gen;
    int typeidx = UImm(0);
    CALL_VM(gen = PyGen2_NewWithSomething(ts, typeidx));
    if (gen == NULL) {
        goto error;
    }
    PyGen2_SetPC(gen, pc + OP_SIZE(COROGEN_HEADER));
    acc = PACK_OBJ((PyObject *)gen);
    goto RETURN_VALUE;
}

TARGET(MAKE_FUNCTION) {
    PyCodeObject2 *code = (PyCodeObject2 *)CONSTANTS()[UImm(0)];
    CALL_VM(acc = vm_make_function(ts, code));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(MAKE_FUNCTION);
}

TARGET(CALL_METHOD) {
    assert(IS_EMPTY(acc));
    intptr_t base = UImm(0);
    intptr_t nargs = UImm16(1);
    if (regs[base].as_int64 == 0) {
        // If the LOAD_METHOD didn't provide us with a "self" then we
        // need to shift each argument down one. Note that opD >= 1.
        assert(nargs >= 1);
        Register *low = &regs[base];
        Register *hi = &regs[base + nargs];
        do {
            *low = *(low +1);
            low++;
        } while (low != hi);
        nargs -= 1;
    }
    acc.as_int64 = nargs;
    goto LABEL(call_function_impl);
}

TARGET(CALL_FUNCTION) {
    // opD = (kwargs << 8) | nargs
    // regs[imm0 - 4] = <empty> (frame delta)
    // regs[imm0 - 3] = <empty> (constants)
    // regs[imm0 - 2] = <empty> (frame link)
    // regs[imm0 - 1] = func
    // regs[imm0 + 0] = arg0
    // regs[imm0 + n] = argsN
    assert(IS_EMPTY(acc));
    acc.as_int64 = UImm16(1);
    goto LABEL(call_function_impl);
}

LABEL(call_function_impl): {
    intptr_t base = UImm(0);
    PyObject *callable = AS_OBJ(regs[base - 1]);
    regs = &regs[base];
    ts->regs = regs;
    regs[-4].as_int64 = base;    // frame delta
    regs[-2].as_int64 = (intptr_t)(pc + OP_SIZE(CALL_FUNCTION));
    if (!PyType_HasFeature(Py_TYPE(callable), Py_TPFLAGS_FUNC_INTERFACE)) {
        goto call_object;
    }
    pc = ((PyFuncBase *)callable)->first_instr;
    NEXT_INSTRUCTION();
}

#ifndef WIDE_OP
call_object: {
    // DEBUG_LABEL(call_object);
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
    pc = (const uint8_t *)regs[-2].as_int64;
    intptr_t frame_delta = regs[-4].as_int64;
    regs[-2].as_int64 = 0;
    regs[-3].as_int64 = 0; // should already be zero?
    regs[-4].as_int64 = 0;
    regs -= frame_delta;
    ts->regs = regs;
    NEXT_INSTRUCTION();
}
#endif

TARGET(CALL_FUNCTION_EX) {
    // imm0 - 6 = *args
    // imm0 - 5 = **kwargs
    // imm0 - 4 = <empty> (frame delta)
    // imm0 - 3 = <empty> (constants/frame size)
    // imm0 - 2 = <empty> (frame link)
    // imm0 - 1 = func
    assert(IS_EMPTY(acc));
    intptr_t base = UImm(0);
    regs = &regs[base];
    ts->regs = regs;
    regs[-4].as_int64 = base;  // frame delta
    regs[-2].as_int64 = (intptr_t)(pc + OP_SIZE(CALL_FUNCTION_EX));

    // pc is no longer valid. The NULL value prevents this
    // partially set-up frame from showing up in tracebacks.
    pc = NULL;

    // ensure that *args is a tuple
    if (UNLIKELY(!PyTuple_CheckExact(AS_OBJ(regs[-FRAME_EXTRA - 2])))) {
        int err;
        CALL_VM(err = vm_callargs_to_tuple(ts));
        if (UNLIKELY(err < 0)) {
            goto error;
        }
    }

    // ensure that **kwargs is a dict
    if (regs[-FRAME_EXTRA - 1].as_int64 != 0 &&
        UNLIKELY(!PyDict_CheckExact(AS_OBJ(regs[-FRAME_EXTRA - 1])))) {
        int err;
        CALL_VM(err = vm_kwargs_to_dict(ts));
        if (UNLIKELY(err < 0)) {
            goto error;
        }
    }

    PyObject *callable = AS_OBJ(regs[-1]);
    if (!PyType_HasFeature(Py_TYPE(callable), Py_TPFLAGS_FUNC_INTERFACE)) {
        goto call_object_ex;
    }
    acc.as_int64 = ACC_FLAG_VARARGS|ACC_FLAG_VARKEYWORDS;
    pc = ((PyFuncBase *)callable)->first_instr;
    NEXT_INSTRUCTION();
}

#ifndef WIDE_OP
call_object_ex: {
    // DEBUG_LABEL(call_object_ex);
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
    CLEAR(regs[-FRAME_EXTRA - 1]);  // clear **kwargs
    CLEAR(regs[-FRAME_EXTRA - 2]);  // clear *args
    CLEAR(regs[-1]);  // clear callable
    pc = (const uint8_t *)regs[-2].as_int64;
    intptr_t frame_delta = regs[-4].as_int64;
    regs[-2].as_int64 = 0;
    // regs[-3] is already zero
    regs[-4].as_int64 = 0;
    regs -= frame_delta;
    ts->regs = regs;
    NEXT_INSTRUCTION();
}
#endif

TARGET(YIELD_VALUE) {
    PyGenObject2 *gen = PyGen2_FromThread(ts);
    gen->status = GEN_YIELD;
    ts->pc = pc + OP_SIZE(YIELD_VALUE);
    // assert((regs[-2].as_int64 & FRAME_C) != 0);
    goto return_to_c;
}

#define IMPL_YIELD_FROM(awaitable, res) do {                                \
    PyGen2_FromThread(ts)->yield_from = awaitable;                          \
    CALL_VM(res = _PyObject_YieldFrom(awaitable, AS_OBJ(acc)));             \
    if (res != NULL) {                                                      \
        SET_ACC(PACK_OBJ(res));                                             \
        PyGenObject2 *gen = PyGen2_FromThread(ts);                          \
        gen->status = GEN_YIELD;                                            \
        ts->pc = pc;  /* will resume with YIELD_FROM */                     \
        goto return_to_c;                                                   \
    }                                                                       \
    PyGen2_FromThread(ts)->yield_from = NULL;                               \
    CALL_VM(res = _PyGen2_FetchStopIterationValue());                       \
    if (UNLIKELY(res == NULL)) {                                            \
        goto error;                                                         \
    }                                                                       \
} while (0)

TARGET(YIELD_FROM) {
    PyObject *awaitable = AS_OBJ(regs[UImm(0)]);
    PyObject *res;
    IMPL_YIELD_FROM(awaitable, res);
    SET_ACC(PACK_OBJ(res));
    DISPATCH(YIELD_FROM);
}

TARGET(RETURN_VALUE) {
#if DEBUG_FRAME
    Py_ssize_t frame_size = THIS_CODE()->co_framesize;
#endif
    CLEAR_REGISTERS(THIS_CODE()->co_nlocals);
    intptr_t frame_link = regs[-2].as_int64;
    intptr_t frame_delta = regs[-4].as_int64;
    regs[-2].as_int64 = 0;
    regs[-3].as_int64 = 0;
    regs[-4].as_int64 = 0;
#if DEBUG_FRAME
    for (Py_ssize_t i = 0; i < frame_size; i++) {
        assert(regs[i].as_int64 == 0);
    }
#endif
    regs -= frame_delta;
    ts->regs = regs;
    if (UNLIKELY(frame_link <= FRAME_C)) {
        ts->pc = NULL;
        if (frame_link == FRAME_C) {
            goto return_to_c;
        }
        else if (frame_link == FRAME_GENERATOR) {
            goto generator_return_to_c;
        }
        else {
            __builtin_unreachable();
        }
    }
    // acc might be an unowned alias of some local up the stack. We must
    // convert it to an owning reference before returning.
    acc = STRONG_REF(acc);
    pc = (const uint8_t *)frame_link;
    NEXT_INSTRUCTION();
}

TARGET(LOAD_NAME) {
    assert(IS_EMPTY(acc));
    PyObject *name = CONSTANTS()[UImm(0)];
    CALL_VM(acc = vm_load_name(regs, name));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(LOAD_NAME);
}

TARGET(LOAD_GLOBAL) {
    assert(IS_EMPTY(acc));
    PyObject **constants = CONSTANTS();
    intptr_t *metadata = (intptr_t *)(char *)constants;
    PyObject *name = constants[UImm(0)];
    PyObject *globals = THIS_FUNC()->globals;
    intptr_t metaidx = UImm(1);
    if (UNLIKELY(!PyDict_CheckExact(globals))) {
         goto LABEL(load_global_slow);
    }

    intptr_t guess = metadata[-metaidx - 1];
    if (guess < 0) {
        if (guess == -1) goto LABEL(load_global_slow);
        else goto LABEL(load_builtin);
    }

  /* load_global: */ {
    struct probe_result probe = dict_probe(globals, name, guess, tid);
    acc = probe.acc;
    if (LIKELY(probe.found)) {
        goto LABEL(dispatch_load_global);
    }
    goto LABEL(load_global_slow);
  }

  LABEL(load_builtin): {
    if (UNLIKELY(dict_may_contain((PyDictObject *)globals, name))) {
        goto LABEL(load_global_slow);
    }
    PyObject *builtins = THIS_FUNC()->builtins;
    if (UNLIKELY(!PyDict_CheckExact(builtins))) {
         goto LABEL(load_global_slow);
    }
    struct probe_result probe = dict_probe(builtins, name, guess, tid);
    acc = probe.acc;
    if (LIKELY(probe.found)) {
        goto LABEL(dispatch_load_global);
    }
    // fallthrough to load_global_slow
  }

  LABEL(load_global_slow): {
    PyObject *value;
    CALL_VM(value = vm_load_global(ts, name, &metadata[-metaidx - 1]));
    if (UNLIKELY(value == NULL)) {
        goto error;
    }
    XSET_ACC(PACK_OBJ(value));
  }

  LABEL(dispatch_load_global):
    DISPATCH(LOAD_GLOBAL);
}

TARGET(LOAD_ATTR) {
    assert(IS_EMPTY(acc));
    PyObject *owner = AS_OBJ(regs[UImm(0)]);
    PyObject *name = CONSTANTS()[UImm(1)];
    PyObject *res;
    CALL_VM(res = _PyObject_GetAttrFast(owner, name));
    if (UNLIKELY(res == NULL)) {
        goto error;
    }
    acc = PACK_OBJ(res);
    DISPATCH(LOAD_ATTR);
}

TARGET(LOAD_METHOD) {
    PyObject *owner = AS_OBJ(acc);
    PyObject *name = CONSTANTS()[UImm(1)];
    int err;
    CALL_VM(err = vm_load_method(ts, owner, name, UImm(0)));
    if (UNLIKELY(err != 0)) {
        goto error;
    }
    DECREF(acc);
    acc.as_int64 = 0;
    DISPATCH(LOAD_METHOD);
}

TARGET(STORE_NAME) {
    PyObject *name = CONSTANTS()[UImm(0)];
    PyObject *locals = AS_OBJ(regs[0]);
    int err;
    if (LIKELY(PyDict_CheckExact(locals))) {
        CALL_VM(err = PyDict_SetItem(locals, name, AS_OBJ(acc)));
    }
    else {
        CALL_VM(err = PyObject_SetItem(locals, name, AS_OBJ(acc)));
    }
    if (UNLIKELY(err < 0)) {
        goto error;
    }
    DECREF(acc);
    acc.as_int64 = 0;
    DISPATCH(STORE_NAME);
}

TARGET(STORE_GLOBAL) {
    PyObject *name = CONSTANTS()[UImm(0)];
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
    PyObject *container = AS_OBJ(regs[UImm(0)]);
    PyObject *sub = AS_OBJ(regs[UImm(1)]);
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
    PyObject *owner = AS_OBJ(regs[UImm(0)]);
    PyObject *name = CONSTANTS()[UImm(1)];
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
    acc = regs[UImm(0)];
    INCREF(acc);
    DISPATCH(LOAD_FAST);
}

TARGET(STORE_FAST) {
    intptr_t dst = UImm(0);
    Register prev = regs[dst];
    regs[dst] = acc;
    acc.as_int64 = 0;
    if (prev.as_int64) {
        DECREF(prev);
    }
    DISPATCH(STORE_FAST);
}

TARGET(MOVE) {
    intptr_t dst = UImm(0);
    intptr_t src = UImm(1);
    Register prev = regs[dst];
    regs[dst] = regs[src];
    regs[src].as_int64 = 0;
    if (prev.as_int64 != 0) {
        DECREF(prev);
    }
    DISPATCH(MOVE);
}

TARGET(COPY) {
    intptr_t dst = UImm(0);
    intptr_t src = UImm(1);
    // FIXME: is this only used for aliases???
    assert(IS_EMPTY(regs[dst]));
    regs[dst].as_int64 = regs[src].as_int64 | NO_REFCOUNT_TAG;
    DISPATCH(COPY);
}

TARGET(CLEAR_FAST) {
    intptr_t dst = UImm(0);
    Register r = regs[dst];
    regs[dst].as_int64 = 0;
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
    intptr_t idx = UImm(0);
    PyObject *cell = AS_OBJ(regs[idx]);
    PyObject *value = PyCell_GET(cell);
    if (UNLIKELY(value == NULL)) {
        CALL_VM(vm_err_unbound(ts, idx));
        goto error;
    }
    acc = PACK_INCREF(value);
    DISPATCH(LOAD_DEREF);
}

TARGET(STORE_DEREF) {
    PyObject *cell = AS_OBJ(regs[UImm(0)]);
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
    intptr_t idx = UImm(0);
    PyObject *name = CONSTANTS()[UImm(1)];
    CALL_VM(acc = vm_load_class_deref(ts, idx, name));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(LOAD_CLASSDEREF);
}

TARGET(DELETE_FAST) {
    intptr_t idx = UImm(0);
    Register r = regs[idx];
    if (UNLIKELY(r.as_int64 == 0)) {
        // FIXME: name error
        goto error;
    }
    regs[idx].as_int64 = 0;
    DECREF(r);
    DISPATCH(DELETE_FAST);
}

TARGET(DELETE_NAME) {
    assert(IS_EMPTY(acc));
    PyObject *name = CONSTANTS()[UImm(0)];
    int err;
    CALL_VM(err = vm_delete_name(ts, name));
    if (UNLIKELY(err != 0)) {
        goto error;
    }
    DISPATCH(DELETE_NAME);
}

TARGET(DELETE_GLOBAL) {
    PyObject *globals = THIS_FUNC()->globals;
    PyObject *name = CONSTANTS()[UImm(0)];
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
    PyObject *name = CONSTANTS()[UImm(0)];
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
    PyObject *container = AS_OBJ(regs[UImm(0)]);
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
    PyObject *cell = AS_OBJ(regs[UImm(0)]);
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
    int cmp = UImm(0);
    assert(cmp <= Py_GE);
    PyObject *left = AS_OBJ(regs[UImm(1)]);
    PyObject *right = AS_OBJ(acc);
    PyObject *res;
    CALL_VM(res = PyObject_RichCompare(left, right, cmp));
    if (UNLIKELY(res == NULL)) {
        goto error;
    }
    SET_ACC(PACK_OBJ(res));
    DISPATCH(COMPARE_OP);
}

TARGET(IS_OP) {
    PyObject *left = AS_OBJ(regs[UImm(0)]);
    PyObject *right = AS_OBJ(acc);
    Register res = primitives[(left == right)];
    SET_ACC(res);
    DISPATCH(IS_OP);
}

TARGET(CONTAINS_OP) {
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *left = AS_OBJ(regs[UImm(0)]);
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
    assert(IS_OBJ(regs[UImm(0)]));
    assert(IS_OBJ(acc));
    PyObject *container = AS_OBJ(regs[UImm(0)]);
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
    PyObject *arg = CONSTANTS()[UImm(0)];
    CALL_VM(acc = vm_import_name(ts, this_func, arg));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(IMPORT_NAME);
}

TARGET(IMPORT_FROM) {
    PyObject *module = AS_OBJ(regs[UImm(0)]);
    PyObject *name = CONSTANTS()[UImm(1)];
    PyObject *res;
    CALL_VM(res = vm_import_from(ts, module, name));
    if (UNLIKELY(res == NULL)) {
        goto error;
    }
    acc = PACK_OBJ(res);
    DISPATCH(IMPORT_FROM);
}

TARGET(IMPORT_STAR) {
    // TODO: assert that we have locals dict
    PyObject *module = AS_OBJ(regs[UImm(0)]);
    PyObject *locals = AS_OBJ(regs[0]);
    int err;
    CALL_VM(err = vm_import_star(ts, module, locals));
    if (UNLIKELY(err != 0)) {
        goto error;
    }
    DISPATCH(IMPORT_STAR);
}

TARGET(GET_ITER) {
    PyObject *obj = AS_OBJ(acc);
    PyObject *iter;
    getiterfunc f = Py_TYPE(obj)->tp_iter;
    if (UNLIKELY(f == NULL)) {
        f = vm_get_iter;
    }
    CALL_VM(iter = (*f)(obj));
    if (iter == NULL) {
        goto error;
    }
    if (Py_TYPE(iter)->tp_iternext == NULL) {
        CALL_VM(vm_err_non_iterator(ts, iter));
        goto error;
    }
    intptr_t dst = UImm(0);
    assert(regs[dst].as_int64 == 0);
    regs[dst] = PACK_OBJ(iter);
    DECREF(acc);
    acc.as_int64 = 0;
    DISPATCH(GET_ITER);
}

TARGET(GET_YIELD_FROM_ITER) {
    intptr_t dst = UImm(0);
    PyObject *obj = AS_OBJ(acc);
    assert(regs[dst].as_int64 == 0);
    if (PyGen_CheckExact(obj)) {
        regs[dst] = acc;
        acc.as_int64 = 0;
    }
    else if (PyCoro_CheckExact(obj)) {
        int flags = THIS_CODE()->co_flags;
        if (UNLIKELY(!(flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE)))) {
            CALL_VM(PyErr_SetString(
                PyExc_TypeError,
                "cannot 'yield from' a coroutine object "
                "in a non-coroutine generator"));
            goto error;
        }
        regs[dst] = acc;
        acc.as_int64 = 0;
    }
    else {
        goto LABEL(GET_ITER);
    }
    DISPATCH(GET_YIELD_FROM_ITER);
}

TARGET(GET_AWAITABLE) {
    PyObject *obj = AS_OBJ(acc);
    if (PyCoro2_CheckExact(obj)) {
        PyObject *yf = ((PyCoroObject2 *)obj)->base.yield_from;
        if (UNLIKELY(yf != NULL)) {
            CALL_VM(vm_err_coroutine_awaited(ts));
            goto error;
        }
        regs[UImm(0)] = acc;
        acc.as_int64 = 0;
    }
    else {
        PyObject *iter;
        CALL_VM(iter = _PyCoro2_GetAwaitableIter(obj));
        if (UNLIKELY(iter == NULL)) {
            CALL_VM(vm_err_awaitable(ts, acc));
            goto error;
        }
        regs[UImm(0)] = PACK_OBJ(iter);
        DECREF(acc);
        acc.as_int64 = 0;
    }
    DISPATCH(GET_AWAITABLE);
}

TARGET(FOR_ITER) {
    PyObject *iter = AS_OBJ(regs[UImm(0)]);
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
        Register r = regs[UImm(0)];
        regs[UImm(0)].as_int64 = 0;
        DECREF(r);
        DISPATCH(FOR_ITER);
    }
    else {
        acc = PACK_OBJ(next);
        pc += JumpImm(1);
        NEXT_INSTRUCTION();
    }
}

TARGET(GET_AITER) {
    unaryfunc getter = NULL;
    PyObject *obj = AS_OBJ(acc);
    if (Py_TYPE(obj)->tp_as_async != NULL) {
        getter = Py_TYPE(obj)->tp_as_async->am_aiter;
    }
    if (UNLIKELY(getter == NULL)) {
        CALL_VM(vm_err_async_for_aiter(ts, Py_TYPE(obj)));
        goto error;
    }

    PyObject *iter;
    CALL_VM(iter = (*getter)(obj));
    if (iter == NULL) {
        goto error;
    }
    assert(regs[UImm(0)].as_int64 == 0);
    regs[UImm(0)] = PACK_OBJ(iter);

    if (UNLIKELY(Py_TYPE(iter)->tp_as_async == NULL ||
                 Py_TYPE(iter)->tp_as_async->am_anext == NULL)) {
        CALL_VM(vm_err_async_for_anext(ts, Py_TYPE(iter)));
        goto error;
    }

    DECREF(acc);
    acc.as_int64 = 0;
    DISPATCH(GET_AITER);
}

TARGET(GET_ANEXT) {
    PyObject *aiter = AS_OBJ(regs[UImm(0)]);
    assert(Py_TYPE(aiter)->tp_as_async->am_anext != NULL);

    unaryfunc getter = Py_TYPE(aiter)->tp_as_async->am_anext;
    PyObject *awaitable;
    // TODO: PyAsyncGen_CheckExact awaitable
    CALL_VM(awaitable = getter(aiter));
    if (awaitable == NULL) {
        goto error;
    }
    regs[UImm(0) + 1] = PACK_OBJ(awaitable);
    if (!PyCoro2_CheckExact(awaitable)) {
        CALL_VM(awaitable = _PyCoro2_GetAwaitableIter(awaitable));
        if (UNLIKELY(awaitable == NULL)) {
            CALL_VM(vm_err_awaitable(ts, acc));
            goto error;
        }
        Register prev = regs[UImm(0) + 1];
        regs[UImm(0) + 1] = PACK_OBJ(awaitable);
        DECREF(prev);
    }
    DISPATCH(GET_ANEXT);
}

TARGET(END_ASYNC_FOR) {
    // imm0 + 0 = loop iterable
    // imm0 + 1 = -1
    // imm0 + 2 = <exception object>
    int err;
    CALL_VM(err = vm_end_async_for(ts, UImm(0)));
    if (UNLIKELY(err != 0)) {
        goto error;
    }
    DISPATCH(END_ASYNC_FOR);
}

TARGET(BUILD_SLICE) {
    CALL_VM(acc = vm_build_slice(&regs[UImm(0)]));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(BUILD_SLICE);
}

TARGET(BUILD_LIST) {
    // imm0 = reg, opD = N
    CALL_VM(acc = vm_build_list(&regs[UImm(0)], UImm(1)));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(BUILD_LIST);
}

TARGET(BUILD_SET) {
    // imm0 = reg, opD = N
    CALL_VM(acc = vm_build_set(ts, UImm(0), UImm(1)));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(BUILD_SET);
}

TARGET(BUILD_TUPLE) {
    // imm0 = reg, opD = N
    CALL_VM(acc = vm_build_tuple(ts, UImm(0), UImm(1)));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(BUILD_TUPLE);
}

TARGET(BUILD_MAP) {
    assert(IS_EMPTY(acc));
    PyObject *res;
    CALL_VM(res = _PyDict_NewPresized(UImm(0)));
    if (UNLIKELY(res == NULL)) {
        goto error;
    }
    acc = PACK(res, REFCOUNT_TAG);
    DISPATCH(BUILD_MAP);
}

TARGET(DICT_UPDATE) {
    PyObject *dict = AS_OBJ(regs[UImm(0)]);
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
    PyObject *dict = AS_OBJ(regs[UImm(0)]);
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
    PyObject *list = AS_OBJ(regs[UImm(0)]);
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
    PyObject *list = AS_OBJ(regs[UImm(0)]);
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
    PyObject *set = AS_OBJ(regs[UImm(0)]);
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
    PyObject *set = AS_OBJ(regs[UImm(0)]);
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
    // iconstants[UImm(0)] = base, argcnt, argcntafter
    PyObject *seq = AS_OBJ(acc);
    Py_ssize_t *args = &THIS_CODE()->co_iconstants[UImm(0)];
    if (LIKELY(args[2] == -1)) {
        if (PyList_CheckExact(seq)) {
            Py_ssize_t argcnt = args[1];
            if (LIKELY(PyList_GET_SIZE(seq) == argcnt)) {
                for (Py_ssize_t i = 0; i != argcnt; i++) {
                    regs[args[0] + i] = PACK_INCREF(PyList_GET_ITEM(seq, i));
                }
                goto LABEL(unpack_done);
            }
        }
        else if (PyTuple_CheckExact(seq)) {
            Py_ssize_t argcnt = args[1];
            if (LIKELY(PyTuple_GET_SIZE(seq) == argcnt)) {
                for (Py_ssize_t i = 0; i != argcnt; i++) {
                    regs[args[0] + i] = PACK_INCREF(PyTuple_GET_ITEM(seq, i));
                }
                goto LABEL(unpack_done);
            }
        }
    }
    int err;
    CALL_VM(err = vm_unpack(ts, seq, args[0], args[1], args[2]));
    if (UNLIKELY(err != 0)) {
        goto error;
    }
  LABEL(unpack_done):
    DECREF(acc);
    acc.as_int64 = 0;
    DISPATCH(UNPACK);
}

TARGET(LOAD_BUILD_CLASS) {
    PyObject *builtins = THIS_FUNC()->builtins;
    CALL_VM(acc = vm_load_build_class(ts, builtins));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(LOAD_BUILD_CLASS);
}

TARGET(RAISE) {
    int err;
    PyObject *exc = AS_OBJ(acc);  // may be NULL
    CALL_VM(err = vm_raise(ts, exc));
    assert(err == -1 || err == -2);
    if (err == -2) {
        goto exception_unwind;
    }
    goto error;
}

TARGET(JUMP_IF_NOT_EXC_MATCH) {
    intptr_t link_reg = UImm(0);
    PyObject *type = AS_OBJ(acc);
    PyObject *exc = AS_OBJ(regs[link_reg + 1]);
    assert(regs[link_reg].as_int64 == -1 && "link reg should be -1");
    const uint8_t *target;
    CALL_VM(target = vm_exc_match(ts, type, exc, pc, JumpImm(1)));
    if (UNLIKELY(target == NULL)) {
        goto error;
    }
    pc = target;
    BREAK_LIVE_RANGE(pc);
    DECREF(acc);
    acc.as_int64 = 0;
    NEXT_INSTRUCTION();
}

TARGET(END_EXCEPT) {
    // FIXME(sgross): is regs[UImm(0)] always -1 ???
    if (regs[UImm(0)].as_int64 == -1) {
        Register r = regs[UImm(0) + 1];
        regs[UImm(0)].as_int64 = 0;
        regs[UImm(0) + 1].as_int64 = 0;
        DECREF(r);
    }
    DISPATCH(END_EXCEPT);
}

TARGET(CALL_FINALLY) {
    uintptr_t return_addr = (uintptr_t)(pc + OP_SIZE(CALL_FINALLY));
    regs[UImm(0)] = PACK(return_addr << 1, NO_REFCOUNT_TAG);
    JUMP_BY(JumpImm(1));
}

TARGET(END_FINALLY) {
    // FIXME: should rename to something else since it's also used at end of
    // try-except with no matches
    intptr_t link_reg = UImm(0);
    uintptr_t link_addr = regs[link_reg].as_int64;
    Register link_val = regs[link_reg + 1];
    regs[link_reg].as_int64 = 0;
    regs[link_reg + 1].as_int64 = 0;
    if (link_addr == (uintptr_t)-1) {
        // re-raise the exception that caused us to enter the block.
        CALL_VM(vm_reraise(ts, link_val));
        goto exception_unwind;
    }
    acc = link_val;
    if (link_addr != 0) {
        JUMP_TO((const uint8_t *)(link_addr >> 1));
    }
    DISPATCH(END_FINALLY);
}

TARGET(SETUP_WITH) {
    regs[UImm(0)] = acc;
    CALL_VM(acc = vm_setup_with(ts, UImm(0)));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(SETUP_WITH);
}

TARGET(SETUP_ASYNC_WITH) {
    intptr_t r = UImm(0);
    regs[r] = acc;
    CALL_VM(acc = vm_setup_async_with(ts, r));
    if (UNLIKELY(acc.as_int64 == 0)) {
        goto error;
    }
    DISPATCH(SETUP_ASYNC_WITH);
}

TARGET(END_WITH) {
    int err;
    CALL_VM(err = vm_exit_with(ts, UImm(0)));
    if (UNLIKELY(err != 0)) {
        if (err == -1) {
            goto error;
        }
        goto exception_unwind;
    }
    DISPATCH(END_WITH);
}

TARGET(END_ASYNC_WITH) {
    // on first execution:
    // acc = NULL
    // imm0 + 0 = <mgr>
    // imm0 + 1 = __exit__
    // imm0 + 2 = 0 or -1 (on error)
    // imm0 + 3 = 0 or <error> (on error)
    //
    // on resumptions:
    // acc = <value to send to coroutine>
    // imm0 + 0 = <awaitable>
    // imm0 + 1 = 0
    // imm0 + 2 = 0 or -1 (on error)
    // imm0 + 3 = 0 or <error> (on error)
    int err;
    if (acc.as_int64 == 0) {
        // first time
        CALL_VM(err = vm_exit_async_with(ts, UImm(0)));
        if (UNLIKELY(err != 0)) {
            goto error;
        }
        acc = PACK(Py_None, NO_REFCOUNT_TAG);
    }
    PyObject *res;
    PyObject *awaitable = AS_OBJ(regs[UImm(0)]);
    IMPL_YIELD_FROM(awaitable, res);
    if (regs[UImm(0) + 2].as_int64 != 0) {
        CALL_VM(err = vm_exit_with_res(ts, UImm(0), res));
        if (err != 0) {
            if (err == -1) {
                goto error;
            }
            goto exception_unwind;
        }
    }
    else {
        _Py_DECREF(res);
        CLEAR(regs[UImm(0)]);
    }
    DECREF(acc);
    acc.as_int64 = 0;
    DISPATCH(END_ASYNC_WITH);
}

TARGET(LOAD_INTRINSIC) {
    assert(IS_EMPTY(acc));
    acc = PACK((UImm(0) << 1), NO_REFCOUNT_TAG);
    DISPATCH(LOAD_INTRINSIC);
}

TARGET(CALL_INTRINSIC_1) {
    intrinsic1 fn = intrinsics_table[UImm(0)].intrinsic1;
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
    CALL_VM(res = vm_call_intrinsic(ts, id, UImm(0), UImm(1)));
    if (UNLIKELY(res == NULL)) {
        acc.as_int64 = 0;
        goto error;
    }
    acc = PACK_OBJ(res);
    DISPATCH(CALL_INTRINSIC_N);
}