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
    def __init__(self, name, opcode, opA, opD):
        self.name = name
        self.opcode = opcode
        self.opA = opA
        self.opD = opD

    def is_jump(self):
        return self.opD == 'jump'

class Intrinsic:
    def __init__(self, name, code, nargs):
        self.name = name
        self.code = code
        self.nargs = nargs

def def_op(name, opcode, opA=None, opD=None):
    bytecode = Bytecode(name, opcode, opA, opD)
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
def_op('FUNC_HEADER', 2, 'lit')
def_op('METHOD_HEADER', 3)
def_op('GENERATOR_HEADER', 4, 'lit')
def_op('CFUNC_HEADER', 6)
def_op('FUNC_TPCALL_HEADER', 7)
def_op('NOP', 9)
# unary math operations
def_op('UNARY_POSITIVE', 10)
def_op('UNARY_NEGATIVE', 11)
def_op('UNARY_NOT', 12)
def_op('UNARY_NOT_FAST', 13)
def_op('UNARY_INVERT', 15)

# binary math/comparison operators
def_op('BINARY_MATRIX_MULTIPLY', 16, 'reg')
def_op('BINARY_POWER', 19, 'reg')
def_op('BINARY_MULTIPLY', 20, 'reg')
def_op('BINARY_MODULO', 22, 'reg')
def_op('BINARY_ADD', 23, 'reg')         # reg + acc
def_op('BINARY_SUBTRACT', 24, 'reg')
def_op('BINARY_SUBSCR', 25, 'reg')      # reg[acc]
def_op('BINARY_FLOOR_DIVIDE', 26, 'reg')
def_op('BINARY_TRUE_DIVIDE', 27, 'reg')
def_op('BINARY_LSHIFT', 62, 'reg')
def_op('BINARY_RSHIFT', 63, 'reg')
def_op('BINARY_AND', 64, 'reg')
def_op('BINARY_XOR', 65, 'reg')
def_op('BINARY_OR', 66, 'reg')
def_op('IS_OP', 117, 'reg')
def_op('CONTAINS_OP', 118, 'reg')
def_op('COMPARE_OP', 107, 'lit', 'reg')       # Comparison operator

# inplace binary operators
def_op('INPLACE_FLOOR_DIVIDE', 28, 'reg')
def_op('INPLACE_TRUE_DIVIDE', 29, 'reg')
def_op('INPLACE_ADD', 55, 'reg')
def_op('INPLACE_SUBTRACT', 56, 'reg')
def_op('INPLACE_MULTIPLY', 57, 'reg')
def_op('INPLACE_LSHIFT', 75, 'reg')
def_op('INPLACE_RSHIFT', 76, 'reg')
def_op('INPLACE_AND', 77, 'reg')
def_op('INPLACE_XOR', 78, 'reg')
def_op('INPLACE_OR', 79, 'reg')
def_op('INPLACE_MODULO', 59, 'reg')
def_op('INPLACE_MATRIX_MULTIPLY', 17, 'reg')
def_op('INPLACE_POWER', 67, 'reg')

# load / store / delete
def_op('LOAD_FAST', 124, 'reg')        # Local variable number
def_op('LOAD_NAME', 101, 'str')        # Index in name list
def_op('LOAD_CONST', 100, 'const')     # Index in const list
def_op('LOAD_ATTR', 106, 'reg', 'str') # Index in name list
def_op('LOAD_GLOBAL', 116, 'str')      # Index in name list
def_op('LOAD_METHOD', 160, 'reg', 'str')
def_op('LOAD_EXC', 30)
def_op('LOAD_INTRINSIC', 34, 'intrinsic')

def_op('STORE_FAST', 125, 'reg')       # Local variable number
def_op('STORE_NAME', 90, 'str')        # Index in name list
def_op('STORE_ATTR', 95, 'reg', 'str') # reg.name = acc
def_op('STORE_GLOBAL', 97, 'str')      # ""
def_op('STORE_SUBSCR', 60, 'reg', 'reg') # reg1[reg2] = acc

def_op('DELETE_FAST', 126, 'reg')      # Local variable number
def_op('DELETE_NAME', 91, 'str')       # del name
def_op('DELETE_ATTR', 96, 'str')       # del acc.name
def_op('DELETE_GLOBAL', 98, 'str')     # del name
def_op('DELETE_SUBSCR', 61, 'reg')     # del reg[acc]
def_op('DELETE_DEREF', 138, 'reg')     # del <reg>?

# call / return / yield
def_op('CALL_FUNCTION', 131, 'base', 'lit')     # #args
def_op('CALL_FUNCTION_KW', 141, 'base', 'lit')  # #args + #kwargs
def_op('CALL_FUNCTION_EX', 142, 'base')  # Flags
def_op('CALL_METHOD', 161, 'base', 'lit')
def_op('CALL_INTRINSIC_1', 35, 'intrinsic')
def_op('CALL_INTRINSIC_N', 36, 'base', 'lit')

def_op('RETURN_VALUE', 83)
def_op('RERAISE', 48, 'reg')
def_op('RAISE', 130)
def_op('YIELD_VALUE', 86)
def_op('YIELD_FROM', 72)

# jmp
def_op('JUMP', 113, None, 'jump')                 # Target byte offset from beginning of code
def_op('JUMP_IF_FALSE', 111, None, 'jump')        # ""
def_op('JUMP_IF_TRUE', 112, None, 'jump')         # ""
def_op('JUMP_IF_NOT_EXC_MATCH', 121, None, 'jump')
def_op('POP_JUMP_IF_FALSE', 114, None, 'jump')    # "" while loop
def_op('POP_JUMP_IF_TRUE', 115, None, 'jump')     # ""

def_op('GET_ITER', 68, 'reg')
def_op('GET_YIELD_FROM_ITER', 69)
def_op('FOR_ITER', 93, 'reg', 'jump')

# imports
def_op('IMPORT_NAME', 108, 'str')      # Index in name list
def_op('IMPORT_FROM', 109, 'reg', 'str')      # Index in name list
def_op('IMPORT_STAR', 84, 'str')

# build built-in objects
def_op('BUILD_SLICE', 133, 'reg')      # Number of items
def_op('BUILD_TUPLE', 102, 'reg', 'lit')      # Number of tuple items
def_op('BUILD_LIST', 103, 'reg', 'lit')       # Number of list items
def_op('BUILD_SET', 104, 'reg', 'lit')        # Number of set items
def_op('BUILD_MAP', 105, 'lit')               # Number of dict entries


# ----
def_op('LOAD_DEREF', 136, 'reg')
def_op('STORE_DEREF', 137, 'reg')
def_op('LOAD_CLASSDEREF', 148, 'reg', 'str')

def_op('END_EXCEPT', 89, 'reg')
def_op('CALL_FINALLY', 123, 'reg', 'jump')
def_op('END_FINALLY', 122, 'reg')

def_op('PRINT_EXPR', 70)
def_op('LOAD_BUILD_CLASS', 71, 'reg')
def_op('LOAD_ASSERTION_ERROR', 74)
def_op('GET_AWAITABLE', 73)
def_op('GET_AITER', 50)
def_op('GET_ANEXT', 51)
def_op('LIST_TO_TUPLE', 82)
def_op('SETUP_ANNOTATIONS', 85)
def_op('POP_BLOCK', 87)
def_op('WITH_EXCEPT_START', 49)
def_op('BEFORE_ASYNC_WITH', 52)
def_op('END_ASYNC_FOR', 54)
def_op('UNPACK_SEQUENCE', 92, 'base', 'lit')   # Number of tuple items
def_op('UNPACK_EX', 94)
def_op('MAKE_FUNCTION', 132, 'const')    # Flags
def_op('SETUP_WITH', 143, 'reg')
def_op('END_WITH', 150, 'reg')
def_op('EXTENDED_ARG', 144)
def_op('SETUP_ASYNC_WITH', 154)
def_op('BUILD_CONST_KEY_MAP', 156)
def_op('LIST_EXTEND', 162, 'reg')
def_op('LIST_APPEND', 145, 'reg')
def_op('SET_ADD', 146, 'reg')
def_op('SET_UPDATE', 163, 'reg')
def_op('MAP_ADD', 147)
def_op('DICT_MERGE', 164, 'reg')
def_op('DICT_UPDATE', 165, 'reg')

def_op('CLEAR_FAST', 168, 'reg')     # Index in name list
def_op('COPY', 169, 'reg', 'reg')
def_op('MOVE', 170, 'reg', 'reg')

def_intrinsic('PyObject_Str', 1)
def_intrinsic('PyObject_Repr', 2)
def_intrinsic('PyObject_ASCII', 3)
def_intrinsic('vm_format_value', 4)
def_intrinsic('vm_format_value_spec', 5, nargs=2)
def_intrinsic('vm_build_string', 6, nargs='N')
def_intrinsic('PyList_AsTuple', 7)

del def_op, def_intrinsic
