#!/usr/bin/env python3
"""
Reference vectors for the arcs the lowering hands to the geometry library (story 8, X5).

Each row is an SVG elliptical arc in endpoint form and the centre form SVG 2 (appendix B.2.4, implementation notes) gives for it,
computed here in 80-digit decimal arithmetic with a Taylor series for the sine and the cosine, sharing no code with the library:

    x0 y0 rx ry phi large sweep x y | cx cy ux uy vx vy

`phi` is in degrees; `u = rx (cos phi, sin phi)` and `v = ry (-sin phi, cos phi)` are the half-diameters after the radius correction.
Rows whose endpoints are equal or whose radius is zero draw no arc and are not in the table (the lowering test covers them).

    gen-arc-vectors.py            write tests/data/arc-vectors.txt
    gen-arc-vectors.py --check    fail if the committed table differs from what this computes
"""
import sys
from decimal import Decimal as D, getcontext

getcontext().prec = 80
PI = D("3.14159265358979323846264338327950288419716939937510582097494459230781640628620899")


def sincos(deg):
    x = D(deg) * PI / 180
    # reduce to [-pi, pi]
    two = 2 * PI
    x = x - two * (x / two).to_integral_value()
    s, c, term = D(0), D(0), D(1)
    n = 0
    sign_s = D(1)
    # c = sum (-1)^k x^(2k)/(2k)!, s = sum (-1)^k x^(2k+1)/(2k+1)!
    c_term, s_term = D(1), x
    c, s = c_term, s_term
    for k in range(1, 60):
        c_term = -c_term * x * x / ((2 * k - 1) * (2 * k))
        s_term = -s_term * x * x / ((2 * k) * (2 * k + 1))
        c += c_term
        s += s_term
    return s, c


def center(x0, y0, rx, ry, phi, large, sweep, x, y):
    x0, y0, rx, ry, x, y = (D(v) for v in (x0, y0, rx, ry, x, y))
    rx, ry = abs(rx), abs(ry)
    s, c = sincos(phi)
    dx, dy = (x0 - x) / 2, (y0 - y) / 2
    x1p = c * dx + s * dy
    y1p = -s * dx + c * dy
    lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
    if lam > 1:
        k = lam.sqrt()
        rx, ry = rx * k, ry * k
    num = (rx * ry) ** 2 - (rx * y1p) ** 2 - (ry * x1p) ** 2
    den = (rx * y1p) ** 2 + (ry * x1p) ** 2
    q = max(D(0), num / den).sqrt()
    if bool(large) == bool(sweep):
        q = -q
    cxp = q * rx * y1p / ry
    cyp = -q * ry * x1p / rx
    cx = c * cxp - s * cyp + (x0 + x) / 2
    cy = s * cxp + c * cyp + (y0 + y) / 2
    return cx, cy, rx * c, rx * s, -ry * s, ry * c


# (x0 y0 rx ry phi large sweep x y)
ROWS = []
for large in (0, 1):
    for sweep in (0, 1):
        ROWS.append((10, 10, 10, 6, 0, large, sweep, 24, 18))          # the four flag combinations
for phi in (0, 90, 45.5, -30, 180, 270):
    ROWS.append((3, 4, 9, 5, phi, 0, 1, 14, 7))                          # rotations
    ROWS.append((3, 4, 9, 5, phi, 1, 0, 14, 7))
ROWS += [
    (0, 0, 1, 1, 0, 0, 1, 10, 0),      # radii far too small: scaled up to the half ellipse
    (0, 0, 2, 3, 30, 1, 1, 20, 5),     # too small with a rotation and a large arc
    (5, 5, -4, -4, 0, 0, 1, 11, 5),    # negative radii are their absolute values
    (5, 5, -4, 4, 0, 1, 0, 11, 5),
    (0, 0, 5, 5, 0, 0, 1, 10, 0),      # exactly the half circle: the flags do not matter to the centre
    (0, 0, 5, 5, 0, 1, 1, 10, 0),
    (100.5, 40.25, 33.3, 12.1, 17, 1, 1, 80.75, 62.5),
]


def lines():
    out = ["# Reference arc vectors: x0 y0 rx ry phi large sweep x y | cx cy ux uy vx vy (tools/lower/gen-arc-vectors.py)"]
    for r in ROWS:
        cx, cy, ux, uy, vx, vy = center(*r)
        a = " ".join(repr(float(v)) if not isinstance(v, int) else str(v) for v in r)
        b = " ".join(repr(float(v)) for v in (cx, cy, ux, uy, vx, vy))
        out.append(a + " | " + b)
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    path = "tests/data/arc-vectors.txt"
    text = lines()
    if "--check" in sys.argv:
        if open(path).read() != text:
            sys.exit(path + " is stale: run tools/lower/gen-arc-vectors.py")
    else:
        open(path, "w").write(text)
