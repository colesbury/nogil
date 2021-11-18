#! /usr/bin/env python
"""Generate C code for the jump table of the threaded code interpreter
(for compilers supporting computed gotos or "labels-as-values", such as gcc).
"""

import os
import sys


try:
    from importlib.machinery import SourceFileLoader
except ImportError:
    import imp

    def find_module(modname):
        """Finds and returns a module in the local dist/checkout.
        """
        modpath = os.path.join(
            os.path.dirname(os.path.dirname(__file__)), "Lib")
        return imp.load_module(modname, *imp.find_module(modname, [modpath]))
else:
    def find_module(modname):
        """Finds and returns a module in the local dist/checkout.
        """
        modpath = os.path.join(
            os.path.dirname(os.path.dirname(__file__)), "Lib", modname + ".py")
        return SourceFileLoader(modname, modpath).load_module()


def write_targets(opcode, f):
    """Write C code contents to the target file object.
    """
    targets = ['&&_unknown_opcode'] * 256
    targets[0] = 'NULL'
    targets[255] = '&&debug_regs'
    for opname, bytecode in opcode.opmap.items():
        targets[bytecode.opcode] = f'&&{opname}'
    for opname, bytecode in opcode.opmap.items():
        if bytecode.has_wide:
            targets[bytecode.opcode + 128] = f"&&WIDE_{opname}"
    f.write("static void *opcode_targets_base[256] = {\n")
    f.write(",\n".join(["    %s" % s for s in targets]))
    f.write("\n};\n")


def write_switch(opcode, f):
    """Write C code contents to the target file object.
    """
    f.write("switch (opcode) {\n")
    targets = ['_unknown_opcode'] * 256
    targets[255] = 'debug_regs'
    for opname, bytecode in opcode.opmap.items():
        f.write(f"  case {opname}: goto {opname};\n");
    for opname, bytecode in opcode.opmap.items():
        if bytecode.has_wide:
            f.write(f"  case 128+{opname}: goto WIDE_{opname};\n");

    f.write(f"  default: goto _unknown_opcode;\n");
    f.write("};\n")


def write_names(opcode, f):
    """Write C code contents to the target file object.
    """
    names = ['unknown_opcode'] * 256
    names[255] = 'debug_regs'
    for opname, bytecode in opcode.opmap.items():
        names[bytecode.opcode] = opname
    f.write("static const char *opcode_names[256] = {\n")
    f.write(",\n".join(['    "%s"' % s for s in names]))
    f.write("\n};\n")


def write_intrinsics(opcode, f):
    """Write C code contents to the target file object.
    """
    names = ['unknown_opcode'] * 256
    names[255] = 'debug_regs'
    f.write("union intrinsic intrinsics_table[] = {\n")
    for i, intrinsic in enumerate(opcode.intrinsics):
        if i != 0:
            f.write(',\n')
        if intrinsic is None:
            f.write('    { &vm_unimplemented }')
        else:
            attr = 'intrinsic1' if intrinsic.nargs == 1 else 'intrinsicN'
            f.write(f'    {{ .{attr} = &{intrinsic.name} }}')
    f.write("\n};\n")


def main():
    if len(sys.argv) >= 3:
        sys.exit("Too many arguments")
    opcode = find_module('opcode')
    with open("Python/opcode_targets.h", "w") as f:
        write_targets(opcode, f)
        print("Jump table written into Python/opcode_targets.h")
    with open("Python/opcode_dispatch.h", "w") as f:
        write_switch(opcode, f)
        print("Switch table written into Python/opcode_dispatch.h")
    with open("Python/opcode_names.h", "w") as f:
        write_names(opcode, f)
        print("Name table written into Python/opcode_names.h")
    with open("Python/ceval_intrinsics.h", "w") as f:
        write_intrinsics(opcode, f)
        print("Intrinsic table written into Python/ceval_intrinsics.h")


if __name__ == "__main__":
    main()
