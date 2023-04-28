#! /usr/bin/env python3

import re

def main(filename):
    with open(filename, "r") as f:
        lines = f.readlines()

    counter = 1
    for i, line in enumerate(lines):
        if line.startswith("def_op("):
            lines[i] = re.sub(r", \d+", f", {counter}", line)
            counter += 1

    with open(filename, "w") as f:
        f.write("".join(lines))

if __name__ == '__main__':
    import sys
    main(sys.argv[1])
