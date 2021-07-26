import time

UNROLL = 100
N = 1000000

# old = 779 ns


x = object()
a0 = object()
a1 = object()
a2 = object()
a3 = object()
a4 = object()
a5 = object()
a6 = object()
a7 = object()

def foo():
    global x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x
    x

def bar():
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range
    range

def run():
    for _ in range(3):
        start = time.perf_counter()
        for _ in range(N):
            foo()
        end = time.perf_counter()
        print('global', (end - start) * 1e9/(UNROLL * N), 'ns')

        start = time.perf_counter()
        for _ in range(N):
            bar()
        end = time.perf_counter()
        print('builtin', (end - start) * 1e9/(UNROLL * N), 'ns')

run()
