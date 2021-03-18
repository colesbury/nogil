#!/usr/bin/env python3
#
# x.py by Larry Hastings
#
# Official test program of the Gilectomy.
#
# Runs a bad fib generator to 30,
# with some number of threads, default 7.
 


def fib(n):
    if n < 2: return 1
    return fib(n-1) + fib(n-2)
 
print(fib(34))