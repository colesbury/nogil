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
        if arg2 is None:
            arg2 = 0
        else:
            arg2 = int(arg2)
            assert arg2 >= -32768 and arg2 < 32768
            arg2 = arg2 & 0xFFFF
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
            assert visitor.next_register <= self.reg
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
        self.varnames  = make_table()
        self.nlocals = 0
        self.next_register = 0
        self.max_registers = 0

    def compile_module(self, t, name):
        assembly = self(t.body) + self.load_const(None) + op.RETURN_VALUE
        return self.make_code(assembly, name, 0, False, False)

    def make_code(self, assembly, name, argcount, has_varargs, has_varkws):
        posonlyargcount = 0
        kwonlyargcount = 0
        nlocals = len(self.varnames)
        # stacksize = plumb_depths(assembly)
        framesize = self.max_registers
        flags = (  (0x02 if nlocals                  else 0)
                 | (0x04 if has_varargs              else 0)
                 | (0x08 if has_varkws               else 0)
                 | (0x10 if self.scope.freevars      else 0)
                 | (0x40 if not self.scope.derefvars else 0))
        firstlineno, lnotab = make_lnotab(assembly)
        code = assemble(assembly)
        return types.Code2Type(code,
                               self.constants.collect(),
                               argcount=argcount,
                               posonlyargcount=posonlyargcount,
                               kwonlyargcount=kwonlyargcount,
                               nlocals=nlocals,
                               framesize=framesize,
                               varnames=collect(self.varnames),
                               filename=self.filename,
                               name=name,
                               firstlineno=firstlineno,
                               linetable=lnotab,
                               freevars=self.scope.freevars,
                               cellvars=self.scope.cellvars)
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

    def visit_NameConstant(self, t): return self.load_const(t.value)
    def visit_Num(self, t):          return self.load_const(t.n)
    def visit_Str(self, t):          return self.load_const(t.s)
    visit_Bytes = visit_Str

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

        if   access == 'fast':  return op.LOAD_FAST(self.varnames[name])
        elif access == 'deref': return op.LOAD_DEREF(self.cell_index(name))
        elif access == 'name':  return op.LOAD_NAME(self.names[name])
        else: assert False

    def store(self, name, value=None):
        access = self.scope.access(name)
        if isinstance(value, Register):
            if access == 'fast':
                opcode = (op.MOVE if value.is_temporary() else
                          op.COPY)
                return opcode(self.varnames[name], value.reg)
            return op.LOAD_FAST(reg) + reg.clear() + self.store(name)
        if   access == 'fast':  return op.STORE_FAST(self.varnames[name])
        elif access == 'deref': return op.STORE_DEREF(self.cell_index(name))
        elif access == 'name':  return op.STORE_NAME(self.names[name])
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
        regs = self.register_list()
        return (regs[0](t.func) +
                concat([regs[2+i](arg) for i,arg in enumerate(t.args)]) +
                opcode(regs.base, len(t.args)))

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

    def assign(self, target, value):
        method = 'assign_' + target.__class__.__name__
        visitor = getattr(self, method)
        return visitor(target, value)

    def assign_Name(self, target, value):
        if isinstance(value, (Register, ast.Name)):
            return self.store(target.id, value)

        return self(value) + self.store(target.id)

    def assign_Attribute(self, target, value):
        reg = self.register()
        attr = self.constants[target.attr]
        # FIXME: clear reg if temporary ???
        return reg(value) + self(target.value) + op.STORE_ATTR(reg, attr) + reg.clear()

    def assign_Subscript(self, target, value):
        reg1 = self.register()
        reg2 = self.register()
        # FIXME: clear regs if temporary ???
        # FIXME: target.slice.value ???
        return (  reg1(value) + reg2(target.value)
                + self(target.slice)
                + op.STORE_SUBSCR(reg1, reg2)
                + reg2.clear() + reg1.clear())


    def assign_List(self, target, value): return self.assign_sequence(target, value)
    def assign_Tuple(self, target, value): return self.assign_sequence(target, value)
    def assign_sequence(self, target, value):
        reg = self.register_list()
        n = len(target.elts)
        return (  op.UNPACK_SEQUENCE(reg.base, len(target.elts))
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
            def load(self, reg):
                return visitor.load(t.id, reg)
            def store(self):
                return visitor.store(t.id)
        return NameRef()

    def visit_Assign(self, t):
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
        return (op.BUILD_MAP(min(0xFFFF, len(t.keys)))
                + concat([self(v) + self(k) + op.STORE_MAP
                          for k, v in zip(t.keys, t.values)]))

    def visit_Subscript(self, t):
        reg = self.register()
        return (  reg(t.value)
                + self(t.slice.value)
                + self.subscr_ops[type(t.ctx)](reg)
                + reg.clear())
    subscr_ops = {ast.Load: op.BINARY_SUBSCR, ast.Store: op.STORE_SUBSCR}

    def visit_Attribute(self, t):
        reg = self.register()
        sub_op = self.attr_ops[type(t.ctx)]
        return (  reg(t.value)
                + sub_op(reg, self.names[t.attr])
                + reg.clear())
    attr_ops = {ast.Load: op.LOAD_ATTR, ast.Store: op.STORE_ATTR}

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
        cmp_index = dis.cmp_op.index(self.ops_cmp[type(operator)])
        return self(t.left) + self(right) + op.COMPARE_OP(cmp_index)
    ops_cmp = {ast.Eq: '==', ast.NotEq: '!=', ast.Is: 'is', ast.IsNot: 'is not',
               ast.Lt: '<',  ast.LtE:   '<=', ast.In: 'in', ast.NotIn: 'not in',
               ast.Gt: '>',  ast.GtE:   '>='}

    def visit_BoolOp(self, t):
        op_jump = self.ops_bool[type(t.op)]
        def compose(left, right):
            after = Label()
            return left + op_jump(after) + right + after
        return reduce(compose, map(self, t.values))
    ops_bool = {ast.And: op.JUMP_IF_FALSE,
                ast.Or:  op.JUMP_IF_TRUE}

    def visit_Pass(self, t):
        return no_op

    def visit_Raise(self, t):
        return self(t.exc) + op.RAISE_VARARGS(1)

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
        return (self.load_const(level)
                + self.load_const(fromlist)
                + op.IMPORT_NAME(self.names[name]))

    def visit_While(self, t):
        loop, end = Label(), Label()
        return (  loop + self(t.test) + op.POP_JUMP_IF_FALSE(end)
                       + self(t.body) + op.JUMP(loop)
                + end)

    def visit_For(self, t):
        reg = self.register()
        start, loop = Label(), Label()
        return (self(t.iter) + op.GET_ITER(reg.allocate())
                + op.JUMP(start) + loop + self(t.body)
                + start + op.FOR_ITER(reg, loop))

    def visit_Return(self, t):
        return ((self(t.value) if t.value else self.load_const(None))
                + op.RETURN_VALUE)

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
        for arg in t.args.args:
            self.varnames[arg.arg]
        if t.args.vararg: self.varnames[t.args.vararg.arg]
        if t.args.kwarg:  self.varnames[t.args.kwarg.arg]
        for local in self.scope.local_defs:
            self.varnames[local]
        self.nlocals = len(self.varnames)
        self.next_register = len(self.varnames)
        self.max_registers = self.next_register
        assembly = self(t.body) + self.load_const(None) + op.RETURN_VALUE
        return self.make_code(assembly, t.name,
                              len(t.args.args), t.args.vararg, t.args.kwarg)

    def visit_ClassDef(self, t):
        code = self.sprout(t).compile_class(t)
        return (op.LOAD_BUILD_CLASS + self.make_closure(code, t.name)
                                    + self.load_const(t.name)
                                    + self(t.bases)
                + op.CALL_FUNCTION(2 + len(t.bases))
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

class constants(collections.defaultdict):
    def __init__(self):
        super().__init__(lambda: len(self))
    def __getitem__(self, key):
        return super().__getitem__((key, type(key)))
    def collect(self):
        return tuple(key for key,_ in self.keys())

def make_table():
    table = collections.defaultdict(lambda: len(table))
    return table

def collect(table):
    return tuple(sorted(table, key=table.get))

def load_file(filename, module_name):
    f = open(filename)
    source = f.read()
    f.close()
    return module_from_ast(module_name, filename, ast.parse(source))

def module_from_ast(module_name, filename, t):
    code = code_for_module(module_name, filename, t)
    module = types.ModuleType(module_name, ast.get_docstring(t))
    print(dis.dis(code))
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
    top = Scope(t, ())
    top.visit(t)
    top.analyze(set())
    return top

class Scope(ast.NodeVisitor):
    def __init__(self, t, defs):
        self.t = t
        self.children = {}       # Enclosed sub-scopes
        self.defs = set(defs)    # Variables defined
        self.uses = set()        # Variables referenced

    def visit_ClassDef(self, t):
        self.defs.add(t.name)
        for expr in t.bases: self.visit(expr)
        subscope = Scope(t, ())
        self.children[t] = subscope
        for stmt in t.body: subscope.visit(stmt)

    def visit_Function(self, t):
        all_args = list(t.args.args) + [t.args.vararg, t.args.kwarg]
        subscope = Scope(t, [arg.arg for arg in all_args if arg])
        self.children[t] = subscope
        for stmt in t.body: subscope.visit(stmt)

    def visit_Import(self, t):
        for alias in t.names:
            self.defs.add(alias.asname or alias.name.split('.')[0])

    def visit_ImportFrom(self, t):
        for alias in t.names:
            self.defs.add(alias.asname or alias.name)

    def visit_Name(self, t):
        if   isinstance(t.ctx, ast.Load):  self.uses.add(t.id)
        elif isinstance(t.ctx, ast.Store): self.defs.add(t.id)
        else: assert False

    def analyze(self, parent_defs):
        self.local_defs = self.defs if isinstance(self.t, Function) else set()
        for child in self.children.values():
            child.analyze(parent_defs | self.local_defs)
        child_uses = set([var for child in self.children.values()
                              for var in child.freevars])
        uses = self.uses | child_uses
        self.cellvars = tuple(child_uses & self.local_defs)
        self.freevars = tuple(uses & (parent_defs - self.local_defs))
        self.derefvars = self.cellvars + self.freevars

    def access(self, name):
        return ('deref' if name in self.derefvars else
                'fast'  if name in self.local_defs else
                'name')

if __name__ == '__main__':
    sys.argv.pop(0)
    load_file(sys.argv[0], '__main__')