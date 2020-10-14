import ast
import symtable
import sys
import os
import opcode2 as opcodes
import contextlib


class BasicBlock:
    def __init__(self):
        self.instrs = []


class CompilerUnit:
    def __init__(self, name):
        self.name = name
        self.consts = {}
        self.varnames = {}
        self.cellvars = {}
        self.freevars = {}
        self.argcount = 0
        self.posonlyargcount = 0
        self.kwonlyargcount = 0
        self.blocks = []

    def assemble(self):
        # return code object?
        pass



class Compiler(ast.NodeVisitor):
    def __init__(self):
        self.unit = CompilerUnit('<module>')
        self.stack = []  # stack of compilation units

    def visit_Module(self, mod):
        # enter scope
        print('mod', mod)
        print(mod.body)
        self.generic_visit(mod)

    def visit_FunctionDef(self, s):
        with self.scope(s.name) as unit:
            for stmt in s.body:
                self.visit(stmt)
            code = unit.assemble()
        self.make_closure(code, flags=0, qualname=s.name)

    def visit_Expr(self, e):
        print(e, e.value)

    def generic_visit(self, node):
        super().generic_visit(node)
    
    def make_closure(self, code, flags, qualname):
        print('make_closure', code, flags, qualname)
        # self.LOAD_CONST(code)
        # self.LOAD_CONST(qualname)
        # self.emit(MAKE_FUNCTION, )
        pass

    @contextlib.contextmanager
    def scope(self, name):
        self.stack.append(self.unit)
        self.unit = CompilerUnit(name)
        yield self.unit
        self.unit = self.stack.pop()


def main():
    filepath = sys.argv[1]
    with open(filepath, 'r') as f:
        contents = f.read()
    filename = os.path.basename(filepath)

    tree = ast.parse(contents, filename)
    st = symtable.symtable(contents, filename, 'exec')
    # print(ast.dump(tree, indent=4))
    print(st)
    Compiler().visit(tree)

if __name__ == '__main__':
    main()