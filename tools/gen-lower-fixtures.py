#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""Write the fixtures of tests/data/svg-lower that are many of one shape.

    gen-lower-fixtures.py [--check]

The nineteen forms of preserveAspectRatio (the nine alignments with `meet` and
with `slice`, and `none`), each mapping a 20 by 10 viewBox onto a 64 by 40
viewport with three markers at known corners, so that the golden raster shows
where the box landed. The other fixtures are written by hand. With --check
nothing is written; the exit status is 1 when a file differs.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "tests", "data", "svg-lower")
ALIGNS = ["none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"]

TEMPLATE = """<?xml version="1.0"?>
<!-- preserveAspectRatio="%(par)s" maps a 20 by 10 viewBox onto a 64 by 40 viewport (P3, SVG 2 section 8.2); written by tools/gen-lower-fixtures.py. -->
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40" viewBox="2 3 20 10" preserveAspectRatio="%(par)s">
  <path d="M2 3H22V13H2Z" fill="#ecf0f1"/>
  <path d="M2 3H8V9H2Z" fill="#c0392b"/>
  <path d="M16 7H22V13H16Z" fill="#2980b9"/>
  <path d="M10 5H14V11H10Z" fill="#27ae60"/>
</svg>
"""


def files():
    for align in ALIGNS:
        for mode in ([""] if align == "none" else ["meet", "slice"]):
            par = (align + " " + mode).strip()
            name = "par-" + align.lower() + ("-" + mode if mode else "") + ".svg"
            yield name, TEMPLATE % {"par": par}


def main():
    stale = False
    for name, text in files():
        path = os.path.join(OUT, name)
        try:
            current = open(path, encoding="utf-8").read()
        except FileNotFoundError:
            current = None
        if "--check" in sys.argv:
            if current != text:
                print("stale: tests/data/svg-lower/" + name)
                stale = True
        elif current != text:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
    sys.exit(1 if stale else 0)


main()
