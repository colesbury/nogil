
#define LOAD_INT(val) \
    (1 | (val << 16))

#define TEST_LESS_THAN(reg) \
    (2 | (reg << 8))

#define FUNC_HEADER(framesize) \
    (3 | (framesize << 8))

#define JUMP_IF_FALSE(offset) \
    (4 | ((offset + 0x8000) << 16))

#define RET() \
    (5)

#define LOAD_GLOBAL(kindx, meta) \
    (6 | (meta << 8) | (kindx << 16))

#define MOVE(dst) \
    (7 | (dst << 8))

#define ADD(reg) \
    (8 | (reg << 8))

#define SUB(reg) \
    (9 | (reg << 8))

#define CALL_FUNCTION(reg, nargs) \
    (10 | (reg << 8) | (nargs << 16))

#define RETURN_TO_C() \
    (11)

#define CLEAR(reg) \
    (12 | (reg << 8))
/*
#define KSHORT(dst, val) \
    (0x1 | (dst << 8) | (val << 16))

#define ISLT(regA, regD) \
    (0x2 | (regA << 8) | (regD << 16))

#define JMP(offset) \
    (0x3 | ((offset + 0x8000) << 16))

#define RET(regA) \
    (0x4 | (regA << 8))


#define BINARY_SUBTRACT_N(dst, var, num) \
    (0x6 | (dst << 8) | (var << 16) | (num << 24))

#define CALL(base, var, num) \
    (0x7 | (dst << 8) | (var << 16) | (num << 24))
*/