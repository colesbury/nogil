import ast
import symtable as symtables
import sys
import os
import opcode2 as opcodes
import contextlib


for name, instr in opcodes.opmap.items():
    globals()[name] = instr


class BasicBlock:
    def __init__(self):
        self.instrs = []


class CompilerUnit:
    def __init__(self, name, symtable):
        self.name = name
        self.symtable = symtable
        self.consts = {}
        self.names = {}
        self.varnames = {}
        self.cellvars = {}
        self.freevars = {}
        self.argcount = 0
        self.posonlyargcount = 0
        self.kwonlyargcount = 0
        block = BasicBlock()
        self.blocks = [block]
        self.curblock = block

        if isinstance(symtable, symtables.Function):
            for i, local in enumerate(symtable.get_locals()):
                self.varnames[local] = i

    def add_const(self, key):
        idx = self.consts.setdefault(key, len(self.consts))
        return idx

    def assemble(self):
        # return code object?
        pass


def constant_key(value):
    return value


class Compiler(ast.NodeVisitor):
    def __init__(self, symtable):
        self.unit = CompilerUnit('<module>', symtable)
        self.top_symtable = symtable
        self.symbol_scopes = {}
        for child in symtable.get_children():
            self.symbol_scopes[child.get_name()] = child
        self.stack = []  # stack of compilation units
        self.next_register = 0
        self.max_registers = 0

    def mangle_name(self, name):
        # TODO
        return name

    def add_const(self, value):
        key = constant_key(value)
        # TODO: merge constants recursively
        arg = self.unit.add_const(key)
        return arg

    def emit(self, instr, arg=0):
        assert instr > 0 and instr < 256
        code = instr | (arg << 8)
        self.unit.curblock.instrs.append(code)

    def visit_Module(self, mod):
        # enter scope
        print('mod', mod)
        print(mod.body)
        self.generic_visit(mod)

    def visit_FunctionDef(self, s):
        with self.scope(s.name) as unit:
            print(s.body)
            for stmt in s.body:
                self.visit(stmt)
            code = unit.assemble()
        self.make_closure(code, flags=0, qualname=s.name)

    def visit_Expr(self, e):
        if type(e.value) == ast.Constant:
            # ignore constant statements
            return
        # visit for side-effect
        self.traverse(e.value)
        # FIXME: clear accumulator

    def visit_Assign(self, node):
        self.traverse(node.value)
        # assert len(node.targets) == 1, "multi-target assign nyi"
        symtable = self.unit.symtable
        # TODO: name mangling
        for i, target in enumerate(node.targets):
            if type(target) == ast.Name:
                print(target.id)
                symbol = symtable.lookup(target.id)
                if symbol.is_local():
                    if symtable.get_type() == 'function':
                        arg = self.add_const(self.mangle_name(symbol.get_name()))
                        self.emit(STORE_NAME, arg)
                    else:
                        print('fail')
            else:
                print('??? =', type(target))
        # print(node.targets)

    binop = {
        "Add": BINARY_ADD,
        "Sub": BINARY_SUBTRACT,
        "Mult": BINARY_MULTIPLY,
        "MatMult": BINARY_MATRIX_MULTIPLY,
        "Div": BINARY_TRUE_DIVIDE,
        "Mod": BINARY_MODULO,
        "LShift": BINARY_LSHIFT,
        "RShift": BINARY_RSHIFT,
        "BitOr": BINARY_OR,
        "BitXor": BINARY_XOR,
        "BitAnd": BINARY_AND,
        "FloorDiv": BINARY_FLOOR_DIVIDE,
        "Pow": BINARY_POWER,
    }

    def visit_BinOp(self, node):
        reg = self.traverse_for_register(node.left)
        self.traverse(node.right)
        self.emit(self.binop[node.op.__class__.__name__], reg)

    def visit_Constant(self, node):
        arg = self.add_const(node.value)
        self.emit(LOAD_CONST, arg)

    def visit_Return(self, node):
        self.traverse(node.value)
        self.emit(RETURN_VALUE)

    def visit_Name(self, node):
        print(self.unit.symtable)
        symbol = self.unit.symtable.lookup(node.id)
        pass

    def visit_ListComp(self, node):
        print('ListComp', node, node.generators, node.elt)
        with self.scope('<listcomp>'):
            pass
        pass

    def traverse_for_register(self, node):
        if isinstance(node, ast.Name):
            symbol = self.unit.symtable.lookup(node.id)
            if symbol.is_local():
                return self.unit.varnames[symbol.get_name()]

        self.traverse(node)
        reg = self.allocate_register()
        self.emit(STORE_FAST, reg)
        return reg

    def traverse(self, node):
        if isinstance(node, list):
            for item in node:
                self.traverse(item)
        else:
            super().visit(node)

    def generic_visit(self, node):
        print('visit', type(node))
        super().generic_visit(node)

    def make_closure(self, code, flags, qualname):
        print('make_closure', code, flags, qualname)
        pass

    @contextlib.contextmanager
    def scope(self, name):
        self.stack.append(self.unit)
        print('scope', self.top_symtable.lookup(name), self.top_symtable.lookup(name).get_namespace())
        scope = self.symbol_scopes[name]
        print('locals', scope.get_locals())
        self.unit = CompilerUnit(name, scope)
        yield self.unit
        self.unit = self.stack.pop()

    def allocate_register(self):
        reg = self.next_register
        self.next_register += 1
        if self.next_register > self.max_registers:
            self.max_registers = self.next_register
        return reg

    @contextlib.contextmanager
    def new_register(self):
        self.next_register += 1
        if self.next_register > self.max_registers:
            self.max_registers = self.next_register
        yield self.next_register
        self.next_register -= 1

def main():
    filepath = sys.argv[1]
    with open(filepath, 'r') as f:
        contents = f.read()
    filename = os.path.basename(filepath)

    tree = ast.parse(contents, filename)
    st = symtables.symtable(contents, filename, 'exec')
    # print(ast.dump(tree, indent=4))
    print(st.get_children())
    Compiler(st).visit(tree)

if __name__ == '__main__':
    main()
