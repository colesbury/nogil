"""
Check if a program conforms to our Python subset.
XXX check that names are legal Python identifiers, since our
    bytecompile assumes it can add illegal ones without clashing
"""

import ast

def check_conformity(t):
    Checker().visit(t)

class Checker(ast.NodeVisitor):

    def __init__(self, scope_type='module', in_loop=False):
        self.scope_type = scope_type
        self.in_loop = in_loop

    def generic_visit(self, t):
        "Any node type we don't know about is an error."
        assert False, "Unsupported syntax: %r" % (t,)

    def __call__(self, t):
        if isinstance(t, list):
            for child in t:
                self.visit(child)
        elif isinstance(t, ast.AST):
            self.visit(t)
        else:
            assert False, "Unexpected type: %r" % (t,)

    def visit_Module(self, t):
        assert self.scope_type == 'module', "Module inside %s" % self.scope_type
        self(t.body)

    def visit_Interactive(self, t):
        assert self.scope_type == 'module', "Interactive inside %s" % self.scope_type
        self(t.body)

    def visit_Function(self, t):
        self.check_arguments(t.args)
        Checker('function', in_loop=False)(t.body)

    def visit_AsyncFunction(self, t):
        self.check_arguments(t.args)
        Checker('function', in_loop=False)(t.body)

    def visit_Class(self, t):
        self.check_identifier(t.name)
        self(t.bases)
        self(t.keywords)
        Checker('class', in_loop=False)(t.body)

    def visit_Return(self, t):
        if t.value is not None:
            self(t.value)

    def visit_Assert(self, t):
        self(t.test)
        if t.msg:
            self(t.msg)

    def visit_AugAssign(self, t):
        assert type(t.op) in self.ops2, "Unsupported assignment op: %r" % (t,)
        self(t.target)
        self(t.value)

    def visit_Assign(self, t):
        assert t.targets, "At least one target required: %r" % (t,)
        self(t.targets)
        self(t.value)

    def visit_AnnAssign(self, t):
        self(t.target)
        self(t.annotation)
        if t.value:
            self(t.value)

    def visit_Delete(self, t):
        assert t.targets, "At least one target required: %r" % (t,)
        self(t.targets)

    def visit_For(self, t):
        self(t.target)
        self(t.iter)
        Checker(self.scope_type, in_loop=True)(t.body)
        if t.orelse is not None:
            self(t.orelse)

    def visit_While(self, t):
        self(t.test)
        Checker(self.scope_type, in_loop=True)(t.body)
        if t.orelse is not None:
            self(t.orelse)

    def visit_If(self, t):
        self(t.test)
        self(t.body)
        self(t.orelse)

    def visit_With(self, t):
        for item in t.items:
            self(item.context_expr)
            if item.optional_vars:
                self(item.optional_vars)
        self(t.body)

    def visit_AsyncWith(self, t):
        for item in t.items:
            self(item.context_expr)
            if item.optional_vars:
                self(item.optional_vars)
        self(t.body)

    def visit_AsyncFor(self, t):
        self(t.target)
        self(t.iter)
        Checker(self.scope_type, in_loop=True)(t.body)
        if t.orelse is not None:
            self(t.orelse)

    def visit_Raise(self, t):
        if t.exc:
            self(t.exc)
        if t.cause:
            self(t.cause)

    def visit_Global(self, t):
        for name in t.names:
            self.check_identifier(name)

    def visit_Nonlocal(self, t):
        for name in t.names:
            self.check_identifier(name)

    def visit_Import(self, t):
        self(t.names)

    def visit_ImportFrom(self, t):
        if t.module:
            self.check_identifier(t.module)
        self(t.names)

    def visit_alias(self, t):
        if t.name == '*':
            return
        self.check_identifier(t.name)
        if t.asname is not None:
            self.check_identifier(t.asname)

    def visit_Expr(self, t):
        self(t.value)

    def visit_Pass(self, t):
        pass

    def visit_Break(self, t):
        pass

    def visit_Continue(self, t):
        pass

    def visit_Yield(self, t):
        if t.value:
            self(t.value)

    def visit_YieldFrom(self, t):
        self(t.value)

    def visit_Await(self, t):
        self(t.value)

    def visit_BoolOp(self, t):
        assert type(t.op) in self.ops_bool, "Unsupported boolean op: %r" % (t,)
        self(t.values)
    ops_bool = {ast.And, ast.Or}

    def visit_BinOp(self, t):
        assert type(t.op) in self.ops2, "Unsupported binary op: %r" % (t.op,)
        self(t.left)
        self(t.right)
    ops2 = {ast.Pow,       ast.Add,
            ast.LShift,    ast.Sub,
            ast.RShift,    ast.Mult,
            ast.BitOr,     ast.Mod,
            ast.BitAnd,    ast.Div,
            ast.BitXor,    ast.FloorDiv,
            ast.MatMult}

    def visit_UnaryOp(self, t):
        assert type(t.op) in self.ops1, "Unsupported unary op: %r" % (t,)
        self(t.operand)
    ops1 = {ast.UAdd,  ast.Invert,
            ast.USub,  ast.Not}

    visit_IfExp = visit_If

    def visit_Dict(self, t):
        for k, v in zip(t.keys, t.values):
            self(v)
            if k:
                self(k)

    def visit_Set(self, t):
        self(t.elts)

    def visit_Compare(self, t):
        self(t.left)
        assert len(t.ops) == len(t.comparators), "Wrong number of arguments: %r" % (t,)
        for op, comparator in zip(t.ops, t.comparators):
            assert type(op) in self.ops_cmp, "Unsupported compare op: %r" % (op,)
            self(comparator)
    ops_cmp = {ast.Eq,  ast.NotEq,  ast.Is,  ast.IsNot,
               ast.Lt,  ast.LtE,    ast.In,  ast.NotIn,
               ast.Gt,  ast.GtE}

    def visit_Call(self, t):
        self(t.func)
        self(t.args)
        self(t.keywords)

    def visit_Starred(self, t):
        self(t.value)

    def visit_keyword(self, t):
        if t.arg is not None:
            self.check_identifier(t.arg)
        self(t.value)

    def visit_Constant(self, t):
        if isinstance(t.value, float):
            assert not has_negzero(t.value), "Negative-zero literals not supported: %r" % (t,)

    def visit_FormattedValue(self, t):
        self(t.value)
        if t.format_spec is not None:
            self(t.format_spec)

    def visit_JoinedStr(self, t):
        self(t.values)

    def visit_ListAppend(self, t):
        pass

    def visit_SetAdd(self, t):
        pass

    def visit_GetIter(self, t):
        pass

    def visit_ForIter(self, t):
        pass

    def visit_Attribute(self, t):
        self(t.value)
        self.check_identifier(t.attr)
        assert isinstance(t.ctx, (ast.Load, ast.Store, ast.Del)), "Unsupported context: %r" % (t.ctx,)

    def visit_Subscript(self, t):
        self(t.value)
        assert isinstance(t.ctx, (ast.Load, ast.Store, ast.Del)), "Unsupported context: %r" % (t.ctx,)
        self(t.slice)

    def visit_Slice(self, t):
        for r in (t.lower, t.upper, t.step):
            if r is not None:
                self(r)

    def visit_NameConstant(self, t):
        pass

    def visit_Name(self, t):
        self.check_identifier(t.id)
        assert isinstance(t.ctx, (ast.Load, ast.Store, ast.Del)), "Unsupported context: %r" % (t.ctx,)

    def visit_Try(self, t):
        self(t.body)
        self(t.handlers)
        self(t.orelse)
        self(t.finalbody)

    def visit_ExceptHandler(self, t):
        if t.type is not None:
            self(t.type)
        if t.name is not None:
            self.check_identifier(t.name)
        self(t.body)

    def visit_sequence(self, t):
        self(t.elts)
        assert isinstance(t.ctx, (ast.Load, ast.Store, ast.Del)), "Unsupported context: %r" % (t.ctx,)

    visit_List = visit_sequence
    visit_Tuple = visit_sequence

    def check_arguments(self, args):
        for arg in args.args: self.check_arg(arg)
        for arg in args.posonlyargs: self.check_arg(arg)
        if args.vararg: self.check_arg(args.vararg)
        for arg in args.kwonlyargs: self.check_arg(arg)
        if args.kwarg: self.check_arg(args.kwarg)

    def check_arg(self, arg):
        self.check_identifier(arg.arg)

    def check_identifier(self, name):
        assert isinstance(name, str), "An identifier must be a string: %r" % (name,)

def has_negzero(num):
    return (is_negzero(num)
            or (isinstance(num, complex)
                and (is_negzero(num.real) or is_negzero(num.imag))))

def is_negzero(num):
    return str(num) == '-0.0'