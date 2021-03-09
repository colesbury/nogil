# Copyright (c) 2016, Darius Bacon
# Byterun Copyright (c) 2013, Ned Batchelder

# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
# of the Software, and to permit persons to whom the Software is furnished to do
# so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import ast, collections, types, sys
import dis2 as dis
from check_subset import check_conformity
import inspect  # ast uses inspect -- early initialize to load it using old interp

def assemble(assembly, addresses):
    return b''.join(instr.encode(None, addresses) for instr,_ in assembly)

def make_ehtable(assembly, addresses):
    return sum((instr.eh_table(addresses) for instr,_ in assembly), ())

def make_lnotab(assembly):
    firstlineno, lnotab = None, []
    byte, line = 0, None
    next_byte = 0
    for instr, next_line in assembly:
        if firstlineno is None:
            firstlineno = line = next_line
        elif line < next_line:
            # Py3.6 changed to use signed bytes here, not unsigned.
            # This is a hack to keep the old logic, without taking advantage of
            # the new possibility of negative values.
            while byte+127 < next_byte:
                lnotab.extend([127, 0])
                byte = byte+127
            while line+127 < next_line:
                lnotab.extend([next_byte-byte, 127])
                byte, line = next_byte, line+127
            if (byte, line) != (next_byte, next_line):
                lnotab.extend([next_byte-byte, next_line-line])
                byte, line = next_byte, next_line
        next_byte += instr.length
    return firstlineno or 1, bytes(lnotab)

def make_addresses(instrs):
    addresses = {}
    offset = 0
    for instr, _ in instrs:
        addresses[instr] = offset
        offset += instr.length
    return addresses

class Assembly:
    length = 0
    def resolve(self, start):
        return ()
    def encode(self, start, addresses):
        return b''
    def line_nos(self, start):
        return ()
    def eh_table(self, addresses):
        return ()

no_op = Assembly()

class Label(Assembly):
    def resolve(self, start):
        return ((self, start),)

class ExceptHandler(Label):
    def __init__(self, start, handler_end, reg):
        self.start = start
        self.handler_end = handler_end
        self.reg = reg
        assert isinstance(reg, int)
    def eh_table(self, addresses):
        return ((addresses[self.start] // 4, addresses[self] // 4, addresses[self.handler_end] // 4, self.reg),)

class Instruction(Assembly):
    def __init__(self, opcode, arg, arg2):
        self.opcode = opcode
        if isinstance(arg, Register):
            assert arg.reg is not None
        if isinstance(arg2, Register):
            assert arg2.reg is not None
        self.arg    = arg
        self.arg2   = arg2

    @property
    def length(self):
        argA = int(self.arg or 0)
        if argA < 256:
            return 4
        elif argA < 65536:
            return 8
        assert False, f'argA out of range: {argA}'

    def encode(self, start, addresses):
        start = addresses[self]
        arg, arg2 = self.arg, self.arg2
        if dis.opcodes[self.opcode].is_jump():
            arg2 = (addresses[arg2] - start) // 4 - 1
            arg2 += 0x8000
        if arg2 is None:
            arg2 = 0
        else:
            arg2 = int(arg2)
        argA = int(self.arg or 0)
        assert argA < 65536, (self.opcode, argA, arg2)
        assert arg2 >= 0 and arg2 < 65536, (self.opcode, argA, arg2)

        if argA < 256:
            return bytes([self.opcode, argA, (arg2 & 0xFF), (arg2 >> 8)])
        else:
            return bytes([
                dis.opmap['EXTENDED_ARG'].opcode, (argA >> 8), 0, 0,
                self.opcode, argA & 0xFF, (arg2 & 0xFF), (arg2 >> 8)])


class Register:
    def __init__(self, visitor, reg):
        self.visitor = visitor
        self.reg = reg
    def __index__(self):
        assert self.reg is not None, 'unassigned placeholder'
        return self.reg
    def is_placeholder(self):
        return self.reg is None
    def is_temporary(self):
        return self.reg >= self.visitor.nlocals
    def allocate(self):
        visitor = self.visitor
        if self.reg is None:
            self.reg = visitor.new_register()
        else:
            # assert visitor.next_register <= self.reg
            while visitor.next_register <= self.reg:
                visitor.new_register()
        return self.reg
    def clear(self):
        assert self.reg is not None
        if self.is_temporary():
            self.visitor.CLEAR_FAST(self.reg)
    def __call__(self, t):
        return self.visitor.to_register(t, self)

class RegisterList:
    def __init__(self, visitor, n):
        self.visitor = visitor
        self.base = visitor.new_register(n)
    def __getitem__(self, i):
        return self.visitor.register(self.base + i)
    def __call__(self, seq):
        for i,t in enumerate(seq):
            self[i](t)

class Reference:
    def __init__(self, visitor):
        self.visitor = visitor
    def load(self):
        return no_op
    def store(self, value):
        return no_op

class Block:
    pass

class TryFinally(Block):
    def __init__(self, handler, reg):
        self.handler = handler
        self.reg = reg

    def on_exit(self, v):
        assert v.next_register == self.reg
        v.CALL_FINALLY(self.reg, self.handler)

class Finally(Block):
    def __init__(self, reg):
        self.reg = reg

    def on_exit(self, v):
        assert v.next_register == self.reg + 2
        v.END_EXCEPT(self.reg)
        v.next_register -= 2

class ExceptBlock(Block):
    def __init__(self, reg):
        self.reg = reg

    def on_exit(self, v):
        assert v.next_register == self.reg + 2
        v.END_EXCEPT(self.reg)
        v.next_register -= 2

class WithBlock(Block):
    def __init__(self, reg):
        self.reg = reg

    def on_exit(self, v):
        assert v.next_register == self.reg + 2
        v.END_WITH(self.reg)
        v.next_register -= 2

class AsyncWithBlock(Block):
    def __init__(self, reg):
        self.reg = reg

    def on_exit(self, v):
        assert v.next_register == self.reg + 3
        v.END_ASYNC_WITH(self.reg)
        v.next_register -= 3

class Loop:
    def __init__(self, reg=None):
        self.top = Label()      # iteration
        self.next = Label()     # loop test (continue target)
        self.anchor = Label()   # before "orelse" block
        self.exit = Label()     # break target
        self.reg = reg

    def on_exit(self, v):
        if self.reg is not None:
            assert v.next_register > self.reg
            while v.next_register != self.reg:
                v.next_register -= 1
                v.CLEAR_FAST(v.next_register)

def denotation(defn):
    opcode = defn.opcode
    opA = defn.opA
    opD = defn.opD
    if opA is None and opD is None:
        return Instruction(opcode, None, None)
    elif opD is None:
        return lambda arg: Instruction(opcode, arg, None)
    elif opA is None:
        return lambda arg2: Instruction(opcode, None, arg2)
    else:
        return lambda arg, arg2: Instruction(opcode, arg, arg2)

op = type('op', (), dict([(bytecode.name, denotation(bytecode))
                          for bytecode in dis.bytecodes]))


def as_load(t):
    attrs = []
    for field in t._fields:
        if field != 'ctx':
            attrs.append(getattr(t, field))
        else:
            attrs.append(ast.Load())
    return type(t)(*attrs)


def make_instruction(bytecode):
    opcode = bytecode.opcode
    opA = bytecode.opA
    opD = bytecode.opD
    if opA is None and opD is None:
        def func(self):
            self.emit(Instruction(opcode, None, None))
        return func
    elif opD is None:
        def func(self, arg):
            self.emit(Instruction(opcode, arg, None))
        return func
    elif opA is None:
        def func(self, arg):
            self.emit(Instruction(opcode, None, arg))
        return func
    else:
        def func(self, arg, arg2):
            self.emit(Instruction(opcode, arg, arg2))
        return func

class ops:
    for bytecode in dis.bytecodes:
        locals()[bytecode.name] = make_instruction(bytecode)

FRAME_EXTRA = 4

class CodeGen(ast.NodeVisitor):
    for bytecode in dis.bytecodes:
        locals()[bytecode.name] = make_instruction(bytecode)

    def __init__(self, filename, scope):
        self.filename  = filename
        self.scope     = scope
        self.constants = constants()
        self.iconstants = []
        self.names     = self.constants
        self.varnames  = scope.regs
        self.instrs    = []
        self.nlocals = self.next_register = len(self.varnames)
        self.max_registers = self.next_register
        self.blocks = []
        self.last_lineno = None
        self.interactive = False

    def compile_module(self, t):
        if isinstance(t, ast.Interactive):
            # TODO: annotations?
            self.interactive = True
        self.FUNC_HEADER(0)
        self(t.body)
        self.load_const(None)
        self.RETURN_VALUE()
        return self.make_code("<module>")

    def make_code(self, name, argcount=0, posonlyargcount=0, kwonlyargcount=0, ndefaultargs=0, has_varargs=False, has_varkws=False, debug=False):
        first_instr = self.instrs[0][0]
        assert dis.opcodes[first_instr.opcode].name == 'FUNC_HEADER'
        first_instr.arg = self.max_registers

        if self.scope.is_generator:
            second_instr = self.instrs[1][0]
            assert dis.opcodes[second_instr.opcode].name == 'COROGEN_HEADER'

        nlocals = self.nlocals
        framesize = self.max_registers
        firstlineno, lnotab = make_lnotab(self.instrs)
        addresses = make_addresses(self.instrs)

        code = assemble(self.instrs, addresses)
        eh_table = tuple(make_ehtable(self.instrs, addresses))
        cell2reg = tuple(reg for name, reg in self.varnames.items() if name in self.scope.cellvars)
        iconstants = tuple(self.iconstants)
        assert len(cell2reg) == len(self.scope.cellvars)

        # ((upval0, reg0), (upval1, reg1), ...)
        free2reg = tuple(self.scope.free2reg.values())
        # assert len(free2reg) == len(self.scope.freevars)

        flags = (  (0x00 if nlocals                  else 0)
                 | (0x00 if self.scope.freevars      else 0)
                 | (0x10 if self.scope.nested        else 0)
                 | (0x010000 if has_varargs          else 0)
                 | (0x040000 if has_varkws           else 0))

        return types.CodeType2(code,
                               self.constants.collect(),
                               argcount=argcount,
                               posonlyargcount=posonlyargcount,
                               kwonlyargcount=kwonlyargcount,
                               ndefaultargs=ndefaultargs,
                               nlocals=nlocals,
                               framesize=framesize,
                               flags=flags,
                               varnames=collect(self.varnames),
                               filename=self.filename,
                               name=name,
                               firstlineno=firstlineno,
                               linetable=lnotab,
                               eh_table=eh_table,
                               freevars=(),
                               cellvars=(),
                               iconstants=iconstants,
                               cell2reg=cell2reg,
                               free2reg=free2reg)

    def emit(self, instr):
        assert isinstance(instr, (Instruction, Label))
        self.instrs.append((instr, self.last_lineno))

    def LABEL(self, label):
        assert isinstance(label, Label)
        self.emit(label)

    def load_const(self, constant):
        self.LOAD_CONST(self.constants[constant])

    def new_register(self, n=1):
        reg = self.next_register
        self.next_register += n
        if self.next_register > self.max_registers:
            self.max_registers = self.next_register
        return reg

    def register(self, reg=None):
        return Register(self, reg)

    def register_list(self, n=0):
        return RegisterList(self, n)

    def as_register(self, t):
        assert isinstance(t, ast.AST)
        if isinstance(t, ast.Name):
            name = mangle(self.scope.private, t.id)
            access = self.scope.access(name)
            if access == 'fast':
                return self.varnames[name]
        self(t)
        r = self.new_register()
        self.STORE_FAST(r)
        return r

    def to_register(self, t, reg):
        if isinstance(t, ast.Name):
            name = mangle(self.scope.private, t.id)
            return self.load_into(name, reg)
        elif isinstance(t, Register):
            return self.copy_register(reg, t)
        elif isinstance(t, GetIter):
            self(t.seq)
            self.GET_ITER(reg.allocate())
        else:
            self(t)
            self.STORE_FAST(reg.allocate())

    def copy_register(self, dst, src):
        if dst.is_placeholder():
            dst.reg = src.reg
        elif src.is_temporary():
            self.MOVE(dst.allocate(), src.reg)
        else:
            self.COPY(dst.allocate(), src.reg)

    def load(self, name):
        access = self.scope.access(name)
        if   access == 'fast':   self.LOAD_FAST(self.varnames[name])
        elif access == 'deref':  self.LOAD_DEREF(self.varnames[name])
        elif access == 'classderef':  self.LOAD_CLASSDEREF(self.varnames[name], self.names[name])
        elif access == 'name':   self.LOAD_NAME(self.names[name])
        elif access == 'global': self.LOAD_GLOBAL(self.names[name])
        else: assert False

    def store(self, name):
        access = self.scope.access(name)
        if   access == 'fast':   self.STORE_FAST(self.varnames[name])
        elif access == 'deref':  self.STORE_DEREF(self.varnames[name])
        elif access == 'name':   self.STORE_NAME(self.names[name])
        elif access == 'global': self.STORE_GLOBAL(self.names[name])
        else: assert False, access

    def delete(self, name):
        access = self.scope.access(name)
        if   access == 'fast':   self.DELETE_FAST(self.varnames[name])
        elif access == 'deref':  self.DELETE_DEREF(self.varnames[name])
        elif access == 'name':   self.DELETE_NAME(self.names[name])
        elif access == 'global': self.DELETE_GLOBAL(self.names[name])
        else: assert False, access

    def load_into(self, name, dst):
        assert isinstance(dst, Register)
        access = self.scope.access(name)
        if access == 'fast':
            src = Register(self, self.varnames[name])
            self.copy_register(dst, src)
        else:
            self.load(name)
            self.STORE_FAST(dst.allocate())

    def store_from(self, name, src):
        assert isinstance(src, Register)
        access = self.scope.access(name)
        if access == 'fast' and src.is_temporary():
            self.MOVE(self.varnames[name], src.reg)
        else:
            self.LOAD_FAST(src.reg)
            self.store(name)
            if src.is_temporary():
                self.CLEAR_FAST(src.reg)

    def clear(self, name):
        access = self.scope.access(name)
        if access == 'fast':
            self.CLEAR_FAST(self.varnames[name])
        else:
            self.load_const(None)
            self.store(name)
            self.delete(name)

    def visit_Constant(self, t):
        self.load_const(t.value)

    def visit_Name(self, t):
        assert isinstance(t.ctx, ast.Load)
        name = mangle(self.scope.private, t.id)
        self.load(name)

    def visit_Assert(self, t):
        label = Label()
        self(t.test)
        self.POP_JUMP_IF_TRUE(label)
        if t.msg is not None:
            self(t.msg)
        self.CALL_INTRINSIC1("vm_raise_assertion_error")
        self.LABEL(label)

    def call_intrinsic(self, name, base=None, nargs=None):
        assert (base is None) == (nargs is None)
        intrinsic = dis.intrinsic_map[name]
        if base is None:
            assert intrinsic.nargs == 1
            self.CALL_INTRINSIC_1(intrinsic.code)
        else:
            assert intrinsic.nargs == nargs or intrinsic.nargs == 'N'
            self.LOAD_INTRINSIC(intrinsic.code)
            self.CALL_INTRINSIC_N(base, nargs)

    def visit_FormattedValue(self, t):
        if t.format_spec is None:
            self(t.value)
            if t.conversion != -1:
                self.call_intrinsic(self.conv_fns[chr(t.conversion)])
            self.call_intrinsic('vm_format_value')
        else:
            regs = self.register_list()
            if t.conversion != -1:
                self(t.value)
                self.call_intrinsic(self.conv_fns[chr(t.conversion)])
                self.STORE_FAST(regs[0].allocate())
            else:
                regs[0](t.value)
            regs[1](t.format_spec)
            self.call_intrinsic('vm_format_value_spec', regs.base, 2)
    conv_fns = {'s': 'PyObject_Str', 'r': 'PyObject_Repr', 'a': 'PyObject_ASCII'}

    def visit_JoinedStr(self, t):
        if len(t.values) == 1:
            self(t.values[0])
            return

        reg = self.register_list()
        for i, value in enumerate(t.values):
            reg[i](value)
        self.call_intrinsic('vm_build_string', reg.base, len(t.values))


    def visit_Call(self, t):
        assert len(t.args) < 256 and len(t.keywords) < 256
        has_starargs = any(type(a) == ast.Starred for a in t.args)
        has_kwargs = any(k.arg is None for k in t.keywords)

        if has_starargs or has_kwargs or len(t.args) >= 256 or len(t.keywords) >= 256:
            return self.call_function_ex(t)
        elif len(t.keywords) == 0 and isinstance(t.func, ast.Attribute):
            return self.call_method(t)

        regs = self.register_list()
        opD = len(t.args) + (len(t.keywords) << 8)
        off = 0
        if len(t.keywords) > 0:
            off += len(t.keywords) + 1

        # store the function
        regs[off + FRAME_EXTRA - 1](t.func)

        # store keywords in caller frame
        if len(t.keywords) > 0:
            for i, kwd in enumerate(t.keywords):
                regs[i](kwd.value)
            kwdnames = tuple(kwd.arg for kwd in t.keywords)
            # TODO: load const directly into register
            self.load_const(kwdnames)
            self.STORE_FAST(regs[len(t.keywords)].allocate())

        # store positional arguments in destination frame
        for i, arg in enumerate(t.args):
            regs[off + FRAME_EXTRA + i](arg)

        self.CALL_FUNCTION(regs.base + off + FRAME_EXTRA, opD)

    def call_method(self, t):
        assert len(t.keywords) == 0
        assert isinstance(t.func, ast.Attribute) and type(t.func.ctx) == ast.Load

        regs = self.register_list()
        self(t.func.value)
        regs[FRAME_EXTRA].allocate()
        self.LOAD_METHOD(regs[FRAME_EXTRA-1], self.names[t.func.attr])
        for i, arg in enumerate(t.args):
            regs[FRAME_EXTRA+i+1](arg)
        self.CALL_METHOD(regs.base + FRAME_EXTRA, len(t.args) + 1)

    def call_function_ex(self, t):
        regs = self.register_list()
        regs[FRAME_EXTRA+1](t.func)
        if len(t.args) == 1 and isinstance(t.args[0], ast.Starred):
            regs[0](t.args[0].value)
        else:
            regs[0](ast.Tuple(elts=t.args, ctx=load))

        if len(t.keywords) == 1 and t.keywords[0].arg is None:
            regs[1](t.keywords[0].value)
        else:
            kwargs = ast.Dict(
                keys=[ast.Constant(value=k.arg) if k.arg is not None else None for k in t.keywords],
                values=[k.value for k in t.keywords]
            )
            kwargs.merge = self.DICT_MERGE  # yuck
            regs[1](kwargs)

        self.CALL_FUNCTION_EX(regs.base + FRAME_EXTRA + 2)

    def visit_keyword(self, t):
        self.load_const(t.arg)
        self(t.value)

    def __call__(self, t):
        if isinstance(t, list):
            for c in t:
                self(c)
            return
        if hasattr(t, 'lineno'):
            self.last_lineno = t.lineno
        top = self.next_register
        block_top = len(self.blocks)
        assembly = self.visit(t)
        self.next_register = top
        while len(self.blocks) > block_top:
            self.blocks.pop()

    def generic_visit(self, t):
        assert False, t

    def visit_Expr(self, t):
        # TODO: skip constants as optimization
        self(t.value)
        if self.interactive:
            self.call_intrinsic('vm_print')
        self.CLEAR_ACC()

    def resolve(self, t):
        if isinstance(t, ast.Name):
            name = mangle(self.scope.private, t.id)
            access = self.scope.access(name)
            if access == 'fast':
                return Register(self, self.varnames[name])
        return t

    def visit_Register(self, t):
        self.LOAD_FAST(t)

    def visit_Assign(self, t):
        if len(t.targets) == 1:
            return self.assign(t.targets[0], t.value)

        # TODO: optimize multi-assignment
        reg = self.register()
        reg(t.value)
        for target in t.targets:
            self.LOAD_FAST(reg.reg)
            self.assign_accumulator(target)
        reg.clear()

    def visit_AnnAssign(self, t):
        if t.value is not None:
            self.assign(t.target, t.value)
        # TODO: annotations

    def assign(self, target, value):
        method = 'assign_' + target.__class__.__name__
        visitor = getattr(self, method)
        return visitor(target, value)

    def assign_accumulator(self, target):
        if isinstance(target, ast.Name):
            self.store(target.id)
        else:
            # FIXME: optimize don't always need to store to reg
            top = self.next_register
            reg = self.register()
            self.STORE_FAST(reg.allocate())
            self.assign(target, reg)
            reg.clear()
            self.next_register = top

    def assign_Name(self, target, value):
        value = self.resolve(value)
        name = mangle(self.scope.private, target.id)
        if isinstance(value, Register):
            return self.store_from(name, value)

        self(value)
        self.store(name)

    def assign_Attribute(self, target, value):
        reg = self.register()
        name = mangle(self.scope.private, target.attr)
        # FIXME: clear reg if temporary ???
        reg(target.value)
        self(value)
        self.STORE_ATTR(reg, self.constants[name])
        reg.clear()
        if isinstance(value, Register) and value.is_temporary:
            value.clear()

    def assign_Subscript(self, target, value):
        reg1 = self.register()
        reg2 = self.register()
        # FIXME: target.slice.value ???
        reg1(target.value)
        reg2(target.slice)
        self(value)
        self.STORE_SUBSCR(reg1, reg2)
        reg2.clear()
        reg1.clear()
        if isinstance(value, Register) and value.is_temporary:
            value.clear()

    def assign_List(self, target, value): return self.assign_sequence(target, value)
    def assign_Tuple(self, target, value): return self.assign_sequence(target, value)
    def assign_sequence(self, target, value):
        self(value)
        if isinstance(value, Register) and value.is_temporary:
            value.clear()
        regs = self.register_list(len(target.elts))

        starred = [i for i,e in enumerate(target.elts) if type(e) == ast.Starred]
        assert len(starred) <= 1
        if len(starred) == 0:
            argcnt = len(target.elts)
            argcntafter = -1
        else:
            argcnt = starred[0]
            argcntafter = len(target.elts) - argcnt - 1

        c = len(self.iconstants)
        self.iconstants.extend([regs.base, argcnt, argcntafter])
        self.UNPACK(c)
        for i, elt in enumerate(target.elts):
            if type(elt) == ast.Starred:
                elt = elt.value
            self.assign(elt, regs[i])

    def visit_AugAssign(self, t):
        ref = self.reference(t.target)
        reg = self.register()
        ref.load(reg)
        self(t.value)
        self.aug_ops[type(t.op)](self, reg)
        reg.clear()
        ref.store()
    aug_ops = {ast.Pow:    ops.INPLACE_POWER,  ast.Add:  ops.INPLACE_ADD,
               ast.LShift: ops.INPLACE_LSHIFT, ast.Sub:  ops.INPLACE_SUBTRACT,
               ast.RShift: ops.INPLACE_RSHIFT, ast.Mult: ops.INPLACE_MULTIPLY,
               ast.BitOr:  ops.INPLACE_OR,     ast.Mod:  ops.INPLACE_MODULO,
               ast.BitAnd: ops.INPLACE_AND,    ast.Div:  ops.INPLACE_TRUE_DIVIDE,
               ast.BitXor: ops.INPLACE_XOR,    ast.FloorDiv: ops.INPLACE_FLOOR_DIVIDE}

    def reference(self, target):
        method = 'reference_' + target.__class__.__name__
        visitor = getattr(self, method)
        return visitor(target)

    def reference_Name(self, t):
        visitor = self
        name = mangle(self.scope.private, t.id)
        class NameRef:
            def load(self, dst):
                return visitor.load_into(name, dst)
            def store(self):
                return visitor.store(name)
        return NameRef()

    def reference_Attribute(self, t):
        visitor = self
        rvalue = self.register()
        name = mangle(self.scope.private, t.attr)
        class AttributeRef:
            def load(self, dst):
                rvalue(t.value)
                visitor.LOAD_ATTR(rvalue, visitor.names[name])
                visitor.STORE_FAST(dst.allocate())
            def store(self):
                visitor.STORE_ATTR(rvalue, visitor.names[name])
                rvalue.clear()
        return AttributeRef()

    def reference_Subscript(self, t):
        # foo[bar()] += baz()
        # foo -> reg
        rvalue = self.register()
        rslice = self.register()
        class SubscrRef:
            def load(ref, dst):
                rvalue(t.value)
                rslice(t.slice)
                self.LOAD_FAST(rslice)
                self.BINARY_SUBSCR(rvalue)
                self.STORE_FAST(dst.allocate())
            def store(ref):
                self.STORE_SUBSCR(rvalue, rslice)
                rslice.clear()
                rvalue.clear()
        return SubscrRef()

    def visit_Delete(self, t):
        for target in t.targets:
            self.del_(target)

    def del_(self, target):
        method = 'del_' + target.__class__.__name__
        visitor = getattr(self, method)
        return visitor(target)

    def del_Name(self, t):
        name = mangle(self.scope.private, t.id)
        self.delete(name)

    def del_Attribute(self, t):
        self(t.value)
        name = mangle(self.scope.private, t.attr)
        self.DELETE_ATTR(self.names[name])

    def del_Subscript(self, t):
        reg = self.register()
        reg(t.value)
        self(t.slice)
        self.DELETE_SUBSCR(reg)
        reg.clear()

    def del_List(self, t):
        for e in t.elts:
            self.del_(e)

    def del_Tuple(self, t):
        for e in t.elts:
            self.del_(e)

    def visit_If(self, t):
        orelse, after = Label(), Label()
        self(t.test)
        self.POP_JUMP_IF_FALSE(orelse if t.orelse else after)
        self(t.body)
        if t.orelse:
            self.JUMP(after)
            self.LABEL(orelse)
            self(t.orelse)
        self.LABEL(after)

    def visit_IfExp(self, t):
        orelse, after = Label(), Label()
        self(t.test)
        self.POP_JUMP_IF_FALSE(orelse)
        self(t.body)
        self.JUMP(after)
        self.LABEL(orelse)
        self(t.orelse)
        self.LABEL(after)

    def visit_With(self, t):
        assert len(t.items) == 1, 'With not desurgared'
        item = t.items[0]
        start = Label()
        handler_end = Label()

        # The register usage is:
        # [ mgr, __exit__, <link>, <exc> ]
        #   ^with_reg      ^link_reg
        #
        # The SETUP_WITH opcode sets mgr and __exit__
        # The END_WITH opcode calls mgr.__exit__(<args>)
        # If link is >=0 the args are (None, None, None)
        # and the interpreter continues to the next instruction.
        # If link is -1, args are *sys.exc_info(), and the
        # interpreter reraises the exception or continues to
        # the next instruction depending on if the bool(result) is
        # true.
        #
        # SETUP_WITH: does 3 things:
        #  copies acc to regs[0]
        #  loads acc.__exit__ and stores in regs[1]
        #  calls acc.__enter__() and result is stored in acc

        with_reg = self.new_register(2)
        link_reg = self.next_register  # don't allocate it yet

        self(item.context_expr)
        self.SETUP_WITH(with_reg)
        # begin try?
        self.LABEL(start)
        self.blocks.append(WithBlock(with_reg))
        if item.optional_vars:
            self.assign_accumulator(item.optional_vars)
        else:
            self.CLEAR_ACC()
        self(t.body)
        self.blocks.pop()
        r = self.new_register(2)
        assert r == link_reg
        self.LABEL(ExceptHandler(start, handler_end, link_reg))
        self.END_WITH(with_reg)
        self.LABEL(handler_end)

    def visit_AsyncWith(self, t):
        assert len(t.items) == 1, 'AsyncWith not desurgared'
        item = t.items[0]
        start = Label()
        handler_end = Label()

        # The register usage is:
        # [ mgr, __exit__, __enter__ | <link>, <exc> ]
        #   ^with_reg      ^link_reg
        with_reg = self.new_register(3)
        link_reg = self.next_register - 1  # don't allocate it yet

        self(item.context_expr)
        self.SETUP_ASYNC_WITH(with_reg)
        self.GET_AWAITABLE(with_reg + 2)
        self.load_const(None)
        self.YIELD_FROM(with_reg + 2)
        self.CLEAR_FAST(with_reg + 2)
        self.next_register -= 1
        self.LABEL(start)
        self.blocks.append(AsyncWithBlock(with_reg))
        if item.optional_vars:
            self.assign_accumulator(item.optional_vars)
        else:
            self.CLEAR_ACC()
        self(t.body)
        self.blocks.pop()
        r = self.new_register(2)
        assert r == link_reg
        self.LABEL(ExceptHandler(start, handler_end, link_reg))
        self.END_ASYNC_WITH(with_reg)
        self.LABEL(handler_end)


    def visit_Dict(self, t):
        self.BUILD_MAP(min(0xFFFF, len(t.keys)))
        if len(t.keys) == 0:
            return
        regs = self.register_list(2)
        self.STORE_FAST(regs[0])
        for k, v in zip(t.keys, t.values):
            if k:
                regs[1](k)
                self(v)
                self.STORE_SUBSCR(regs[0], regs[1])
                regs[1].clear()
            else:
                self(v)
                if hasattr(t, 'merge'):
                    t.merge(regs[0])
                else:
                    self.DICT_UPDATE(regs[0])
        self.LOAD_FAST(regs[0])
        self.CLEAR_FAST(regs[0])

    def visit_Subscript(self, t):
        assert type(t.ctx) == ast.Load
        reg = self.register()
        reg(t.value)
        self(t.slice)
        self.BINARY_SUBSCR(reg)
        reg.clear()

    def visit_Slice(self, t):
        lower, upper, step = (
            ast.Constant(None) if x is None else x
            for x in (t.lower, t.upper, t.step)
        )

        if all(isinstance(x, ast.Constant) for x in (lower, upper, step)):
            self.load_const(slice(lower.value, upper.value, step.value))
            return

        regs = self.register_list()
        regs[0](lower)
        regs[1](upper)
        regs[2](step)
        self.BUILD_SLICE(regs[0])

    def visit_Attribute(self, t):
        name = mangle(self.scope.private, t.attr)
        reg = self.register()
        reg(t.value)
        self.LOAD_ATTR(reg, self.names[name])
        reg.clear()

    def visit_sequence(self, t):
        assert not hasattr(t, 'ctx') or isinstance(t.ctx, ast.Load)
        base = self.next_register
        seen_star = False
        for i,e in enumerate(t.elts):
            if type(e) == ast.Starred:
                if not seen_star:
                    if type(t) == ast.Set:
                        self.BUILD_SET(base, i)
                    else:
                        self.BUILD_LIST(base, i)
                    self.next_register = base + 1
                    self.STORE_FAST(base)
                    seen_star = True
                self(e.value)
                if type(t) == ast.Set:
                    self.SET_UPDATE(base)
                else:
                    self.LIST_EXTEND(base)
            elif seen_star:
                self(e)
                if type(t) == ast.Set:
                    self.SET_ADD(base)
                else:
                    self.LIST_APPEND(base)
            else:
                reg = self.register()
                reg.allocate()  # TODO: reserve only
                self.to_register(e, reg)
        if not seen_star:
            if type(t) == ast.Set:
                self.BUILD_SET(base, len(t.elts))
            elif type(t) == ast.Tuple:
                self.BUILD_TUPLE(base, len(t.elts))
            else:
                self.BUILD_LIST(base, len(t.elts))
        else:
            self.LOAD_FAST(base)
            self.CLEAR_FAST(base)
            if type(t) == ast.Tuple:
                self.call_intrinsic('PyList_AsTuple')

    visit_List = visit_sequence
    visit_Tuple = visit_sequence
    visit_Set = visit_sequence

    def visit_ListAppend(self, t):
        reg = self.register()
        reg(t.list)
        self(t.item)
        self.LIST_APPEND(reg)
        reg.clear()

    def visit_SetAdd(self, t):
        reg = self.register()
        reg(t.set)
        self(t.elt)
        self.SET_ADD(reg)
        reg.clear()

    def visit_UnaryOp(self, t):
        self(t.operand)
        self.ops1[type(t.op)](self)
    ops1 = {ast.UAdd: ops.UNARY_POSITIVE,  ast.Invert: ops.UNARY_INVERT,
            ast.USub: ops.UNARY_NEGATIVE,  ast.Not:    ops.UNARY_NOT}

    def visit_BinOp(self, t):
        reg = self.register()
        reg(t.left)
        self(t.right)
        self.ops2[type(t.op)](self, reg)
        reg.clear()
    ops2 = {ast.Pow:    ops.BINARY_POWER,  ast.Add:  ops.BINARY_ADD,
            ast.LShift: ops.BINARY_LSHIFT, ast.Sub:  ops.BINARY_SUBTRACT,
            ast.RShift: ops.BINARY_RSHIFT, ast.Mult: ops.BINARY_MULTIPLY,
            ast.BitOr:  ops.BINARY_OR,     ast.Mod:  ops.BINARY_MODULO,
            ast.BitAnd: ops.BINARY_AND,    ast.Div:  ops.BINARY_TRUE_DIVIDE,
            ast.BitXor: ops.BINARY_XOR,    ast.FloorDiv: ops.BINARY_FLOOR_DIVIDE,
            ast.MatMult: ops.BINARY_MATRIX_MULTIPLY}

    def emit_compare(self, operator, reg):
        optype = type(operator)
        if optype == ast.Is:
            self.IS_OP(reg)
        elif optype == ast.IsNot:
            self.IS_OP(reg)
            self.UNARY_NOT_FAST()
        elif optype == ast.In:
            self.CONTAINS_OP(reg)
        elif optype == ast.NotIn:
            self.CONTAINS_OP(reg)
            self.UNARY_NOT_FAST()
        else:
            cmp_index = dis.cmp_op.index(self.ops_cmp[optype])
            self.COMPARE_OP(cmp_index, reg)

    def is_temporary(self, r):
        assert isinstance(r, int)
        return r >= self.nlocals

    def visit_Compare(self, t):
        N = len(t.ops)
        label = Label()
        r1 = self.as_register(t.left)
        r2 = None
        for i, (operator, right) in enumerate(zip(t.ops, t.comparators)):
            if i > 0:
                if self.is_temporary(r1) and self.is_temporary(r2):
                    self.MOVE(r1, r2)
                    self.next_register = r2
                else:
                    if self.is_temporary(r1):
                        self.CLEAR_FAST(r1)
                    r1 = r2
                r2 = None
                self.JUMP_IF_FALSE(label)
            if i < len(t.ops) - 1:
                r2 = self.as_register(right)
                self.LOAD_FAST(r2)
            else:
                self(right)
            self.emit_compare(operator, r1)
        self.LABEL(label)
        if self.is_temporary(r1):
            self.CLEAR_FAST(r1)
    ops_cmp = {ast.Eq: '==', ast.NotEq: '!=', ast.Is: 'is', ast.IsNot: 'is not',
               ast.Lt: '<',  ast.LtE:   '<=', ast.In: 'in', ast.NotIn: 'not in',
               ast.Gt: '>',  ast.GtE:   '>='}

    def visit_BoolOp(self, t):
        after = Label()
        for i, value in enumerate(t.values):
            if i != 0:
                self.CLEAR_ACC()
            self(value)
            if type(t.op) == ast.And:
                self.JUMP_IF_FALSE(after)
            elif type(t.op) == ast.Or:
                self.JUMP_IF_TRUE(after)
            else:
                assert False, type(t.op)
        self.LABEL(after)

    def visit_Pass(self, t):
        return no_op

    def visit_Raise(self, t):
        if t.cause is not None:
            regs = self.register_list()
            regs[0](t.exc)
            regs[1](t.cause)
            self.call_intrinsic('vm_exc_set_cause', regs.base, 2)
        elif t.exc:
            self(t.exc)
        self.RAISE()

    def visit_Global(self, t):
        return no_op

    def visit_Nonlocal(self, t):
        return no_op

    def visit_Import(self, t):
        for alias in t.names:
            self.import_name(0, None, alias.name)
            self.store(alias.asname or alias.name.split('.')[0])

    def visit_ImportFrom(self, t):
        fromlist = tuple([alias.name for alias in t.names])
        self.import_name(t.level, fromlist, t.module)
        reg = self.register()
        self.STORE_FAST(reg.allocate())
        for alias in t.names:
            if alias.name == '*':
                self.IMPORT_STAR(reg)
            else:
                self.IMPORT_FROM(reg, self.constants[alias.name])
                self.store(alias.asname or alias.name)
        reg.clear()

    def import_name(self, level, fromlist, name):
        arg = (name or '', fromlist, level)
        self.IMPORT_NAME(self.constants[arg])

    def visit_While(self, t):
        loop = self.push_loop()
        self.JUMP(loop.next)
        self.LABEL(loop.top)
        self(t.body)
        self.LABEL(loop.next)
        self(t.test)
        self.POP_JUMP_IF_TRUE(loop.top)
        self.LABEL(loop.anchor)
        if t.orelse:
            self(t.orelse)
        self.LABEL(loop.exit)

    def visit_For(self, t):
        # top, next, anchor, exit
        self(t.iter)
        loop = self.push_loop(self.new_register())
        self.GET_ITER(loop.reg)
        self.JUMP(loop.next)
        self.LABEL(loop.top)
        self.assign_accumulator(t.target)
        self(t.body)
        self.LABEL(loop.next)
        self.FOR_ITER(loop.reg, loop.top)
        self.LABEL(loop.anchor)
        if t.orelse:
            self(t.orelse)
        self.LABEL(loop.exit)

    def visit_ForIter(self, t):
        top, next = Label(), Label()
        reg = self.as_register(t.iter)
        self.JUMP(next)
        self.LABEL(top)
        self.assign_accumulator(t.target)
        self(t.body)
        self.LABEL(next)
        self.FOR_ITER(reg, top)

    def visit_AsyncFor(self, t):
        self(t.iter)
        loop = self.push_loop(self.new_register())
        self.GET_AITER(loop.reg)
        self.JUMP(loop.next)
        self.LABEL(loop.top)
        self.assign_accumulator(t.target)
        self(t.body)
        self.LABEL(loop.next)
        awaitable = self.new_register()
        assert awaitable == loop.reg + 1  # GET_ANEXT uses two adjacent registers
        self.GET_ANEXT(loop.reg)
        self.load_const(None)
        self.YIELD_FROM(awaitable)
        self.CLEAR_FAST(awaitable)
        self.next_register = awaitable
        self.JUMP(loop.top)
        link_reg = self.new_register(2)
        self.LABEL(ExceptHandler(loop.next, loop.anchor, link_reg))
        self.END_ASYNC_FOR(loop.reg)
        self.next_register = loop.reg
        self.LABEL(loop.anchor)
        if t.orelse:
            self(t.orelse)
        self.LABEL(loop.exit)

    def push_loop(self, reg=None):
        loop = Loop(reg)
        self.blocks.append(loop)
        return loop

    def visit_Return(self, t):
        if t.value:
            self(t.value)
        else:
            self.load_const(None)

        for block in reversed(self.blocks):
            if isinstance(block, TryFinally):
                self.STORE_FAST(block.reg + 1)
            block.on_exit(self)

        self.RETURN_VALUE()

    def visit_Try(self, t):
        if t.finalbody:
            self.try_finally(t)
        else:
            self.try_except(t)

    def try_finally(self, t):
        start, handler_end = Label(), Label()
        link_reg = self.next_register
        handler = ExceptHandler(start, handler_end, link_reg)

        self.LABEL(start)
        self.blocks.append(TryFinally(handler, link_reg))
        if len(t.handlers) > 0:
            self.try_except(t)
        else:
            self(t.body)
        self.blocks.pop()  # pop try-finally block
        self.next_register = link_reg # no scoping in try_except because we don't go through __call__
        self.LABEL(handler)
        self.blocks.append(Finally(link_reg))
        r = self.new_register(2)  # actually allocate token register
        assert r == link_reg, (r, link_reg)
        self(t.finalbody)
        self.END_FINALLY(link_reg)
        self.LABEL(handler_end)
        self.blocks.pop()

    def try_except(self, t):
        start = Label()
        labels = [Label() for _ in range(len(t.handlers))]
        orelse, end = Label(), Label()
        link_reg = self.next_register

        self.LABEL(start)
        self(t.body)
        self.JUMP(orelse)
        r = self.new_register(2)  # actually allocate token register
        assert r == link_reg
        self.LABEL(ExceptHandler(start, orelse, link_reg))
        self.blocks.append(ExceptBlock(link_reg))
        for i,handler in enumerate(t.handlers):
            if handler.type:
                self(handler.type)
                self.JUMP_IF_NOT_EXC_MATCH(link_reg, labels[i])
            if handler.name:
                # FIXME: we need to wrap another exception handler
                # here to clear the name. Also another exception block
                # to clear the name on control flow out of it.
                self.LOAD_FAST(link_reg + 1)
                self.store(handler.name)
                self(handler.body)
                self.clear(handler.name)
            else:
                self(handler.body)
            self.END_EXCEPT(link_reg)
            self.JUMP(end)
            self.LABEL(labels[i])
        self.END_FINALLY(link_reg)
        self.blocks.pop()
        self.next_register = r
        self.LABEL(orelse)
        if t.orelse:
            self(t.orelse)
        self.LABEL(end)

    def visit_Break(self, t):
        for block in reversed(self.blocks):
            block.on_exit(self)
            if isinstance(block, Loop):
                self.JUMP(block.exit)
                return

    def visit_Continue(self, t):
        for block in reversed(self.blocks):
            if isinstance(block, Loop):
                self.JUMP(block.next)
                return
            block.on_exit(self)

    def visit_Yield(self, t):
        if t.value:
            self(t.value)
        else:
            self.load_const(None)
        self.YIELD_VALUE()

    def visit_YieldFrom(self, t):
        reg = self.register()
        self(t.value)
        self.GET_YIELD_FROM_ITER(reg.allocate())
        self.load_const(None)
        self.YIELD_FROM(reg)
        reg.clear()

    def visit_Await(self, t):
        reg = self.register()
        self(t.value)
        self.GET_AWAITABLE(reg.allocate())
        self.load_const(None)
        self.YIELD_FROM(reg)
        reg.clear()

    def visit_Function(self, t):
        regs = self.register_list()
        defaults = t.args.defaults + t.args.kw_defaults
        for i,arg in enumerate(defaults):
            if arg is not None:
                regs[i](arg)
            scope = self.scope.children[t]
            name = scope.argname(scope.default_idx(i))
            scope.free2reg[name] = (regs.base + i, scope.free2reg[name][1])

        code = self.sprout(t).compile_function(t)
        self.make_closure(code)
        for i in reversed(range(len(defaults))):
            regs[i].clear()

    def visit_AsyncFunction(self, t):
        return self.visit_Function(t)

    def sprout(self, t):
        return CodeGen(self.filename, self.scope.children[t])

    def make_closure(self, code):
        self.MAKE_FUNCTION(self.constants[code])

    def compile_function(self, t):
        # self.load_const(ast.get_docstring(t))
        self.FUNC_HEADER(0)
        if type(t) == AsyncFunction:
            self.COROGEN_HEADER(2)
        elif self.scope.is_generator:
            self.COROGEN_HEADER(1)
        self(t.body)
        self.load_const(None)
        self.RETURN_VALUE()

        code = self.make_code(
            t.name,
            argcount=len(t.args.args) + len(t.args.posonlyargs),
            posonlyargcount=len(t.args.posonlyargs),
            kwonlyargcount=len(t.args.kwonlyargs),
            ndefaultargs=len(t.args.defaults + t.args.kw_defaults),
            has_varargs=t.args.vararg is not None,
            has_varkws=t.args.kwarg is not None)
        return code

    def visit_BuildClass(self, t):
        self.LOAD_BUILD_CLASS()

    def visit_MakeClosure(self, t):
        self.make_closure(t.code)

    def visit_Class(self, t):
        code = self.sprout(t).compile_class(t)
        regs = self.register_list()

        self(ast.Call(
            func=BuildClass(),
            args=[MakeClosure(code, t.name), ast.Constant(t.name)] + t.bases,
            keywords=t.keywords,
        ))

    def compile_class(self, t):
        docstring = ast.get_docstring(t)
        self.FUNC_HEADER(0)
        self.load('__name__')
        self.store('__module__')
        self.load_const(t.name)
        self.store('__qualname__')
        if docstring is not None:
            self.load_const(docstring)
            self.store('__doc__')
        self(t.body)
        if self.scope.needs_class_closure:
            # Store the __class__ cell in locals()['__classcell__]
            self.LOAD_FAST(self.varnames['__class__'])
            self.store('__classcell__')
            # Return the __class__ cell
            self.LOAD_FAST(self.varnames['__class__'])
        else:
            self.load_const(None)
        self.RETURN_VALUE()
        return self.make_code(t.name)

def unpack(key, tp):
    if tp == slice:
        return slice(*key)
    return key

def pack(key):
    tp = type(key)
    if isinstance(key, slice):
        key = (key.start, key.stop, key.step)
    return (key, tp)

class constants(collections.defaultdict):
    def __init__(self):
        super().__init__(lambda: len(self))
    def __getitem__(self, key):
        return super().__getitem__(pack(key))
    def collect(self):
        return tuple(unpack(key, tp) for key,tp in self.keys())

def make_table():
    table = collections.defaultdict(lambda: len(table))
    return table

def collect(table):
    return tuple(sorted(table, key=table.get))

def compile3(ast, filename, optimize):
    print('compiling', filename, file=sys.stderr)
    return code_for_module(filename, ast)

def load_file(filename, module_name):
    f = open(filename)
    source = f.read()
    f.close()
    astobj = compile(source, filename, 'exec', ast.PyCF_ONLY_AST|ast.PyCF_OPTIMIZE_AST)
    return module_from_ast(module_name, filename, astobj)

def module_from_ast(module_name, filename, t):
    code = code_for_module(filename, t)
    module = types.ModuleType(module_name, ast.get_docstring(t))
    print(dis.dis(code))
    import time
    start = time.perf_counter()
    exec(code, module.__dict__)
    end = time.perf_counter()
    print(end - start)
    return module

def code_for_module(filename, t):
    t = desugar(t)
    check_conformity(t)
    return CodeGen(filename, top_scope(t)).compile_module(t)

def desugar(t):
    return ast.fix_missing_locations(Desugarer().visit(t))

def rewriter(rewrite):
    def visit(self, t):
        return ast.copy_location(rewrite(self, self.generic_visit(t)),
                                 t)
    return visit

def Call(fn, args):
    return ast.Call(fn, args, [])

class ListAppend(ast.stmt):
    _fields = ('list', 'item')

class SetAdd(ast.stmt):
    _fields = ('set', 'elt')

class GetIter(ast.expr):
    _fields = ('seq',)

class ForIter(ast.stmt):
    _fields = ('target', 'iter', 'body')

class Desugarer(ast.NodeTransformer):

    @rewriter
    def visit_Assert(self, t):
        return ast.If(t.test,
                      [],
                      [ast.Raise(Call(ast.Name('AssertionError', load),
                                      [] if t.msg is None else [t.msg]),
                                 None)])

    @rewriter
    def visit_Lambda(self, t):
        return Function('<lambda>', t.args, [ast.Return(t.body)])

    @rewriter
    def visit_Index(self, t):
        return t.value

    @rewriter
    def visit_With(self, t):
        for item in reversed(t.items[1:]):
            t.body = ast.With([item], t.body)
        t.items = [t.items[0]]
        return t

    @rewriter
    def visit_AsyncWith(self, t):
        for item in reversed(t.items[1:]):
            t.body = ast.AsyncWith([item], t.body)
        t.items = [t.items[0]]
        return t

    @rewriter
    def visit_FunctionDef(self, t):
        fn = Function(t.name, t.args, t.body)
        for d in reversed(t.decorator_list):
            fn = Call(d, [fn])
        return ast.Assign([ast.Name(t.name, store)], fn)

    @rewriter
    def visit_AsyncFunctionDef(self, t):
        fn = AsyncFunction(t.name, t.args, t.body)
        for d in reversed(t.decorator_list):
            fn = Call(d, [fn])
        return ast.Assign([ast.Name(t.name, store)], fn)

    @rewriter
    def visit_ClassDef(self, t):
        cls = Class(t.name, t.bases, t.keywords, t.body)
        for d in reversed(t.decorator_list):
            cls = Call(d, [cls])
        return ast.Assign([ast.Name(t.name, store)], cls)

    def rewrite_comprehension(self, t):
        if isinstance(t, ast.ListComp):
            name = '<listcomp>'
            initial_value = ast.List([], load)
            body = ListAppend(ast.Name('.1', load), t.elt)
        elif isinstance(t, ast.DictComp):
            name = '<dictcomp>'
            initial_value = ast.Dict(keys=[], values=[])
            body = ast.Assign(
                targets=[ast.Subscript(value=ast.Name('.1', load), slice=t.key, ctx=store)],
                value=[t.value])
        elif isinstance(t, ast.SetComp):
            name = '<setcomp>'
            initial_value = ast.Set([])
            body = SetAdd(ast.Name('.1', load), t.elt)
        elif isinstance(t, ast.GeneratorExp):
            name = '<genexpr>'
            body = ast.Yield(value=t.elt)
        else:
            assert False, type(t)

        for loop in reversed(t.generators):
            for test in reversed(loop.ifs):
                body = ast.If(test, [body], [])
            if loop == t.generators[0]:
                body = ForIter(loop.target, ast.Name('.0', load), [body])
            else:
                body = ast.For(loop.target, loop.iter, [body], [])
        if isinstance(t, ast.GeneratorExp):
            body = [body]
        else:
            body = [
                ast.Assign([ast.Name('.1', store)], initial_value),
                body,
                ast.Return(ast.Name('.1', load)),
            ]
        args = ast.arguments([], [ast.arg('.0', None)], None, [], [], [], [])
        return Call(Function(name, args, body),
                    [GetIter(t.generators[0].iter)])

    @rewriter
    def visit_ListComp(self, t):
        return self.rewrite_comprehension(t)

    @rewriter
    def visit_DictComp(self, t):
        return self.rewrite_comprehension(t)

    @rewriter
    def visit_SetComp(self, t):
        return self.rewrite_comprehension(t)

    @rewriter
    def visit_GeneratorExp(self, t):
        return self.rewrite_comprehension(t)

class Function(ast.FunctionDef):
    _fields = ('name', 'args', 'body')

class AsyncFunction(ast.AsyncFunctionDef):
    _fields = ('name', 'args', 'body')

class Class(ast.ClassDef):
    _fields = ('name', 'bases', 'keywords', 'body')

class BuildClass(ast.Expression):
    pass

class MakeClosure(ast.Expression):
    _fields = ('code', 'name')

load, store = ast.Load(), ast.Store()

def mangle(private, name):
    if private is None or not name.startswith('__'):
        return name
    if name.endswith('__') or '.' in name:
        return name
    private = private.lstrip('_')
    return f'_{private}{name}'

def top_scope(t):
    top = Scope(t, 'module', (), None)
    top.visit(t)
    top.analyze(set())
    top.assign_regs({})
    return top

class Scope(ast.NodeVisitor):
    def __init__(self, t, scope_type, defs, parent_scope):
        self.t = t
        self.children = {}       # Enclosed sub-scopes
        self.defs = {name: i for i, name in enumerate(defs)}  # Variables defined
        self.uses = set()        # Variables referenced
        self.globals = set()
        self.nonlocals = set()
        self.free2reg = {}
        self.scope_type = scope_type
        self.is_generator = False
        self.is_async = False
        self.private = (t.name if scope_type == 'class' else
                        parent_scope.private if parent_scope is not None else
                        None)
        self.needs_class_closure = False  # true if a closure over __class__ should be created
        self.nested = (parent_scope is not None and 
                       (parent_scope.nested or
                        parent_scope.scope_type == 'function'))

    def visit_Class(self, t):
        self.define(t.name)
        for expr in t.bases: self.visit(expr)
        subscope = Scope(t, 'class', (), self)
        self.children[t] = subscope
        for stmt in t.body: subscope.visit(stmt)

    def argname(self, i):
        args = self.t.args
        all_args = args.posonlyargs + args.args + args.kwonlyargs
        return all_args[i].arg

    def default_idx(self, i):
        args = self.t.args
        defaults = args.defaults + args.kw_defaults
        return len(args.args) - len(defaults) + i

    def assign_defaults(self, t):
        defaults = t.args.defaults + t.args.kw_defaults
        for i,d in enumerate(defaults):
            reg = self.default_idx(i)
            name = self.argname(reg)
            upval = 'upvalTODO'
            self.free2reg[name] = (upval, reg)

    def function(self, t, is_async):
        args = t.args
        all_args = args.posonlyargs + args.args + args.kwonlyargs + [args.vararg, args.kwarg]
        subscope = Scope(t, 'function', [arg.arg for arg in all_args if arg], self)
        subscope.is_async = is_async
        subscope.assign_defaults(t)
        self.children[t] = subscope
        for stmt in t.body: subscope.visit(stmt)

    def visit_Function(self, t):
        self.function(t, is_async=False)

    def visit_AsyncFunction(self, t):
        self.function(t, is_async=True)

    def define(self, name):
        name = mangle(self.private, name)
        if name in self.nonlocals:
            self.uses.add(name)
        elif name not in self.defs and name not in self.globals:
            self.defs[name] = len(self.defs)

    def visit_Import(self, t):
        # not mangled?
        for alias in t.names:
            self.define(alias.asname or alias.name.split('.')[0])

    def visit_ImportFrom(self, t):
        for alias in t.names:
            if alias.name == '*':
                continue
            self.define(alias.asname or alias.name)

    def visit_ExceptHandler(self, t):
        if t.name:
            self.define(t.name)
        super().generic_visit(t)

    def visit_Name(self, t):
        if isinstance(t.ctx, ast.Load):
            name = mangle(self.private, t.id)
            self.uses.add(name)
            if name == 'super' and self.scope_type == 'function':
                self.uses.add('__class__')
        elif isinstance(t.ctx, (ast.Store, ast.Del)):
            self.define(t.id)
        else:
            assert False

    def visit_Yield(self, t):
        self.is_generator = True
        super().generic_visit(t)

    def visit_YieldFrom(self, t):
        self.is_generator = True
        super().generic_visit(t)

    def visit_Await(self, t):
        assert self.is_async, 'await outside of async function'
        super().generic_visit(t)

    def visit_Global(self, t):
        for name in t.names:
            name = mangle(self.private, name)
            assert name not in self.defs, "name '%r' is assigned to before global declaration" % (name,)
            assert name not in self.uses, "name '%r' is used prior to global declaration" % (name,)
            self.globals.add(name)

    def visit_Nonlocal(self, t):
        for name in t.names:
            name = mangle(self.private, name)
            assert name not in self.defs, "name '%r' is assigned to before nonlocal declaration" % (name,)
            assert name not in self.uses, "name '%r' is used prior to nonlocal declaration" % (name,)
            self.nonlocals.add(name)

    def analyze(self, parent_defs):
        self.local_defs = set(self.defs.keys()) if self.scope_type == 'function' else set()
        newbound = parent_defs | self.local_defs
        if self.scope_type == 'class':
            newbound.add('__class__')
        for child in self.children.values():
            child.analyze(newbound)
        child_uses = set([var for child in self.children.values()
                              for var in child.freevars])
        uses = self.uses | child_uses
        self.cellvars = child_uses & self.local_defs
        self.freevars = (uses & (parent_defs - set(self.defs.keys())))
        self.derefvars = self.cellvars | self.freevars
        if self.scope_type == 'class' and '__class__' in child_uses:
            self.needs_class_closure = True
            self.cellvars.add('__class__')  # but not in derefvars!

    def assign_regs(self, parent_regs):
        self.regs = self.defs.copy() if self.scope_type == 'function' else {'<locals>': 0}
        if self.needs_class_closure:
            assert len(self.regs) == 1
            self.regs['__class__'] = len(self.regs)
        for name, upval in parent_regs.items():
            if name in self.freevars:
                reg = len(self.regs)
                self.regs[name] = reg
                self.free2reg[name] = (upval, reg)

        assert all(name in self.regs for name in self.freevars), parent_regs
        for child in self.children.values():
            child.assign_regs(self.regs)


    def access(self, name):
        return ('classderef' if name in self.derefvars and self.scope_type == 'class' else
                'deref' if name in self.derefvars else
                'fast'  if name in self.local_defs else
                'global' if name in self.globals or self.scope_type == 'function' else
                'name')

if __name__ == '__main__':
    sys.argv.pop(0)
    load_file(sys.argv[0], '__main__')