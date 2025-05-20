# Python Multithreading without GIL

Copyright (c) 2001-2022 Python Software Foundation.  All rights reserved.

See [Doc/license.rst](/Doc/license.rst) for copyright and license information.

## Overview

This was a proof-of-concept implementation of CPython that supports multithreading without the global interpreter lock (GIL). An overview of the  design is described in the [Python Multithreading without GIL](https://docs.google.com/document/d/18CXhDb1ygxg-YXNBJNzfzZsDFosB5e6BfnXLlejd9l0/edit) Google doc.

Now that ["PEP 703 – Making the Global Interpreter Lock Optional in CPython"](https://peps.python.org/pep-0703/) has been accepted, this project is not being actively worked on. Development is focused on making Python 3.13 and later releases support running without the global interpreter lock in a configuration called "free-threading".

Please use the "free-threaded" build of Python 3.13. For installation instructions, see https://py-free-threading.github.io/installing-cpython/.
