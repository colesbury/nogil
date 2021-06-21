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
%3d           0 LOAD_FAST                1 (x)
              2 LOAD_CONST               1 (1)
              4 COMPARE_OP               2 (==)
              6 LOAD_FAST                0 (self)
              8 STORE_ATTR               0 (x)
             10 LOAD_CONST               0 (None)
             12 RETURN_VALUE
""" % (_C.__init__.__code__.co_firstlineno + 1,)

dis_c_instance_method_bytes = """\
          0 LOAD_FAST                1 (1)
          2 LOAD_CONST               1 (1)
          4 COMPARE_OP               2 (==)
          6 LOAD_FAST                0 (0)
          8 STORE_ATTR               0 (0)
         10 LOAD_CONST               0 (0)
         12 RETURN_VALUE
"""

dis_c_class_method = """\
%3d           0 LOAD_FAST                1 (x)
              2 LOAD_CONST               1 (1)
              4 COMPARE_OP               2 (==)
              6 LOAD_FAST                0 (cls)
              8 STORE_ATTR               0 (x)
             10 LOAD_CONST               0 (None)
             12 RETURN_VALUE
""" % (_C.cm.__code__.co_firstlineno + 2,)

dis_c_static_method = """\
%3d           0 LOAD_FAST                0 (x)
              2 LOAD_CONST               1 (1)
              4 COMPARE_OP               2 (==)
              6 STORE_FAST               0 (x)
              8 LOAD_CONST               0 (None)
             10 RETURN_VALUE
""" % (_C.sm.__code__.co_firstlineno + 2,)

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
%3d           0 LOAD_GLOBAL_FOR_CALL     0 (print)
              2 LOAD_FAST                0 (a)
              4 CALL_FUNCTION            1
              6 POP_TOP

%3d           8 LOAD_CONST               1 (1)
             10 RETURN_VALUE
""" % (_f.__code__.co_firstlineno + 1,
       _f.__code__.co_firstlineno + 2)


dis_f_co_code = """\
          0 LOAD_GLOBAL_FOR_CALL     0 (0)
          2 LOAD_FAST                0 (0)
          4 CALL_FUNCTION            1
          6 POP_TOP
          8 LOAD_CONST               1 (1)
         10 RETURN_VALUE
"""


def bug708901():
    for res in range(1,
                     10):
        pass

dis_bug708901 = """\
%3d           0 LOAD_GLOBAL_FOR_CALL     0 (range)
              2 LOAD_CONST               1 (1)

%3d           4 LOAD_CONST               2 (10)

%3d           6 CALL_FUNCTION            2
              8 GET_ITER
        >>   10 FOR_ITER                 4 (to 16)
             12 STORE_FAST               0 (res)

%3d          14 JUMP_ABSOLUTE           10
        >>   16 LOAD_CONST               0 (None)
             18 RETURN_VALUE
""" % (bug708901.__code__.co_firstlineno + 1,
       bug708901.__code__.co_firstlineno + 2,
       bug708901.__code__.co_firstlineno + 1,
       bug708901.__code__.co_firstlineno + 3)


def bug1333982(x=[]):
    assert 0, ([s for s in x] +
              1)
    pass

dis_bug1333982 = """\
%3d           0 LOAD_CONST               1 (0)
              2 POP_JUMP_IF_TRUE        30
              4 LOAD_ASSERTION_ERROR
              6 DEFER_REFCOUNT
              8 LOAD_CONST               2 (<code object <listcomp> at 0x..., file "%s", line %d>)
             10 LOAD_CONST               3 ('bug1333982.<locals>.<listcomp>')
             12 MAKE_FUNCTION            0
             14 DEFER_REFCOUNT
             16 LOAD_FAST                0 (x)
             18 GET_ITER
             20 CALL_FUNCTION            1

%3d          22 LOAD_CONST               4 (1)

%3d          24 BINARY_ADD
             26 CALL_FUNCTION            1
             28 RAISE_VARARGS            1

%3d     >>   30 LOAD_CONST               0 (None)
             32 RETURN_VALUE
""" % (bug1333982.__code__.co_firstlineno + 1,
       __file__,
       bug1333982.__code__.co_firstlineno + 1,
       bug1333982.__code__.co_firstlineno + 2,
       bug1333982.__code__.co_firstlineno + 1,
       bug1333982.__code__.co_firstlineno + 3)

_BIG_LINENO_FORMAT = """\
%3d           0 LOAD_GLOBAL              0 (spam)
              2 POP_TOP
              4 LOAD_CONST               0 (None)
              6 RETURN_VALUE
"""

_BIG_LINENO_FORMAT2 = """\
%4d           0 LOAD_GLOBAL              0 (spam)
               2 POP_TOP
               4 LOAD_CONST               0 (None)
               6 RETURN_VALUE
"""

dis_module_expected_results = """\
Disassembly of f:
  4           0 LOAD_CONST               0 (None)
              2 RETURN_VALUE

Disassembly of g:
  5           0 LOAD_CONST               0 (None)
              2 RETURN_VALUE

"""

expr_str = "x + 1"

dis_expr_str = """\
  1           0 LOAD_NAME                0 (x)
              2 LOAD_CONST               0 (1)
              4 BINARY_ADD
              6 RETURN_VALUE
"""

simple_stmt_str = "x = x + 1"

dis_simple_stmt_str = """\
  1           0 LOAD_NAME                0 (x)
              2 LOAD_CONST               0 (1)
              4 BINARY_ADD
              6 STORE_NAME               0 (x)
              8 LOAD_CONST               1 (None)
             10 RETURN_VALUE
"""

annot_stmt_str = """\

x: int = 1
y: fun(1)
lst[fun(0)]: int = 1
"""
# leading newline is for a reason (tests lineno)

dis_annot_stmt_str = """\
  2           0 SETUP_ANNOTATIONS
              2 LOAD_CONST               0 (1)
              4 STORE_NAME               0 (x)
              6 LOAD_NAME                1 (int)
              8 LOAD_NAME                2 (__annotations__)
             10 LOAD_CONST               1 ('x')
             12 STORE_SUBSCR

  3          14 LOAD_NAME                3 (fun)
             16 DEFER_REFCOUNT
             18 LOAD_CONST               0 (1)
             20 CALL_FUNCTION            1
             22 LOAD_NAME                2 (__annotations__)
             24 LOAD_CONST               2 ('y')
             26 STORE_SUBSCR

  4          28 LOAD_CONST               0 (1)
             30 LOAD_NAME                4 (lst)
             32 LOAD_NAME                3 (fun)
             34 DEFER_REFCOUNT
             36 LOAD_CONST               3 (0)
             38 CALL_FUNCTION            1
             40 STORE_SUBSCR
             42 LOAD_NAME                1 (int)
             44 POP_TOP
             46 LOAD_CONST               4 (None)
             48 RETURN_VALUE
"""

compound_stmt_str = """\
x = 0
while 1:
    x += 1"""
# Trailing newline has been deliberately omitted

dis_compound_stmt_str = """\
  1           0 LOAD_CONST               0 (0)
              2 STORE_NAME               0 (x)

  3     >>    4 LOAD_NAME                0 (x)
              6 LOAD_CONST               1 (1)
              8 INPLACE_ADD
             10 STORE_NAME               0 (x)
             12 JUMP_ABSOLUTE            4
             14 LOAD_CONST               2 (None)
             16 RETURN_VALUE
"""

dis_traceback = """\
%3d           0 SETUP_FINALLY           12 (to 14)

%3d           2 LOAD_CONST               1 (1)
              4 LOAD_CONST               2 (0)
    -->       6 BINARY_TRUE_DIVIDE
              8 POP_TOP
             10 POP_BLOCK
             12 JUMP_FORWARD            42 (to 56)

%3d     >>   14 DUP_TOP
             16 LOAD_GLOBAL              0 (Exception)
             18 JUMP_IF_NOT_EXC_MATCH    54
             20 POP_TOP
             22 STORE_FAST               0 (e)
             24 POP_TOP
             26 SETUP_FINALLY           18 (to 46)

%3d          28 LOAD_FAST                0 (e)
             30 LOAD_ATTR                1 (__traceback__)
             32 STORE_FAST               1 (tb)
             34 POP_BLOCK
             36 POP_EXCEPT
             38 LOAD_CONST               0 (None)
             40 STORE_FAST               0 (e)
             42 DELETE_FAST              0 (e)
             44 JUMP_FORWARD            10 (to 56)
        >>   46 LOAD_CONST               0 (None)
             48 STORE_FAST               0 (e)
             50 DELETE_FAST              0 (e)
             52 RERAISE
        >>   54 RERAISE

%3d     >>   56 LOAD_FAST                1 (tb)
             58 RETURN_VALUE
""" % (TRACEBACK_CODE.co_firstlineno + 1,
       TRACEBACK_CODE.co_firstlineno + 2,
       TRACEBACK_CODE.co_firstlineno + 3,
       TRACEBACK_CODE.co_firstlineno + 4,
       TRACEBACK_CODE.co_firstlineno + 5)

def _fstring(a, b, c, d):
    return f'{a} {b:4} {c!r} {d!r:4}'

dis_fstring = """\
%3d           0 LOAD_FAST                0 (a)
              2 FORMAT_VALUE             0
              4 LOAD_CONST               1 (' ')
              6 LOAD_FAST                1 (b)
              8 LOAD_CONST               2 ('4')
             10 FORMAT_VALUE             4 (with format)
             12 LOAD_CONST               1 (' ')
             14 LOAD_FAST                2 (c)
             16 FORMAT_VALUE             2 (repr)
             18 LOAD_CONST               1 (' ')
             20 LOAD_FAST                3 (d)
             22 LOAD_CONST               2 ('4')
             24 FORMAT_VALUE             6 (repr, with format)
             26 BUILD_STRING             7
             28 RETURN_VALUE
""" % (_fstring.__code__.co_firstlineno + 1,)

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
%3d           0 SETUP_FINALLY           12 (to 14)

%3d           2 LOAD_FAST                0 (a)
              4 POP_BLOCK

%3d           6 LOAD_FAST_FOR_CALL       1 (b)
              8 CALL_FUNCTION            0
             10 POP_TOP

%3d          12 RETURN_VALUE

%3d     >>   14 LOAD_FAST_FOR_CALL       1 (b)
             16 CALL_FUNCTION            0
             18 POP_TOP
             20 RERAISE
             22 LOAD_CONST               0 (None)
             24 RETURN_VALUE
""" % (_tryfinally.__code__.co_firstlineno + 1,
       _tryfinally.__code__.co_firstlineno + 2,
       _tryfinally.__code__.co_firstlineno + 4,
       _tryfinally.__code__.co_firstlineno + 2,
       _tryfinally.__code__.co_firstlineno + 4,
       )

dis_tryfinallyconst = """\
%3d           0 SETUP_FINALLY           12 (to 14)

%3d           2 POP_BLOCK

%3d           4 LOAD_FAST_FOR_CALL       0 (b)
              6 CALL_FUNCTION            0
              8 POP_TOP

%3d          10 LOAD_CONST               1 (1)
             12 RETURN_VALUE

%3d     >>   14 LOAD_FAST_FOR_CALL       0 (b)
             16 CALL_FUNCTION            0
             18 POP_TOP
             20 RERAISE
             22 LOAD_CONST               0 (None)
             24 RETURN_VALUE
""" % (_tryfinallyconst.__code__.co_firstlineno + 1,
       _tryfinallyconst.__code__.co_firstlineno + 2,
       _tryfinallyconst.__code__.co_firstlineno + 4,
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
%3d           0 LOAD_CLOSURE             0 (y)
              2 BUILD_TUPLE              1
              4 LOAD_CONST               1 (<code object foo at 0x..., file "%s", line %d>)
              6 LOAD_CONST               2 ('_h.<locals>.foo')
              8 MAKE_FUNCTION            8 (closure)
             10 STORE_FAST               1 (foo)

%3d          12 LOAD_FAST                1 (foo)
             14 RETURN_VALUE
""" % (_h.__code__.co_firstlineno + 1,
       __file__,
       _h.__code__.co_firstlineno + 1,
       _h.__code__.co_firstlineno + 4,
)

dis_nested_1 = """%s
Disassembly of <code object foo at 0x..., file "%s", line %d>:
%3d           0 LOAD_CLOSURE             0 (x)
              2 BUILD_TUPLE              1
              4 LOAD_CONST               1 (<code object <listcomp> at 0x..., file "%s", line %d>)
              6 LOAD_CONST               2 ('_h.<locals>.foo.<locals>.<listcomp>')
              8 MAKE_FUNCTION            8 (closure)
             10 DEFER_REFCOUNT
             12 LOAD_DEREF               1 (y)
             14 GET_ITER
             16 CALL_FUNCTION            1
             18 RETURN_VALUE
""" % (dis_nested_0,
       __file__,
       _h.__code__.co_firstlineno + 1,
       _h.__code__.co_firstlineno + 3,
       __file__,
       _h.__code__.co_firstlineno + 3,
)

dis_nested_2 = """%s
Disassembly of <code object <listcomp> at 0x..., file "%s", line %d>:
%3d           0 BUILD_LIST               0
              2 LOAD_FAST                0 (.0)
        >>    4 FOR_ITER                12 (to 18)
              6 STORE_FAST               1 (z)
              8 LOAD_DEREF               0 (x)
             10 LOAD_FAST                1 (z)
             12 BINARY_ADD
             14 LIST_APPEND              2
             16 JUMP_ABSOLUTE            4
        >>   18 RETURN_VALUE
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

    def test_opmap(self):
        self.assertEqual(dis.opmap["NOP"], 9)
        self.assertIn(dis.opmap["LOAD_CONST"], dis.hasconst)
        self.assertIn(dis.opmap["STORE_NAME"], dis.hasname)

    def test_opname(self):
        self.assertEqual(dis.opname[dis.opmap["LOAD_FAST"]], "LOAD_FAST")

    def test_boundaries(self):
        self.assertEqual(dis.opmap["EXTENDED_ARG"], dis.EXTENDED_ARG)
        self.assertEqual(dis.opmap["STORE_NAME"], dis.HAVE_ARGUMENT)

    def test_widths(self):
        for opcode, opname in enumerate(dis.opname):
            if opname in ('BUILD_MAP_UNPACK_WITH_CALL',
                          'BUILD_TUPLE_UNPACK_WITH_CALL',
                          'JUMP_IF_NOT_EXC_MATCH'):
                continue
            with self.subTest(opname=opname):
                width = dis._OPNAME_WIDTH
                if opcode < dis.HAVE_ARGUMENT:
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
           %*d LOAD_FAST                0 (x)
           %*d LOAD_CONST               1 (1)
           %*d BINARY_ADD
           %*d STORE_FAST               0 (x)
''' % (w, 8*i, w, 8*i + 2, w, 8*i + 4, w, 8*i + 6)
                 for i in range(count)]
            s += ['''\

  3        %*d LOAD_FAST                0 (x)
           %*d RETURN_VALUE
''' % (w, 8*count, w, 8*count + 2)]
            s[0] = '  2' + s[0][3:]
            return ''.join(s)

        for i in range(1, 5):
            self.do_disassembly_test(func(i), expected(i, 4))
        self.do_disassembly_test(func(1249), expected(1249, 4))
        self.do_disassembly_test(func(1250), expected(1250, 5))

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
Stack size:        3
Flags:             OPTIMIZED, NEWLOCALS, NOFREE
Constants:
   0: %r
Names:
   0: _format_code_info
   1: _get_code_object
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
Number of locals:  10
Stack size:        9
Flags:             OPTIMIZED, NEWLOCALS, VARARGS, VARKEYWORDS, GENERATOR
Constants:
   0: None
   1: <code object f at (.*), file "(.*)", line (.*)>
   2: 'tricky.<locals>.f'
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
Cell variables:
   0: [abedfxyz]
   1: [abedfxyz]
   2: [abedfxyz]
   3: [abedfxyz]
   4: [abedfxyz]
   5: [abedfxyz]"""
# NOTE: the order of the cell variables above depends on dictionary order!

co_tricky_nested_f = tricky.__func__.__code__.co_consts[1]

code_info_tricky_nested_f = """\
Filename:          (.*)
Argument count:    1
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  1
Stack size:        10
Flags:             OPTIMIZED, NEWLOCALS, NESTED
Constants:
   0: None
Names:
   0: print
Variable names:
   0: c
Free variables:
   0: [abedfxyz]
   1: [abedfxyz]
   2: [abedfxyz]
   3: [abedfxyz]
   4: [abedfxyz]
   5: [abedfxyz]"""

code_info_expr_str = """\
Name:              <module>
Filename:          <disassembly>
Argument count:    0
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  0
Stack size:        2
Flags:             NOFREE
Constants:
   0: 1
Names:
   0: x"""

code_info_simple_stmt_str = """\
Name:              <module>
Filename:          <disassembly>
Argument count:    0
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  0
Stack size:        2
Flags:             NOFREE
Constants:
   0: 1
   1: None
Names:
   0: x"""

code_info_compound_stmt_str = """\
Name:              <module>
Filename:          <disassembly>
Argument count:    0
Positional-only arguments: 0
Kw-only arguments: 0
Number of locals:  0
Stack size:        2
Flags:             NOFREE
Constants:
   0: 0
   1: 1
   2: None
Names:
   0: x"""


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
Stack size:        10
Flags:             OPTIMIZED, NEWLOCALS, NOFREE, COROUTINE
Constants:
   0: None
   1: 1
Names:
   0: b
   1: c
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
  Instruction(opname='FUNC_HEADER', opcode=5, imm=[14], argval=14, argrepr='14', offset=0, starts_line=1, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[2], argval=3, argrepr='3', offset=2, starts_line=2, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[3], argval=3, argrepr='.t0', offset=4, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[3], argval=4, argrepr='4', offset=6, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[4], argval=4, argrepr='.t1', offset=8, starts_line=None, is_jump_target=False),
  Instruction(opname='MAKE_FUNCTION', opcode=101, imm=[4], argval=code_object_f, argrepr=repr(code_object_f), offset=10, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[4], argval=4, argrepr='.t1', offset=12, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[3], argval=3, argrepr='.t0', offset=14, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[2], argval=2, argrepr='f', offset=16, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[5, 0], argval=('print', 0), argrepr="'print'; 0", offset=18, starts_line=7, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[6], argval=6, argrepr='.t3', offset=21, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[0], argval=0, argrepr='a', offset=23, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[7], argval=7, argrepr='.t4', offset=25, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[1], argval=1, argrepr='b', offset=27, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[8], argval=8, argrepr='.t5', offset=29, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[6], argval='', argrepr="''", offset=31, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[9], argval=9, argrepr='.t6', offset=33, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[7], argval=1, argrepr='1', offset=35, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[10], argval=10, argrepr='.t7', offset=37, starts_line=None, is_jump_target=False),
  Instruction(opname='BUILD_LIST', opcode=88, imm=[11, 0], argval=(11, 0), argrepr='.t8; 0', offset=39, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[11], argval=11, argrepr='.t8', offset=42, starts_line=None, is_jump_target=False),
  Instruction(opname='BUILD_MAP', opcode=90, imm=[0], argval=0, argrepr='0', offset=44, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[12], argval=12, argrepr='.t9', offset=46, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[8], argval='Hello world!', argrepr="'Hello world!'", offset=48, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[13], argval=13, argrepr='.t10', offset=50, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[7, 7], argval=(7, 7), argrepr='.t4 to .t11', offset=52, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=56, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[2], argval=2, argrepr='f', offset=57, starts_line=8, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=70, imm=[], argval=None, argrepr='', offset=59, starts_line=None, is_jump_target=False),
]

expected_opinfo_f = [
  Instruction(opname='FUNC_HEADER', opcode=5, imm=[13], argval=13, argrepr='13', offset=0, starts_line=2, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[2], argval=5, argrepr='5', offset=2, starts_line=3, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[5], argval=5, argrepr='.t0', offset=4, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[3], argval=6, argrepr='6', offset=6, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[6], argval=6, argrepr='.t1', offset=8, starts_line=None, is_jump_target=False),
  Instruction(opname='MAKE_FUNCTION', opcode=101, imm=[4], argval=code_object_inner, argrepr=repr(code_object_inner), offset=10, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[6], argval=6, argrepr='.t1', offset=12, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[5], argval=5, argrepr='.t0', offset=14, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[2], argval=2, argrepr='inner', offset=16, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[5, 0], argval=('print', 0), argrepr="'print'; 0", offset=18, starts_line=5, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[8], argval=8, argrepr='.t3', offset=21, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[3], argval=3, argrepr='a', offset=23, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[9], argval=9, argrepr='.t4', offset=25, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[4], argval=4, argrepr='b', offset=27, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[10], argval=10, argrepr='.t5', offset=29, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[0], argval=0, argrepr='c', offset=31, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[11], argval=11, argrepr='.t6', offset=33, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[1], argval=1, argrepr='d', offset=35, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[12], argval=12, argrepr='.t7', offset=37, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[9, 4], argval=(9, 4), argrepr='.t4 to .t8', offset=39, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=43, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[2], argval=2, argrepr='inner', offset=44, starts_line=6, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=70, imm=[], argval=None, argrepr='', offset=46, starts_line=None, is_jump_target=False),
]

expected_opinfo_inner = [
  Instruction(opname='FUNC_HEADER', opcode=5, imm=[16], argval=16, argrepr='16', offset=0, starts_line=3, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[2, 0], argval=('print', 0), argrepr="'print'; 0", offset=2, starts_line=4, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[9], argval=9, argrepr='.t3', offset=5, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[2], argval=2, argrepr='a', offset=7, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[10], argval=10, argrepr='.t4', offset=9, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[3], argval=3, argrepr='b', offset=11, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[11], argval=11, argrepr='.t5', offset=13, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[4], argval=4, argrepr='c', offset=15, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[12], argval=12, argrepr='.t6', offset=17, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_DEREF', opcode=51, imm=[5], argval=5, argrepr='d', offset=19, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[13], argval=13, argrepr='.t7', offset=21, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[0], argval=0, argrepr='e', offset=23, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[14], argval=14, argrepr='.t8', offset=25, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[1], argval=1, argrepr='f', offset=27, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[15], argval=15, argrepr='.t9', offset=29, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[10, 6], argval=(10, 6), argrepr='.t4 to .t10', offset=31, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=35, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[0], argval=None, argrepr='None', offset=36, starts_line=None, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=70, imm=[], argval=None, argrepr='', offset=38, starts_line=None, is_jump_target=False),
]

expected_opinfo_jumpy = [
  Instruction(opname='FUNC_HEADER', opcode=5, imm=[9], argval=9, argrepr='9', offset=0, starts_line=1, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[2, 0], argval=('range', 0), argrepr="'range'; 0", offset=2, starts_line=3, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[5], argval=5, argrepr='.t3', offset=5, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[3], argval=10, argrepr='10', offset=7, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[6], argval=6, argrepr='.t4', offset=9, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=11, starts_line=None, is_jump_target=False),
  Instruction(opname='GET_ITER', opcode=80, imm=[2], argval=2, argrepr='.t0', offset=15, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[43], argval=60, argrepr='to 60', offset=17, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[0], argval=0, argrepr='i', offset=20, starts_line=None, is_jump_target=True),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[4, 1], argval=('print', 1), argrepr="'print'; 1", offset=22, starts_line=4, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[6], argval=6, argrepr='.t4', offset=25, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[0], argval=0, argrepr='i', offset=27, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[7], argval=7, argrepr='.t5', offset=29, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[7, 1], argval=(7, 1), argrepr='.t5 to .t6', offset=31, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=35, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[5], argval=4, argrepr='4', offset=36, starts_line=5, is_jump_target=False),
  Instruction(opname='COMPARE_OP', opcode=31, imm=[0, 0], argval=(0, 0), argrepr='0; i', offset=38, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=78, imm=[6], argval=47, argrepr='to 47', offset=41, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[16], argval=60, argrepr='to 60', offset=44, starts_line=6, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[6], argval=6, argrepr='6', offset=47, starts_line=7, is_jump_target=True),
  Instruction(opname='COMPARE_OP', opcode=31, imm=[4, 0], argval=(4, 0), argrepr='4; i', offset=49, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=78, imm=[8], argval=60, argrepr='to 60', offset=52, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[2], argval=2, argrepr='.t0', offset=55, starts_line=8, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[21], argval=78, argrepr='to 78', offset=57, starts_line=None, is_jump_target=False),
  Instruction(opname='FOR_ITER', opcode=82, imm=[2, -40], argval=(2, 20), argrepr='.t0; to 20', offset=60, starts_line=3, is_jump_target=True),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[4, 1], argval=('print', 1), argrepr="'print'; 1", offset=64, starts_line=10, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[5], argval=5, argrepr='.t3', offset=67, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[7], argval='I can haz else clause?', argrepr="'I can haz else clause?'", offset=69, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[6], argval=6, argrepr='.t4', offset=71, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=73, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=77, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[45], argval=123, argrepr='to 123', offset=78, starts_line=11, is_jump_target=True),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[4, 1], argval=('print', 1), argrepr="'print'; 1", offset=81, starts_line=12, is_jump_target=True),
  Instruction(opname='STORE_FAST', opcode=53, imm=[5], argval=5, argrepr='.t3', offset=84, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[0], argval=0, argrepr='i', offset=86, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[6], argval=6, argrepr='.t4', offset=88, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=90, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=94, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[8], argval=1, argrepr='1', offset=95, starts_line=13, is_jump_target=False),
  Instruction(opname='INPLACE_SUBTRACT', opcode=35, imm=[0], argval=0, argrepr='i', offset=97, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[0], argval=0, argrepr='i', offset=99, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[6], argval=6, argrepr='6', offset=101, starts_line=14, is_jump_target=False),
  Instruction(opname='COMPARE_OP', opcode=31, imm=[4, 0], argval=(4, 0), argrepr='4; i', offset=103, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=78, imm=[6], argval=112, argrepr='to 112', offset=106, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[14], argval=123, argrepr='to 123', offset=109, starts_line=15, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[5], argval=4, argrepr='4', offset=112, starts_line=16, is_jump_target=True),
  Instruction(opname='COMPARE_OP', opcode=31, imm=[0, 0], argval=(0, 0), argrepr='0; i', offset=114, starts_line=None, is_jump_target=False),
  Instruction(opname='POP_JUMP_IF_FALSE', opcode=78, imm=[6], argval=123, argrepr='to 123', offset=117, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[22], argval=142, argrepr='to 142', offset=120, starts_line=17, is_jump_target=False),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[0], argval=0, argrepr='i', offset=123, starts_line=11, is_jump_target=True),
  Instruction(opname='POP_JUMP_IF_TRUE', opcode=79, imm=[-44], argval=81, argrepr='to 81', offset=125, starts_line=17, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[4, 1], argval=('print', 1), argrepr="'print'; 1", offset=128, starts_line=19, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[5], argval=5, argrepr='.t3', offset=131, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[9], argval='Who let lolcatz into this test suite?', argrepr="'Who let lolcatz into this test suite?'", offset=133, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[6], argval=6, argrepr='.t4', offset=135, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[6, 1], argval=(6, 1), argrepr='.t4 to .t5', offset=137, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=141, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[8], argval=1, argrepr='1', offset=142, starts_line=21, is_jump_target=True),
  Instruction(opname='STORE_FAST', opcode=53, imm=[2], argval=2, argrepr='.t0', offset=144, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[10], argval=0, argrepr='0', offset=146, starts_line=None, is_jump_target=False),
  Instruction(opname='BINARY_TRUE_DIVIDE', opcode=23, imm=[2], argval=2, argrepr='.t0', offset=148, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_FAST', opcode=2, imm=[2], argval=2, argrepr='.t0', offset=150, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=152, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[31], argval=184, argrepr='to 184', offset=153, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[11, 2], argval=('ZeroDivisionError', 2), argrepr="'ZeroDivisionError'; 2", offset=156, starts_line=22, is_jump_target=False),
  Instruction(opname='JUMP_IF_NOT_EXC_MATCH', opcode=77, imm=[2, 23], argval=(2, 182), argrepr='.t0; to 182', offset=159, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[4, 1], argval=('print', 1), argrepr="'print'; 1", offset=163, starts_line=23, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[7], argval=7, argrepr='.t5', offset=166, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[12], argval='Here we go, here we go, here we go...', argrepr="'Here we go, here we go, here we go...'", offset=168, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[8], argval=8, argrepr='.t6', offset=170, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[8, 1], argval=(8, 1), argrepr='.t6 to .t7', offset=172, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=176, starts_line=None, is_jump_target=False),
  Instruction(opname='END_EXCEPT', opcode=91, imm=[2], argval=2, argrepr='.t0', offset=177, starts_line=None, is_jump_target=False),
  Instruction(opname='JUMP', opcode=74, imm=[27], argval=206, argrepr='to 206', offset=179, starts_line=None, is_jump_target=False),
  Instruction(opname='END_FINALLY', opcode=93, imm=[2], argval=2, argrepr='.t0', offset=182, starts_line=None, is_jump_target=True),
  Instruction(opname='LOAD_FAST', opcode=45, imm=[0], argval=0, argrepr='i', offset=184, starts_line=25, is_jump_target=True),
  Instruction(opname='SETUP_WITH', opcode=102, imm=[2], argval=2, argrepr='.t0', offset=186, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[1], argval=1, argrepr='dodgy', offset=188, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[4, 1], argval=('print', 1), argrepr="'print'; 1", offset=190, starts_line=26, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[7], argval=7, argrepr='.t5', offset=193, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[13], argval='Never reach this', argrepr="'Never reach this'", offset=195, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[8], argval=8, argrepr='.t6', offset=197, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[8, 1], argval=(8, 1), argrepr='.t6 to .t7', offset=199, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=203, starts_line=None, is_jump_target=False),
  Instruction(opname='END_WITH', opcode=103, imm=[2], argval=2, argrepr='.t0', offset=204, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_GLOBAL', opcode=49, imm=[4, 1], argval=('print', 1), argrepr="'print'; 1", offset=206, starts_line=28, is_jump_target=True),
  Instruction(opname='STORE_FAST', opcode=53, imm=[7], argval=7, argrepr='.t5', offset=209, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[14], argval="OK, now we're done", argrepr='"OK, now we\'re done"', offset=211, starts_line=None, is_jump_target=False),
  Instruction(opname='STORE_FAST', opcode=53, imm=[8], argval=8, argrepr='.t6', offset=213, starts_line=None, is_jump_target=False),
  Instruction(opname='CALL_FUNCTION', opcode=65, imm=[8, 1], argval=(8, 1), argrepr='.t6 to .t7', offset=215, starts_line=None, is_jump_target=False),
  Instruction(opname='CLEAR_ACC', opcode=1, imm=[], argval=None, argrepr='', offset=219, starts_line=None, is_jump_target=False),
  Instruction(opname='END_FINALLY', opcode=93, imm=[2], argval=2, argrepr='.t0', offset=220, starts_line=None, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[0], argval=None, argrepr='None', offset=222, starts_line=None, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=70, imm=[], argval=None, argrepr='', offset=224, starts_line=None, is_jump_target=False),
]

# One last piece of inspect fodder to check the default line number handling
def simple(): pass
expected_opinfo_simple = [
  Instruction(opname='FUNC_HEADER', opcode=5, imm=[0], argval=0, argrepr='0', offset=0, starts_line=simple.__code__.co_firstlineno, is_jump_target=False),
  Instruction(opname='LOAD_CONST', opcode=47, imm=[0], argval=None, argrepr='None', offset=2, starts_line=None, is_jump_target=False),
  Instruction(opname='RETURN_VALUE', opcode=70, imm=[], argval=None, argrepr='', offset=4, starts_line=None, is_jump_target=False),
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

if __name__ == "__main__":
    unittest.main()
