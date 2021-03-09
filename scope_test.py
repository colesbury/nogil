class Outer:
    def func(self):
        class Inner1:
            def run(self):
                assert __class__ == Inner1
            assert __class__ == Outer

        class Inner2:
            def run(self):
                assert __class__ == Inner2
            assert __class__ == Outer

        Inner1().run()
        Inner2().run()

Outer().func()

def outer1():
    x = 9
    class Outer:
        assert x == 9
        def func(self):
            assert x == 9

    Outer().func()

outer1()

def outer2():
    x = 9
    class Outer:
        x = 3
        assert x == 3
        def func(self):
            assert x == 9

    Outer().func()
    assert x == 9

outer2()

def a():
    a = 9
    print('a:', locals())
    def b():
        def c():
            print('c:', locals())
            return a
        print('b:', locals())
        c()
    b()
a()
