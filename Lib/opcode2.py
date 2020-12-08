opmap = {}
opname = ['<%r>' % (op,) for op in range(256)]

def def_op(name, op):
    opname[op] = name
    opmap[name] = op


def_op('NOP', 9)
# unary math operations
def_op('UNARY_POSITIVE', 10)
def_op('UNARY_NEGATIVE', 11)
def_op('UNARY_NOT', 12)
def_op('UNARY_INVERT', 15)

# binary math/comparison operators
def_op('BINARY_MATRIX_MULTIPLY', 16)
def_op('BINARY_POWER', 19)
def_op('BINARY_MULTIPLY', 20)
def_op('BINARY_MODULO', 22)
def_op('BINARY_ADD', 23)
def_op('BINARY_SUBTRACT', 24)
def_op('BINARY_SUBSCR', 25)
def_op('BINARY_FLOOR_DIVIDE', 26)
def_op('BINARY_TRUE_DIVIDE', 27)
def_op('BINARY_LSHIFT', 62)
def_op('BINARY_RSHIFT', 63)
def_op('BINARY_AND', 64)
def_op('BINARY_XOR', 65)
def_op('BINARY_OR', 66)
def_op('IS_OP', 117)
def_op('CONTAINS_OP', 118)
def_op('COMPARE_OP', 107)       # Comparison operator

# inplace binary operators
def_op('INPLACE_FLOOR_DIVIDE', 28)
def_op('INPLACE_TRUE_DIVIDE', 29)
def_op('INPLACE_ADD', 55)
def_op('INPLACE_SUBTRACT', 56)
def_op('INPLACE_MULTIPLY', 57)
def_op('INPLACE_LSHIFT', 75)
def_op('INPLACE_RSHIFT', 76)
def_op('INPLACE_AND', 77)
def_op('INPLACE_XOR', 78)
def_op('INPLACE_OR', 79)
def_op('INPLACE_MODULO', 59)
def_op('INPLACE_MATRIX_MULTIPLY', 17)
def_op('INPLACE_POWER', 67)

# load / store / delete
def_op('LOAD_FAST', 124)        # Local variable number
def_op('LOAD_NAME', 101)        # Index in name list
def_op('LOAD_CONST', 100)       # Index in const list
def_op('LOAD_ATTR', 106)        # Index in name list
def_op('LOAD_GLOBAL', 116)      # Index in name list
def_op('LOAD_METHOD', 160)

def_op('STORE_FAST', 125)       # Local variable number
def_op('STORE_NAME', 90)        # Index in name list
def_op('STORE_ATTR', 95)        # Index in name list
def_op('STORE_GLOBAL', 97)      # ""
def_op('STORE_SUBSCR', 60)

def_op('DELETE_FAST', 126)      # Local variable number
def_op('DELETE_NAME', 91)       # ""
def_op('DELETE_ATTR', 96)       # ""
def_op('DELETE_GLOBAL', 98)     # ""
def_op('DELETE_SUBSCR', 61)

# call / return / yield
def_op('CALL_FUNCTION', 131)     # #args
def_op('CALL_FUNCTION_KW', 141)  # #args + #kwargs
def_op('CALL_FUNCTION_EX', 142)  # Flags
def_op('CALL_METHOD', 161)

def_op('RETURN_VALUE', 83)
def_op('RERAISE', 48)
def_op('RAISE_VARARGS', 130)     # Number of raise arguments (1, 2, or 3)
def_op('YIELD_VALUE', 86)
def_op('YIELD_FROM', 72)

# jmp
def_op('JUMP_FORWARD', 110)         # Number of bytes to skip
def_op('JUMP_IF_FALSE_OR_POP', 111) # Target byte offset from beginning of code
def_op('JUMP_IF_TRUE_OR_POP', 112)  # ""
def_op('JUMP_ABSOLUTE', 113)        # ""
def_op('JUMP_IF_NOT_EXC_MATCH', 121)
def_op('POP_JUMP_IF_FALSE', 114)    # ""
def_op('POP_JUMP_IF_TRUE', 115)     # ""

def_op('GET_ITER', 68)
def_op('GET_YIELD_FROM_ITER', 69)

# imports
def_op('IMPORT_NAME', 108)      # Index in name list
def_op('IMPORT_FROM', 109)      # Index in name list
def_op('IMPORT_STAR', 84)

# build built-in objects
def_op('BUILD_SLICE', 133)      # Number of items
def_op('BUILD_TUPLE', 102)      # Number of tuple items
def_op('BUILD_LIST', 103)       # Number of list items
def_op('BUILD_SET', 104)        # Number of set items
def_op('BUILD_MAP', 105)        # Number of dict entries


# ----
def_op('LOAD_CLOSURE', 135)
def_op('LOAD_DEREF', 136)
def_op('STORE_DEREF', 137)
def_op('DELETE_DEREF', 138)

# f-strings
def_op('FORMAT_VALUE', 155)
def_op('BUILD_STRING', 157)

def_op('PRINT_EXPR', 70)
def_op('LOAD_BUILD_CLASS', 71)
def_op('LOAD_ASSERTION_ERROR', 74)
def_op('GET_AWAITABLE', 73)
def_op('GET_AITER', 50)
def_op('GET_ANEXT', 51)
def_op('LIST_TO_TUPLE', 82)
def_op('SETUP_ANNOTATIONS', 85)
def_op('POP_BLOCK', 87)
def_op('POP_EXCEPT', 89)
def_op('WITH_EXCEPT_START', 49)
def_op('BEFORE_ASYNC_WITH', 52)
def_op('END_ASYNC_FOR', 54)
def_op('FOR_ITER', 93)
def_op('UNPACK_SEQUENCE', 92)   # Number of tuple items
def_op('UNPACK_EX', 94)
def_op('SETUP_FINALLY', 122)   # Distance to target address
def_op('MAKE_FUNCTION', 132)    # Flags
def_op('SETUP_WITH', 143)
def_op('LOAD_CLASSDEREF', 148)
def_op('EXTENDED_ARG', 144)
def_op('SETUP_ASYNC_WITH', 154)
def_op('BUILD_CONST_KEY_MAP', 156)
def_op('LIST_EXTEND', 162)
def_op('LIST_APPEND', 145)
def_op('SET_ADD', 146)
def_op('SET_UPDATE', 163)
def_op('MAP_ADD', 147)
def_op('DICT_MERGE', 164)
def_op('DICT_UPDATE', 165)

def_op('LOAD_GLOBAL_FOR_CALL', 166)     # Index in name list
def_op('LOAD_FAST_FOR_CALL', 167)     # Index in name list


def_op('CLEAR', 168)     # Index in name list
def_op('MOVE', 169)

del def_op