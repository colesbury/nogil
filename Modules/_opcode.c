#include "Python.h"
#include "opcode.h"

/*[clinic input]
module _opcode
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=117442e66eb376e6]*/

#include "clinic/_opcode.c.h"

/*[clinic input]

_opcode.stack_effect -> int

  opcode: int
  oparg: object = None
  /
  *
  jump: object = None

Compute the stack effect of the opcode.
[clinic start generated code]*/

static int
_opcode_stack_effect_impl(PyObject *module, int opcode, PyObject *oparg,
                          PyObject *jump)
/*[clinic end generated code: output=64a18f2ead954dbb input=461c9d4a44851898]*/
{
    return 0;
}




static PyMethodDef
opcode_functions[] =  {
    _OPCODE_STACK_EFFECT_METHODDEF
    {NULL, NULL, 0, NULL}
};


static struct PyModuleDef opcodemodule = {
    PyModuleDef_HEAD_INIT,
    "_opcode",
    "Opcode support module.",
    -1,
    opcode_functions,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit__opcode(void)
{
    return PyModule_Create(&opcodemodule);
}
