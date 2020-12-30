/*[clinic input]
preserve
[clinic start generated code]*/

PyDoc_STRVAR(code_new__doc__,
"code(bytecode, constants, argcount=0, posonlyargcount=0,\n"
"     kwonlyargcount=0, nlocals=0, framesize=0, flags=0, names=(),\n"
"     varnames=(), filename=None, name=None, firstlineno=0,\n"
"     linetable=None, freevars=(), cellvars=(), cell2reg=(), free2reg=())\n"
"--\n"
"\n"
"Create a code object.  Not for the faint of heart.");

static PyObject *
code_new_impl(PyTypeObject *type, PyObject *bytecode, PyObject *consts,
              int argcount, int posonlyargcount, int kwonlyargcount,
              int nlocals, int framesize, int flags, PyObject *names,
              PyObject *varnames, PyObject *filename, PyObject *name,
              int firstlineno, PyObject *linetable, PyObject *freevars,
              PyObject *cellvars, PyObject *cell2reg, PyObject *free2reg);

static PyObject *
code_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"bytecode", "constants", "argcount", "posonlyargcount", "kwonlyargcount", "nlocals", "framesize", "flags", "names", "varnames", "filename", "name", "firstlineno", "linetable", "freevars", "cellvars", "cell2reg", "free2reg", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "code", 0};
    PyObject *argsbuf[18];
    PyObject * const *fastargs;
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t noptargs = nargs + (kwargs ? PyDict_GET_SIZE(kwargs) : 0) - 2;
    PyObject *bytecode;
    PyObject *consts;
    int argcount = 0;
    int posonlyargcount = 0;
    int kwonlyargcount = 0;
    int nlocals = 0;
    int framesize = 0;
    int flags = 0;
    PyObject *names = NULL;
    PyObject *varnames = NULL;
    PyObject *filename = Py_None;
    PyObject *name = Py_None;
    int firstlineno = 0;
    PyObject *linetable = Py_None;
    PyObject *freevars = NULL;
    PyObject *cellvars = NULL;
    PyObject *cell2reg = NULL;
    PyObject *free2reg = NULL;

    fastargs = _PyArg_UnpackKeywords(_PyTuple_CAST(args)->ob_item, nargs, kwargs, NULL, &_parser, 2, 18, 0, argsbuf);
    if (!fastargs) {
        goto exit;
    }
    if (!PyBytes_Check(fastargs[0])) {
        _PyArg_BadArgument("code", "argument 'bytecode'", "bytes", fastargs[0]);
        goto exit;
    }
    bytecode = fastargs[0];
    if (!PyTuple_Check(fastargs[1])) {
        _PyArg_BadArgument("code", "argument 'constants'", "tuple", fastargs[1]);
        goto exit;
    }
    consts = fastargs[1];
    if (!noptargs) {
        goto skip_optional_pos;
    }
    if (fastargs[2]) {
        if (PyFloat_Check(fastargs[2])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        argcount = _PyLong_AsInt(fastargs[2]);
        if (argcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[3]) {
        if (PyFloat_Check(fastargs[3])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        posonlyargcount = _PyLong_AsInt(fastargs[3]);
        if (posonlyargcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[4]) {
        if (PyFloat_Check(fastargs[4])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        kwonlyargcount = _PyLong_AsInt(fastargs[4]);
        if (kwonlyargcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[5]) {
        if (PyFloat_Check(fastargs[5])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        nlocals = _PyLong_AsInt(fastargs[5]);
        if (nlocals == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[6]) {
        if (PyFloat_Check(fastargs[6])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        framesize = _PyLong_AsInt(fastargs[6]);
        if (framesize == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[7]) {
        if (PyFloat_Check(fastargs[7])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        flags = _PyLong_AsInt(fastargs[7]);
        if (flags == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[8]) {
        if (!PyTuple_Check(fastargs[8])) {
            _PyArg_BadArgument("code", "argument 'names'", "tuple", fastargs[8]);
            goto exit;
        }
        names = fastargs[8];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[9]) {
        if (!PyTuple_Check(fastargs[9])) {
            _PyArg_BadArgument("code", "argument 'varnames'", "tuple", fastargs[9]);
            goto exit;
        }
        varnames = fastargs[9];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[10]) {
        if (!PyUnicode_Check(fastargs[10])) {
            _PyArg_BadArgument("code", "argument 'filename'", "str", fastargs[10]);
            goto exit;
        }
        if (PyUnicode_READY(fastargs[10]) == -1) {
            goto exit;
        }
        filename = fastargs[10];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[11]) {
        if (!PyUnicode_Check(fastargs[11])) {
            _PyArg_BadArgument("code", "argument 'name'", "str", fastargs[11]);
            goto exit;
        }
        if (PyUnicode_READY(fastargs[11]) == -1) {
            goto exit;
        }
        name = fastargs[11];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[12]) {
        if (PyFloat_Check(fastargs[12])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        firstlineno = _PyLong_AsInt(fastargs[12]);
        if (firstlineno == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[13]) {
        if (!PyBytes_Check(fastargs[13])) {
            _PyArg_BadArgument("code", "argument 'linetable'", "bytes", fastargs[13]);
            goto exit;
        }
        linetable = fastargs[13];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[14]) {
        if (!PyTuple_Check(fastargs[14])) {
            _PyArg_BadArgument("code", "argument 'freevars'", "tuple", fastargs[14]);
            goto exit;
        }
        freevars = fastargs[14];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[15]) {
        if (!PyTuple_Check(fastargs[15])) {
            _PyArg_BadArgument("code", "argument 'cellvars'", "tuple", fastargs[15]);
            goto exit;
        }
        cellvars = fastargs[15];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[16]) {
        if (!PyTuple_Check(fastargs[16])) {
            _PyArg_BadArgument("code", "argument 'cell2reg'", "tuple", fastargs[16]);
            goto exit;
        }
        cell2reg = fastargs[16];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (!PyTuple_Check(fastargs[17])) {
        _PyArg_BadArgument("code", "argument 'free2reg'", "tuple", fastargs[17]);
        goto exit;
    }
    free2reg = fastargs[17];
skip_optional_pos:
    return_value = code_new_impl(type, bytecode, consts, argcount, posonlyargcount, kwonlyargcount, nlocals, framesize, flags, names, varnames, filename, name, firstlineno, linetable, freevars, cellvars, cell2reg, free2reg);

exit:
    return return_value;
}
/*[clinic end generated code: output=84c682e2d23cf1b2 input=a9049054013a1b77]*/
