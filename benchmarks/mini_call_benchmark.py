import time

N = 1000000
loop_delta = 0
UNROLL = 10

def call4(a, b, c, d):
    pass

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

benchmark_call4_vararg_stararg()