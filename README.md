# Python Multithreading without GIL

Copyright (c) 2001-2022 Python Software Foundation.  All rights reserved.

See [Doc/license.rst](/Doc/license.rst) for copyright and license information.

## Overview

This is a proof-of-concept implementation of CPython that supports multithreading without the global interpreter lock (GIL). An overview of the  design is described in the [Python Multithreading without GIL](https://docs.google.com/document/d/18CXhDb1ygxg-YXNBJNzfzZsDFosB5e6BfnXLlejd9l0/edit) Google doc.


## Installation

The proof-of-concept works best on Linux x86-64. It also builds on macOS, Linux ARM64, and Windows (64-bit), but you will have to recompile extension modules yourself for these platforms.

- [Install with pyenv](#install-with-pyenv)
- [Run with Docker](#docker)
- [Build from source](#build-from-source)

## Install with pyenv

First you need to install pyenv [on Linux](https://github.com/colesbury/nogil/wiki/Install-nogil-with-pyenv-on-Linux) or [on macOS](https://github.com/colesbury/nogil/wiki/Install-nogil-with-pyenv-on-macOS) . To install the "nogil-3.9.10-1" version:

    pyenv install nogil-3.9.10-1

You can make the nogil installation the default Python installation with:

    pyenv global nogil-3.9.10-1

## Docker

A pre-built Docker image [nogil/python](https://hub.docker.com/r/nogil/python) is available on Docker Hub. For CUDA support, use [nogil/python-cuda](https://hub.docker.com/r/nogil/python-cuda).  
For arm64 devices, use [nogil/python-arm64](https://hub.docker.com/r/nogil/python-arm64).

For example:

    docker run -it nogil/python

## Build from source

The build process has not changed from upstream CPython. See https://devguide.python.org/ for instructions on how to build from source, or follow the steps below.

Install:

    ./configure [--prefix=PREFIX] [--enable-optimizations]
    make -j
    make install
    
The optional ``--prefix=PREFIX`` specifies the destination directory for the Python installation. The optional ``--enable-optimizations`` enables profile guided optimizations (PGO). This slows down the build process, but makes the compiled Python a bit faster.

## Packages

Use ``pip install <package>`` as usual to install packages. Please file an issue if you are unable to install a pip package you would like to use.

The proof-of-concept comes with a modified bundled "pip" that includes an [alternative package index](https://d1yxz45j0ypngg.cloudfront.net/). The alternative package index includes C extensions that are either slow to build from source or require some modifications for compatibility.


## GIL control

The GIL is disabled by default, but if you wish, you can enable it at runtime using the environment variable ``PYTHONGIL=1``. You can check if the GIL is disabled from Python by accessing ``sys.flags.nogil``:

    python3 -c "import sys; print(sys.flags.nogil)"  # True
    PYTHONGIL=1 python3 -c "import sys; print(sys.flags.nogil)"  # False

## Example

You can use the existing Python APIs, such as the [threading](https://docs.python.org/3/library/threading.html) module and the  [ThreadPoolExecutor](https://docs.python.org/3/library/concurrent.futures.html#concurrent.futures.ThreadPoolExecutor) class.

Here is an example based on Larry Hastings's [Gilectomy benchmark](https://github.com/larryhastings/gilectomy/blob/gilectomy/x.py):

```python
import sys
from concurrent.futures import ThreadPoolExecutor

print(f"nogil={getattr(sys.flags, 'nogil', False)}")

def fib(n):
    if n < 2: return 1
    return fib(n-1) + fib(n-2)

threads = 8
if len(sys.argv) > 1:
    threads = int(sys.argv[1])

with ThreadPoolExecutor(max_workers=threads) as executor:
    for _ in range(threads):
        executor.submit(lambda: print(fib(34)))
```

Run it with, e.g.:

    time python3 fib.py 1   # 1 thread, 1x work
    time python3 fib.py 20  # 20 threads, 20x work

The program parallelizes well up to the number of available cores. On a 20 core Intel Xeon E5-2698 v4  one thread takes 1.50 seconds and 20 threads take 1.52 seconds [1].

[1] Turbo boost was [disabled](https://askubuntu.com/questions/619875/disabling-intel-turbo-boost-in-ubuntu) to measure the scaling of the program without the effects of CPU frequency scaling. Additionally, you may get more reliable measurements by using [taskset](https://man7.org/linux/man-pages/man1/taskset.1.html) to avoid virtual "hyperthreading" cores.
