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
    opmap[name] = opcode
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
def_op('CFUNC_HEADER', 8)
def_op('CFUNC_HEADER_NOARGS', 9)
def_op('CFUNC_HEADER_O', 10)
def_op('CMETHOD_NOARGS', 11)
def_op('CMETHOD_O', 12)
def_op('FUNC_TPCALL_HEADER', 13)

# unary math operations
def_op('UNARY_POSITIVE', 14)
def_op('UNARY_NEGATIVE', 15)
def_op('UNARY_NOT', 16)
def_op('UNARY_NOT_FAST', 17)
def_op('UNARY_INVERT', 18)

# binary math/comparison operators (reg OP acc)
def_op('BINARY_MATRIX_MULTIPLY', 19, 'reg')
def_op('BINARY_POWER', 20, 'reg')
def_op('BINARY_MULTIPLY', 21, 'reg')
def_op('BINARY_MODULO', 22, 'reg')
def_op('BINARY_ADD', 23, 'reg')
def_op('BINARY_SUBTRACT', 24, 'reg')
def_op('BINARY_SUBSCR', 25, 'reg')          # reg[acc]
def_op('BINARY_FLOOR_DIVIDE', 26, 'reg')
def_op('BINARY_TRUE_DIVIDE', 27, 'reg')
def_op('BINARY_LSHIFT', 28, 'reg')
def_op('BINARY_RSHIFT', 29, 'reg')
def_op('BINARY_AND', 30, 'reg')
def_op('BINARY_XOR', 31, 'reg')
def_op('BINARY_OR', 32, 'reg')
def_op('IS_OP', 33, 'reg')
def_op('CONTAINS_OP', 34, 'reg')
def_op('COMPARE_OP', 35, 'lit', 'reg')

# inplace binary operators
def_op('INPLACE_FLOOR_DIVIDE', 36, 'reg')
def_op('INPLACE_TRUE_DIVIDE', 37, 'reg')
def_op('INPLACE_ADD', 38, 'reg')
def_op('INPLACE_SUBTRACT', 39, 'reg')
def_op('INPLACE_MULTIPLY', 40, 'reg')
def_op('INPLACE_LSHIFT', 41, 'reg')
def_op('INPLACE_RSHIFT', 42, 'reg')
def_op('INPLACE_AND', 43, 'reg')
def_op('INPLACE_XOR', 44, 'reg')
def_op('INPLACE_OR', 45, 'reg')
def_op('INPLACE_MODULO', 46, 'reg')
def_op('INPLACE_MATRIX_MULTIPLY', 47, 'reg')
def_op('INPLACE_POWER', 48, 'reg')

# load / store / delete
def_op('LOAD_FAST', 49, 'reg')
def_op('LOAD_NAME', 50, 'str', 'lit')
def_op('LOAD_CONST', 51, 'const')
def_op('LOAD_ATTR', 52, 'reg', 'str', 'lit')
def_op('LOAD_GLOBAL', 53, 'str', 'lit')
def_op('LOAD_METHOD', 54, 'reg', 'str', 'lit')
def_op('LOAD_DEREF', 55, 'reg')
def_op('LOAD_CLASSDEREF', 56, 'reg', 'str')

def_op('STORE_FAST', 57, 'reg')
def_op('STORE_NAME', 58, 'str')
def_op('STORE_ATTR', 59, 'reg', 'str')
def_op('STORE_ATTR_REG', 60, 'reg', 'str')
def_op('STORE_GLOBAL', 61, 'str')
def_op('STORE_SUBSCR', 62, 'reg', 'reg')
def_op('STORE_SUBSCR_REG', 63, 'reg', 'str')
def_op('STORE_DEREF', 64, 'reg')

def_op('DELETE_FAST', 65, 'reg')
def_op('DELETE_NAME', 66, 'str')
def_op('DELETE_ATTR', 67, 'str')
def_op('DELETE_GLOBAL', 68, 'str')
def_op('DELETE_SUBSCR', 69, 'reg')
def_op('DELETE_DEREF', 70, 'reg')

# call / return / yield
def_op('CALL_FUNCTION', 71, 'base', 'imm16')
def_op('CALL_FUNCTION_EX', 72, 'base')
def_op('CALL_METHOD', 73, 'base', 'imm16')
def_op('CALL_INTRINSIC_1', 74, 'intrinsic')
def_op('CALL_INTRINSIC_N', 75, 'intrinsic', 'base', 'lit')

def_op('RETURN_VALUE', 76)
def_op('RAISE', 77)
def_op('YIELD_VALUE', 78)
def_op('YIELD_FROM', 79, 'reg')

def_op('JUMP', 80, 'jump')
def_op('JUMP_IF_FALSE', 81, 'jump')
def_op('JUMP_IF_TRUE', 82, 'jump')
def_op('JUMP_IF_NOT_EXC_MATCH', 83, 'reg', 'jump')
def_op('POP_JUMP_IF_FALSE', 84, 'jump')
def_op('POP_JUMP_IF_TRUE', 85, 'jump')

def_op('GET_ITER', 86, 'reg')
def_op('GET_YIELD_FROM_ITER', 87, 'reg')
def_op('FOR_ITER', 88, 'reg', 'jump')

def_op('IMPORT_NAME', 89, 'str')
def_op('IMPORT_FROM', 90, 'reg', 'str')
def_op('IMPORT_STAR', 91, 'reg')

def_op('BUILD_SLICE', 92, 'reg')
def_op('BUILD_TUPLE', 93, 'reg', 'lit')
def_op('BUILD_LIST', 94, 'reg', 'lit')
def_op('BUILD_SET', 95, 'reg', 'lit')
def_op('BUILD_MAP', 96, 'lit')

def_op('END_EXCEPT', 97, 'reg')
def_op('CALL_FINALLY', 98, 'reg', 'jump')
def_op('END_FINALLY', 99, 'reg')
def_op('LOAD_BUILD_CLASS', 100)
def_op('GET_AWAITABLE', 101, 'reg', 'lit')
def_op('GET_AITER', 102, 'reg')
def_op('GET_ANEXT', 103, 'reg')
def_op('END_ASYNC_WITH', 104, 'reg')
def_op('END_ASYNC_FOR', 105, 'reg')
def_op('UNPACK', 106, 'lit', 'lit', 'lit')
def_op('MAKE_FUNCTION', 107, 'const')
def_op('SETUP_WITH', 108, 'reg')
def_op('END_WITH', 109, 'reg')
def_op('SETUP_ASYNC_WITH', 110, 'reg')
def_op('LIST_EXTEND', 111, 'reg')
def_op('LIST_APPEND', 112, 'reg')
def_op('SET_ADD', 113, 'reg')
def_op('SET_UPDATE', 114, 'reg')
def_op('DICT_MERGE', 115, 'reg')
def_op('DICT_UPDATE', 116, 'reg')
def_op('SETUP_ANNOTATIONS', 117)
def_op('SET_FUNC_ANNOTATIONS', 118, 'reg')
def_op('WIDE', 119)

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
