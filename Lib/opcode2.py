opmap = {}
opname = ['<%r>' % (op,) for op in range(256)]
cmp_op = ('<', '<=', '==', '!=', '>', '>=')
bytecodes = []
opcodes = [None] * 256
intrinsics = [None] * 256
intrinsic_map = {}

__all__ = [
    "cmp_op", "opname", "opmap", "opcodes", "bytecodes", "intrinsics",
    "intrinsic_map",
]

class Bytecode:
    def __init__(self, name, opcode, *imm):
        self.name = name
        self.opcode = opcode
        self.imm = imm

    @property
    def size(self):
        size = 1 + sum(self.imm_size(i) for i in range(len(self.imm)))
        return size

    def imm_size(self, i, wide=False):
        if self.imm[i] == 'imm16':
            return 2
        elif wide:
            return 4
        elif self.imm[i] == 'jump':
            return 2
        return 1

    @property
    def wide_size(self):
        return 2 + sum(self.imm_size(i, wide=True) for i in range(len(self.imm)))

    def is_jump(self):
        return 'jump' in self.imm

class Intrinsic:
    def __init__(self, name, code, nargs):
        self.name = name
        self.code = code
        self.nargs = nargs

def def_op(name, opcode, *imm):
    bytecode = Bytecode(name, opcode, *imm)
    bytecodes.append(bytecode)
    opmap[name] = bytecode
    opname[opcode] = name
    opcodes[opcode] = bytecode

def def_intrinsic(name, code, nargs=1):
    assert nargs == 'N' or isinstance(nargs, int)
    intrinsic = Intrinsic(name, code, nargs)
    intrinsics[code] = intrinsic
    intrinsic_map[name] = intrinsic

def_op('CLEAR_ACC', 1)
def_op('CLEAR_FAST', 2, 'reg')
def_op('COPY', 3, 'reg', 'reg')
def_op('MOVE', 4, 'reg', 'reg')
def_op('FUNC_HEADER', 5, 'lit')
def_op('METHOD_HEADER', 6)
def_op('COROGEN_HEADER', 7, 'lit')
def_op('CFUNC_HEADER', 8)
def_op('CFUNC_HEADER_NOARGS', 115)
def_op('CFUNC_HEADER_O', 114)
def_op('CMETHOD_NOARGS', 116)
def_op('CMETHOD_O', 117)
def_op('FUNC_TPCALL_HEADER', 9)

# unary math operations
def_op('UNARY_POSITIVE', 10)
def_op('UNARY_NEGATIVE', 11)
def_op('UNARY_NOT', 12)
def_op('UNARY_NOT_FAST', 13)
def_op('UNARY_INVERT', 14)

# binary math/comparison operators (reg OP acc)
def_op('BINARY_MATRIX_MULTIPLY', 15, 'reg')
def_op('BINARY_POWER', 16, 'reg')
def_op('BINARY_MULTIPLY', 17, 'reg')
def_op('BINARY_MODULO', 18, 'reg')
def_op('BINARY_ADD', 19, 'reg')
def_op('BINARY_SUBTRACT', 20, 'reg')
def_op('BINARY_SUBSCR', 21, 'reg')          # reg[acc]
def_op('BINARY_FLOOR_DIVIDE', 22, 'reg')
def_op('BINARY_TRUE_DIVIDE', 23, 'reg')
def_op('BINARY_LSHIFT', 24, 'reg')
def_op('BINARY_RSHIFT', 25, 'reg')
def_op('BINARY_AND', 26, 'reg')
def_op('BINARY_XOR', 27, 'reg')
def_op('BINARY_OR', 28, 'reg')
def_op('IS_OP', 29, 'reg')
def_op('CONTAINS_OP', 30, 'reg')
def_op('COMPARE_OP', 31, 'lit', 'reg')

# inplace binary operators
def_op('INPLACE_FLOOR_DIVIDE', 32, 'reg')
def_op('INPLACE_TRUE_DIVIDE', 33, 'reg')
def_op('INPLACE_ADD', 34, 'reg')
def_op('INPLACE_SUBTRACT', 35, 'reg')
def_op('INPLACE_MULTIPLY', 36, 'reg')
def_op('INPLACE_LSHIFT', 37, 'reg')
def_op('INPLACE_RSHIFT', 38, 'reg')
def_op('INPLACE_AND', 39, 'reg')
def_op('INPLACE_XOR', 40, 'reg')
def_op('INPLACE_OR', 41, 'reg')
def_op('INPLACE_MODULO', 42, 'reg')
def_op('INPLACE_MATRIX_MULTIPLY', 43, 'reg')
def_op('INPLACE_POWER', 44, 'reg')

# load / store / delete
def_op('LOAD_FAST', 45, 'reg')
def_op('LOAD_NAME', 46, 'str', 'lit')
def_op('LOAD_CONST', 47, 'const')
def_op('LOAD_ATTR', 48, 'reg', 'str', 'lit')
def_op('LOAD_GLOBAL', 49, 'str', 'lit')
def_op('LOAD_METHOD', 50, 'reg', 'str', 'lit')
def_op('LOAD_DEREF', 51, 'reg')
def_op('LOAD_CLASSDEREF', 52, 'reg', 'str')

def_op('STORE_FAST', 53, 'reg')
def_op('STORE_NAME', 54, 'str')
def_op('STORE_ATTR', 55, 'reg', 'str')
def_op('STORE_GLOBAL', 56, 'str')
def_op('STORE_SUBSCR', 57, 'reg', 'reg')
def_op('STORE_DEREF', 58, 'reg')

def_op('DELETE_FAST', 59, 'reg')
def_op('DELETE_NAME', 60, 'str')
def_op('DELETE_ATTR', 61, 'str')
def_op('DELETE_GLOBAL', 62, 'str')
def_op('DELETE_SUBSCR', 63, 'reg')
def_op('DELETE_DEREF', 64, 'reg')

# call / return / yield
def_op('CALL_FUNCTION', 65, 'base', 'imm16')
def_op('CALL_FUNCTION_EX', 66, 'base')
def_op('CALL_METHOD', 67, 'base', 'imm16')
def_op('CALL_INTRINSIC_1', 68, 'intrinsic')
def_op('CALL_INTRINSIC_N', 69, 'intrinsic', 'base', 'lit')

def_op('RETURN_VALUE', 70)
def_op('RAISE', 71)
def_op('YIELD_VALUE', 72)
def_op('YIELD_FROM', 73, 'reg')

def_op('JUMP', 74, 'jump')
def_op('JUMP_IF_FALSE', 75, 'jump')
def_op('JUMP_IF_TRUE', 76, 'jump')
def_op('JUMP_IF_NOT_EXC_MATCH', 77, 'reg', 'jump')
def_op('POP_JUMP_IF_FALSE', 78, 'jump')
def_op('POP_JUMP_IF_TRUE', 79, 'jump')

def_op('GET_ITER', 80, 'reg')
def_op('GET_YIELD_FROM_ITER', 81, 'reg')
def_op('FOR_ITER', 82, 'reg', 'jump')

def_op('IMPORT_NAME', 83, 'str')
def_op('IMPORT_FROM', 84, 'reg', 'str')
def_op('IMPORT_STAR', 85, 'reg')

def_op('BUILD_SLICE', 86, 'reg')
def_op('BUILD_TUPLE', 87, 'reg', 'lit')
def_op('BUILD_LIST', 88, 'reg', 'lit')
def_op('BUILD_SET', 89, 'reg', 'lit')
def_op('BUILD_MAP', 90, 'lit')

def_op('END_EXCEPT', 91, 'reg')
def_op('CALL_FINALLY', 92, 'reg', 'jump')
def_op('END_FINALLY', 93, 'reg')
def_op('LOAD_BUILD_CLASS', 94)
def_op('GET_AWAITABLE', 95, 'reg', 'lit')
def_op('GET_AITER', 96, 'reg')
def_op('GET_ANEXT', 97, 'reg')
def_op('END_ASYNC_WITH', 98, 'reg')
def_op('END_ASYNC_FOR', 99, 'reg')
def_op('UNPACK', 100, 'lit', 'lit', 'lit')
def_op('MAKE_FUNCTION', 101, 'const')
def_op('SETUP_WITH', 102, 'reg')
def_op('END_WITH', 103, 'reg')
def_op('SETUP_ASYNC_WITH', 104, 'reg')
def_op('LIST_EXTEND', 105, 'reg')
def_op('LIST_APPEND', 106, 'reg')
def_op('SET_ADD', 107, 'reg')
def_op('SET_UPDATE', 108, 'reg')
def_op('DICT_MERGE', 109, 'reg')
def_op('DICT_UPDATE', 110, 'reg')
def_op('SETUP_ANNOTATIONS', 112)
def_op('SET_FUNC_ANNOTATIONS', 113, 'reg')
def_op('WIDE', 111)

def_intrinsic('PyObject_Str', 1)
def_intrinsic('PyObject_Repr', 2)
def_intrinsic('PyObject_ASCII', 3)
def_intrinsic('vm_format_value', 4)
def_intrinsic('vm_format_value_spec', 5, nargs=2)
def_intrinsic('vm_build_string', 6, nargs='N')
def_intrinsic('PyList_AsTuple', 7)
def_intrinsic('vm_raise_assertion_error', 8)
def_intrinsic('vm_exc_set_cause', 9, nargs=2)
def_intrinsic('vm_print', 10)
def_intrinsic('_PyAsyncGenValueWrapperNew', 11)

del def_op, def_intrinsic
