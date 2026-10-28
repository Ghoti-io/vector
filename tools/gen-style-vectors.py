#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
#
# Ghoti.io Vector is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Write the reference vectors of the style layer's used values, or with --check fail unless the committed ones are what this writes.

    gen-style-vectors.py [--check]

The expected values are computed here, not by the library, from the specifications' own constants in exact rational arithmetic
(Python's fractions) and, where a function is not rational, in 60 decimal digits (the decimal module), then rounded once to the nearest
double. Nothing is taken from the library's output. Each file is tab separated, the first column naming what is tested and the last the
expected values as the hex of the double (`float.hex()`):

    tests/data/svg-style-vectors/lengths.tsv      <unit> <value> <expected user units>             1in = 96 px, 1in = 2.54 cm = 25.4 mm = 101.6 Q = 72 pt = 6 pc
    tests/data/svg-style-vectors/percentages.tsv  <basis> <width> <height> <percent> <expected>     SVG 2, 8.9: the width, the height, sqrt((w^2 + h^2) / 2)
    tests/data/svg-style-vectors/colors.tsv       <css color> <space> <expected components...>    hsl()/hwb() of CSS Color 4 section 7, rec2020/prophoto of section 10
    tests/data/svg-style-vectors/rotations.tsv    <transform function> <expected a b c d e f>      the rotation and skew matrices
"""

import os
import sys
from decimal import Decimal, getcontext
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "data", "svg-style-vectors")
getcontext().prec = 80


def to_double(x):
    """The double nearest to an exact rational or decimal."""
    if isinstance(x, Decimal):
        x = Fraction(x)
    return float(x)


def hexd(x):
    return float(x).hex()


# ---- lengths -----------------------------------------------------------------------------------------

# How many of each unit are in one inch (CSS Values 4, 6.2).
PER_INCH = {"px": Fraction(96), "in": Fraction(1), "cm": Fraction(254, 100), "mm": Fraction(254, 10), "Q": Fraction(1016, 10),
            "pt": Fraction(72), "pc": Fraction(6)}
VALUES = ["0", "1", "0.5", "2", "10", "100", "2.54", "25.4", "0.001", "123.456", "1e-6", "1e6", "37.5", "0.1", "3.14159265358979"]


def lengths():
    rows = []
    for unit, per in PER_INCH.items():
        for v in VALUES:
            exact = Fraction(Decimal(v)) * 96 / per if unit != "px" else Fraction(Decimal(v))
            rows.append("%s\t%s\t%s" % (unit, v, hexd(to_double(exact))))
    return rows


# ---- percentages -------------------------------------------------------------------------------------

def sqrt_dec(x):
    return x.sqrt()


def percentages():
    rows = []
    sizes = [(200, 100), (300, 150), (20, 10), (1, 1), (1234.5, 678.25), (100, 100)]
    for w, h in sizes:
        for p in ["0", "1", "10", "12.5", "33.333", "50", "100", "150", "0.1"]:
            fw, fh = Fraction(Decimal(str(w))), Fraction(Decimal(str(h)))
            fp = Fraction(Decimal(p))
            rows.append("width\t%s\t%s\t%s\t%s" % (w, h, p, hexd(to_double(fp * fw / 100))))
            rows.append("height\t%s\t%s\t%s\t%s" % (w, h, p, hexd(to_double(fp * fh / 100))))
            diagonal = sqrt_dec(Decimal(fw * fw + fh * fh) / 2) if False else sqrt_dec((Decimal(fw.numerator) / Decimal(fw.denominator)) ** 2 / 2 + (Decimal(fh.numerator) / Decimal(fh.denominator)) ** 2 / 2)
            rows.append("diagonal\t%s\t%s\t%s\t%s" % (w, h, p, hexd(to_double(Decimal(fp.numerator) / Decimal(fp.denominator) * diagonal / 100))))
    return rows


# ---- colors ------------------------------------------------------------------------------------------

def hsl_to_rgb(h, s, l):
    """CSS Color 4 section 7.1, in exact rational arithmetic (the hue modulus is exact)."""
    h = Fraction(h) % 360
    s, l = Fraction(s) / 100, Fraction(l) / 100

    def f(n):
        k = (n + h / 30) % 12
        a = s * min(l, 1 - l)
        return l - a * max(-1, min(k - 3, 9 - k, 1))

    return [f(0), f(8), f(4)]


def hwb_to_rgb(h, w, b):
    w, b = Fraction(w) / 100, Fraction(b) / 100
    if w + b >= 1:
        g = w / (w + b)
        return [g, g, g]
    rgb = hsl_to_rgb(h, 100, 50)
    return [c * (1 - w - b) + w for c in rgb]


def dec_pow(x, y):
    """x ** y for positive Decimals."""
    if x == 0:
        return Decimal(0)
    return (x.ln() * y).exp()


def rec2020_to_linear(v):
    alpha, beta = Decimal("1.09929682680944"), Decimal("0.018053968510807")
    v = Decimal(v)
    a = abs(v)
    sign = -1 if v < 0 else 1
    if a < beta * Decimal("4.5"):
        return v / Decimal("4.5")
    return sign * dec_pow((a + alpha - 1) / alpha, Decimal(1) / Decimal("0.45"))


def colors():
    rows = []
    for h, s, l in [(120, 100, 25), (210, 50, "33.3"), (0, 100, 50), (-120, 100, 50), (360, 100, 50), (45.5, 80, 60), (300, 20, 90), (30, 0, 50), (725, 100, 50)]:
        rgb = hsl_to_rgb(Fraction(Decimal(str(h))), Fraction(Decimal(str(s))), Fraction(Decimal(str(l))))
        rows.append("hsl(%s %s%% %s%%)\tsrgb\t%s" % (h, s, l, "\t".join(hexd(to_double(c)) for c in rgb)))
    for h, w, b in [(0, 0, 0), (30, 10, 20), (200, 60, 60), (120, 0, 50), (330, "12.5", "37.5")]:
        rgb = hwb_to_rgb(Fraction(Decimal(str(h))), Fraction(Decimal(str(w))), Fraction(Decimal(str(b))))
        rows.append("hwb(%s %s%% %s%%)\tsrgb\t%s" % (h, w, b, "\t".join(hexd(to_double(c)) for c in rgb)))
    for v in ["0", "0.01", "0.05", "0.081", "0.1", "0.25", "0.5", "0.75", "1", "-0.25"]:
        lin = rec2020_to_linear(v)
        rows.append("color(rec2020 %s %s %s)\trec2020\t%s" % (v, v, v, "\t".join(hexd(to_double(lin)) for _ in range(3))))
    return rows


# ---- rotations ---------------------------------------------------------------------------------------

def pi():
    # Machin: pi = 16 atan(1/5) - 4 atan(1/239)
    def atan_inv(n):
        n = Decimal(n)
        x = 1 / n
        total, term, k = x, x, 1
        while abs(term) > Decimal(10) ** -(getcontext().prec + 2):
            term = -term / (n * n)
            k += 2
            total += term / k
        return total

    return 16 * atan_inv(5) - 4 * atan_inv(239)


def sincos(x):
    """sin and cos of a Decimal, by the Taylor series."""
    term_s, term_c = x, Decimal(1)
    s, c = x, Decimal(1)
    k = 1
    while abs(term_s) > Decimal(10) ** -(getcontext().prec + 2):
        term_c = -term_c * x * x / ((2 * k - 1) * (2 * k))
        term_s = -term_s * x * x / ((2 * k) * (2 * k + 1))
        c += term_c
        s += term_s
        k += 1
    return s, c


def rotations():
    rows = []
    P = pi()
    for deg in ["0", "30", "45", "60", "-30", "90", "120", "180", "270", "359.5", "0.001", "725"]:
        d = Decimal(deg)
        # Reduce to the nearest quarter turns exactly, as the shape family's rotation does (the quarter turns are exact).
        q = (d / 90).to_integral_value()
        r = d - 90 * q
        s, c = sincos(r * P / 180)
        qm = int(q) % 4
        # (cos, sin) after qm quarter turns: rotate (c, s) by qm * 90 degrees
        for _ in range(qm):
            c, s = -s, c
        a, b, cc, dd = c, s, -s, c
        rows.append("rotate(%s)\t%s" % (deg, "\t".join(hexd(to_double(x)) + "" for x in (a, b, cc, dd, 0, 0))))
    for deg in ["0", "30", "45", "-20", "10"]:
        t = Decimal(deg)
        s, c = sincos(t * P / 180)
        tan = s / c
        rows.append("skewX(%s)\t%s" % (deg, "\t".join(hexd(to_double(x)) for x in (1, 0, tan, 1, 0, 0))))
        rows.append("skewY(%s)\t%s" % (deg, "\t".join(hexd(to_double(x)) for x in (1, tan, 0, 1, 0, 0))))
    return rows


FILES = {
    "lengths.tsv": ("# <unit> <value> <expected user units, hex double>: CSS Values 4, 6.2 (1in = 96px = 2.54cm = 25.4mm = 101.6Q = 72pt = 6pc), exactly rational.\n", lengths),
    "percentages.tsv": ("# <basis> <width> <height> <percent> <expected, hex double>: SVG 2, 8.9 (the width, the height, sqrt((w^2 + h^2) / 2) of the viewport).\n", percentages),
    "colors.tsv": ("# <css color> <space> <expected components, hex doubles>: CSS Color 4 sections 7 and 10, rational (hsl, hwb) and 80 digit decimal (rec2020).\n", colors),
    "rotations.tsv": ("# <transform function> <a b c d e f, hex doubles>: the rotation (quarter turns exact) and skew matrices, 80 digit decimal.\n", rotations),
}


def main(argv):
    check = "--check" in argv
    bad = False
    os.makedirs(OUT, exist_ok=True)
    for name, (header, make) in FILES.items():
        text = header + "\n".join(make()) + "\n"
        path = os.path.join(OUT, name)
        if check:
            have = open(path).read() if os.path.exists(path) else None
            if have != text:
                sys.stderr.write("gen-style-vectors: %s is not what the generator writes\n" % path)
                bad = True
        else:
            with open(path, "w") as h:
                h.write(text)
    if check and not bad:
        print("gen-style-vectors: the committed vectors are what the generator writes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
