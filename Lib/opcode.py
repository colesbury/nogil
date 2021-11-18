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
    def has_wide(self):
        return len(self.imm) > 0 and self.name not in ('WIDE', 'JUMP_SIDE_TABLE')

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
def_op('ALIAS', 3, 'reg', 'reg')
def_op('COPY', 4, 'reg', 'reg')
def_op('MOVE', 5, 'reg', 'reg')
def_op('FUNC_HEADER', 6, 'lit')
def_op('METHOD_HEADER', 7)
def_op('CFUNC_HEADER', 9)
def_op('CFUNC_HEADER_NOARGS', 10)
def_op('CFUNC_HEADER_O', 11)
def_op('CMETHOD_NOARGS', 12)
def_op('CMETHOD_O', 13)
def_op('FUNC_TPCALL_HEADER', 14)

# unary math operations
def_op('UNARY_POSITIVE', 15)
def_op('UNARY_NEGATIVE', 16)
def_op('UNARY_NOT', 17)
def_op('UNARY_NOT_FAST', 18)
def_op('UNARY_INVERT', 19)

# binary math/comparison operators (reg OP acc)
def_op('BINARY_MATRIX_MULTIPLY', 20, 'reg')
def_op('BINARY_POWER', 21, 'reg')
def_op('BINARY_MULTIPLY', 22, 'reg')
def_op('BINARY_MODULO', 23, 'reg')
def_op('BINARY_ADD', 24, 'reg')
def_op('BINARY_SUBTRACT', 25, 'reg')
def_op('BINARY_SUBSCR', 26, 'reg')          # reg[acc]
def_op('BINARY_FLOOR_DIVIDE', 27, 'reg')
def_op('BINARY_TRUE_DIVIDE', 28, 'reg')
def_op('BINARY_LSHIFT', 29, 'reg')
def_op('BINARY_RSHIFT', 30, 'reg')
def_op('BINARY_AND', 31, 'reg')
def_op('BINARY_XOR', 32, 'reg')
def_op('BINARY_OR', 33, 'reg')
def_op('IS_OP', 34, 'reg')
def_op('CONTAINS_OP', 35, 'reg')
def_op('COMPARE_OP', 36, 'lit', 'reg')

# inplace binary operators
def_op('INPLACE_FLOOR_DIVIDE', 37, 'reg')
def_op('INPLACE_TRUE_DIVIDE', 38, 'reg')
def_op('INPLACE_ADD', 39, 'reg')
def_op('INPLACE_SUBTRACT', 40, 'reg')
def_op('INPLACE_MULTIPLY', 41, 'reg')
def_op('INPLACE_LSHIFT', 42, 'reg')
def_op('INPLACE_RSHIFT', 43, 'reg')
def_op('INPLACE_AND', 44, 'reg')
def_op('INPLACE_XOR', 45, 'reg')
def_op('INPLACE_OR', 46, 'reg')
def_op('INPLACE_MODULO', 47, 'reg')
def_op('INPLACE_MATRIX_MULTIPLY', 48, 'reg')
def_op('INPLACE_POWER', 49, 'reg')

# load / store / delete
def_op('LOAD_FAST', 50, 'reg')
def_op('LOAD_NAME', 51, 'str', 'lit')
def_op('LOAD_CONST', 52, 'const')
def_op('LOAD_ATTR', 53, 'reg', 'str', 'lit')
def_op('LOAD_GLOBAL', 54, 'str', 'lit')
def_op('LOAD_METHOD', 55, 'reg', 'str', 'lit')
def_op('LOAD_DEREF', 56, 'reg')
def_op('LOAD_CLASSDEREF', 57, 'reg', 'str')

def_op('STORE_FAST', 58, 'reg')
def_op('STORE_NAME', 59, 'str')
def_op('STORE_ATTR', 60, 'reg', 'str')
def_op('STORE_GLOBAL', 61, 'str')
def_op('STORE_SUBSCR', 62, 'reg', 'reg')
def_op('STORE_DEREF', 63, 'reg')

def_op('DELETE_FAST', 64, 'reg')
def_op('DELETE_NAME', 65, 'str')
def_op('DELETE_ATTR', 66, 'str')
def_op('DELETE_GLOBAL', 67, 'str')
def_op('DELETE_SUBSCR', 68, 'reg')
def_op('DELETE_DEREF', 69, 'reg')

# call / return / yield
def_op('CALL_FUNCTION', 70, 'base', 'imm16')
def_op('CALL_FUNCTION_EX', 71, 'base')
def_op('CALL_METHOD', 72, 'base', 'imm16')
def_op('CALL_INTRINSIC_1', 73, 'intrinsic')
def_op('CALL_INTRINSIC_N', 74, 'intrinsic', 'base', 'lit')

def_op('RETURN_VALUE', 75)
def_op('RAISE', 76)
def_op('YIELD_VALUE', 77)
def_op('YIELD_FROM', 78, 'reg')

def_op('JUMP', 79, 'jump')
def_op('JUMP_IF_FALSE', 80, 'jump')
def_op('JUMP_IF_TRUE', 81, 'jump')
def_op('JUMP_IF_NOT_EXC_MATCH', 82, 'reg', 'jump')
def_op('POP_JUMP_IF_FALSE', 83, 'jump')
def_op('POP_JUMP_IF_TRUE', 84, 'jump')

def_op('GET_ITER', 85, 'reg')
def_op('GET_YIELD_FROM_ITER', 86, 'reg')
def_op('FOR_ITER', 87, 'reg', 'jump')

def_op('IMPORT_NAME', 88, 'str')
def_op('IMPORT_FROM', 89, 'reg', 'str')
def_op('IMPORT_STAR', 90, 'reg')

def_op('BUILD_SLICE', 91, 'reg')
def_op('BUILD_TUPLE', 92, 'reg', 'lit')
def_op('BUILD_LIST', 93, 'reg', 'lit')
def_op('BUILD_SET', 94, 'reg', 'lit')
def_op('BUILD_MAP', 95, 'lit')

def_op('END_EXCEPT', 96, 'reg')
def_op('CALL_FINALLY', 97, 'reg', 'jump')
def_op('END_FINALLY', 98, 'reg')
def_op('LOAD_BUILD_CLASS', 99)
def_op('GET_AWAITABLE', 100, 'reg', 'lit')
def_op('GET_AITER', 101, 'reg')
def_op('GET_ANEXT', 102, 'reg')
def_op('END_ASYNC_WITH', 103, 'reg')
def_op('END_ASYNC_FOR', 104, 'reg')
def_op('UNPACK', 105, 'lit', 'lit', 'lit')
def_op('MAKE_FUNCTION', 106, 'const')
def_op('SETUP_WITH', 107, 'reg')
def_op('END_WITH', 108, 'reg')
def_op('SETUP_ASYNC_WITH', 109, 'reg')
def_op('LIST_EXTEND', 110, 'reg')
def_op('LIST_APPEND', 111, 'reg')
def_op('SET_ADD', 112, 'reg')
def_op('SET_UPDATE', 113, 'reg')
def_op('DICT_MERGE', 114, 'reg')
def_op('DICT_UPDATE', 115, 'reg')
def_op('SETUP_ANNOTATIONS', 116)
def_op('SET_FUNC_ANNOTATIONS', 117, 'reg')
def_op('WIDE', 118)

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
