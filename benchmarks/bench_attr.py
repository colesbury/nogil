import time
import math as _math
from dataclasses import dataclass


UNROLL = 100
N = 100000

@dataclass
class Foo:
    name: str
    value: int

    @property
    def prop(self):
        return 3

global_f = Foo("name", 3)

def load_attr():
    f = global_f
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value
    f.value

def load_prop():
    f = global_f
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop
    f.prop

def update_attr():
    f = global_f
    f.value = 0
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1
    f.value += 1


def run():
    for _ in range(3):
        start = time.perf_counter()
        for _ in range(N):
            load_attr()
        end = time.perf_counter()
        print('load_attr', (end - start) * 1e9/(UNROLL * N), 'ns')

        start = time.perf_counter()
        for _ in range(N):
            load_prop()
        end = time.perf_counter()
        print('load_prop', (end - start) * 1e9/(UNROLL * N), 'ns')

        start = time.perf_counter()
        for _ in range(N):
            update_attr()
        end = time.perf_counter()
        print('update_attr', (end - start) * 1e9/(UNROLL * N), 'ns')

run()

# import dis
# dis.dis(load_attr)
