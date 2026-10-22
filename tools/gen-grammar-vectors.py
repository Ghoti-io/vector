#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""Write tests/data/svg-grammar/numbers.tsv: decimal numbers and the doubles they round to.

    gen-grammar-vectors.py [--check]

Each line is `<text><TAB><16 hex digits of the double's bits | INVALID>`. The reference is Python's
float(), which rounds correctly (it is David Gay's algorithm in CPython); a magnitude beyond the
largest double is INVALID because the SVG number grammar of this library refuses an overflow. The
generator is seeded, so the file is the same every time; --check fails when the committed file is
not what this writes.
"""
import os
import random
import struct
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "tests", "data", "svg-grammar", "numbers.tsv")


def bits(x):
    return "%016x" % struct.unpack(">Q", struct.pack(">d", x))[0]


def cases():
    fixed = [
        "0", "-0", "+0", "1", "-1", "+1", ".5", "-.5", "5.", "0.5", "00012", "1e0", "1E3", "1e+3", "1e-3",
        "1.5e3", "-1.5E-3", ".1e1", "5.e1", "123456789012345678901234567890", "0.000000000000000000001",
        # halfway cases and the neighbours of 2^53
        "9007199254740992", "9007199254740993", "9007199254740994", "9007199254740995", "9007199254740993.00000000001",
        "9007199254740992.99999999999", "0.1", "0.2", "0.3", "0.30000000000000004", "2.2250738585072014e-308",
        "2.2250738585072011e-308", "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324",
        "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1.797693134862315807e308",
        "1e308", "1e309", "1e-323", "1e-324", "1e-325", "1e400", "1e-400", "0e999999", "1e999999999999",
        "1e-999999999999", "123456789e-400", "5e-324", "3e-324", "2e-324",
    ]
    out = []
    for text in fixed:
        out.append(text)
    rng = random.Random(0x5167)
    for _ in range(3000):
        digits = "".join(rng.choice("0123456789") for _ in range(rng.randrange(1, 25)))
        point = rng.randrange(0, len(digits) + 1)
        mantissa = digits[:point] + "." + digits[point:] if rng.random() < 0.7 else digits
        exp = "" if rng.random() < 0.4 else "e%d" % rng.randrange(-330, 320)
        sign = rng.choice(["", "", "-", "+"])
        out.append(sign + mantissa + exp)
    return out


def lines():
    for text in cases():
        try:
            value = float(text)
        except ValueError:
            yield "%s\tINVALID" % text
            continue
        if value in (float("inf"), float("-inf")):
            yield "%s\tINVALID" % text
        else:
            # `float` accepts a few spellings the grammar does not ("5." and ".5" are fine; "1e" is not generated).
            yield "%s\t%s" % (text, bits(value))


def main(argv):
    text = "\n".join(lines()) + "\n"
    if "--check" in argv:
        with open(OUT, encoding="utf-8") as handle:
            if handle.read() != text:
                sys.stderr.write("numbers.tsv is not what gen-grammar-vectors.py writes\n")
                return 1
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
