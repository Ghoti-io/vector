#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""Write the exact reference vectors for the lowering's viewBox map.

    gen-lower-vectors.py [--check]

SVG 2, 8.2 defines the map from a viewBox to its viewport. With the box
(vx, vy, vw, vh), the viewport size (w, h) and the alignment (ax, ay) in
{0, 1/2, 1} that preserveAspectRatio names, the scale is s = w / vw (or the
smaller or larger of the two ratios for `meet` and `slice`) and the translation
is  ax * w - s * (vx + ax * vw)  (and the same in y). This computes them in exact
rational arithmetic from the doubles that are written, rounds each once to the
nearest double (Fraction -> float is correctly rounded), and writes the doubles
as hexadecimal floats. The library never sees these numbers, so a test that
reads them is an independent reference for the library's arithmetic.

The scale is the correctly rounded quotient, and the translation is computed from
that rounded scale (the rounding of the scale is the only one outside the final
rounding of each entry).
"""
import os
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "tests", "data", "svg-lower-vectors", "viewbox.tsv")

ALIGNS = ["none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"]
FACTOR = {"Min": Fraction(0), "Mid": Fraction(1, 2), "Max": Fraction(1)}

# (vx, vy, vw, vh, w, h): the numbers are the decimal spellings, read as doubles.
BOXES = [
    ("0", "0", "10", "10", "100", "50"),
    ("0", "0", "10", "10", "50", "100"),
    ("10", "20", "50", "50", "200", "100"),
    ("-5", "-5", "30", "10", "90", "90"),
    ("0", "0", "3", "7", "1", "1"),
    ("0.1", "0.2", "0.3", "0.7", "10", "3"),
    ("1e-3", "-2e-3", "3.3333", "1.1", "7.7", "9.9"),
    ("123456.789", "-98765.4321", "1000.5", "250.25", "640", "480"),
    ("0", "0", "1", "1", "1", "1"),
    ("0", "0", "16", "16", "300", "150"),
    ("7", "11", "13", "17", "19", "23"),
    ("-1e5", "1e5", "3e5", "2e5", "1e3", "1e3"),
]


def awkward():
    """A deterministic set of boxes of many digits, whose translations cancel and so are sensitive to how the sum is rounded."""
    import random
    rng = random.Random(7)
    out = []
    for _ in range(40):
        def num(lo, hi, digits):
            return repr(round(rng.uniform(lo, hi), digits))
        out.append((num(-500, 500, rng.choice([3, 6, 9])), num(-500, 500, rng.choice([3, 6, 9])), num(0.01, 900, rng.choice([3, 6, 9])),
                    num(0.01, 900, rng.choice([3, 6, 9])), num(0.5, 2000, rng.choice([2, 5, 8])), num(0.5, 2000, rng.choice([2, 5, 8]))))
    return out


def rn(x):
    return float(x)


def vector(box, align, slice_):
    vx, vy, vw, vh, w, h = (float(v) for v in box)
    vxf, vyf, vwf, vhf, wf, hf = (Fraction(v) for v in (vx, vy, vw, vh, w, h))
    sx = rn(wf / vwf)
    sy = rn(hf / vhf)
    if align == "none":
        a, d = sx, sy
        tx = -rn(vxf * Fraction(sx))
        ty = -rn(vyf * Fraction(sy))
    else:
        s = (max if slice_ else min)(sx, sy)
        a = d = s
        ax = FACTOR[align[1:4]]
        ay = FACTOR[align[5:8]]
        sf = Fraction(s)
        tx = rn(ax * wf - sf * (vxf + ax * vwf))
        ty = rn(ay * hf - sf * (vyf + ay * vhf))
    return a, d, tx + 0.0, ty + 0.0


def rows():
    yield "# viewBox vectors: vx vy vw vh w h align slice -> a d tx ty (hexadecimal floats); written by tools/gen-lower-vectors.py"
    for box in BOXES + awkward():
        for align in ALIGNS:
            for slice_ in (0, 1) if align != "none" else (0,):
                a, d, tx, ty = vector(box, align, slice_)
                yield "\t".join(list(box) + [align, str(slice_)] + [x.hex() for x in (a, d, tx, ty)])


def main():
    text = "\n".join(rows()) + "\n"
    if "--check" in sys.argv:
        try:
            current = open(OUT, encoding="utf-8").read()
        except FileNotFoundError:
            current = None
        if current != text:
            print("stale: tests/data/svg-lower-vectors/viewbox.tsv")
            sys.exit(1)
        return
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)


main()
