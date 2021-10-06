#!/usr/bin/env python3
#
# Based on Larry Hastings's Gilectomy test program

import sys
import time
from concurrent.futures import ThreadPoolExecutor

print(f"nogil={getattr(sys.flags, 'nogil', False)}")

def fib(n):
    if n < 2: return 1
    return fib(n-1) + fib(n-2)

threads = 8
if len(sys.argv) > 1:
    threads = int(sys.argv[1])

with ThreadPoolExecutor(max_workers=threads) as executor:
    start = time.time()
    for _ in range(threads):
        executor.submit(lambda: print(fib(34)))
    executor.shutdown()
    print(f'time: {time.time() - start} s')
