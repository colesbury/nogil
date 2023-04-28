# Minimal tests for dis module

from test.support import captured_stdout
from test.support.bytecode_helper import BytecodeTestCase
import unittest
import sys
import dis
import io
import re
import types
import contextlib

def get_tb():
    def _error():
        try:
            1 / 0
        except Exception as e:
            tb = e.__traceback__
        return tb

    tb = _error()
    while tb.tb_next:
        tb = tb.tb_next
    return tb

TRACEBACK_CODE = get_tb().tb_frame.f_code

class _C:
    def __init__(self, x):
        self.x = x == 1

    @staticmethod
    def sm(x):
        x = x == 1

    @classmethod
    def cm(cls, x):
        cls.x = x == 1

dis_c_instance_method = """\
%3d           0 FUNC_HEADER              2 (2)

%3d           2 LOAD_CONST               2 (1)
              4 COMPARE_OP             2 1 (2; x)
              7 STORE_ATTR             0 3 (self.'x'=acc)
             10 LOAD_CONST               0 (None)
             12 RETURN_VALUE
""" % (_C.__init__.__code__.co_firstlineno + 0,
       _C.__init__.__code__.co_firstlineno + 1,)

dis_c_instance_method_bytes = """\
          0 FUNC_HEADER              2 (2)
          2 LOAD_CONST               2 (2)
          4 COMPARE_OP             2 1 (2; 1)
          7 STORE_ATTR             0 3 (0.3=acc)
         10 LOAD_CONST               0 (0)
         12 RETURN_VALUE
"""

dis_c_class_method = """\
%3d           0 FUNC_HEADER              2 (2)

%3d           2 LOAD_CONST               2 (1)
              4 COMPARE_OP             2 1 (2; x)
              7 STORE_ATTR             0 3 (cls.'x'=acc)
             10 LOAD_CONST               0 (None)
             12 RETURN_VALUE
""" % (_C.cm.__code__.co_firstlineno + 0,
       _C.cm.__code__.co_firstlineno + 2,)

dis_c_static_method = """\
%3d           0 FUNC_HEADER              1 (1)

%3d           2 LOAD_CONST               2 (1)
              4 COMPARE_OP             2 0 (2; x)
              7 STORE_FAST               0 (x)
              9 LOAD_CONST               0 (None)
             11 RETURN_VALUE
""" % (_C.sm.__code__.co_firstlineno + 0,
       _C.sm.__code__.co_firstlineno + 2,)

# Class disassembling info has an extra newline at end.
dis_c = """\
Disassembly of %s:
%s
Disassembly of %s:
%s
Disassembly of %s:
%s
""" % (_C.__init__.__name__, dis_c_instance_method,
       _C.cm.__name__, dis_c_class_method,
       _C.sm.__name__, dis_c_static_method)

def _f(a):
    print(a)
    return 1

dis_f = """\
%3d           0 FUNC_HEADER              6 (6)

%3d           2 LOAD_GLOBAL          2 254 ('print'; 254)
              5 STORE_FAST               4 (.t3)
              7 COPY                   5 0 (.t4 <- a)
             10 CALL_FUNCTION          5 1 (.t4 to .t5)
             14 CLEAR_ACC

%3d          15 LOAD_CONST               3 (1)
             17 RETURN_VALUE
""" % (_f.__code__.co_firstlineno,
       _f.__code__.co_firstlineno + 1,
       _f.__code__.co_firstlineno + 2)


dis_f_co_code = """\
          0 FUNC_HEADER              6 (6)
          2 LOAD_GLOBAL          2 254 (2; 254)
          5 STORE_FAST               4 (4)
          7 COPY                   5 0 (5 <- 0)
         10 CALL_FUNCTION          5 1 (5 to 6)
         14 CLEAR_ACC
         15 LOAD_CONST               3 (3)
         17 RETURN_VALUE
"""


def bug708901():
    for res in range(1,
                     10):
        pass

dis_bug708901 = """\
%3d           0 FUNC_HEADER              7 (7)

%3d           2 LOAD_GLOBAL          2 254 ('range'; 254)
              5 STORE_FAST               4 (.t3)
              7 LOAD_CONST               3 (1)
              9 STORE_FAST               5 (.t4)

%3d          11 LOAD_CONST               4 (10)

%3d          13 STORE_FAST               6 (.t5)
             15 CALL_FUNCTION          5 2 (.t4 to .t6)
             19 GET_ITER                 1 (.t0)
             21 JUMP                     6 (to 27)
        >>   24 STORE_FAST               0 (res)

%3d          26 CLEAR_ACC

%3d     >>   27 FOR_ITER              1 -3 (.t0; to 24)
             31 LOAD_CONST               0 (None)
             33 RETURN_VALUE
""" % (bug708901.__code__.co_firstlineno,
       bug708901.__code__.co_firstlineno + 1,
       bug708901.__code__.co_firstlineno + 2,
       bug708901.__code__.co_firstlineno + 1,
       bug708901.__code__.co_firstlineno + 3,
       bug708901.__code__.co_firstlineno + 1)


def bug1333982(x=[]):
    assert 0, ([s for s in x] +
              1)
    pass

dis_bug1333982 = """\
%3d           0 FUNC_HEADER              6 (6)

%3d           2 LOAD_CONST               2 (0)
              4 POP_JUMP_IF_TRUE        25 (to 29)
              7 MAKE_FUNCTION            3 (<code object <listcomp> at 0x..., file "%s", line %d>)
              9 STORE_FAST               4 (.t3)
             11 LOAD_FAST                0 (x)
             13 GET_ITER                 5 (.t4)
             15 CALL_FUNCTION          5 1 (.t4 to .t5)
             19 STORE_FAST               1 (.t0)

%3d          21 LOAD_CONST               4 (1)

%3d          23 BINARY_ADD               1 (.t0)
             25 CLEAR_FAST               1 (.t0)
             27 CALL_INTRINSIC_1         8 (vm_raise_assertion_error)

%3d     >>   29 LOAD_CONST               0 (None)
             31 RETURN_VALUE
  Free variables: [(1, 0)]
""" % (bug1333982.__code__.co_firstlineno,
       bug1333982.__code__.co_firstlineno + 1,
       __file__,
       bug1333982.__code__.co_firstlineno + 1,
       bug1333982.__code__.co_firstlineno + 2,
       bug1333982.__code__.co_firstlineno + 1,
       bug1333982.__code__.co_firstlineno + 3)

_BIG_LINENO_FORMAT = """\
  1           0 FUNC_HEADER              0 (0)

%3d           2 LOAD_GLOBAL          2 254 ('spam'; 254)
              5 CLEAR_ACC
              6 LOAD_CONST               0 (None)
              8 RETURN_VALUE
"""

_BIG_LINENO_FORMAT2 = """\
   1           0 FUNC_HEADER              0 (0)

%4d           2 LOAD_GLOBAL          2 254 ('spam'; 254)
               5 CLEAR_ACC
               6 LOAD_CONST               0 (None)
               8 RETURN_VALUE
"""

dis_module_expected_results = """\
Disassembly of f:
  4           0 FUNC_HEADER              0 (0)
              2 LOAD_CONST               0 (None)
              4 RETURN_VALUE

Disassembly of g:
  5           0 FUNC_HEADER              0 (0)
              2 LOAD_CONST               0 (None)
              4 RETURN_VALUE

"""

expr_str = "x + 1"

dis_expr_str = """\
  1           0 FUNC_HEADER              2 (2)
              2 LOAD_NAME            0 254 ('x'; 254)
              5 STORE_FAST               1 (.t0)
              7 LOAD_CONST               1 (1)
              9 BINARY_ADD               1 (.t0)
             11 CLEAR_FAST               1 (.t0)
             13 RETURN_VALUE
"""

simple_stmt_str = "x = x + 1"

dis_simple_stmt_str = """\
  1           0 FUNC_HEADER              2 (2)
              2 LOAD_NAME            0 254 ('x'; 254)
              5 STORE_FAST               1 (.t0)
              7 LOAD_CONST               1 (1)
              9 BINARY_ADD               1 (.t0)
             11 CLEAR_FAST               1 (.t0)
             13 STORE_NAME               0 ('x')
             15 LOAD_CONST               2 (None)
             17 RETURN_VALUE
"""

annot_stmt_str = """\

x: int = 1
y: fun(1)
lst[fun(0)]: int = 1
"""
# leading newline is for a reason (tests lineno)

dis_annot_stmt_str = """\
  2           0 FUNC_HEADER              8 (8)
              2 SETUP_ANNOTATIONS
              3 LOAD_CONST               0 (1)
              5 STORE_NAME               1 ('x')
              7 LOAD_NAME            2 254 ('__annotations__'; 254)
             10 STORE_FAST               1 (.t0)
             12 LOAD_CONST               1 ('x')
             14 STORE_FAST               2 (.t1)
             16 LOAD_NAME            3 252 ('int'; 252)
             19 STORE_SUBSCR           1 2 (.t0[.t1]=acc)
             22 CLEAR_FAST               2 (.t1)
             24 CLEAR_FAST               1 (.t0)

  3          26 LOAD_NAME            2 254 ('__annotations__'; 254)
             29 STORE_FAST               1 (.t0)
             31 LOAD_CONST               4 ('y')
             33 STORE_FAST               2 (.t1)
             35 LOAD_NAME            5 250 ('fun'; 250)
             38 STORE_FAST               6 (.t5)
             40 LOAD_CONST               0 (1)
             42 STORE_FAST               7 (.t6)
             44 CALL_FUNCTION          7 1 (.t6 to .t7)
             48 STORE_SUBSCR           1 2 (.t0[.t1]=acc)
             51 CLEAR_FAST               2 (.t1)
             53 CLEAR_FAST               1 (.t0)

  4          55 LOAD_CONST               0 (1)
             57 STORE_FAST               1 (.t0)
             59 LOAD_NAME            6 248 ('lst'; 248)
             62 STORE_FAST               2 (.t1)
             64 LOAD_NAME            5 250 ('fun'; 250)
             67 STORE_FAST               6 (.t5)
             69 LOAD_CONST               7 (0)
             71 STORE_FAST               7 (.t6)
             73 CALL_FUNCTION          7 1 (.t6 to .t7)
             77 STORE_SUBSCR_REG       1 2 ('__annotations__'[acc]=.t0)
             80 CLEAR_FAST               2 (.t1)
             82 CLEAR_FAST               1 (.t0)
             84 LOAD_NAME            3 252 ('int'; 252)
             87 CLEAR_ACC
             88 LOAD_CONST               8 (None)
             90 RETURN_VALUE
"""

compound_stmt_str = """\
x = 0
while 1:
    x += 1"""
# Trailing newline has been deliberately omitted

dis_compound_stmt_str = """\
  1           0 FUNC_HEADER              2 (2)
              2 LOAD_CONST               0 (0)
              4 STORE_NAME               1 ('x')

  3     >>    6 LOAD_NAME            1 254 ('x'; 254)
              9 STORE_FAST               1 (.t0)
             11 LOAD_CONST               2 (1)
             13 INPLACE_ADD              1 (.t0)
             15 STORE_NAME               1 ('x')
             17 CLEAR_FAST               1 (.t0)

  2          19 JUMP                   -13 (to 6)
"""

dis_traceback = """\
%3d           0 FUNC_HEADER              6 (6)

%3d           2 LOAD_CONST               2 (1)
              4 STORE_FAST               2 (.t0)
              6 LOAD_CONST               3 (0)
    -->       8 BINARY_TRUE_DIVIDE       2 (.t0)
             10 CLEAR_FAST               2 (.t0)
             12 CLEAR_ACC
             13 JUMP                    31 (to 44)

%3d          16 LOAD_GLOBAL          4 254 ('Exception'; 254)
             19 JUMP_IF_NOT_EXC_MATCH  2 23 (.t0; to 42)
             23 LOAD_FAST                3 (.t1)
             25 STORE_FAST               0 (e)

%3d          27 LOAD_ATTR            0 5 253 (e.'__traceback__')
             31 STORE_FAST               1 (tb)
             33 CLEAR_FAST               0 (e)
             35 END_FINALLY              4 (.t2)
             37 END_EXCEPT               2 (.t0)
             39 JUMP                     5 (to 44)
        >>   42 END_FINALLY              2 (.t0)

%3d     >>   44 LOAD_FAST                1 (tb)
             46 RETURN_VALUE
  Exception handlers (2):
    start  ->  (handler,  end)
        2  ->        16,   44  [reg=2]
       27  ->        33,   37  [reg=4]
""" % (TRACEBACK_CODE.co_firstlineno + 0,
       TRACEBACK_CODE.co_firstlineno + 2,
       TRACEBACK_CODE.co_firstlineno + 3,
       TRACEBACK_CODE.co_firstlineno + 4,
       TRACEBACK_CODE.co_firstlineno + 5)

def _fstring(a, b, c, d):
    return f'{a} {b:4} {c!r} {d!r:4}'

dis_fstring = """\
%3d           0 FUNC_HEADER             12 (12)

%3d           2 LOAD_FAST                0 (a)
              4 CALL_INTRINSIC_1         4 (vm_format_value)
              6 STORE_FAST               4 (.t0)
              8 LOAD_CONST               2 (' ')
             10 STORE_FAST               5 (.t1)
             12 COPY                   6 1 (.t2 <- b)
             15 LOAD_CONST               3 ('4')
             17 STORE_FAST               7 (.t3)
             19 CALL_INTRINSIC_N     5 6 2 (vm_format_value_spec; .t2; 2)
             23 STORE_FAST               6 (.t2)
             25 LOAD_CONST               2 (' ')
             27 STORE_FAST               7 (.t3)
             29 LOAD_FAST                2 (c)
             31 CALL_INTRINSIC_1         2 (PyObject_Repr)
             33 CALL_INTRINSIC_1         4 (vm_format_value)
             35 STORE_FAST               8 (.t4)
             37 LOAD_CONST               2 (' ')
             39 STORE_FAST               9 (.t5)
             41 LOAD_FAST                3 (d)
             43 CALL_INTRINSIC_1         2 (PyObject_Repr)
             45 STORE_FAST              10 (.t6)
             47 LOAD_CONST               3 ('4')
             49 STORE_FAST              11 (.t7)
             51 CALL_INTRINSIC_N     5 10 2 (vm_format_value_spec; .t6; 2)
             55 STORE_FAST              10 (.t6)
             57 CALL_INTRINSIC_N     6 4 7 (vm_build_string; .t0; 7)
             61 RETURN_VALUE
""" % (_fstring.__code__.co_firstlineno + 0,
       _fstring.__code__.co_firstlineno + 1,)

def _tryfinally(a, b):
    try:
        return a
    finally:
        b()

def _tryfinallyconst(b):
    try:
        return 1
    finally:
        b()

dis_tryfinally = """\
%3d           0 FUNC_HEADER              8 (8)

%3d           2 LOAD_FAST                0 (a)
              4 STORE_FAST               3 (.t1)
              6 CALL_FINALLY           2 5 (.t0; to 11)
             10 RETURN_VALUE

%3d     >>   11 COPY                   7 1 (.t5 <- b)
             14 CALL_FUNCTION          8 0 (.t6 to .t6)
             18 CLEAR_ACC
             19 END_FINALLY              2 (.t0)
             21 LOAD_CONST               0 (None)
             23 RETURN_VALUE
  Exception handlers (1):
    start  ->  (handler,  end)
        2  ->        11,   21  [reg=2]
""" % (_tryfinally.__code__.co_firstlineno + 0,
       _tryfinally.__code__.co_firstlineno + 2,
       _tryfinally.__code__.co_firstlineno + 4,
       )

dis_tryfinallyconst = """\
%3d           0 FUNC_HEADER              7 (7)

%3d           2 LOAD_CONST               2 (1)
              4 STORE_FAST               2 (.t1)
              6 CALL_FINALLY           1 5 (.t0; to 11)
             10 RETURN_VALUE

%3d     >>   11 COPY                   6 0 (.t5 <- b)
             14 CALL_FUNCTION          7 0 (.t6 to .t6)
             18 CLEAR_ACC
             19 END_FINALLY              1 (.t0)
             21 LOAD_CONST               0 (None)
             23 RETURN_VALUE
  Exception handlers (1):
    start  ->  (handler,  end)
        2  ->        11,   21  [reg=1]
""" % (_tryfinallyconst.__code__.co_firstlineno + 0,
       _tryfinallyconst.__code__.co_firstlineno + 2,
       _tryfinallyconst.__code__.co_firstlineno + 4,
       )

def _g(x):
    yield x

async def _ag(x):
    yield x

async def _co(x):
    async for item in _ag(x):
        pass

def _h(y):
    def foo(x):
        '''funcdoc'''
        return [x + z for z in y]
    return foo

dis_nested_0 = """\
%3d           0 FUNC_HEADER              2 (2)

%3d           2 MAKE_FUNCTION            2 (<code object foo at 0x..., file "%s", line %d>)
              4 STORE_FAST               1 (foo)

%3d           6 LOAD_FAST                1 (foo)
              8 RETURN_VALUE
  Cell variables: [0]
""" % (_h.__code__.co_firstlineno + 0,
       _h.__code__.co_firstlineno + 1,
       __file__,
       _h.__code__.co_firstlineno + 1,
       _h.__code__.co_firstlineno + 4,
)

dis_nested_1 = """%s
Disassembly of <code object foo at 0x..., file "%s", line %d>:
%3d           0 FUNC_HEADER              7 (7)

%3d           2 MAKE_FUNCTION            2 (<code object <listcomp> at 0x..., file "%s", line %d>)
              4 STORE_FAST               5 (.t3)
              6 LOAD_DEREF               1 (y)
              8 GET_ITER                 6 (.t4)
             10 CALL_FUNCTION          6 1 (.t4 to .t5)
             14 RETURN_VALUE
  Cell variables: [0]
  Free variables: [(0, 1)]
""" % (dis_nested_0,
       __file__,
       _h.__code__.co_firstlineno + 1,
       _h.__code__.co_firstlineno + 1,
       _h.__code__.co_firstlineno + 3,
       __file__,
       _h.__code__.co_firstlineno + 3,
)

dis_nested_2 = """%s
Disassembly of <code object <listcomp> at 0x..., file "%s", line %d>:
%3d           0 FUNC_HEADER              5 (5)
              2 BUILD_LIST             0 0 (.0; 0)
              5 STORE_FAST               3 (.t0)
              7 JUMP                    17 (to 24)
        >>   10 STORE_FAST               1 (z)
             12 LOAD_DEREF               2 (x)
             14 STORE_FAST               4 (.t1)
             16 LOAD_FAST                1 (z)
             18 BINARY_ADD               4 (.t1)
             20 CLEAR_FAST               4 (.t1)
             22 LIST_APPEND              3 (.t0)
        >>   24 FOR_ITER             0 -14 (.0; to 10)
             28 LOAD_FAST                3 (.t0)
             30 CLEAR_FAST               3 (.t0)
             32 RETURN_VALUE
  Free variables: [(0, 2)]
""" % (dis_nested_1,
       __file__,
       _h.__code__.co_firstlineno + 3,
       _h.__code__.co_firstlineno + 3,
)


class DisTests(unittest.TestCase):

    maxDiff = None

    def get_disassembly(self, func, lasti=-1, wrapper=True, **kwargs):
        # We want to test the default printing behaviour, not the file arg
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            if wrapper:
                dis.dis(func, **kwargs)
            else:
                dis.disassemble(func, lasti, **kwargs)
        return output.getvalue()

    def get_disassemble_as_string(self, func, lasti=-1):
        return self.get_disassembly(func, lasti, False)

    def strip_addresses(self, text):
        return re.sub(r'\b0x[0-9A-Fa-f]+\b', '0x...', text)

    def do_disassembly_test(self, func, expected):
        got = self.get_disassembly(func, depth=0)
        if got != expected:
            got = self.strip_addresses(got)
        self.assertEqual(got, expected)

    def test_opname(self):
        self.assertEqual(dis.opname[dis.opmap["LOAD_FAST"]], "LOAD_FAST")

    def test_widths(self):
        for opcode, opname in enumerate(dis.opname):
            with self.subTest(opname=opname):
                width = dis._OPNAME_WIDTH
                width += 1 + dis._OPARG_WIDTH
                self.assertLessEqual(len(opname), width)

    def test_dis(self):
        self.do_disassembly_test(_f, dis_f)

    def test_bug_708901(self):
        self.do_disassembly_test(bug708901, dis_bug708901)

    def test_bug_1333982(self):
        # This one is checking bytecodes generated for an `assert` statement,
        # so fails if the tests are run with -O.  Skip this test then.
        if not __debug__:
            self.skipTest('need asserts, run without -O')

        self.do_disassembly_test(bug1333982, dis_bug1333982)

    def test_big_linenos(self):
        def func(count):
            namespace = {}
            func = "def foo():\n " + "".join(["\n "] * count + ["spam\n"])
            exec(func, namespace)
            return namespace['foo']

        # Test all small ranges
        for i in range(1, 300):
            expected = _BIG_LINENO_FORMAT % (i + 2)
            self.do_disassembly_test(func(i), expected)

        # Test some larger ranges too
        for i in range(300, 1000, 10):
            expected = _BIG_LINENO_FORMAT % (i + 2)
            self.do_disassembly_test(func(i), expected)

        for i in range(1000, 5000, 10):
            expected = _BIG_LINENO_FORMAT2 % (i + 2)
            self.do_disassembly_test(func(i), expected)

        from test import dis_module
        self.do_disassembly_test(dis_module, dis_module_expected_results)

    def test_big_offsets(self):
        def func(count):
            namespace = {}
            func = "def foo(x):\n " + ";".join(["x = x + 1"] * count) + "\n return x"
            exec(func, namespace)
            return namespace['foo']

        def expected(count, w):
            s = ['''\
  1        %*d FUNC_HEADER              1 (1)

''' % (w, 0)]
            s += ['''\
           %*d LOAD_CONST               2 (1)
           %*d BINARY_ADD               0 (x)
           %*d STORE_FAST               0 (x)
''' % (w, 6*i + 2, w, 6*i + 4, w, 6*i + 6)
                 for i in range(count)]
            s += ['''\

  3        %*d LOAD_FAST                0 (x)
           %*d RETURN_VALUE
''' % (w, 6*count + 2, w, 6*count + 4)]
            s[1] = '  2' + s[1][3:]
            return ''.join(s)

        for i in range(1, 5):
            self.do_disassembly_test(func(i), expected(i, 4))
        self.do_disassembly_test(func(1666), expected(1666, 4))
        self.do_disassembly_test(func(1667), expected(1667, 5))

    def test_disassemble_str(self):
        self.do_disassembly_test(expr_str, dis_expr_str)
        self.do_disassembly_test(simple_stmt_str, dis_simple_stmt_str)
        self.do_disassembly_test(annot_stmt_str, dis_annot_stmt_str)
        self.do_disassembly_test(compound_stmt_str, dis_compound_stmt_str)

    def test_disassemble_bytes(self):
        self.do_disassembly_test(_f.__code__.co_code, dis_f_co_code)

    def test_disassemble_class(self):
        self.do_disassembly_test(_C, dis_c)

    def test_disassemble_instance_method(self):
        self.do_disassembly_test(_C(1).__init__, dis_c_instance_method)

    def test_disassemble_instance_method_bytes(self):
        method_bytecode = _C(1).__init__.__code__.co_code
        self.do_disassembly_test(method_bytecode, dis_c_instance_method_bytes)

    def test_disassemble_static_method(self):
        self.do_disassembly_test(_C.sm, dis_c_static_method)

    def test_disassemble_class_method(self):
        self.do_disassembly_test(_C.cm, dis_c_class_method)

    def test_disassemble_generator(self):
        gen_func_disas = self.get_disassembly(_g)  # Generator function
        gen_disas = self.get_disassembly(_g(1))  # Generator iterator
        self.assertEqual(gen_disas, gen_func_disas)

    def test_disassemble_async_generator(self):
        agen_func_disas = self.get_disassembly(_ag)  # Async generator function
        agen_disas = self.get_disassembly(_ag(1))  # Async generator iterator
        self.assertEqual(agen_disas, agen_func_disas)

    def test_disassemble_coroutine(self):
        coro_func_disas = self.get_disassembly(_co)  # Coroutine function
        coro = _co(1)  # Coroutine object
        coro.close()  # Avoid a RuntimeWarning (never awaited)
        coro_disas = self.get_disassembly(coro)
        self.assertEqual(coro_disas, coro_func_disas)

    def test_disassemble_fstring(self):
        self.do_disassembly_test(_fstring, dis_fstring)

    def test_disassemble_try_finally(self):
        self.do_disassembly_test(_tryfinally, dis_tryfinally)
        self.do_disassembly_test(_tryfinallyconst, dis_tryfinallyconst)

    def test_dis_none(self):
        try:
            del sys.last_traceback
        except AttributeError:
            pass
        self.assertRaises(RuntimeError, dis.dis, None)

    def test_dis_traceback(self):
        try:
            del sys.last_traceback
        except AttributeError:
            pass

        try:
            1/0
        except Exception as e:
            tb = e.__traceback__
            sys.last_traceback = tb

        tb_dis = self.get_disassemble_as_string(tb.tb_frame.f_code, tb.tb_lasti)
        self.do_disassembly_test(None, tb_dis)

    def test_dis_object(self):
        self.assertRaises(TypeError, dis.dis, object())

    def test_disassemble_recursive(self):
        def check(expected, **kwargs):
            dis = self.get_disassembly(_h, **kwargs)
            dis = self.strip_addresses(dis)
            self.assertEqual(dis, expected)

        check(dis_nested_0, depth=0)
        check(dis_nested_1, depth=1)
        check(dis_nested_2, depth=2)
        check(dis_nested_2, depth=3)
        check(dis_nested_2, depth=None)
        check(dis_nested_2)


class DisWithFileTests(DisTests):

    # Run the tests again, using the file arg instead of print
    def get_disassembly(self, func, lasti=-1, wrapper=True, **kwargs):
        output = io.StringIO()
        if wrapper:
            dis.dis(func, file=output, **kwargs)
        else:
            dis.disassemble(func, lasti, file=output, **kwargs)
        return output.getvalue()



code_info_code_info = """\
Name:              code_info
Filename:          (.*)
Argument count:    1
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  1
Stack size:        10
Flags:             OPTIMIZED, NEWLOCALS
Constants:
   0: %r
   1: 'code_info'
   2: '_format_code_info'
   3: '_get_code_object'
Variable names:
   0: x""" % (('Formatted details of methods, functions, or code.',)
              if sys.flags.optimize < 2 else (None,))

@staticmethod
def tricky(a, b, /, x, y, z=True, *args, c, d, e=[], **kwds):
    def f(c=c):
        print(a, b, x, y, z, c, d, e, f)
    yield a, b, x, y, z, c, d, e, f

code_info_tricky = """\
Name:              tricky
Filename:          (.*)
Argument count:    5
Positional-only arguments: 2
Kw-only arguments: 3
Number of locals:  11
Stack size:        20
Flags:             OPTIMIZED, NEWLOCALS, VARARGS, VARKEYWORDS, GENERATOR
Constants:
   0: None
   1: 'tricky'
   2: <code object f at (.*), file "(.*)", line (.*)>
Variable names:
   0: a
   1: b
   2: x
   3: y
   4: z
   5: c
   6: d
   7: e
   8: args
   9: kwds
  10: f
Cell variables:
   0: a
   1: b
   2: x
   3: y
   4: z
   5: d
   6: e
   7: f"""

co_tricky_nested_f = tricky.__func__.__code__.co_consts[2]

code_info_tricky_nested_f = """\
Name:              f
Filename:          (.*)
Argument count:    1
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  9
Stack size:        22
Flags:             OPTIMIZED, NEWLOCALS, NESTED
Constants:
   0: None
   1: 'tricky.<locals>.f'
   2: 'print'
Variable names:
   0: c
   1: a
   2: b
   3: x
   4: y
   5: z
   6: d
   7: e
   8: f
Free variables:
   0: [abedfxyz]
   1: [abedfxyz]
   2: [abedfxyz]
   3: [abedfxyz]
   4: [abedfxyz]
   5: [abedfxyz]
   6: [abedfxyz]
   7: [abedfxyz]"""

code_info_expr_str = """\
Name:              <module>
Filename:          <disassembly>
Argument count:    0
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  1
Stack size:        2
Flags:             0x0
Constants:
   0: 'x'
   1: 1
Variable names:
   0: <locals>"""

code_info_simple_stmt_str = """\
Name:              <module>
Filename:          <disassembly>
Argument count:    0
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  1
Stack size:        2
Flags:             0x0
Constants:
   0: 'x'
   1: 1
   2: None
Variable names:
   0: <locals>"""

code_info_compound_stmt_str = """\
Name:              <module>
Filename:          <disassembly>
Argument count:    0
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  1
Stack size:        2
Flags:             0x0
Constants:
   0: 0
   1: 'x'
   2: 1
Variable names:
   0: <locals>"""


async def async_def():
    await 1
    async for a in b: pass
    async with c as d: pass

code_info_async_def = """\
Name:              async_def
Filename:          (.*)
Argument count:    0
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  2
Stack size:        6
Flags:             OPTIMIZED, NEWLOCALS, COROUTINE
Constants:
   0: None
   1: 'async_def'
   2: 1
   3: 'b'
   4: 'c'
Variable names:
   0: a
   1: d"""

class CodeInfoTests(unittest.TestCase):
    test_pairs = [
      (dis.code_info, code_info_code_info),
      (tricky, code_info_tricky),
      (co_tricky_nested_f, code_info_tricky_nested_f),
      (expr_str, code_info_expr_str),
      (simple_stmt_str, code_info_simple_stmt_str),
      (compound_stmt_str, code_info_compound_stmt_str),
      (async_def, code_info_async_def)
    ]

    def test_code_info(self):
        self.maxDiff = 1000
        for x, expected in self.test_pairs:
            self.assertRegex(dis.code_info(x), expected)

    def test_show_code(self):
        self.maxDiff = 1000
        for x, expected in self.test_pairs:
            with captured_stdout() as output:
                dis.show_code(x)
            self.assertRegex(output.getvalue(), expected+"\n")
            output = io.StringIO()
            dis.show_code(x, file=output)
            self.assertRegex(output.getvalue(), expected)

    def test_code_info_object(self):
        self.assertRaises(TypeError, dis.code_info, object())

    def test_pretty_flags_no_flags(self):
        self.assertEqual(dis.pretty_flags(0), '0x0')


# Fodder for instruction introspection tests
#   Editing any of these may require recalculating the expected output
def outer(a=1, b=2):
    def f(c=3, d=4):
        def inner(e=5, f=6):
            print(a, b, c, d, e, f)
        print(a, b, c, d)
        return inner
    print(a, b, '', 1, [], {}, "Hello world!")
    return f

def jumpy():
    # This won't actually run (but that's OK, we only disassemble it)
    for i in range(10):
        print(i)
        if i < 4:
            continue
        if i > 6:
            break
    else:
        print("I can haz else clause?")
    while i:
        print(i)
        i -= 1
        if i > 6:
            continue
        if i < 4:
            break
    else:
        print("Who let lolcatz into this test suite?")
    try:
        1 / 0
    except ZeroDivisionError:
        print("Here we go, here we go, here we go...")
    else:
        with i as dodgy:
            print("Never reach this")
    finally:
        print("OK, now we're done")

# End fodder for opinfo generation tests
expected_outer_line = 1
_line_offset = outer.__code__.co_firstlineno - 1
code_object_f = outer.__code__.co_consts[4]
expected_f_line = code_object_f.co_firstlineno - _line_offset
code_object_inner = code_object_f.co_consts[4]
expected_inner_line = code_object_inner.co_firstlineno - _line_offset
expected_jumpy_line = 1

# The following lines are useful to regenerate the expected results after
# either the fodder is modified or the bytecode generation changes
# After regeneration, update the references to code_object_f and
# code_object_inner before rerunning the tests

#_instructions = dis.get_instructions(outer, first_line=expected_outer_line)
#print('expected_opinfo_outer = [\n  ',
      #',\n  '.join(map(str, _instructions)), ',\n]', sep='')
#_instructions = dis.get_instructions(outer(), first_line=expected_f_line)
#print('expected_opinfo_f = [\n  ',
      #',\n  '.join(map(str, _instructions)), ',\n]', sep='')
#_instructions = dis.get_instructions(outer()(), first_line=expected_inner_line)
#print('expected_opinfo_inner = [\n  ',
      #',\n  '.join(map(str, _instructions)), ',\n]', sep='')
#_instructions = dis.get_instructions(jumpy, first_line=expected_jumpy_line)
#print('expected_opinfo_jumpy = [\n  ',
      #',\n  '.join(map(str, _instructions)), ',\n]', sep='')


Instruction = dis.Instruction
expected_opinfo_outer = [
  Instruction(opname='FUNC_HEADER', opcode=6, imm=[14], argval=14, argrepr='14', offset=0, starts_line=1, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[2], argval=3, argrepr='3', offset=2, starts_line=2, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[3], argval=3, argrepr='.t0', offset=4, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[3], argval=4, argrepr='4', offset=6, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[4], argval=4, argrepr='.t1', offset=8, starts_line=None, is_jump_target=False),
  Instruction(opname='MAKE_FUNCTION', opcode=107, imm=[4], argval=code_object_f, argrepr=repr(code_object_f), offset=10, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[4], argval=4, argrepr='.t1', offset=12, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[3], argval=3, argrepr='.t0', offset=14, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[2], argval=2, argrepr='f', offset=16, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[5, 254], argval='print', argrepr="'print'; 254", offset=18, starts_line=7, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[6], argval=6, argrepr='.t3', offset=21, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[0], argval=0, argrepr='a', offset=23, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[7], argval=7, argrepr='.t4', offset=25, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[1], argval=1, argrepr='b', offset=27, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[8], argval=8, argrepr='.t5', offset=29, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[6], argval='', argrepr="''", offset=31, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[9], argval=9, argrepr='.t6', offset=33, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[7], argval=1, argrepr='1', offset=35, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[10], argval=10, argrepr='.t7', offset=37, starts_line=None, is_jump_target=False),
  Instruction(opname='BUILD_LIST', opcode=94, imm=[11, 0], argval=(11, 0), argrepr='.t8; 0', offset=39, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[11], argval=11, argrepr='.t8', offset=42, starts_line=None, is_jump_target=False),
  Instruction(opname='BUILD_MAP', opcode=96, imm=[0], argval=0, argrepr='0', offset=44, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[12], argval=12, argrepr='.t9', offset=46, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[8], argval='Hello world!', argrepr="'Hello world!'", offset=48, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[13], argval=13, argrepr='.t10', offset=50, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[7, 7], argval=(7, 7), argrepr='.t4 to .t11', offset=52, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=56, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=49, imm=[2], argval=2, argrepr='f', offset=57, starts_line=8, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=76, imm=[], argval=None, argrepr='', offset=59, starts_line=None, is_jump_target=False),
]

expected_opinfo_f = [
  Instruction(opname='FUNC_HEADER', opcode=6, imm=[13], argval=13, argrepr='13', offset=0, starts_line=2, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[2], argval=5, argrepr='5', offset=2, starts_line=3, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[5], argval=5, argrepr='.t0', offset=4, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[3], argval=6, argrepr='6', offset=6, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[6], argval=6, argrepr='.t1', offset=8, starts_line=None, is_jump_target=False),
  Instruction(opname='MAKE_FUNCTION', opcode=107, imm=[4], argval=code_object_inner, argrepr=repr(code_object_inner), offset=10, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[6], argval=6, argrepr='.t1', offset=12, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[5], argval=5, argrepr='.t0', offset=14, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[2], argval=2, argrepr='inner', offset=16, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[5, 254], argval='print', argrepr="'print'; 254", offset=18, starts_line=5, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[8], argval=8, argrepr='.t3', offset=21, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[3], argval=3, argrepr='a', offset=23, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[9], argval=9, argrepr='.t4', offset=25, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[4], argval=4, argrepr='b', offset=27, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[10], argval=10, argrepr='.t5', offset=29, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[0], argval=0, argrepr='c', offset=31, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[11], argval=11, argrepr='.t6', offset=33, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[1], argval=1, argrepr='d', offset=35, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[12], argval=12, argrepr='.t7', offset=37, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[9, 4], argval=(9, 4), argrepr='.t4 to .t8', offset=39, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=43, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=49, imm=[2], argval=2, argrepr='inner', offset=44, starts_line=6, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=76, imm=[], argval=None, argrepr='', offset=46, starts_line=None, is_jump_target=False),
]

expected_opinfo_inner = [
  Instruction(opname='FUNC_HEADER', opcode=6, imm=[16], argval=16, argrepr='16', offset=0, starts_line=3, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[2, 254], argval='print', argrepr="'print'; 254", offset=2, starts_line=4, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[9], argval=9, argrepr='.t3', offset=5, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[2], argval=2, argrepr='a', offset=7, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[10], argval=10, argrepr='.t4', offset=9, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[3], argval=3, argrepr='b', offset=11, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[11], argval=11, argrepr='.t5', offset=13, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[4], argval=4, argrepr='c', offset=15, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[12], argval=12, argrepr='.t6', offset=17, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=55, imm=[5], argval=5, argrepr='d', offset=19, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[13], argval=13, argrepr='.t7', offset=21, starts_line=None, is_jump_target=False),
  Instruction(opname='COPY', opcode=4, imm=[14, 0], argval=(14, 0), argrepr='.t8 <- e', offset=23, starts_line=None, is_jump_target=False),
  Instruction(opname='COPY', opcode=4, imm=[15, 1], argval=(15, 1), argrepr='.t9 <- f', offset=26, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[10, 6], argval=(10, 6), argrepr='.t4 to .t10', offset=29, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=33, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[0], argval=None, argrepr='None', offset=34, starts_line=None, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=76, imm=[], argval=None, argrepr='', offset=36, starts_line=None, is_jump_target=False),
]

expected_opinfo_jumpy = [
  Instruction(opname='FUNC_HEADER', opcode=6, imm=[9], argval=9, argrepr='9', offset=0, starts_line=1, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[2, 254], argval='range', argrepr="'range'; 254", offset=2, starts_line=3, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[5], argval=5, argrepr='.t3', offset=5, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[3], argval=10, argrepr='10', offset=7, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[6], argval=6, argrepr='.t4', offset=9, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=11, starts_line=None, is_jump_target=False),
  Instruction(opname='GET_ITER', opcode=86, imm=[2], argval=2, argrepr='.t0', offset=15, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[42], argval=59, argrepr='to 59', offset=17, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[0], argval=0, argrepr='i', offset=20, starts_line=None, is_jump_target=True),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[4, 252], argval='print', argrepr="'print'; 252", offset=22, starts_line=4, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[6], argval=6, argrepr='.t4', offset=25, starts_line=None, is_jump_target=False),
  Instruction(opname='COPY', opcode=4, imm=[7, 0], argval=(7, 0), argrepr='.t5 <- i', offset=27, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[7, 1], argval=(7, 1), argrepr='.t5 to .t6', offset=30, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=34, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[5], argval=4, argrepr='4', offset=35, starts_line=5, is_jump_target=False),
  Instruction(opname='COMPARE_OP', opcode=35, imm=[0, 0], argval=(0, 0), argrepr='0; i', offset=37, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=84, imm=[6], argval=46, argrepr='to 46', offset=40, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[16], argval=59, argrepr='to 59', offset=43, starts_line=6, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[6], argval=6, argrepr='6', offset=46, starts_line=7, is_jump_target=True),
  Instruction(opname='COMPARE_OP', opcode=35, imm=[4, 0], argval=(4, 0), argrepr='4; i', offset=48, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=84, imm=[8], argval=59, argrepr='to 59', offset=51, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[2], argval=2, argrepr='.t0', offset=54, starts_line=8, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[21], argval=77, argrepr='to 77', offset=56, starts_line=None, is_jump_target=False),
  Instruction(opname='FOR_ITER', opcode=88, imm=[2, -39], argval=(2, 20), argrepr='.t0; to 20', offset=59, starts_line=3, is_jump_target=True),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[4, 252], argval='print', argrepr="'print'; 252", offset=63, starts_line=10, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[5], argval=5, argrepr='.t3', offset=66, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[7], argval='I can haz else clause?', argrepr="'I can haz else clause?'", offset=68, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[6], argval=6, argrepr='.t4', offset=70, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=72, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=76, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[44], argval=121, argrepr='to 121', offset=77, starts_line=11, is_jump_target=True),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[4, 252], argval='print', argrepr="'print'; 252", offset=80, starts_line=12, is_jump_target=True),
  Instruction(opname='STORE_FAST', opcode=57, imm=[5], argval=5, argrepr='.t3', offset=83, starts_line=None, is_jump_target=False),
  Instruction(opname='COPY', opcode=4, imm=[6, 0], argval=(6, 0), argrepr='.t4 <- i', offset=85, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=88, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=92, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[8], argval=1, argrepr='1', offset=93, starts_line=13, is_jump_target=False),
  Instruction(opname='INPLACE_SUBTRACT', opcode=39, imm=[0], argval=0, argrepr='i', offset=95, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[0], argval=0, argrepr='i', offset=97, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[6], argval=6, argrepr='6', offset=99, starts_line=14, is_jump_target=False),
  Instruction(opname='COMPARE_OP', opcode=35, imm=[4, 0], argval=(4, 0), argrepr='4; i', offset=101, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=84, imm=[6], argval=110, argrepr='to 110', offset=104, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[14], argval=121, argrepr='to 121', offset=107, starts_line=15, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[5], argval=4, argrepr='4', offset=110, starts_line=16, is_jump_target=True),
  Instruction(opname='COMPARE_OP', opcode=35, imm=[0, 0], argval=(0, 0), argrepr='0; i', offset=112, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=84, imm=[6], argval=121, argrepr='to 121', offset=115, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[22], argval=140, argrepr='to 140', offset=118, starts_line=17, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=49, imm=[0], argval=0, argrepr='i', offset=121, starts_line=11, is_jump_target=True),
  Instruction(opname='POP_JUMP_IF_TRUE', opcode=85, imm=[-43], argval=80, argrepr='to 80', offset=123, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[4, 252], argval='print', argrepr="'print'; 252", offset=126, starts_line=19, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[5], argval=5, argrepr='.t3', offset=129, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[9], argval='Who let lolcatz into this test suite?', argrepr="'Who let lolcatz into this test suite?'", offset=131, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[6], argval=6, argrepr='.t4', offset=133, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=135, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=139, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[8], argval=1, argrepr='1', offset=140, starts_line=21, is_jump_target=True),
  Instruction(opname='STORE_FAST', opcode=57, imm=[2], argval=2, argrepr='.t0', offset=142, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[10], argval=0, argrepr='0', offset=144, starts_line=None, is_jump_target=False),
  Instruction(opname='BINARY_TRUE_DIVIDE', opcode=27, imm=[2], argval=2, argrepr='.t0', offset=146, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[2], argval=2, argrepr='.t0', offset=148, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=150, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[31], argval=182, argrepr='to 182', offset=151, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[11, 250], argval='ZeroDivisionError', argrepr="'ZeroDivisionError'; 250", offset=154, starts_line=22, is_jump_target=False),
  Instruction(opname='JUMP_IF_NOT_EXC_MATCH', opcode=83, imm=[2, 23], argval=(2, 180), argrepr='.t0; to 180', offset=157, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[4, 252], argval='print', argrepr="'print'; 252", offset=161, starts_line=23, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[7], argval=7, argrepr='.t5', offset=164, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[12], argval='Here we go, here we go, here we go...', argrepr="'Here we go, here we go, here we go...'", offset=166, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[8], argval=8, argrepr='.t6', offset=168, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[8, 1], argval=(8, 1), argrepr='.t6 to .t7', offset=170, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=174, starts_line=None, is_jump_target=False),
  Instruction(opname='END_EXCEPT', opcode=97, imm=[2], argval=2, argrepr='.t0', offset=175, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=80, imm=[27], argval=204, argrepr='to 204', offset=177, starts_line=None, is_jump_target=False),
  Instruction(opname='END_FINALLY', opcode=99, imm=[2], argval=2, argrepr='.t0', offset=180, starts_line=None, is_jump_target=True),
  Instruction(opname='LOAD_FAST', opcode=49, imm=[0], argval=0, argrepr='i', offset=182, starts_line=25, is_jump_target=True),
  Instruction(opname='SETUP_WITH', opcode=108, imm=[2], argval=2, argrepr='.t0', offset=184, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[1], argval=1, argrepr='dodgy', offset=186, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[4, 252], argval='print', argrepr="'print'; 252", offset=188, starts_line=26, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[7], argval=7, argrepr='.t5', offset=191, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[13], argval='Never reach this', argrepr="'Never reach this'", offset=193, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[8], argval=8, argrepr='.t6', offset=195, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[8, 1], argval=(8, 1), argrepr='.t6 to .t7', offset=197, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=201, starts_line=None, is_jump_target=False),
  Instruction(opname='END_WITH', opcode=109, imm=[2], argval=2, argrepr='.t0', offset=202, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=53, imm=[4, 252], argval='print', argrepr="'print'; 252", offset=204, starts_line=28, is_jump_target=True),
  Instruction(opname='STORE_FAST', opcode=57, imm=[7], argval=7, argrepr='.t5', offset=207, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[14], argval="OK, now we're done", argrepr='"OK, now we\'re done"', offset=209, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=57, imm=[8], argval=8, argrepr='.t6', offset=211, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=71, imm=[8, 1], argval=(8, 1), argrepr='.t6 to .t7', offset=213, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=217, starts_line=None, is_jump_target=False),
  Instruction(opname='END_FINALLY', opcode=99, imm=[2], argval=2, argrepr='.t0', offset=218, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[0], argval=None, argrepr='None', offset=220, starts_line=None, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=76, imm=[], argval=None, argrepr='', offset=222, starts_line=None, is_jump_target=False),
]

# One last piece of inspect fodder to check the default line number handling
def simple(): pass
expected_opinfo_simple = [
  Instruction(opname='FUNC_HEADER', opcode=6, imm=[0], argval=0, argrepr='0', offset=0, starts_line=simple.__code__.co_firstlineno, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=51, imm=[0], argval=None, argrepr='None', offset=2, starts_line=None, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=76, imm=[], argval=None, argrepr='', offset=4, starts_line=None, is_jump_target=False),
]


class InstructionTests(BytecodeTestCase):

    def __init__(self, *args):
        super().__init__(*args)
        self.maxDiff = None

    def test_default_first_line(self):
        actual = dis.get_instructions(simple)
        self.assertEqual(list(actual), expected_opinfo_simple)

    def test_first_line_set_to_None(self):
        actual = dis.get_instructions(simple, first_line=None)
        self.assertEqual(list(actual), expected_opinfo_simple)

    def test_outer(self):
        actual = dis.get_instructions(outer, first_line=expected_outer_line)
        self.assertEqual(list(actual), expected_opinfo_outer)

    def test_nested(self):
        with captured_stdout():
            f = outer()
        actual = dis.get_instructions(f, first_line=expected_f_line)
        self.assertEqual(list(actual), expected_opinfo_f)

    def test_doubly_nested(self):
        with captured_stdout():
            inner = outer()()
        actual = dis.get_instructions(inner, first_line=expected_inner_line)
        self.assertEqual(list(actual), expected_opinfo_inner)

    def test_jumpy(self):
        actual = dis.get_instructions(jumpy, first_line=expected_jumpy_line)
        self.assertEqual(list(actual), expected_opinfo_jumpy)

# get_instructions has its own tests above, so can rely on it to validate
# the object oriented API
class BytecodeTests(unittest.TestCase):

    def test_instantiation(self):
        # Test with function, method, code string and code object
        for obj in [_f, _C(1).__init__, "a=1", _f.__code__]:
            with self.subTest(obj=obj):
                b = dis.Bytecode(obj)
                self.assertIsInstance(b.codeobj, types.CodeType)

        self.assertRaises(TypeError, dis.Bytecode, object())

    def test_iteration(self):
        for obj in [_f, _C(1).__init__, "a=1", _f.__code__]:
            with self.subTest(obj=obj):
                via_object = list(dis.Bytecode(obj))
                via_generator = list(dis.get_instructions(obj))
                self.assertEqual(via_object, via_generator)

    def test_explicit_first_line(self):
        actual = dis.Bytecode(outer, first_line=expected_outer_line)
        self.assertEqual(list(actual), expected_opinfo_outer)

    def test_source_line_in_disassembly(self):
        # Use the line in the source code
        actual = dis.Bytecode(simple).dis()
        actual = actual.strip().partition(" ")[0]  # extract the line no
        expected = str(simple.__code__.co_firstlineno)
        self.assertEqual(actual, expected)
        # Use an explicit first line number
        actual = dis.Bytecode(simple, first_line=350).dis()
        actual = actual.strip().partition(" ")[0]  # extract the line no
        self.assertEqual(actual, "350")

    def test_info(self):
        self.maxDiff = 1000
        for x, expected in CodeInfoTests.test_pairs:
            b = dis.Bytecode(x)
            self.assertRegex(b.info(), expected)

    def test_disassembled(self):
        actual = dis.Bytecode(_f).dis()
        self.assertEqual(actual, dis_f)

    def test_from_traceback(self):
        tb = get_tb()
        b = dis.Bytecode.from_traceback(tb)
        while tb.tb_next: tb = tb.tb_next

        self.assertEqual(b.current_offset, tb.tb_lasti)

    def test_from_traceback_dis(self):
        tb = get_tb()
        b = dis.Bytecode.from_traceback(tb)
        self.assertEqual(b.dis(), dis_traceback)


class TestDisTraceback(unittest.TestCase):
    def setUp(self) -> None:
        try:  # We need to clean up existing tracebacks
            del sys.last_traceback
        except AttributeError:
            pass
        return super().setUp()

    def get_disassembly(self, tb):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            dis.distb(tb)
        return output.getvalue()

    def test_distb_empty(self):
        with self.assertRaises(RuntimeError):
            dis.distb()

    def test_distb_last_traceback(self):
        # We need to have an existing last traceback in `sys`:
        tb = get_tb()
        sys.last_traceback = tb

        self.assertEqual(self.get_disassembly(None), dis_traceback)

    def test_distb_explicit_arg(self):
        tb = get_tb()

        self.assertEqual(self.get_disassembly(tb), dis_traceback)


class TestDisTracebackWithFile(TestDisTraceback):
    # Run the `distb` tests again, using the file arg instead of print
    def get_disassembly(self, tb):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            dis.distb(tb, file=output)
        return output.getvalue()


if __name__ == "__main__":
    unittest.main()
