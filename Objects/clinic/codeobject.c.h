/*[clinic input]
preserve
[clinic start generated code]*/

PyDoc_STRVAR(code_new__doc__,
"code(argcount=0, posonlyargcount=0, kwonlyargcount=0, nlocals=0,\n"
"     framesize=0, ndefaultargs=0, nmeta=0, flags=0, code=None,\n"
"     constants=(), varnames=(), filename=None, name=None,\n"
"     firstlineno=0, linetable=None, eh_table=(), jump_table=(),\n"
"     freevars=(), cellvars=(), free2reg=(), cell2reg=())\n"
"--\n"
"\n"
"Create a code object.  Not for the faint of heart.");

static PyObject *
code_new_impl(PyTypeObject *type, int argcount, int posonlyargcount,
              int kwonlyargcount, int nlocals, int framesize,
              int ndefaultargs, int nmeta, int flags, PyObject *code,
              PyObject *consts, PyObject *varnames, PyObject *filename,
              PyObject *name, int firstlineno, PyObject *linetable,
              PyObject *eh_table, PyObject *jump_table, PyObject *freevars,
              PyObject *cellvars, PyObject *free2reg, PyObject *cell2reg);

static PyObject *
code_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"argcount", "posonlyargcount", "kwonlyargcount", "nlocals", "framesize", "ndefaultargs", "nmeta", "flags", "code", "constants", "varnames", "filename", "name", "firstlineno", "linetable", "eh_table", "jump_table", "freevars", "cellvars", "free2reg", "cell2reg", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "code", 0};
    PyObject *argsbuf[21];
    PyObject * const *fastargs;
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t noptargs = nargs + (kwargs ? PyDict_GET_SIZE(kwargs) : 0) - 0;
    int argcount = 0;
    int posonlyargcount = 0;
    int kwonlyargcount = 0;
    int nlocals = 0;
    int framesize = 0;
    int ndefaultargs = 0;
    int nmeta = 0;
    int flags = 0;
    PyObject *code = Py_None;
    PyObject *consts = NULL;
    PyObject *varnames = NULL;
    PyObject *filename = Py_None;
    PyObject *name = Py_None;
    int firstlineno = 0;
    PyObject *linetable = Py_None;
    PyObject *eh_table = NULL;
    PyObject *jump_table = NULL;
    PyObject *freevars = NULL;
    PyObject *cellvars = NULL;
    PyObject *free2reg = NULL;
    PyObject *cell2reg = NULL;

    fastargs = _PyArg_UnpackKeywords(_PyTuple_CAST(args)->ob_item, nargs, kwargs, NULL, &_parser, 0, 21, 0, argsbuf);
    if (!fastargs) {
        goto exit;
    }
    if (!noptargs) {
        goto skip_optional_pos;
    }
    if (fastargs[0]) {
        if (PyFloat_Check(fastargs[0])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        argcount = _PyLong_AsInt(fastargs[0]);
        if (argcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[1]) {
        if (PyFloat_Check(fastargs[1])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        posonlyargcount = _PyLong_AsInt(fastargs[1]);
        if (posonlyargcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[2]) {
        if (PyFloat_Check(fastargs[2])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        kwonlyargcount = _PyLong_AsInt(fastargs[2]);
        if (kwonlyargcount == -1 && PyErr_Occurred()) {
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
        nlocals = _PyLong_AsInt(fastargs[3]);
        if (nlocals == -1 && PyErr_Occurred()) {
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
        framesize = _PyLong_AsInt(fastargs[4]);
        if (framesize == -1 && PyErr_Occurred()) {
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
        nmeta = _PyLong_AsInt(fastargs[6]);
        if (nmeta == -1 && PyErr_Occurred()) {
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
        if (!PyBytes_Check(fastargs[8])) {
            _PyArg_BadArgument("code", "argument 'code'", "bytes", fastargs[8]);
            goto exit;
        }
        code = fastargs[8];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[9]) {
        if (!PyTuple_Check(fastargs[9])) {
            _PyArg_BadArgument("code", "argument 'constants'", "tuple", fastargs[9]);
            goto exit;
        }
        consts = fastargs[9];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[10]) {
        if (!PyTuple_Check(fastargs[10])) {
            _PyArg_BadArgument("code", "argument 'varnames'", "tuple", fastargs[10]);
            goto exit;
        }
        varnames = fastargs[10];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[11]) {
        if (!PyUnicode_Check(fastargs[11])) {
            _PyArg_BadArgument("code", "argument 'filename'", "str", fastargs[11]);
            goto exit;
        }
        if (PyUnicode_READY(fastargs[11]) == -1) {
            goto exit;
        }
        filename = fastargs[11];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[12]) {
        if (!PyUnicode_Check(fastargs[12])) {
            _PyArg_BadArgument("code", "argument 'name'", "str", fastargs[12]);
            goto exit;
        }
        if (PyUnicode_READY(fastargs[12]) == -1) {
            goto exit;
        }
        name = fastargs[12];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[13]) {
        if (PyFloat_Check(fastargs[13])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        firstlineno = _PyLong_AsInt(fastargs[13]);
        if (firstlineno == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[14]) {
        if (!PyBytes_Check(fastargs[14])) {
            _PyArg_BadArgument("code", "argument 'linetable'", "bytes", fastargs[14]);
            goto exit;
        }
        linetable = fastargs[14];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[15]) {
        if (!PyTuple_Check(fastargs[15])) {
            _PyArg_BadArgument("code", "argument 'eh_table'", "tuple", fastargs[15]);
            goto exit;
        }
        eh_table = fastargs[15];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[16]) {
        if (!PyTuple_Check(fastargs[16])) {
            _PyArg_BadArgument("code", "argument 'jump_table'", "tuple", fastargs[16]);
            goto exit;
        }
        jump_table = fastargs[16];
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
            _PyArg_BadArgument("code", "argument 'free2reg'", "tuple", fastargs[19]);
            goto exit;
        }
        free2reg = fastargs[19];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (!PyTuple_Check(fastargs[20])) {
        _PyArg_BadArgument("code", "argument 'cell2reg'", "tuple", fastargs[20]);
        goto exit;
    }
    cell2reg = fastargs[20];
skip_optional_pos:
    return_value = code_new_impl(type, argcount, posonlyargcount, kwonlyargcount, nlocals, framesize, ndefaultargs, nmeta, flags, code, consts, varnames, filename, name, firstlineno, linetable, eh_table, jump_table, freevars, cellvars, free2reg, cell2reg);

exit:
    return return_value;
}

PyDoc_STRVAR(code_replace__doc__,
"replace($self, /, *, co_argcount=-1, co_posonlyargcount=-1,\n"
"        co_kwonlyargcount=-1, co_ndefaultargs=-1, co_nlocals=-1,\n"
"        co_framesize=-1, co_nmeta=-1, co_flags=-1, co_firstlineno=-1,\n"
"        co_code=None, co_consts=None, co_varnames=None,\n"
"        co_freevars=None, co_cellvars=None, co_filename=None,\n"
"        co_name=None, co_lnotab=None)\n"
"--\n"
"\n"
"Return a copy of the code object with new values for the specified fields.");

#define CODE_REPLACE_METHODDEF    \
    {"replace", (PyCFunction)(void(*)(void))code_replace, METH_FASTCALL|METH_KEYWORDS, code_replace__doc__},

static PyObject *
code_replace_impl(PyCodeObject *self, int co_argcount,
                  int co_posonlyargcount, int co_kwonlyargcount,
                  int co_ndefaultargs, int co_nlocals, int co_framesize,
                  int co_nmeta, int co_flags, int co_firstlineno,
                  PyObject *co_code, PyObject *co_consts,
                  PyObject *co_varnames, PyObject *co_freevars,
                  PyObject *co_cellvars, PyObject *co_filename,
                  PyObject *co_name, PyObject *co_lnotab);

static PyObject *
code_replace(PyCodeObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_ndefaultargs", "co_nlocals", "co_framesize", "co_nmeta", "co_flags", "co_firstlineno", "co_code", "co_consts", "co_varnames", "co_freevars", "co_cellvars", "co_filename", "co_name", "co_lnotab", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "replace", 0};
    PyObject *argsbuf[17];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 0;
    int co_argcount = self->co_argcount;
    int co_posonlyargcount = self->co_posonlyargcount;
    int co_kwonlyargcount = self->co_kwonlyargcount;
    int co_ndefaultargs = self->co_ndefaultargs;
    int co_nlocals = self->co_nlocals;
    int co_framesize = self->co_framesize;
    int co_nmeta = self->co_nmeta;
    int co_flags = self->co_flags;
    int co_firstlineno = self->co_firstlineno;
    PyObject *co_code = NULL;
    PyObject *co_consts = NULL;
    PyObject *co_varnames = self->co_varnames;
    PyObject *co_freevars = self->co_freevars;
    PyObject *co_cellvars = self->co_cellvars;
    PyObject *co_filename = self->co_filename;
    PyObject *co_name = self->co_name;
    PyObject *co_lnotab = self->co_lnotab;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser, 0, 0, 0, argsbuf);
    if (!args) {
        goto exit;
    }
    if (!noptargs) {
        goto skip_optional_kwonly;
    }
    if (args[0]) {
        if (PyFloat_Check(args[0])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_argcount = _PyLong_AsInt(args[0]);
        if (co_argcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[1]) {
        if (PyFloat_Check(args[1])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_posonlyargcount = _PyLong_AsInt(args[1]);
        if (co_posonlyargcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[2]) {
        if (PyFloat_Check(args[2])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_kwonlyargcount = _PyLong_AsInt(args[2]);
        if (co_kwonlyargcount == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[3]) {
        if (PyFloat_Check(args[3])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_ndefaultargs = _PyLong_AsInt(args[3]);
        if (co_ndefaultargs == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[4]) {
        if (PyFloat_Check(args[4])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_nlocals = _PyLong_AsInt(args[4]);
        if (co_nlocals == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[5]) {
        if (PyFloat_Check(args[5])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_framesize = _PyLong_AsInt(args[5]);
        if (co_framesize == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[6]) {
        if (PyFloat_Check(args[6])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_nmeta = _PyLong_AsInt(args[6]);
        if (co_nmeta == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[7]) {
        if (PyFloat_Check(args[7])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_flags = _PyLong_AsInt(args[7]);
        if (co_flags == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[8]) {
        if (PyFloat_Check(args[8])) {
            PyErr_SetString(PyExc_TypeError,
                            "integer argument expected, got float" );
            goto exit;
        }
        co_firstlineno = _PyLong_AsInt(args[8]);
        if (co_firstlineno == -1 && PyErr_Occurred()) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[9]) {
        if (!PyBytes_Check(args[9])) {
            _PyArg_BadArgument("replace", "argument 'co_code'", "bytes", args[9]);
            goto exit;
        }
        co_code = args[9];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[10]) {
        if (!PyTuple_Check(args[10])) {
            _PyArg_BadArgument("replace", "argument 'co_consts'", "tuple", args[10]);
            goto exit;
        }
        co_consts = args[10];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[11]) {
        if (!PyTuple_Check(args[11])) {
            _PyArg_BadArgument("replace", "argument 'co_varnames'", "tuple", args[11]);
            goto exit;
        }
        co_varnames = args[11];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[12]) {
        if (!PyTuple_Check(args[12])) {
            _PyArg_BadArgument("replace", "argument 'co_freevars'", "tuple", args[12]);
            goto exit;
        }
        co_freevars = args[12];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[13]) {
        if (!PyTuple_Check(args[13])) {
            _PyArg_BadArgument("replace", "argument 'co_cellvars'", "tuple", args[13]);
            goto exit;
        }
        co_cellvars = args[13];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[14]) {
        if (!PyUnicode_Check(args[14])) {
            _PyArg_BadArgument("replace", "argument 'co_filename'", "str", args[14]);
            goto exit;
        }
        if (PyUnicode_READY(args[14]) == -1) {
            goto exit;
        }
        co_filename = args[14];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[15]) {
        if (!PyUnicode_Check(args[15])) {
            _PyArg_BadArgument("replace", "argument 'co_name'", "str", args[15]);
            goto exit;
        }
        if (PyUnicode_READY(args[15]) == -1) {
            goto exit;
        }
        co_name = args[15];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (!PyBytes_Check(args[16])) {
        _PyArg_BadArgument("replace", "argument 'co_lnotab'", "bytes", args[16]);
        goto exit;
    }
    co_lnotab = args[16];
skip_optional_kwonly:
    return_value = code_replace_impl(self, co_argcount, co_posonlyargcount, co_kwonlyargcount, co_ndefaultargs, co_nlocals, co_framesize, co_nmeta, co_flags, co_firstlineno, co_code, co_consts, co_varnames, co_freevars, co_cellvars, co_filename, co_name, co_lnotab);

exit:
    return return_value;
}
/*[clinic end generated code: output=1a40c19ff7c61e8b input=a9049054013a1b77]*/
