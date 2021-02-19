import time

# N = 1000
N = 1000000
UNROLL = 10
loop_delta = 2.0 * N * UNROLL / 1e9

def call0():
    pass

def call2(a, b):
    pass

def call4(a, b, c, d):
    pass

def call4_dflt(a, b, c=3, d=4):
    pass

def call_vararg(*args, **kwargs):
    pass

def benchmark_loop():
    # global loop_delta
    start = time.perf_counter()
    for _ in range(N):
        pass
    loop_delta2 = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'loop overhead (unaccounted for) {loop_delta2 / (N * UNROLL):.1f} ns')

def benchmark_call0():
    start = time.perf_counter()
    f = call0
    for _ in range(N):
        f()
        f()
        f()
        f()
        f()
        f()
        f()
        f()
        f()
        f()
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call0 {delta / (N * UNROLL):.1f} ns')

def benchmark_call2():
    start = time.perf_counter()
    a, b = 0, 1
    f = call2
    for _ in range(N):
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call2 {delta / (N * UNROLL):.1f} ns')

def benchmark_call4():
    start = time.perf_counter()
    f = call4
    a, b, c, d = 0, 1, 2, 3
    for _ in range(N):
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
        f(a, b, c, d)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call4 {delta / (N * UNROLL):.1f} ns')

def benchmark_call4_dflt():
    start = time.perf_counter()
    f = call4_dflt
    a, b = 0, 1
    for _ in range(N):
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
        f(a, b)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call4_dflt {delta / (N * UNROLL):.1f} ns')

def benchmark_call4_kwd():
    start = time.perf_counter()
    f = call4
    a, b, c, d = 0, 1, 2, 3
    for _ in range(N):
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call4_kwd {delta / (N * UNROLL):.1f} ns')

def benchmark_call4_kwd_mismatch():
    start = time.perf_counter()
    f = call4
    a, b, c, d = 0, 1, 2, 3
    for _ in range(N):
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
        f(a, b, d=d, c=c)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call4_kwd_mismatch {delta / (N * UNROLL):.1f} ns')

def benchmark_call4_vararg_stararg():
    start = time.perf_counter()
    f = call4
    args = (1, 2)
    kwargs = {"c": 3, "d": 4}
    for _ in range(N):
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call4_vararg_stararg {delta / (N * UNROLL):.1f} ns')

def benchmark_call_vararg_stararg():
    start = time.perf_counter()
    f = call_vararg
    args = ()
    kwargs = {}
    for _ in range(N):
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call_vararg_stararg {delta / (N * UNROLL):.1f} ns')

def benchmark_call_vararg4_stararg():
    start = time.perf_counter()
    f = call_vararg
    args = (1, 2)
    kwargs = {"c": 3, "d": 4}
    for _ in range(N):
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
        f(*args, **kwargs)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call_vararg4_stararg {delta / (N * UNROLL):.1f} ns')

def benchmark_call_vararg4_kwd():
    start = time.perf_counter()
    f = call_vararg
    a, b, c, d = 0, 1, 2, 3
    for _ in range(N):
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
        f(a, b, c=c, d=d)
    delta = (time.perf_counter() - start - loop_delta) * 1e9
    print(f'call_vararg4_kwd {delta / (N * UNROLL):.1f} ns')

def benchmark():
    benchmark_loop()
    for _ in range(3):
        benchmark_call0()
    for _ in range(3):
        benchmark_call2()
    for _ in range(3):
        benchmark_call4()
    for _ in range(3):
        benchmark_call4_dflt()
    for _ in range(3):
        benchmark_call4_kwd()
    for _ in range(3):
        benchmark_call4_kwd_mismatch()
    for _ in range(3):
        benchmark_call4_vararg_stararg()
    for _ in range(3):
        benchmark_call_vararg_stararg()
    for _ in range(3):
        benchmark_call_vararg4_stararg()
    for _ in range(3):
        benchmark_call_vararg4_kwd()

benchmark()