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
import contextlib
from functools import reduce
from itertools import chain
from check_subset import check_conformity

def assemble(assembly):
    return bytes(iter(assembly.encode(0, dict(assembly.resolve(0)))))

def make_lnotab(assembly):
    firstlineno, lnotab = None, []
    byte, line = 0, None
    for next_byte, next_line in assembly.line_nos(0):
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
    return firstlineno or 1, bytes(lnotab)

def concat(assemblies):
    return sum(assemblies, no_op)

class Assembly:
    def __add__(self, other):
        assert isinstance(other, Assembly), other
        return Chain(self, other)
    length = 0
    def resolve(self, start):
        return ()
    def encode(self, start, addresses):
        return b''
    def line_nos(self, start):
        return ()
    def plumb(self, depths):
        pass

no_op = Assembly()

class Accumulator:
    pass

class Label(Assembly):
    def resolve(self, start):
        return ((self, start),)

class SetLineNo(Assembly):
    def __init__(self, line):
        self.line = line
    def line_nos(self, start):
        return ((start, self.line),)

class Instruction(Assembly):
    length = 4

    def __init__(self, opcode, arg, arg2):
        self.opcode = opcode
        self.arg    = arg
        self.arg2   = arg2
    def encode(self, start, addresses):
        arg, arg2 = self.arg, self.arg2
        if dis.opcodes[self.opcode].is_jump():
            arg2 = (addresses[arg2] - start) // 4 - 1
            arg2 += 0x8000
        if arg2 is None:
            arg2 = 0
        else:
            arg2 = int(arg2)
            assert arg2 >= 0 and arg2 < 65536
            # arg2 = arg2 & 0xFFFF
        argA = self.arg or 0
        argB = arg2 & 0xFF
        argC = (arg2 >> 8)
        return bytes([self.opcode, argA, argB, argC])

class Chain(Assembly):
    def __init__(self, assembly1, assembly2):
        self.part1 = assembly1
        self.part2 = assembly2
        self.length = assembly1.length + assembly2.length
    def resolve(self, start):
        return chain(self.part1.resolve(start),
                     self.part2.resolve(start + self.part1.length))
    def encode(self, start, addresses):
        return chain(self.part1.encode(start, addresses),
                     self.part2.encode(start + self.part1.length, addresses))
    def line_nos(self, start):
        return chain(self.part1.line_nos(start),
                     self.part2.line_nos(start + self.part1.length))

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
        return op.CLEAR_FAST(self.reg) if self.is_temporary() else no_op
    def __call__(self, t):
        return self.visitor.to_register(t, self)

class RegisterList:
    def __init__(self, visitor, n):
        self.visitor = visitor
        self.base = visitor.new_register(n)
    def __getitem__(self, i):
        return self.visitor.register(self.base + i)
    def __call__(self, seq):
        return concat([self[i](t) for i,t in enumerate(seq)])

class Reference:
    def __init__(self, visitor):
        self.visitor = visitor
    def load(self):
        return no_op
    def store(self, value):
        return no_op

class Loop:
    def __init__(self):
        self.top = Label()      # iteration
        self.next = Label()     # loop test (continue target)
        self.anchor = Label()   # before "orelse" block
        self.exit = Label()     # break target

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

def register_scope(visitor):
    def visit(self, t):
        top = self.next_register
        ret = visitor(self, t)
        self.next_register = top
        return ret
    return visit

class CodeGen(ast.NodeVisitor):
    def __init__(self, filename, scope):
        self.filename  = filename
        self.scope     = scope
        self.constants = constants()
        self.names     = self.constants
        self.varnames  = scope.regs
        if self.scope.scope_type in ('module', 'class'):
            assert len(self.scope.local_defs) == 0
            self.nlocals = 1
            self.next_register = 1
        else:
            self.nlocals = len(self.scope.local_defs)
            self.next_register = len(self.varnames)
        self.max_registers = self.next_register
        self.loops = []

    def compile_module(self, t, name):
        assembly = self(t.body) + self.load_const(None) + op.RETURN_VALUE
        return self.make_code(assembly, name, 0, False, False)

    def make_code(self, assembly, name, argcount, has_varargs, has_varkws):
        posonlyargcount = 0
        kwonlyargcount = 0
        # stacksize = plumb_depths(assembly)
        nlocals = self.nlocals
        framesize = self.max_registers
        firstlineno, lnotab = make_lnotab(assembly)
        code = assemble(assembly)
        cell2reg = tuple(reg for name, reg in self.varnames.items() if name in self.scope.cellvars)
        assert len(cell2reg) == len(self.scope.cellvars)

        # ((upval0, reg0), (upval1, reg1), ...)
        free2reg = tuple(self.scope.free2reg.values())
        assert len(free2reg) == len(self.scope.freevars)

        flags = (  (0x00 if nlocals                  else 0)
                 | (0x00 if self.scope.freevars      else 0)
                 | (0x10 if self.scope.nested        else 0))

        print('varnames', dict(self.varnames), 'regs', self.scope.regs, 'nlocals', nlocals, 'framesize', framesize)
        print('cell2reg', cell2reg, self.scope.cellvars)
        print('free2reg', free2reg, self.scope.freevars)
        print('free2reg', self.scope.free2reg)
        print()
        return types.Code2Type(code,
                               self.constants.collect(),
                               argcount=argcount,
                               posonlyargcount=posonlyargcount,
                               kwonlyargcount=kwonlyargcount,
                               nlocals=nlocals,
                               framesize=framesize,
                               flags=flags,
                               varnames=collect(self.varnames),
                               filename=self.filename,
                               name=name,
                               firstlineno=firstlineno,
                               linetable=lnotab,
                               freevars=(),
                               cellvars=(),
                               cell2reg=cell2reg,
                               free2reg=free2reg)
        # return types.Code2Type(argcount, posonlyargcount, kwonlyargcount,
        #                       nlocals, stacksize, flags, code,
        #                       self.collect_constants(),
        #                       collect(self.names), collect(self.varnames),
        #                       self.filename, name, firstlineno, lnotab,
        #                       self.scope.freevars, self.scope.cellvars)

    def load_const(self, constant):
        return op.LOAD_CONST(self.constants[constant])

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

    def to_register(self, t, reg):
        if isinstance(t, ast.Name):
            return self.load(t.id, reg)
        elif isinstance(t, Register):
            return self.copy_register(reg, t)
        return self(t) + op.STORE_FAST(reg.allocate())

    def visit_Constant(self, t):
        return self.load_const(t.value)

    def visit_Name(self, t):
        assert isinstance(t.ctx, ast.Load)
        return self.load(t.id)

    def copy_register(self, dst, src):
        if dst.is_placeholder():
            dst.reg = src.reg
            return no_op
        elif src.is_temporary():
            return op.MOVE(dst.allocate(), src.reg)
        else:
            return op.COPY(dst.allocate(), src.reg)

    def load(self, name, dst=None):
        access = self.scope.access(name)
        if isinstance(dst, Register):
            if access == 'fast':
                src = Register(self, self.varnames[name])
                return self.copy_register(dst, src)
            return self.load(name) + op.STORE_FAST(dst.allocate())

        if   access == 'fast':   return op.LOAD_FAST(self.varnames[name])
        elif access == 'deref':  return op.LOAD_DEREF(self.varnames[name])
        elif access == 'name':   return op.LOAD_NAME(self.names[name])
        elif access == 'global': return op.LOAD_GLOBAL(self.names[name])
        else: assert False

    def store(self, name, value=None):
        access = self.scope.access(name)
        if isinstance(value, Register):
            if access == 'fast':
                if value.is_temporary():
                    return op.MOVE(self.varnames[name], value.reg)
                else:
                    return op.LOAD_FAST(value.reg) + op.STORE_FAST(self.varnames[name])
        if   access == 'fast':   return op.STORE_FAST(self.varnames[name])
        elif access == 'deref':  return op.STORE_DEREF(self.varnames[name])
        elif access == 'name':   return op.STORE_NAME(self.names[name])
        elif access == 'global': return op.STORE_GLOBAL(self.names[name])
        else: assert False

    def cell_index(self, name):
        return self.scope.derefvars.index(name)

    def visit_Call(self, t):
        assert len(t.args) < 256 and len(t.keywords) < 256
        assert len(t.keywords) == 0
        # FIXME base register
        opcode = (
                  # op.CALL_FUNCTION_VAR_KW if t.starargs and t.kwargs else
                  # op.CALL_FUNCTION_VAR    if t.starargs else
                  # op.CALL_FUNCTION_KW     if t.kwargs else
                  op.CALL_FUNCTION)
        FRAME_EXTRA = 3
        regs = self.register_list()
        return (regs[2](t.func) +
                concat([regs[3+i](arg) for i,arg in enumerate(t.args)]) +
                opcode(regs.base + FRAME_EXTRA, len(t.args)))

    def visit_keyword(self, t):
        return self.load_const(t.arg) + self(t.value)

    def __call__(self, t):
        if isinstance(t, list): return concat(map(self, t))
        top = self.next_register
        assembly = self.visit(t)
        self.next_register = top
        return SetLineNo(t.lineno) + assembly if hasattr(t, 'lineno') else assembly

    def generic_visit(self, t):
        assert False, t

    def visit_Expr(self, t):
        # TODO: skip constants as optimization
        return self(t.value) + op.CLEAR_ACC

    def resolve(self, t):
        if isinstance(t, ast.Name):
            access = self.scope.access(t.id)
            if access == 'fast':
                return Register(self, self.varnames[t.id])
        return t

    def assign(self, target, value):
        method = 'assign_' + target.__class__.__name__
        visitor = getattr(self, method)
        return visitor(target, value)

    def assign_Name(self, target, value):
        value = self.resolve(value)
        if isinstance(value, Register):
            return self.store(target.id, value)
        return self(value) + self.store(target.id)

    def assign_Attribute(self, target, value):
        reg = self.register()
        attr = self.constants[target.attr]
        # FIXME: clear reg if temporary ???
        return reg(target.value) + self(value) + op.STORE_ATTR(reg, attr) + reg.clear()

    def assign_Subscript(self, target, value):
        reg1 = self.register()
        reg2 = self.register()
        # FIXME: clear regs if temporary ???
        # FIXME: target.slice.value ???
        return (  reg1(target.value)
                + reg2(target.slice)
                + self(value)
                + op.STORE_SUBSCR(reg1, reg2)
                + reg2.clear()
                + reg1.clear())

    def assign_List(self, target, value): return self.assign_sequence(target, value)
    def assign_Tuple(self, target, value): return self.assign_sequence(target, value)
    def assign_sequence(self, target, value):
        reg = self.register_list()
        n = len(target.elts)
        return (  self(value)
                + op.UNPACK_SEQUENCE(reg.base, len(target.elts))
                + concat([self.assign(target.elts[i], reg[i]) for i in range(n)]))

    def visit_AugAssign(self, t):
        ref = self.reference(t.target)
        reg = self.register()
        return (  ref.load(reg)
                + self(t.value)
                + self.aug_ops[type(t.op)](reg)
                + reg.clear()
                + ref.store())
    aug_ops = {ast.Pow:    op.INPLACE_POWER,  ast.Add:  op.INPLACE_ADD,
               ast.LShift: op.INPLACE_LSHIFT, ast.Sub:  op.INPLACE_SUBTRACT,
               ast.RShift: op.INPLACE_RSHIFT, ast.Mult: op.INPLACE_MULTIPLY,
               ast.BitOr:  op.INPLACE_OR,     ast.Mod:  op.INPLACE_MODULO,
               ast.BitAnd: op.INPLACE_AND,    ast.Div:  op.INPLACE_TRUE_DIVIDE,
               ast.BitXor: op.INPLACE_XOR,    ast.FloorDiv: op.INPLACE_FLOOR_DIVIDE}

    def reference(self, target):
        method = 'reference_' + target.__class__.__name__
        visitor = getattr(self, method)
        return visitor(target)

    def reference_Name(self, t):
        visitor = self
        class NameRef:
            def load(self, dst):
                return visitor.load(t.id, dst)
            def store(self):
                return visitor.store(t.id)
        return NameRef()

    def reference_Attribute(self, t):
        visitor = self
        rvalue = self.register()
        class AttributeRef:
            def load(self, dst):
                return (  rvalue(t.value)
                        + op.LOAD_ATTR(rvalue, visitor.names[t.attr])
                        + op.STORE_FAST(dst.allocate()))
            def store(self):
                return (  op.STORE_ATTR(rvalue, visitor.names[t.attr])
                        + rvalue.clear())
        return AttributeRef()

    def reference_Subscript(self, t):
        visitor = self
        # foo[bar()] += baz()
        # foo -> reg
        rvalue = self.register()
        rslice = self.register()
        class SubscrRef:
            def load(self, dst):
                return (  rvalue(t.value)
                        + rslice(t.slice)
                        + op.LOAD_FAST(rslice)
                        + op.BINARY_SUBSCR(rvalue)
                        + op.STORE_FAST(dst.allocate()))
            def store(self):
                return (  op.STORE_SUBSCR(rvalue, rslice)
                        + rslice.clear()
                        + rvalue.clear())
        return SubscrRef()

    def visit_Assign(self, t):
        assert len(t.targets) == 1
        return self.assign(t.targets[0], t.value)

    def visit_If(self, t):
        orelse, after = Label(), Label()
        return (           self(t.test) + op.POP_JUMP_IF_FALSE(orelse)
                         + self(t.body) + op.JUMP(after)
                + orelse + self(t.orelse)
                + after)

    def visit_IfExp(self, t):
        orelse, after = Label(), Label()
        return (           self(t.test) + op.POP_JUMP_IF_FALSE(orelse)
                         + self(t.body) + op.JUMP(after)
                + orelse + self(t.orelse)
                + after)

    def visit_Dict(self, t):
        regs = self.register_list(2)
        return (  op.BUILD_MAP(min(0xFFFF, len(t.keys)))
                + op.STORE_FAST(regs[0])
                + concat([regs[1](k) + self(v) +
                          op.STORE_SUBSCR(regs[0], regs[1]) +
                          regs[1].clear()
                          for k, v in zip(t.keys, t.values)])
                + op.LOAD_FAST(regs[0])
                + op.CLEAR_FAST(regs[0]))

    def visit_Subscript(self, t):
        assert type(t.ctx) == ast.Load
        reg = self.register()
        return (  reg(t.value)
                + self(t.slice)
                + op.BINARY_SUBSCR(reg)
                + reg.clear())

    def visit_Index(self, t):
        return self(t.value)

    def as_constant(self, arg):
        if arg is None:
            return ast.Constant(None)
        return arg

    def visit_Slice(self, t):
        lower, upper, step = (
            ast.Constant(None) if x is None else x
            for x in (t.lower, t.upper, t.step)
        )
        if all(isinstance(x, ast.Constant) for x in (lower, upper, step)):
            return self.load_const(slice(lower.value, upper.value, step.value))
        regs = self.register_list()
        return (regs[0](lower) +
                regs[1](upper) +
                regs[2](step) +
                op.BUILD_SLICE(regs[0]))

    def visit_Attribute(self, t):
        reg = self.register()
        return (  reg(t.value)
                + op.LOAD_ATTR(reg, self.names[t.attr])
                + reg.clear())

    def visit_List(self, t):  return self.visit_sequence(t, op.BUILD_LIST)
    def visit_Tuple(self, t): return self.visit_sequence(t, op.BUILD_TUPLE)

    def visit_sequence(self, t, build_op):
        assert isinstance(t.ctx, ast.Load)
        regs = self.register_list()
        return regs(t.elts) + build_op(regs[0], len(t.elts))

    def visit_ListAppend(self, t):
        reg = self.register()
        return reg(t.list) + self(t.item) + op.LIST_APPEND(reg) + reg.clear()

    def visit_UnaryOp(self, t):
        return self(t.operand) + self.ops1[type(t.op)]
    ops1 = {ast.UAdd: op.UNARY_POSITIVE,  ast.Invert: op.UNARY_INVERT,
            ast.USub: op.UNARY_NEGATIVE,  ast.Not:    op.UNARY_NOT}

    def visit_Accumulator(self, t):
        return no_op

    def visit_BinOp(self, t):
        reg = self.register()
        return reg(t.left) + self(t.right) + self.ops2[type(t.op)](reg) + reg.clear()

    ops2 = {ast.Pow:    op.BINARY_POWER,  ast.Add:  op.BINARY_ADD,
            ast.LShift: op.BINARY_LSHIFT, ast.Sub:  op.BINARY_SUBTRACT,
            ast.RShift: op.BINARY_RSHIFT, ast.Mult: op.BINARY_MULTIPLY,
            ast.BitOr:  op.BINARY_OR,     ast.Mod:  op.BINARY_MODULO,
            ast.BitAnd: op.BINARY_AND,    ast.Div:  op.BINARY_TRUE_DIVIDE,
            ast.BitXor: op.BINARY_XOR,    ast.FloorDiv: op.BINARY_FLOOR_DIVIDE}

    def visit_Compare(self, t):
        [operator], [right] = t.ops, t.comparators
        optype = type(operator)

        if optype == ast.Is:
            opcode = lambda reg: op.IS_OP(reg)
        elif optype == ast.IsNot:
            opcode = lambda reg: op.IS_OP(reg) + op.UNARY_NOT_FAST
        elif optype == ast.In:
            opcode = lambda reg: op.CONTAINS_OP(reg)
        elif optype == ast.NotIn:
            opcode = lambda reg: op.CONTAINS_OP(reg) + op.UNARY_NOT_FAST
        else:
            cmp_index = dis.cmp_op.index(self.ops_cmp[type(operator)])
            opcode = lambda reg: op.COMPARE_OP(cmp_index, reg)
        reg = self.register()
        return reg(t.left) + self(right) + opcode(reg) + reg.clear()
    ops_cmp = {ast.Eq: '==', ast.NotEq: '!=', ast.Is: 'is', ast.IsNot: 'is not',
               ast.Lt: '<',  ast.LtE:   '<=', ast.In: 'in', ast.NotIn: 'not in',
               ast.Gt: '>',  ast.GtE:   '>='}

    def visit_BoolOp(self, t):
        op_jump = self.ops_bool[type(t.op)]
        def compose(left, right):
            after = Label()
            return left + op_jump(after) + op.CLEAR_ACC + right + after
        return reduce(compose, map(self, t.values))
    ops_bool = {ast.And: op.JUMP_IF_FALSE,
                ast.Or:  op.JUMP_IF_TRUE}

    def visit_Pass(self, t):
        return no_op

    def visit_Raise(self, t):
        return self(t.exc) + op.RAISE_VARARGS(1)

    def visit_Global(self, t):
        return no_op

    def visit_Import(self, t):
        return concat([self.import_name(0, None, alias.name)
                       + self.store(alias.asname or alias.name.split('.')[0])
                       for alias in t.names])

    def visit_ImportFrom(self, t):
        fromlist = tuple([alias.name for alias in t.names])
        return (self.import_name(t.level, fromlist, t.module)
                + concat([op.IMPORT_FROM(self.names[alias.name])
                          + self.store(alias.asname or alias.name)
                         for alias in t.names])
                + op.CLEAR_ACC)

    def import_name(self, level, fromlist, name):
        arg = (name, fromlist, level)
        return op.IMPORT_NAME(self.constants[arg])

    def visit_While(self, t):
        with self.loop() as loop:
            return (  op.JUMP(loop.next)
                    + loop.top
                    + self(t.body)
                    + loop.next + self(t.test) + op.POP_JUMP_IF_TRUE(loop.top)
                    + loop.anchor + (self(t.orelse) if t.orelse else no_op)
                    + loop.exit)

    def visit_For(self, t):
        # top, next, anchor, exit
        reg = self.register()
        with self.loop() as loop:
            return (  self(t.iter) + op.GET_ITER(reg.allocate())
                    + op.JUMP(loop.next)
                    + loop.top + self.assign(t.target, Accumulator())
                    + self(t.body)
                    + loop.next + op.FOR_ITER(reg, loop.top)
                    + loop.anchor + (self(t.orelse) if t.orelse else no_op)
                    + loop.exit)

    def visit_Return(self, t):
        return ((self(t.value) if t.value else self.load_const(None))
                + op.RETURN_VALUE)

    @contextlib.contextmanager
    def loop(self):
        self.loops.append(Loop())
        yield self.loops[-1]
        self.loops.pop()

    def visit_Break(self, t):
        return op.JUMP(self.loops[-1].exit)

    def visit_Function(self, t):
        code = self.sprout(t).compile_function(t)
        return self.make_closure(code, t.name)

    def sprout(self, t):
        return CodeGen(self.filename, self.scope.children[t])

    def make_closure(self, code, name):
        return op.MAKE_FUNCTION(self.constants[code])
        # if code.co_freevars:
        #     return (concat([op.LOAD_CLOSURE(self.cell_index(freevar))
        #                     for freevar in code.co_freevars])
        #             + op.BUILD_TUPLE(255, len(code.co_freevars))
        #             + self.load_const(code) + self.load_const(name)
        #             + op.MAKE_FUNCTION(0x08))
        # else:
        #     return (self.load_const(code) + self.load_const(name)
        #             + op.MAKE_FUNCTION(0))

    def compile_function(self, t):
        self.load_const(ast.get_docstring(t))
        assembly = self(t.body) + self.load_const(None) + op.RETURN_VALUE
        assembly = op.FUNC_HEADER(self.max_registers) + assembly
        return self.make_code(assembly, t.name,
                              len(t.args.args), t.args.vararg, t.args.kwarg)

    def visit_ClassDef(self, t):
        code = self.sprout(t).compile_class(t)
        regs = self.register_list()
        FRAME_EXTRA = 3
        return (  op.LOAD_BUILD_CLASS(regs[2])
                + self.make_closure(code, t.name)
                + op.STORE_FAST(regs[3])
                + self.load_const(t.name)
                + op.STORE_FAST(regs[4])
                + concat([regs[5+i](base) for i,base in enumerate(t.bases)])
                + op.CALL_FUNCTION(regs.base + FRAME_EXTRA, len(t.bases) + 2)
                + self.store(t.name))

    def compile_class(self, t):
        docstring = ast.get_docstring(t)
        assembly = (  self.load('__name__')      + self.store('__module__')
                    + self.load_const(t.name)    + self.store('__qualname__')
                    + (no_op if docstring is None else
                       self.load_const(docstring) + self.store('__doc__'))
                    + self(t.body)
                    + self.load_const(None) + op.RETURN_VALUE)
        return self.make_code(assembly, t.name, 0, False, False)

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

def load_file(filename, module_name):
    f = open(filename)
    source = f.read()
    f.close()
    astobj = compile(source, filename, 'exec', ast.PyCF_ONLY_AST|ast.PyCF_OPTIMIZE_AST)
    return module_from_ast(module_name, filename, astobj)

def module_from_ast(module_name, filename, t):
    code = code_for_module(module_name, filename, t)
    module = types.ModuleType(module_name, ast.get_docstring(t))
    print(dis.dis(code))
    import time
    start = time.perf_counter()
    code.exec(module.__dict__)
    end = time.perf_counter()
    print(end - start)
    # exec(code, module.__dict__)
    return module

def code_for_module(module_name, filename, t):
    t = desugar(t)
    check_conformity(t)
    return CodeGen(filename, top_scope(t)).compile_module(t, module_name)

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
    pass

class StoreSubcript(ast.stmt):
    _fields = ('container', 'slice', 'value')
    pass

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
    def visit_FunctionDef(self, t):
        fn = Function(t.name, t.args, t.body)
        for d in reversed(t.decorator_list):
            fn = Call(d, [fn])
        return ast.Assign([ast.Name(t.name, store)], fn)

    @rewriter
    def visit_ListComp(self, t):
        body = ListAppend(ast.Name('.0', load), t.elt)
        for loop in reversed(t.generators):
            for test in reversed(loop.ifs):
                body = ast.If(test, [body], [])
            body = ast.For(loop.target, loop.iter, [body], [])
        fn = [body,
              ast.Return(ast.Name('.0', load))]
        args = ast.arguments(None, [ast.arg('.0', None)], None, [], None, [], [])

        return Call(Function('<listcomp>', args, fn),
                    [ast.List([], load)])

class Function(ast.FunctionDef):
    _fields = ('name', 'args', 'body')

load, store = ast.Load(), ast.Store()

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
        self.scope_type = scope_type
        self.nested = (parent_scope is not None and 
                       (parent_scope.nested or
                        parent_scope.scope_type == 'function'))

    def visit_ClassDef(self, t):
        self.define(t.name)
        for expr in t.bases: self.visit(expr)
        subscope = Scope(t, 'class', (), self)
        self.children[t] = subscope
        for stmt in t.body: subscope.visit(stmt)

    def visit_Function(self, t):
        all_args = list(t.args.args) + [t.args.vararg, t.args.kwarg]
        subscope = Scope(t, 'function', [arg.arg for arg in all_args if arg], self)
        self.children[t] = subscope
        for stmt in t.body: subscope.visit(stmt)

    def define(self, name):
        if name not in self.defs:
            self.defs[name] = len(self.defs)

    def visit_Import(self, t):
        for alias in t.names:
            self.define(alias.asname or alias.name.split('.')[0])

    def visit_ImportFrom(self, t):
        for alias in t.names:
            self.define(alias.asname or alias.name)

    def visit_Name(self, t):
        if   isinstance(t.ctx, ast.Load):  self.uses.add(t.id)
        elif isinstance(t.ctx, ast.Store): self.define(t.id)
        else: assert False

    def visit_Global(self, t):
        for name in t.names:
            assert name not in self.defs, "name '%r' is assigned to before global declaration" % (name,)
            assert name not in self.uses, "name '%r' is used prior to global declaration" % (name,)
            self.globals.add(name)

    def analyze(self, parent_defs):
        self.local_defs = set(self.defs.keys()) if isinstance(self.t, Function) else set()
        for child in self.children.values():
            child.analyze(parent_defs | self.local_defs)
        child_uses = set([var for child in self.children.values()
                              for var in child.freevars])
        uses = self.uses | child_uses
        self.cellvars = child_uses & self.local_defs
        self.freevars = (uses & (parent_defs - self.local_defs))
        self.derefvars = self.cellvars | self.freevars

    def assign_regs(self, parent_regs):
        self.regs = self.defs.copy() if isinstance(self.t, Function) else {}
        self.free2reg = {}
        for name, upval in parent_regs.items():
            if name in self.freevars:
                assert name not in self.regs
                reg = len(self.regs)
                self.regs[name] = reg
                self.free2reg[name] = (upval, reg)

        assert(all(name in self.regs for name in self.freevars))
        for child in self.children.values():
            child.assign_regs(self.regs)


    def access(self, name):
        return ('deref' if name in self.derefvars else
                'fast'  if name in self.local_defs else
                'global' if name in self.globals or self.scope_type == 'function' else
                'name')

if __name__ == '__main__':
    sys.argv.pop(0)
    load_file(sys.argv[0], '__main__')