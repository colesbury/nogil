/*[clinic input]
preserve
[clinic start generated code]*/

PyDoc_STRVAR(code_new__doc__,
"code(bytecode, constants, argcount=0, posonlyargcount=0,\n"
"     kwonlyargcount=0, ndefaultargs=0, nlocals=0, framesize=0, nmeta=0,\n"
"     flags=0, names=(), varnames=(), filename=None, name=None,\n"
"     firstlineno=0, linetable=None, eh_table=(), freevars=(),\n"
"     cellvars=(), cell2reg=(), free2reg=(), iconstants=())\n"
"--\n"
"\n"
"Create a code object.  Not for the faint of heart.");

static PyObject *
code_new_impl(PyTypeObject *type, PyObject *bytecode, PyObject *consts,
              int argcount, int posonlyargcount, int kwonlyargcount,
              int ndefaultargs, int nlocals, int framesize, int nmeta,
              int flags, PyObject *names, PyObject *varnames,
              PyObject *filename, PyObject *name, int firstlineno,
              PyObject *linetable, PyObject *eh_table, PyObject *freevars,
              PyObject *cellvars, PyObject *cell2reg, PyObject *free2reg,
              PyObject *iconstants);

static PyObject *
code_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"bytecode", "constants", "argcount", "posonlyargcount", "kwonlyargcount", "ndefaultargs", "nlocals", "framesize", "nmeta", "flags", "names", "varnames", "filename", "name", "firstlineno", "linetable", "eh_table", "freevars", "cellvars", "cell2reg", "free2reg", "iconstants", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "code", 0};
    PyObject *argsbuf[22];
    PyObject * const *fastargs;
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t noptargs = nargs + (kwargs ? PyDict_GET_SIZE(kwargs) : 0) - 2;
    PyObject *bytecode;
    PyObject *consts;
    int argcount = 0;
    int posonlyargcount = 0;
    int kwonlyargcount = 0;
    int ndefaultargs = 0;
    int nlocals = 0;
    int framesize = 0;
    int nmeta = 0;
    int flags = 0;
    PyObject *names = NULL;
    PyObject *varnames = NULL;
    PyObject *filename = Py_None;
    PyObject *name = Py_None;
    int firstlineno = 0;
    PyObject *linetable = Py_None;
    PyObject *eh_table = NULL;
    PyObject *freevars = NULL;
    PyObject *cellvars = NULL;
    PyObject *cell2reg = NULL;
    PyObject *free2reg = NULL;
    PyObject *iconstants = NULL;

    fastargs = _PyArg_UnpackKeywords(_PyTuple_CAST(args)->ob_item, nargs, kwargs, NULL, &_parser, 2, 22, 0, argsbuf);
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
        ndefaultargs = _PyLong_AsInt(fastargs[5]);
        if (ndefaultargs == -1 && PyErr_Occurred()) {
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
        nlocals = _PyLong_AsInt(fastargs[6]);
        if (nlocals == -1 && PyErr_Occurred()) {
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
        framesize = _PyLong_AsInt(fastargs[7]);
        if (framesize == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[8]) {
        if (PyFloat_Check(fastargs[8])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        nmeta = _PyLong_AsInt(fastargs[8]);
        if (nmeta == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[9]) {
        if (PyFloat_Check(fastargs[9])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        flags = _PyLong_AsInt(fastargs[9]);
        if (flags == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[10]) {
        if (!PyTuple_Check(fastargs[10])) {
            _PyArg_BadArgument("code", "argument 'names'", "tuple", fastargs[10]);
            goto exit;
        }
        names = fastargs[10];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[11]) {
        if (!PyTuple_Check(fastargs[11])) {
            _PyArg_BadArgument("code", "argument 'varnames'", "tuple", fastargs[11]);
            goto exit;
        }
        varnames = fastargs[11];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[12]) {
        if (!PyUnicode_Check(fastargs[12])) {
            _PyArg_BadArgument("code", "argument 'filename'", "str", fastargs[12]);
            goto exit;
        }
        if (PyUnicode_READY(fastargs[12]) == -1) {
            goto exit;
        }
        filename = fastargs[12];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[13]) {
        if (!PyUnicode_Check(fastargs[13])) {
            _PyArg_BadArgument("code", "argument 'name'", "str", fastargs[13]);
            goto exit;
        }
        if (PyUnicode_READY(fastargs[13]) == -1) {
            goto exit;
        }
        name = fastargs[13];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[14]) {
        if (PyFloat_Check(fastargs[14])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        firstlineno = _PyLong_AsInt(fastargs[14]);
        if (firstlineno == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[15]) {
        if (!PyBytes_Check(fastargs[15])) {
            _PyArg_BadArgument("code", "argument 'linetable'", "bytes", fastargs[15]);
            goto exit;
        }
        linetable = fastargs[15];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[16]) {
        if (!PyTuple_Check(fastargs[16])) {
            _PyArg_BadArgument("code", "argument 'eh_table'", "tuple", fastargs[16]);
            goto exit;
        }
        eh_table = fastargs[16];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[17]) {
        if (!PyTuple_Check(fastargs[17])) {
            _PyArg_BadArgument("code", "argument 'freevars'", "tuple", fastargs[17]);
            goto exit;
        }
        freevars = fastargs[17];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[18]) {
        if (!PyTuple_Check(fastargs[18])) {
            _PyArg_BadArgument("code", "argument 'cellvars'", "tuple", fastargs[18]);
            goto exit;
        }
        cellvars = fastargs[18];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[19]) {
        if (!PyTuple_Check(fastargs[19])) {
            _PyArg_BadArgument("code", "argument 'cell2reg'", "tuple", fastargs[19]);
            goto exit;
        }
        cell2reg = fastargs[19];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[20]) {
        if (!PyTuple_Check(fastargs[20])) {
            _PyArg_BadArgument("code", "argument 'free2reg'", "tuple", fastargs[20]);
            goto exit;
        }
        free2reg = fastargs[20];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (!PyTuple_Check(fastargs[21])) {
        _PyArg_BadArgument("code", "argument 'iconstants'", "tuple", fastargs[21]);
        goto exit;
    }
    iconstants = fastargs[21];
skip_optional_pos:
    return_value = code_new_impl(type, bytecode, consts, argcount, posonlyargcount, kwonlyargcount, ndefaultargs, nlocals, framesize, nmeta, flags, names, varnames, filename, name, firstlineno, linetable, eh_table, freevars, cellvars, cell2reg, free2reg, iconstants);

exit:
    return return_value;
}
/*[clinic end generated code: output=1de1a83d9c7cb354 input=a9049054013a1b77]*/
