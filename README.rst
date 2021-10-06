Python Multithreading without GIL
====================================

Copyright (c) 2001-2020 Python Software Foundation.  All rights reserved.

See `Doc/license.rst </Doc/license.rst>`_ for copyright and license information.

Overview
-------------------

This is a proof-of-concept implementation of CPython that supports multithreading without the global interpreter lock (GIL). An overview of the  design is described in the `Python Multithreading without GIL <https://docs.google.com/document/d/18CXhDb1ygxg-YXNBJNzfzZsDFosB5e6BfnXLlejd9l0/edit>`__ Google doc.


Installation
-------------------

The proof-of-concept supports Linux on x86-64 and ARM64. I plan to fix the Windows and macOS builds soon.

The build process is has not changed from upstream CPython. See https://devguide.python.org/ for instructions on how to build from source, or follow the steps below.

Install::

    ./configure [--prefix=PREFIX] [--enable-optimizations]
    make -j
    make install
    
The optional ``--prefix=PREFIX`` specifies the destination directory for the Python installation. The optional ``--enable-optimizations`` enables profile guided optimizations (PGO). This slows down the build process, but makes the compiled Python a bit faster.


Usage
-------------------

To reduce the risk of issues with Python-based tools, **the GIL is enabled by default**. To run with the GIL disabled, use the environment variable PYTHONGIL=0. You can check if the GIL is enabled from Python by accessing ``sys.flags.nogil``::

    PYTHONGIL=0 python3 -c "import sys; print(sys.flags.nogil)"

Packages
-------------------

Use ``pip install <package>`` as usual to install packages. Please file an issue if you are unable to install a pip package you would like to use.

The proof-of-concept comes with a modified bundled "pip" that includes an `alternative package index <https://d1yxz45j0ypngg.cloudfront.net/>`_. The alternative package index includes C extensions that are either slow to build from source or require some modifications for compatibility.

