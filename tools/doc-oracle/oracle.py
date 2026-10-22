#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""An independent reader of SVG documents, and the check of the typed document's dump against it.

    oracle.py check --dump PROGRAM [--random N] [--seed S] FILE...
    oracle.py selftest --dump PROGRAM

`check` loads each FILE (and N random documents it generates) with PROGRAM (tools/doc-oracle/dump.c, which
writes the library's dump), reads the same bytes with Python's own XML parser, and compares what the
specification determines: the kinds of the elements, their ids, the values of the attributes this oracle
parses (numbers, lengths, lists, points, viewBox, preserveAspectRatio, path data in both forms, orient, offsets,
integers), which `href` resolves where, which `use` and gradient elements are on a reference cycle, how many
elements each `use` instantiates, which elements are conditionally false and which child a `switch` selects.

Nothing here shares code with the library. Numbers are read with Python's float(), which rounds correctly; the
path data, the lists and the graph rules are written again from the specification's text. A difference is a
finding in one of the two.

`selftest` shows that the comparison fails on a planted difference: a changed digit, a swapped relative flag, a
dropped implicit-repeat marker, a flipped cycle flag, a wrong expansion count, a wrong href, a wrong switch
selection and a typed attribute made invalid.
"""

import argparse
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

SVG = "http://www.w3.org/2000/svg"
XLINK = "http://www.w3.org/1999/xlink"
XML = "http://www.w3.org/XML/1998/namespace"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


# --------------------------------------------------------------------------------------------------
# What the SVG 2 element index names.
# --------------------------------------------------------------------------------------------------

def known_elements():
    path = os.path.join(ROOT, "third_party/svg2/eltindex.html")
    with open(path, encoding="utf-8") as handle:
        page = handle.read()
    names = set(re.findall(r"<span>([A-Za-z][A-Za-z0-9]*)</span></a>’</span>", page))
    names.discard("unknown")
    if len(names) < 60:
        raise SystemExit("oracle: the element index parsed to %d names" % len(names))
    return names


def attribute_elements():
    """{attribute name: set of element names} from the attribute index, read again here."""
    path = os.path.join(ROOT, "third_party/svg2/attindex.html")
    with open(path, encoding="utf-8") as handle:
        page = handle.read()
    out = {}
    for m in re.finditer(r'attr-name"><a [^>]*><span>([A-Za-z0-9:_-]+)</span></a></span></th><td>(.*?)</td>', page):
        out.setdefault(m.group(1), set()).update(re.findall(r"<span>([A-Za-z][A-Za-z0-9]*)</span>", m.group(2)))
    if "systemLanguage" not in out or "viewBox" not in out:
        raise SystemExit("oracle: the attribute index parsed wrongly")
    return out


ATTRS = None


def applies(kind, name):
    """Whether the attribute index defines `name` for an element of this kind (the index lists `unknown` for the attributes an unrecognised element may carry)."""
    global ATTRS
    if ATTRS is None:
        ATTRS = attribute_elements()
    els = ATTRS.get(name)
    if not els:
        return False
    return kind in els


# --------------------------------------------------------------------------------------------------
# The grammars, written again.
# --------------------------------------------------------------------------------------------------

NUM = re.compile(r"[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
WSP = " \t\n\f\r"
UNITS = {"px", "em", "ex", "ch", "rem", "vw", "vh", "vmin", "vmax", "cm", "mm", "q", "in", "pt", "pc", "%"}
ANGLE_UNITS = {"deg", "grad", "rad", "turn"}


def number_at(s, i):
    """(value, end) of the number starting at s[i], or None; an overflow is not a number."""
    m = NUM.match(s, i)
    if not m:
        return None
    try:
        v = float(m.group(0))
    except OverflowError:
        return None
    if math.isinf(v):
        return None
    return v, m.end()


def skip(s, i):
    while i < len(s) and s[i] in WSP:
        i += 1
    return i


def parse_number(s):
    i = skip(s, 0)
    r = number_at(s, i)
    if not r:
        return None
    return r[0] if skip(s, r[1]) == len(s) else None


def dimension(s, i, units):
    r = number_at(s, i)
    if not r:
        return None
    v, j = r
    k = j
    if k < len(s) and s[k] == "%":
        k += 1
    else:
        while k < len(s) and s[k].isalpha() and s[k].isascii():
            k += 1
    unit = s[j:k].lower()
    if unit and unit not in units:
        return None
    return v, unit, k


def parse_length(s):
    r = dimension(s, skip(s, 0), UNITS)
    return (r[0], r[1]) if r and skip(s, r[2]) == len(s) else None


def parse_angle_degrees(s):
    r = dimension(s, skip(s, 0), ANGLE_UNITS)
    if not r or skip(s, r[2]) != len(s):
        return None
    v, u = r[0], r[1]
    return {"": v, "deg": v, "grad": v * 0.9, "rad": v * 57.29577951308232, "turn": v * 360.0}[u]


def list_items(s, read, adjacent_negative=False):
    """Items separated by comma-wsp; (items, ok)."""
    items = []
    i = skip(s, 0)
    while i < len(s):
        r = read(s, i)
        if not r:
            return items, False
        items.append(r[0])
        j = r[1]
        k = skip(s, j)
        comma = k < len(s) and s[k] == ","
        if comma:
            k = skip(s, k + 1)
        if k >= len(s):
            return items, not comma
        if k == j and not (adjacent_negative and s[k] == "-"):
            return items, False
        i = k
    return items, True


def parse_numbers(s, pairs=False):
    items, ok = list_items(s, number_at, adjacent_negative=pairs)
    if not ok:
        return None
    if pairs and len(items) % 2:
        return ("partial", items[:-1])
    return ("ok", items)


def parse_lengths(s):
    def read(s, i):
        r = dimension(s, i, UNITS)
        return ((r[0], r[1]), r[2]) if r else None
    items, ok = list_items(s, read)
    return items if ok else None


def parse_viewbox(s):
    i = skip(s, 0)
    vals = []
    for k in range(4):
        r = number_at(s, i)
        if not r:
            return None
        vals.append(r[0])
        i = r[1]
        if k < 3:
            j = skip(s, i)
            if j < len(s) and s[j] == ",":
                j = skip(s, j + 1)
            elif j == i:
                return None
            i = j
    if skip(s, i) != len(s) or vals[2] < 0 or vals[3] < 0:
        return None
    return vals


ALIGNS = ["none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"]


def parse_par(s):
    t = s.split()
    defer = 0
    if t and t[0] == "defer":
        defer = 1
        t = t[1:]
    if not t or t[0] not in ALIGNS or len(t) > 2:
        return None
    slice_ = 0
    if len(t) == 2:
        if t[1] not in ("meet", "slice"):
            return None
        slice_ = 1 if t[1] == "slice" else 0
    return ALIGNS.index(t[0]), slice_, defer


def parse_integer(s):
    m = re.fullmatch(r"[%s]*([+-]?[0-9]+)[%s]*" % (WSP, WSP), s)
    if not m:
        return None
    v = int(m.group(1))
    return v if -(1 << 63) <= v < (1 << 63) else None


def parse_offset(s):
    i = skip(s, 0)
    r = number_at(s, i)
    if not r:
        return None
    v, j = r
    if j < len(s) and s[j] == "%":
        v /= 100.0
        j += 1
    return v if skip(s, j) == len(s) else None


def parse_orient(s):
    t = s.strip(WSP)
    if t == "auto":
        return ("auto", 0.0)
    if t == "auto-start-reverse":
        return ("auto-start-reverse", 0.0)
    a = parse_angle_degrees(s)
    return ("angle", a) if a is not None else None


# ---- path data: first the commands, then the absolute form (two layers, unlike the library) ----------

ARGS = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2, "A": 7, "Z": 0}


def path_commands(s):
    """(commands, error_offset or None): each command (letter, args, implicit) in source order."""
    cmds = []
    i = skip(s, 0)
    first = True
    while i < len(s):
        letter = s[i]
        up = letter.upper()
        if up not in ARGS or (first and up != "M"):
            return cmds, i
        first = False
        i += 1
        implicit = False
        while True:
            args = []
            if up != "Z":
                i = skip(s, i)
                for k in range(ARGS[up]):
                    if k:
                        i = skip(s, i)
                        if i < len(s) and s[i] == ",":
                            i = skip(s, i + 1)
                    if up == "A" and k in (3, 4):
                        if i < len(s) and s[i] in "01":
                            args.append(float(s[i]))
                            i += 1
                        else:
                            return cmds, i
                    else:
                        r = number_at(s, i)
                        if not r:
                            return cmds, i
                        args.append(r[0])
                        i = r[1]
            cmds.append((letter, args, implicit))
            if up == "Z":
                break
            j = skip(s, i)
            if j >= len(s):
                i = j
                break
            if s[j] == ",":
                k = skip(s, j + 1)
                if k >= len(s) or not (s[k] in "+-." or s[k].isdigit()):
                    return cmds, i
                i = k
            elif s[j] in "+-." or s[j].isdigit():
                i = j
            else:
                i = j
                break
            implicit = True
            if up == "M":
                up = "L"
                letter = "l" if letter == "m" else "L"
        i = skip(s, i)
    return cmds, None


def path_absolute(cmds):
    """Segments (kind, x0, y0, ..., x, y) for the commands, and how many were complete (all that are finite)."""
    segs = []
    cx = cy = sx = sy = 0.0
    last = None  # ('C', x, y) or ('Q', x, y)
    for letter, a, _implicit in cmds:
        up = letter.upper()
        rel = letter != up
        ox, oy = (cx, cy) if rel else (0.0, 0.0)
        x0, y0 = cx, cy
        if up == "M":
            x, y = a[0] + ox, a[1] + oy
            seg = ("M", x0, y0, x, y)
            sx, sy = x, y
            last = None
        elif up == "L":
            x, y = a[0] + ox, a[1] + oy
            seg = ("L", x0, y0, x, y)
            last = None
        elif up == "H":
            x, y = a[0] + ox, cy
            seg = ("L", x0, y0, x, y)
            last = None
        elif up == "V":
            x, y = cx, a[0] + oy
            seg = ("L", x0, y0, x, y)
            last = None
        elif up == "C":
            x1, y1, x2, y2, x, y = a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy
            seg = ("C", x0, y0, x1, y1, x2, y2, x, y)
            last = ("C", x2, y2)
        elif up == "S":
            x1, y1 = (2 * cx - last[1], 2 * cy - last[2]) if last and last[0] == "C" else (cx, cy)
            x2, y2, x, y = a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy
            seg = ("C", x0, y0, x1, y1, x2, y2, x, y)
            last = ("C", x2, y2)
        elif up == "Q":
            x1, y1, x, y = a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy
            seg = ("Q", x0, y0, x1, y1, x, y)
            last = ("Q", x1, y1)
        elif up == "T":
            x1, y1 = (2 * cx - last[1], 2 * cy - last[2]) if last and last[0] == "Q" else (cx, cy)
            x, y = a[0] + ox, a[1] + oy
            seg = ("Q", x0, y0, x1, y1, x, y)
            last = ("Q", x1, y1)
        elif up == "A":
            x, y = a[5] + ox, a[6] + oy
            seg = ("A", x0, y0, a[0], a[1], a[2], int(a[3]), int(a[4]), x, y)
            last = None
        else:
            x, y = sx, sy
            seg = ("Z", x0, y0, x, y)
            last = None
        if not all(math.isfinite(v) for v in seg[1:] if isinstance(v, float)):
            break
        segs.append(seg)
        cx, cy = x, y
    return segs


def parse_path(s):
    """('ok'|'partial'|'invalid', commands, segments, error_offset)."""
    cmds, err = path_commands(s)
    segs = path_absolute(cmds)
    complete = len(segs)
    if err is None and complete == len(cmds):
        return "ok", cmds[:complete], segs, None
    if complete == 0:
        return "invalid", [], [], err
    return "partial", cmds[:complete], segs, err


# --------------------------------------------------------------------------------------------------
# Reading a dump.
# --------------------------------------------------------------------------------------------------

def unquote(text, i):
    """The quoted string at text[i] ('"'), and the index after it."""
    assert text[i] == '"', text[i:i + 20]
    out = bytearray()
    i += 1
    while text[i] != '"':
        if text[i] == "\\":
            assert text[i + 1] == "x"
            out.append(int(text[i + 2:i + 4], 16))
            i += 4
        else:
            out.append(ord(text[i]))
            i += 1
    return out.decode("utf-8", "surrogateescape"), i + 1


def decode_number(token):
    m = re.fullmatch(r"(-?)([0-9]+)e(-?[0-9]+)", token)
    if not m:
        raise ValueError("not a dump number: %r" % token)
    return float("%s%se%s" % (m.group(1), m.group(2), m.group(3)))


def decode_length(token):
    m = re.fullmatch(r"(-?[0-9]+e-?[0-9]+)([a-z%]*)", token)
    if not m:
        raise ValueError("not a dump length: %r" % token)
    return decode_number(m.group(1)), m.group(2)


def parse_dump(text):
    lines = text.split("\n")
    elements = []
    stack = {}
    for line in lines:
        if line.startswith("e "):
            m = re.match(r"e (\d+) ([0-9a-f]{16}) (\S+) ", line)
            depth, key, kind = int(m.group(1)), m.group(2), m.group(3)
            name, i = unquote(line, m.end())
            rest = line[i + 1:]
            if rest.startswith('"'):
                ns, i = unquote(rest, 0)
                rest = rest[i + 1:]
            else:
                ns, rest = None, rest.split(" ", 1)[1]
            loc, rest = rest.split(" ", 1)
            fields = rest.split(" ")
            flags = fields[0]
            extra = {}
            for f in fields[1:]:
                k, v = f.split("=")
                extra[k] = v
            el = {"depth": depth, "key": key, "kind": kind, "name": name, "ns": ns, "flags": set() if flags == "-" else set(flags.split(",")),
                  "extra": extra, "attrs": [], "line": line}
            elements.append(el)
        elif line.startswith("a "):
            name, i = unquote(line, 2)
            rest = line[i + 1:]
            cls, grammar, loc, pres, rest = rest.split(" ", 4)
            ans = None
            if rest.startswith('"'):
                ans, i = unquote(rest, 0)
                rest = rest[i + 1:]
            assert rest.startswith("raw=")
            raw, i = unquote(rest, 4)
            assert rest[i:].startswith(" value="), rest[i:i + 30]
            value = rest[i + 7:]
            elements[-1]["attrs"].append({"name": name, "class": cls, "grammar": grammar, "pres": pres, "ns": ans, "raw": raw, "value": value})
    return elements


def decode_value(value):
    """The comparable structure of a dump value string, or None for '-'."""
    if value == "-":
        return None
    tag, _, body = value.partition(":")
    if tag in ("n", "off"):
        return decode_number(body)
    if tag == "i":
        return int(body)
    if tag == "l":
        return decode_length(body)
    if tag in ("ns", "pts"):
        inner = body[1:-1]
        return [decode_number(t) for t in inner.split(",")] if inner else []
    if tag == "ls":
        inner = body[1:-1]
        return [decode_length(t) for t in inner.split(",")] if inner else []
    if tag == "vb":
        return [decode_number(t) for t in body.split(",")]
    if tag == "par":
        a, s, d = body.split(":")
        return int(a), int(s), int(d)
    if tag == "o":
        mode, _, angle = body.partition(":")
        return {"0": "angle", "1": "auto", "2": "auto-start-reverse"}[mode], decode_number(angle)
    if tag == "p":
        cmds_part, _, abs_part = body.partition(" abs:")
        cmds = []
        for item in cmds_part.split(";") if cmds_part else []:
            implicit = item.startswith("+")
            if implicit:
                item = item[1:]
            letter = item[0]
            args = [decode_number(t) for t in item[2:].split(",")] if len(item) > 1 else []
            cmds.append((letter, args, implicit))
        segs = []
        for item in abs_part.split(";") if abs_part else []:
            kind = item[0]
            parts = item[2:].split(",")
            if kind == "A":
                nums = [decode_number(t) for t in parts[:5]] + [int(parts[5]), int(parts[6])] + [decode_number(t) for t in parts[7:]]
                segs.append(("A", *nums))
            else:
                segs.append((kind, *[decode_number(t) for t in parts]))
        return cmds, segs
    return value


# --------------------------------------------------------------------------------------------------
# The independent view of a document.
# --------------------------------------------------------------------------------------------------

CANDIDATES = {"circle", "ellipse", "line", "path", "polygon", "polyline", "rect", "a", "audio", "canvas", "foreignObject", "g",
              "iframe", "image", "svg", "switch", "text", "use", "video"}
CHAIN = {"linearGradient", "radialGradient", "pattern"}


def walk(root, known):
    """Elements in document order: dicts with kind, name, attrs (list of (name, value)), parent index, order."""
    out = []

    def visit(e, parent):
        ns, _, local = e.tag[1:].partition("}") if e.tag.startswith("{") else (None, "", e.tag)
        if ns == SVG:
            kind = local if local in known else "unknown"
        else:
            kind = "foreign"
        attrs = []
        for k, v in e.attrib.items():
            if k.startswith("{"):
                ans, _, al = k[1:].partition("}")
                if ans == XLINK:
                    attrs.append(("xlink:" + al, v, None))
                elif ans == XML:
                    attrs.append(("xml:" + al, v, None))
                else:
                    attrs.append((al, v, ans))
            else:
                attrs.append((k, v, None))
        rec = {"kind": kind, "name": local, "ns": ns, "attrs": attrs, "parent": parent, "order": len(out), "size": 1}
        out.append(rec)
        for c in e:
            if not isinstance(c.tag, str):
                continue
            visit(c, rec["order"])
        rec["size"] = len(out) - rec["order"]

    visit(root, None)
    return out


def get_attr(rec, name):
    for n, v, ns in rec["attrs"]:
        if n == name and ns is None:
            return v
    return None


def href_of(rec):
    v = get_attr(rec, "href")
    if v is None:
        v = get_attr(rec, "xlink:href")
    return v


def fragment(v):
    v = v.strip(WSP)
    if not v.startswith("#"):
        return None
    return re.sub(r"%([0-9A-Fa-f]{2})", lambda m: chr(int(m.group(1), 16)), v[1:])


def language_true(attr, languages):
    tags = [t.strip(WSP) for t in attr.split(",") if t.strip(WSP)]
    for t in tags:
        for u in languages:
            tl, ul = t.lower(), u.lower()
            if tl == ul or (tl.startswith(ul) and tl[len(ul):len(ul) + 1] == "-"):
                return True
    return False


def graph_facts(recs, languages):
    """Per element order: on_cycle, expansion (for use), cond_false, selected child, href target."""
    ids = {}
    for r in recs:
        i = get_attr(r, "id")
        if i and not re.search(r"[ \t\n\f\r]", i) and i not in ids:
            ids[i] = r["order"]
    target = {}
    for r in recs:
        h = href_of(r)
        if h is not None:
            f = fragment(h)
            if f is not None and f in ids:
                target[r["order"]] = ids[f]
    uses = [r["order"] for r in recs if r["kind"] == "use"]
    inside = lambda t: [u for u in uses if t <= u < t + recs[t]["size"]]

    def reach(u):
        """Uses instantiated, directly or not, by use u."""
        seen, todo = set(), [u]
        while todo:
            cur = todo.pop()
            t = target.get(cur)
            if t is None:
                continue
            for w in inside(t):
                if w not in seen:
                    seen.add(w)
                    todo.append(w)
        return seen

    on_cycle = set()
    for u in uses:
        if u in reach(u):
            on_cycle.add(u)
    # paint server chains
    chain = {r["order"]: target[r["order"]] for r in recs if r["kind"] in CHAIN and r["order"] in target and recs[target[r["order"]]]["kind"] in CHAIN}
    for s in chain:
        cur, seen = s, set()
        while cur in chain and cur not in seen:
            seen.add(cur)
            cur = chain[cur]
        if cur == s:
            on_cycle.add(s)
    expansion = {}

    def expand(u, stack=()):
        if u in expansion:
            return expansion[u]
        t = target.get(u)
        if u in on_cycle or t is None:
            expansion[u] = 0
            return 0
        total = recs[t]["size"]
        for w in inside(t):
            total += expand(w)
        expansion[u] = total
        return total

    sys.setrecursionlimit(10000)
    for u in uses:
        expand(u)
    cond_false = set()
    for r in recs:
        if r["kind"] == "foreign":
            continue
        sl = get_attr(r, "systemLanguage")
        if sl is not None and applies(r["kind"], "systemLanguage") and not language_true(sl, languages):
            cond_false.add(r["order"])
        if get_attr(r, "requiredExtensions") is not None and applies(r["kind"], "requiredExtensions"):
            cond_false.add(r["order"])
    selected = {}
    for r in recs:
        if r["kind"] == "switch":
            for c in recs:
                if c["parent"] == r["order"] and c["kind"] in CANDIDATES and c["order"] not in cond_false:
                    selected[r["order"]] = c["order"]
                    break
    return {"on_cycle": on_cycle, "expansion": expansion, "cond_false": cond_false, "selected": selected, "target": target}


# --------------------------------------------------------------------------------------------------
# The comparison.
# --------------------------------------------------------------------------------------------------

# Attributes this oracle parses: (element or None for any, name) -> reader
def oracle_value(kind, name, raw):
    """(expected class, expected value) for attributes the oracle reads, or None."""
    if not (name == "d" and kind == "path") and not applies(kind, name):
        return None
    if name == "viewBox":
        v = parse_viewbox(raw)
        return ("typed", v) if v else ("invalid", None)
    if name == "preserveAspectRatio":
        v = parse_par(raw)
        return ("typed", v) if v else ("invalid", None)
    if name == "points" and kind in ("polygon", "polyline"):
        r = parse_numbers(raw, pairs=True)
        if r is None:
            return ("invalid", None)
        return ("typed" if r[0] == "ok" else "partial", r[1])
    if name == "d" and kind == "path":
        st, cmds, segs, err = parse_path(raw)
        if st == "invalid":
            return ("invalid", None)
        return ("typed" if st == "ok" else "partial", (cmds, segs), err)
    if name == "pathLength":
        v = parse_number(raw)
        return ("typed", v) if v is not None and v >= 0 else ("invalid", None)
    if name == "tabindex":
        v = parse_integer(raw)
        return ("typed", v) if v is not None else ("invalid", None)
    if name == "offset" and kind == "stop":
        v = parse_offset(raw)
        return ("typed", v) if v is not None else ("invalid", None)
    if name in ("x1", "y1", "x2", "y2") and kind in ("line", "linearGradient"):
        v = parse_length(raw)
        return ("typed", v) if v else ("invalid", None)
    if name in ("markerWidth", "markerHeight") and kind == "marker":
        v = parse_length(raw)
        return ("typed", v) if v else ("invalid", None)
    if name == "orient" and kind == "marker":
        v = parse_orient(raw)
        return ("typed", v) if v else ("invalid", None)
    if name in ("x", "y", "dx", "dy") and kind in ("text", "tspan"):
        v = parse_lengths(raw)
        return ("typed", v) if v is not None else ("invalid", None)
    if name == "rotate" and kind in ("text", "tspan"):
        r = parse_numbers(raw)
        return ("typed", r[1]) if r else ("invalid", None)
    return None


def close(a, b):
    return a == b or (isinstance(a, float) and isinstance(b, float) and a == b)


def compare(dump_text, svg_text, languages, known, stats):
    """The differences between the library's dump and the oracle's reading, as strings."""
    problems = []
    try:
        recs = walk(ET.fromstring(svg_text), known)
    except ET.ParseError as err:
        return ["the oracle cannot parse the document: %s" % err]
    els = parse_dump(dump_text)
    if len(els) != len(recs):
        return ["%d elements in the dump, %d read by the oracle" % (len(els), len(recs))]
    facts = graph_facts(recs, languages)
    keys = [e["key"] for e in els]
    for r, e in zip(recs, els):
        where = "element %d <%s>" % (r["order"], r["name"])
        stats["elements"] += 1
        if (e["kind"] if e["kind"] in ("unknown", "foreign") else e["kind"]) != r["kind"] and not (r["kind"] not in ("unknown", "foreign") and e["kind"] == r["name"]):
            problems.append("%s: kind %s, oracle %s" % (where, e["kind"], r["kind"]))
        if e["name"] != r["name"]:
            problems.append("%s: name %s" % (where, e["name"]))
        # depth
        depth = 0
        p = r["parent"]
        while p is not None:
            depth += 1
            p = recs[p]["parent"]
        if e["depth"] != depth:
            problems.append("%s: depth %d, oracle %d" % (where, e["depth"], depth))
        if len(e["attrs"]) != len(r["attrs"]):
            problems.append("%s: %d attributes, oracle %d" % (where, len(e["attrs"]), len(r["attrs"])))
            continue
        for (n, raw, ns), a in zip(r["attrs"], e["attrs"]):
            if a["name"] != n or a["raw"] != raw:
                problems.append("%s: attribute %r raw %r, oracle %r %r" % (where, a["name"], a["raw"], n, raw))
                continue
            if ns is not None:
                continue
            want = oracle_value(r["kind"], n, raw)
            if want is None:
                continue
            stats["attributes"] += 1
            cls = a["class"]
            partial = "partial" in a["pres"]
            if want[0] == "invalid":
                if cls != "invalid":
                    problems.append("%s: %s=%r is invalid for the oracle, class %s in the dump" % (where, n, raw, cls))
                continue
            if cls == "invalid":
                problems.append("%s: %s=%r is %s for the oracle, invalid in the dump" % (where, n, raw, want[0]))
                continue
            if cls != "typed" or (want[0] == "partial") != partial:
                problems.append("%s: %s=%r class %s partial=%s, oracle %s" % (where, n, raw, cls, partial, want[0]))
                continue
            got = decode_value(a["value"])
            if n == "d":
                if got != want[1]:
                    problems.append("%s: path %r differs:\n   dump   %r\n   oracle %r" % (where, raw, got, want[1]))
                if partial:
                    m = re.search(r"partial@(\d+)", a["pres"])
                    if int(m.group(1)) != want[2]:
                        problems.append("%s: path %r error offset %s, oracle %s" % (where, raw, m.group(1), want[2]))
                continue
            expected = want[1]
            if n in ("viewBox",):
                expected = list(expected)
            if n == "orient":
                expected = (expected[0], expected[1])
            if n in ("x1", "y1", "x2", "y2", "markerWidth", "markerHeight"):
                expected = (expected[0], expected[1])
            if got != expected:
                problems.append("%s: %s=%r is %r in the dump, %r for the oracle" % (where, n, raw, got, expected))
        # graph facts
        o = r["order"]
        flags = e["flags"]
        for flag, member in (("on-cycle", facts["on_cycle"]), ("conditional-false", facts["cond_false"])):
            if (flag in flags) != (o in member):
                problems.append("%s: flag %s is %s in the dump, %s for the oracle" % (where, flag, flag in flags, o in member))
        stats["flags"] += 2
        tgt = facts["target"].get(o)
        if r["kind"] in ("use", "linearGradient", "radialGradient", "pattern") or "href" in e["extra"]:
            expect_key = keys[tgt] if tgt is not None and o not in facts["on_cycle"] else None
            got_key = e["extra"].get("href")
            if got_key != expect_key:
                problems.append("%s: href %s in the dump, %s for the oracle" % (where, got_key, expect_key))
            stats["hrefs"] += 1
        if r["kind"] == "use":
            exp = facts["expansion"].get(o, 0) if tgt is not None or o in facts["on_cycle"] else 0
            if str(exp) != e["extra"].get("expansion"):
                problems.append("%s: expansion %s in the dump, %d for the oracle" % (where, e["extra"].get("expansion"), exp))
            stats["expansions"] += 1
        if r["kind"] == "switch":
            sel = facts["selected"].get(o)
            want_key = keys[sel] if sel is not None else None
            if e["extra"].get("selected") != want_key:
                problems.append("%s: selected %s in the dump, %s for the oracle" % (where, e["extra"].get("selected"), want_key))
            stats["switches"] += 1
    return problems


# --------------------------------------------------------------------------------------------------
# Random documents.
# --------------------------------------------------------------------------------------------------

def rnd_number(rng):
    forms = [lambda: str(rng.randrange(-50, 50)), lambda: "%.3f" % rng.uniform(-100, 100), lambda: "%de%d" % (rng.randrange(1, 9), rng.randrange(-5, 5)),
             lambda: ".%d" % rng.randrange(1, 99), lambda: "%d." % rng.randrange(0, 20), lambda: "+%d" % rng.randrange(0, 9)]
    return rng.choice(forms)()


def corrupt(s, rng):
    if not s or rng.random() > 0.3:
        return s
    i = rng.randrange(len(s))
    return rng.choice([s[:i] + s[i + 1:], s[:i] + rng.choice(" ,-.e%x") + s[i:], s[:i] + "z" + s[i + 1:], s + rng.choice([",", " ", "1", "e"])])


def rnd_path(rng):
    out = ["M" if rng.random() < 0.95 else "L", " ", rnd_number(rng), ",", rnd_number(rng)]
    for _ in range(rng.randrange(0, 8)):
        c = rng.choice("MmLlHhVvCcSsQqTtAaZz")
        out.append(rng.choice([" ", "", " "]) + c)
        for k in range(ARGS[c.upper()]):
            if c.upper() == "A" and k in (3, 4):
                out.append(rng.choice(["0", "1", "1"]))
            else:
                out.append(rng.choice([" ", ",", " "]) + rnd_number(rng))
            if c.upper() != "A" or k not in (3, 4):
                pass
    return "".join(out)


def rnd_doc(rng):
    ids = ["i%d" % k for k in range(rng.randrange(3, 12))]
    kinds = ["g", "g", "rect", "circle", "line", "path", "polygon", "polyline", "use", "use", "linearGradient", "radialGradient",
             "pattern", "stop", "marker", "switch", "text", "tspan", "defs", "symbol", "blink", "title"]
    languages = ["en", "en-US", "fr", "de-CH", "", "english", "EN", "fr-ca"]

    def attrs(kind):
        out = []
        if rng.random() < 0.55:
            out.append('id="%s"' % rng.choice(ids))
        if kind in ("use", "linearGradient", "radialGradient", "pattern") and rng.random() < 0.8:
            q = rng.choice(["href", "xlink:href"])
            out.append('%s="#%s"' % (q, rng.choice(ids + ["nowhere"])))
        if rng.random() < 0.2:
            out.append('systemLanguage="%s"' % ", ".join(rng.sample(languages, rng.randrange(0, 3))))
        if rng.random() < 0.05:
            out.append('requiredExtensions="%s"' % rng.choice(["", "http://x"]))
        if kind == "path":
            out.append('d="%s"' % corrupt(rnd_path(rng), rng))
        if kind in ("polygon", "polyline"):
            out.append('points="%s"' % corrupt(" ".join(rnd_number(rng) + "," + rnd_number(rng) for _ in range(rng.randrange(0, 5))) + rng.choice(["", "", " 7"]), rng))
        if kind == "line":
            for n in ("x1", "y1", "x2", "y2"):
                if rng.random() < 0.7:
                    out.append('%s="%s"' % (n, corrupt(rnd_number(rng) + rng.choice(["", "px", "em", "%", "PX", "mm"]), rng)))
        if kind == "stop":
            out.append('offset="%s"' % corrupt(rnd_number(rng) + rng.choice(["", "%"]), rng))
        if kind == "marker":
            out.append('orient="%s"' % corrupt(rng.choice(["auto", "auto-start-reverse", "45", "1.5rad", "0.25turn", "100grad"]), rng))
            out.append('markerWidth="%s"' % corrupt(rnd_number(rng) + "px", rng))
        if kind in ("text", "tspan"):
            out.append('x="%s"' % corrupt(" ".join(rnd_number(rng) + rng.choice(["", "em", "%"]) for _ in range(rng.randrange(0, 4))), rng))
            out.append('rotate="%s"' % corrupt(" ".join(rnd_number(rng) for _ in range(rng.randrange(0, 4))), rng))
        if kind == "svg" or rng.random() < 0.05:
            out.append('viewBox="%s"' % corrupt(" ".join(rnd_number(rng) for _ in range(4)), rng))
            out.append('preserveAspectRatio="%s"' % corrupt(rng.choice(ALIGNS) + rng.choice(["", " meet", " slice"]), rng))
        if rng.random() < 0.05:
            out.append('tabindex="%s"' % corrupt(str(rng.randrange(-5, 5)), rng))
        return " ".join(out)

    def element(depth):
        kind = rng.choice(kinds)
        a = attrs(kind)
        if kind in ("g", "defs", "symbol", "switch", "text", "marker", "pattern", "linearGradient", "radialGradient") and depth < 4:
            body = "".join(element(depth + 1) for _ in range(rng.randrange(0, 5)))
            return "<%s %s>%s</%s>" % (kind, a, body, kind)
        return "<%s %s/>" % (kind, a)

    root_attrs = attrs("svg")
    body = "".join(element(1) for _ in range(rng.randrange(2, 9)))
    return ('<svg xmlns="%s" xmlns:xlink="%s" %s>%s</svg>' % (SVG, XLINK, root_attrs, body))


# --------------------------------------------------------------------------------------------------
# Driving.
# --------------------------------------------------------------------------------------------------

def run_dump(program, path, languages):
    done = subprocess.run([program, path] + list(languages), capture_output=True)
    if done.returncode != 0:
        return None, done.stderr.decode("utf-8", "replace")
    return done.stdout.decode("utf-8", "surrogateescape"), ""


def check(args):
    known = known_elements()
    stats = {"elements": 0, "attributes": 0, "flags": 0, "hrefs": 0, "expansions": 0, "switches": 0, "documents": 0}
    problems = []
    inputs = []
    for f in args.files:
        with open(f, "rb") as handle:
            inputs.append((f, handle.read().decode("utf-8")))
    rng = random.Random(args.seed)
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(args.random):
            inputs.append(("random-%d" % k, rnd_doc(rng)))
        for name, text in inputs:
            for languages in ([], ["fr", "de"]):
                path = os.path.join(tmp, "doc.svg")
                with open(path, "w", encoding="utf-8", newline="") as handle:
                    handle.write(text)
                dump, why = run_dump(args.dump, path, languages)
                if dump is None:
                    # A document the library refuses must be one the oracle also finds unusable; skip it.
                    continue
                stats["documents"] += 1
                for p in compare(dump, text, languages or ["en"], known, stats):
                    problems.append("%s%s: %s" % (name, " " + ",".join(languages) if languages else "", p))
    for line in problems[:40]:
        sys.stderr.write("doc-oracle: %s\n" % line)
    print("doc-oracle: %(documents)d documents, %(elements)d elements, %(attributes)d attribute values, %(flags)d flags, "
          "%(hrefs)d hrefs, %(expansions)d expansions, %(switches)d switches compared; %(n)d differences" % dict(stats, n=len(problems)))
    return 1 if problems else 0


def selftest(args):
    known = known_elements()
    svg = ('<svg xmlns="%s" xmlns:xlink="%s" viewBox="0 0 10 20"><defs><g id="a"><rect/></g><g id="b"><use href="#c"/></g>'
           '<g id="c"><use href="#b"/></g></defs><path d="m 1 2 3 4 L 5 6 C 1 1 2 2 3 3 S 4 4 5 5 z"/><use href="#a"/>'
           '<switch><g systemLanguage="fr"/><g id="en" systemLanguage="en"/></switch><polygon points="1,2 3,4 5"/>'
           '<stop offset="10%%"/></svg>' % (SVG, XLINK))
    stats = {"elements": 0, "attributes": 0, "flags": 0, "hrefs": 0, "expansions": 0, "switches": 0}
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "doc.svg")
        with open(path, "w") as handle:
            handle.write(svg)
        dump, why = run_dump(args.dump, path, [])
        if dump is None:
            sys.stderr.write("selftest: the control did not load: %s\n" % why)
            return 1
    control = compare(dump, svg, ["en"], known, stats)
    if control:
        sys.stderr.write("selftest: the control differs:\n%s\n" % "\n".join(control))
        return 1
    plants = [
        ("a changed digit", lambda d: d.replace("vb:0e0,0e0,1e1,2e1", "vb:0e0,0e0,1e1,3e1")),
        ("a swapped relative flag", lambda d: d.replace("p:m 1e0,2e0;+l", "p:M 1e0,2e0;+l", 1)),
        ("a dropped implicit marker", lambda d: d.replace(";+l 3e0,4e0", ";l 3e0,4e0", 1)),
        ("a wrong absolute segment", lambda d: d.replace("abs:M 0e0,0e0,1e0,2e0;L 1e0,2e0,4e0,6e0", "abs:M 0e0,0e0,1e0,2e0;L 1e0,2e0,4e0,7e0", 1)),
        ("a flipped cycle flag", lambda d: re.sub(r'("b" )?(e 3 [0-9a-f]{16} use [^\n]*?) on-cycle', r"\1\2 -", d, count=1) if "on-cycle" in d else d),
        ("a wrong expansion", lambda d: re.sub(r"expansion=(\d+)", lambda m: "expansion=%d" % (int(m.group(1)) + 1), d, count=1)),
        ("a wrong href", lambda d: re.sub(r"href=[0-9a-f]{16}", "href=0000000000000000", d, count=1)),
        ("a wrong switch selection", lambda d: re.sub(r"selected=[0-9a-f]{16}", "selected=0000000000000000", d, count=1)),
        ("a typed attribute made invalid", lambda d: d.replace("viewBox\" typed", "viewBox\" invalid", 1)),
        ("a dropped partial marker", lambda d: re.sub(r",partial@\d+", "", d, count=1)),
    ]
    failed = 0
    for name, mutate in plants:
        mutated = mutate(dump)
        if mutated == dump:
            sys.stderr.write("selftest: the plant %r did not change the dump\n" % name)
            failed += 1
            continue
        try:
            found = compare(mutated, svg, ["en"], known, dict(stats))
        except Exception as err:  # a malformed dump is also a refusal
            found = [str(err)]
        if not found:
            sys.stderr.write("selftest: NOT CAUGHT: %s\n" % name)
            failed += 1
    print("doc-oracle selftest: %d of %d planted differences caught; the control agrees" % (len(plants) - failed, len(plants)))
    return 1 if failed else 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--dump", required=True)
    c.add_argument("--random", type=int, default=0)
    c.add_argument("--seed", type=int, default=1)
    c.add_argument("files", nargs="*")
    s = sub.add_parser("selftest")
    s.add_argument("--dump", required=True)
    args = ap.parse_args(argv)
    return check(args) if args.cmd == "check" else selftest(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
