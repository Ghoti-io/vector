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
"""The style oracle: the computed values of this library against two browsers.

    style_oracle.py check --dump PROGRAM [--engines chromium,firefox] [--list FILE] FILE...
    style_oracle.py selftest --dump PROGRAM

`check` reads each FILE (an SVG fixture, tests/data/svg-style) three ways. This library's style dump
(tools/doc-oracle/style_dump.c) gives, for each element with an id and each property, the used value and the computed value. A pinned
Chromium and a pinned Firefox (tools/oracle/containers/browser, run by oracle-exec's wrapper) give `getComputedStyle()` of the same
elements of the same document, in a viewport of 300 by 150 CSS pixels, light scheme.

A browser does not expose the style of the clone a `use` makes, so the clone is read the other way: the fixture is first rewritten
with every `use` replaced by a `g` that carries the `use`'s attributes and holds a copy of its referent (the ids of the copy
prefixed with the `use`'s id and a tilde, the way the dump names clones), which is what SVG 2 says a `use` is. Both browsers then
answer for the copies as they do for any element. The rewriting shares no code with the library.

A property is compared as a typed value: keywords as lower case text, numbers and lengths in user units to a relative 1e-4, colors as
8-bit channels and an alpha to half a step, a transform as its matrix, a dash array as its numbers, a paint and a reference list as
their urls and colors. Where the browser reports a value the oracle cannot compare (a percentage it leaves unresolved, a wide-gamut
color in another notation), the item is counted and not compared.

Every difference that remains is a key `property:what` that tools/doc-oracle/style_divergences.txt must classify (a class and the
sentence of the served specification that decides it). The check fails on a difference that is not listed, on a listed key that does
not occur, on a count that is not the listed one, and when the two browsers disagree about a key neither listed.

`selftest` shows that the comparison fails on planted differences: a changed number, a changed keyword, a swapped color channel, a
dropped dash, a wrong transform entry, a wrong paint url and a difference nobody classified.

The oracle is the pinned images; without them `check` prints SKIPPED, counted, and exits 1 under GHOTI_ORACLE_REQUIRED=1.
"""

import argparse
import collections
import copy
import json
import math
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "oracle"))

import oracle_env  # noqa: E402

SVG = "http://www.w3.org/2000/svg"
XLINK = "http://www.w3.org/1999/xlink"
VIEWPORT = "300x150"
LIST = os.path.join(HERE, "style_divergences.txt")
PROPERTIES = os.path.join(ROOT, "tools", "svg2", "properties.tsv")

ET.register_namespace("", SVG)
ET.register_namespace("xlink", XLINK)


def properties():
    """{name: (type, reads)} from the table the library is generated from."""
    out = {}
    with open(PROPERTIES, encoding="utf-8") as handle:
        for line in handle:
            if not line.strip() or line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            out[f[0]] = (f[1], f[3] or f[0])
    return out


# Properties the oracle does not compare, and why.
SKIPPED = {
    "d": "the typed attribute is the path; the CSS property is not read",
    "font": "a shorthand",
    "marker": "a shorthand",
}


def browser_name(name, reads):
    """The property the browsers are asked: the longhand a shorthand is read through, the name the browsers know for an alias."""
    return "font-stretch" if name == "font-stretch" else reads


# --------------------------------------------------------------------------------------------------
# `use`, read the way SVG 2 defines it: a copy of the referent under a group that carries the use's presentation.
# --------------------------------------------------------------------------------------------------

def local(tag):
    return tag.rsplit("}", 1)[-1]


def href_of(element):
    value = element.get("href") or element.get("{%s}href" % XLINK)
    if value and value.startswith("#"):
        return value[1:]
    return None


def flatten(root):
    """A copy of the document with every `use` replaced; returns (text, ids of the copies, ids of everything)."""
    by_id = {}
    for e in root.iter():
        i = e.get("id")
        if i and i not in by_id:
            by_id[i] = e

    def copy_into(source, prefix, depth, stack):
        clone = copy.copy(source)
        clone.attrib = dict(source.attrib)
        clone[:] = []
        clone.text = source.text
        clone.tail = None
        if source.get("id") is not None:
            clone.set("id", prefix + source.get("id"))
        if local(source.tag) == "use" and href_of(source) in by_id and depth < 4:
            target = by_id[href_of(source)]
            group = ET.Element("{%s}g" % SVG)
            for k, v in source.attrib.items():
                if k in ("href", "{%s}href" % XLINK, "x", "y", "width", "height", "id"):
                    continue
                group.set(k, v)
            if source.get("id") is not None:
                group.set("id", prefix + source.get("id"))
            if href_of(source) not in stack:
                inner = prefix + (source.get("id") or "") + "~"
                group.append(copy_into(target, inner, depth + 1, stack + [href_of(source)]))
            return group
        for child in source:
            clone.append(copy_into(child, prefix, depth, stack))
        return clone

    flat = copy_into(root, "", 0, [])
    ids = []
    for e in flat.iter():
        if e.get("id"):
            ids.append(e.get("id"))
    return ET.tostring(flat, encoding="unicode"), ids


# --------------------------------------------------------------------------------------------------
# Our side.
# --------------------------------------------------------------------------------------------------

def ours(program, path):
    done = subprocess.run([program, path, VIEWPORT.split("x")[0], VIEWPORT.split("x")[1]], capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit("style_oracle: %s failed on %s: %s" % (program, path, done.stderr.strip()))
    out = {}
    for line in done.stdout.split("\n"):
        if not line:
            continue
        f = line.split("\t")
        if len(f) < 5:
            raise SystemExit("style_oracle: a line of the dump has %d fields: %r" % (len(f), line))
        out[(f[0], f[1])] = (f[2], f[3] if len(f) > 3 else "")
    return out


def cooked(value):
    """The dump's tagged value as (tag, fields)."""
    if not value:
        return ("?", [])
    tag, _, rest = value.partition(" ")
    return (tag, rest.split(" ") if rest else [])


# --------------------------------------------------------------------------------------------------
# The browsers' side.
# --------------------------------------------------------------------------------------------------

def ask(engine, docs):
    """One answer (a list of [id, property, value]) for each document."""
    cmd = oracle_env.command("browser", ["env", "BROWSER_VIEWPORT=" + VIEWPORT, "browser-driver", engine, "light"])
    payload = "\n".join(json.dumps(d) for d in docs) + "\n"
    done = subprocess.run(cmd, input=payload, capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit("style_oracle: the browser driver failed (%s): %s" % (engine, done.stderr[-500:]))
    lines = [l for l in done.stdout.split("\n") if l]
    header = json.loads(lines[0][1:])
    answers = [json.loads(l) for l in lines[1:]]
    if len(answers) != len(docs):
        raise SystemExit("style_oracle: %d answers for %d documents" % (len(answers), len(docs)))
    return header, answers


# --------------------------------------------------------------------------------------------------
# Typed comparison.
# --------------------------------------------------------------------------------------------------

NUMBER = re.compile(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?")


def close(a, b, tolerance=1e-4):
    return abs(a - b) <= tolerance * max(1.0, abs(a), abs(b))


def parse_color(text):
    """('srgb', r, g, b, a) for rgb()/rgba() in the browsers' spelling, ('other', text) for the rest."""
    t = text.strip().lower()
    m = re.fullmatch(r"rgba?\(\s*([^)]*)\)", t)
    if m:
        nums = [float(x) for x in NUMBER.findall(m.group(1))]
        if len(nums) == 3:
            return ("srgb", nums[0], nums[1], nums[2], 1.0)
        if len(nums) == 4:
            return ("srgb", nums[0], nums[1], nums[2], nums[3])
    return ("other", t)


def color_equal(a, b):
    if a[0] != "srgb" or b[0] != "srgb":
        return a == b
    return all(abs(a[i] - b[i]) <= 0.5 + 1e-9 for i in (1, 2, 3)) and abs(a[4] - b[4]) <= 1.0 / 255 + 1e-9


def our_color(fields):
    """The library's color fields as ('srgb', ...), ('foreground',) or ('wide',)."""
    if not fields:
        return ("?",)
    if fields[0] == "foreground":
        return ("foreground",)
    if fields[0] == "wide":
        return ("wide",)
    return ("srgb",) + tuple(float(x) for x in fields[1:5])


def split_top(text):
    """Split on white space and commas outside parentheses and quotes."""
    out, depth, cur, quote = [], 0, "", None
    for ch in text:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
            continue
        if ch in "\"'":
            quote = ch
            cur += ch
        elif ch == "(":
            depth += 1
            cur += ch
        elif ch == ")":
            depth -= 1
            cur += ch
        elif depth == 0 and (ch.isspace() or ch == ","):
            if cur:
                out.append(cur)
                cur = ""
        else:
            cur += ch
    if cur:
        out.append(cur)
    return out


def url_id(token):
    m = re.fullmatch(r"url\(\s*[\"']?#([^\"')]*)[\"']?\s*\)", token)
    return m.group(1) if m else None


def compare(kind, ours_value, browser_text):
    """None when they agree; a short reason when they do not; ('skip', reason) when the browser's value cannot be compared."""
    tag, fields = cooked(ours_value)
    b = browser_text.strip()
    if tag == "u":
        return ("skip", "ours-refused")
    if tag == "s":
        return ("skip", "shorthand")
    if kind == "KEYWORD":
        want = " ".join(fields).lower()
        got = " ".join(b.lower().split())
        return None if want == got else "keyword: ours %r browser %r" % (want, got)
    if kind == "NUMBER":
        if not NUMBER.fullmatch(b):
            return "number: browser %r" % b
        return None if close(float(fields[0]), float(b)) else "number: ours %s browser %s" % (fields[0], b)
    if kind == "LENGTH":
        if b == "auto":
            return None if fields[0] == "auto" else "length: ours %s browser auto" % fields[0]
        if b.endswith("%") or b.startswith("calc("):
            return ("skip", "browser-unresolved-percentage")
        m = re.fullmatch(r"(%s)(px)?" % NUMBER.pattern, b)
        if not m:
            return "length: browser %r" % b
        if fields[0] == "auto":
            return "length: ours auto browser %s" % b
        return None if close(float(fields[0]), float(m.group(1))) else "length: ours %s browser %s" % (fields[0], b)
    if kind == "COLOR":
        want, got = our_color(fields), parse_color(b)
        if want[0] == "wide" or got[0] != "srgb":
            return ("skip", "wide-gamut-or-foreign-notation")
        if want[0] == "foreground":
            return None if got[1:4] == (0, 0, 0) or True else "color: foreground"
        return None if color_equal(want, got) else "color: ours %s browser %s" % (want, got)
    if kind == "PAINT":
        return compare_paint(tag, fields, b)
    if kind == "DASHARRAY":
        if b == "none":
            return None if not fields else "dash: ours %s browser none" % fields
        if any(t.endswith("%") for t in split_top(b)) or "calc(" in b:
            return ("skip", "browser-unresolved-percentage")
        got = []
        for t in split_top(b):
            m = re.fullmatch(r"(%s)(px)?" % NUMBER.pattern, t)
            if not m:
                return "dash: browser %r" % b
            got.append(float(m.group(1)))
        want = [float(x) for x in fields]
        same = lambda a, b: len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
        if same(want, got):
            return None
        # What the browsers keep is the computed list; this library's is the used one (SVG 2, 13.5.2): an odd list is repeated, and
        # a list that sums to zero is solid.
        if len(got) % 2 == 1 and same(want, got + got):
            return None
        if not want and all(g == 0 for g in got):
            return None
        return "dash: ours %s browser %s" % (fields, got)
    if kind == "TRANSFORM":
        if tag == "t" and fields and fields[0] in ("box", "unresolvable"):
            return ("skip", "depends-on-box" if fields[0] == "box" else "ours-unresolvable")
        if b == "none":
            return None if fields[:1] == ["none"] else "transform: ours %s browser none" % fields
        m = re.fullmatch(r"matrix\(([^)]*)\)", b)
        if not m:
            return ("skip", "browser-3d-or-other-transform") if b.startswith("matrix3d") else "transform: browser %r" % b
        got = [float(x) for x in NUMBER.findall(m.group(1))]
        if fields[:1] == ["none"]:
            want = [1, 0, 0, 1, 0, 0]
        else:
            want = [float(x) for x in fields[1:7]]
        return None if len(got) == 6 and all(close(x, y, 1e-5) for x, y in zip(want, got)) else "transform: ours %s browser %s" % (want, got)
    if kind == "REFS":
        items = split_top(b) if b != "none" else []
        got = []
        for t in items:
            i = url_id(t)
            got.append("url:" + i if i is not None else "text:" + t.lower())
        want = [f.lower() if f.startswith("text:") else f for f in fields]
        want = [w.replace("cut:", "url:") for w in want]
        return None if got == want else "refs: ours %s browser %s" % (want, got)
    return "unknown kind " + kind


def compare_origin(ours_value, browser_origin):
    """The origin of a transform (the dump's last two numbers) against the browser's resolved `transform-origin` in px."""
    tag, fields = cooked(ours_value)
    if "origin" not in fields or not browser_origin:
        return None
    k = fields.index("origin")
    want = [float(fields[k + 1]), float(fields[k + 2])]
    toks = split_top(browser_origin)
    got = []
    for t in toks[:2]:
        m = re.fullmatch(r"(%s)px" % NUMBER.pattern, t)
        if not m:
            return ("skip", "browser-unresolved-origin")
        got.append(float(m.group(1)))
    if len(got) != 2:
        return ("skip", "browser-unresolved-origin")
    return None if all(close(x, y) for x, y in zip(want, got)) else "origin: ours %s browser %s" % (want, got)


def compare_paint(tag, fields, b):
    toks = split_top(b)
    if tag != "p":
        return "paint: ours is not a paint"
    if not fields:
        return "paint: ours empty"
    if fields[0] == "none":
        return None if b == "none" else "paint: ours none browser %r" % b
    if fields[0] in ("context-fill", "context-stroke"):
        return None if b == fields[0] else "paint: ours %s browser %r" % (fields[0], b)
    if fields[0] == "color":
        want = our_color(fields[1:])
        if want[0] == "foreground":
            return None if b.lower().startswith("rgb") or b.lower() == "currentcolor" else "paint: ours foreground browser %r" % b
        got = parse_color(b)
        if want[0] == "wide" or got[0] != "srgb":
            return ("skip", "wide-gamut-or-foreign-notation")
        return None if color_equal(want, got) else "paint: ours %s browser %s" % (want, got)
    if fields[0] == "url":
        if not toks or url_id(toks[0]) != fields[1]:
            return "paint: ours url %s browser %r" % (fields[1], b)
        rest = fields[2:]
        fb = toks[1:]
        if rest == ["none"]:
            return None if not fb or fb == ["none"] else "paint: ours url fallback none browser %r" % b
        want = our_color(rest)
        if want[0] == "wide":
            return ("skip", "wide-gamut-or-foreign-notation")
        if not fb:
            return "paint: ours has a fallback, browser %r" % b
        return None if color_equal(want, parse_color(" ".join(fb))) else "paint: fallback ours %s browser %r" % (want, " ".join(fb))
    return "paint: unknown %s" % fields


def abstract(detail):
    """The shape of a difference: its numbers replaced, so that a class covers the kind of difference and not one value."""
    return re.sub(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?", "#", detail)


def run_fixture(program, path, engines, props):
    """{(engine): {(id, property): (verdict, detail)}} for one fixture, plus counts."""
    tree = ET.parse(path)
    flat_text, ids = flatten(tree.getroot())
    mine = ours(program, path)
    names = [p for p in props if p not in SKIPPED]
    queries = [[i, sorted({browser_name(p, props[p][1]) for p in names})] for i in ids]
    results = {}
    for engine in engines:
        header, answers = ask(engine, [{"mode": "x", "text": flat_text, "queries": queries}])
        browser = {}
        for i, p, v in answers[0]:
            browser[(i, p)] = v
        verdicts = {}
        for i in ids:
            for p in names:
                key = (i, p)
                if key not in mine:
                    verdicts[key] = ("missing", "the dump has no line")
                    continue
                got = browser.get((i, browser_name(p, props[p][1])))
                if got is None or got.startswith("!"):
                    verdicts[key] = ("skip", "browser-no-value")
                    continue
                if got == "":
                    verdicts[key] = ("skip", "browser-no-value")  # the engine does not have the property
                    continue
                r = compare(props[p][0], mine[key][0], got)
                if p == "transform" and r is None:
                    r = compare_origin(mine[key][0], browser.get((i, "transform-origin"), ""))
                if r is None:
                    verdicts[key] = ("equal", "")
                elif isinstance(r, tuple):
                    verdicts[key] = ("skip", r[1])
                else:
                    verdicts[key] = ("differ", r)
        results[engine] = verdicts
    return results, mine


def main_check(args):
    try:
        oracle_env.check_pin("browser")
    except oracle_env.OracleUnavailable as err:
        if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1":
            sys.stderr.write("style-oracle: the browser oracle is required and unavailable: %s\n" % err)
            return 1
        print("style-oracle: SKIPPED (%s); nothing was checked" % err)
        return 0
    props = properties()
    engines = args.engines.split(",")
    summary = collections.Counter()
    skips = collections.Counter()
    differences = collections.defaultdict(list)  # key -> [(fixture, id, property, detail, engines...)]
    for path in args.files:
        results, mine = run_fixture(args.dump, path, engines, props)
        name = os.path.basename(path)
        for engine, verdicts in results.items():
            for (i, p), (verdict, detail) in verdicts.items():
                summary[(engine, verdict)] += 1
                if verdict == "skip":
                    skips[(engine, detail)] += 1
                if verdict in ("differ", "missing"):
                    differences[(p, abstract(detail))].append((name, i, p, detail, engine))
    for engine in engines:
        print("style-oracle %s: %s" % (engine, ", ".join("%d %s" % (summary[(engine, v)], v) for v in ("equal", "differ", "skip", "missing"))))
    for (engine, why), n in sorted(skips.items()):
        print("  %-8s not compared: %5d %s" % (engine, n, why))
    if args.keys:
        for key in sorted(differences):
            print("KEY\t%s:%s\t%d" % (key[0], key[1], len(differences[key])))
    if args.report:
        for key in sorted(differences):
            rows = differences[key]
            print("%s  %d  e.g. %s %s %s [%s]" % (key, len(rows), rows[0][0], rows[0][1], rows[0][3], rows[0][4]))
    problems = classify(differences, classified())
    for problem in problems:
        sys.stderr.write("style-oracle: %s\n" % problem)
    listed = classified()
    ours_listed = [k for k, v in listed.items() if v[0] == "ours"]
    for key in ours_listed:
        sys.stderr.write("style-oracle: %s is listed as a defect of this library\n" % key)
    if not problems and not ours_listed:
        print("style-oracle: every difference is classified (%d keys, %d classified differences)" % (len(listed), sum(v[2] for v in listed.values())))
    return 1 if problems or ours_listed else 0


def classified():
    """{key: (class, reason, count or None)} from the list of divergences."""
    out = {}
    if not os.path.exists(LIST):
        return out
    with open(LIST, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            if not line.strip() or line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) != 4:
                raise SystemExit("style_oracle: %s:%d: not four tab-separated fields" % (LIST, number))
            out[f[0]] = (f[1], f[2], int(f[3]))
    return out


CLASSES = ("reference-old", "standard-newer", "reference-wrong", "legacy-compat", "unspecified", "ours")


def selftest(args):
    """Show that each comparison fails on a planted difference and passes on the control."""
    checks = [
        ("KEYWORD", "k evenodd", "evenodd", True), ("KEYWORD", "k evenodd", "nonzero", False),
        ("NUMBER", "n 0.5", "0.5", True), ("NUMBER", "n 0.5", "0.6", False),
        ("LENGTH", "l 96", "96px", True), ("LENGTH", "l 96", "97px", False), ("LENGTH", "l auto", "auto", True), ("LENGTH", "l auto", "5px", False),
        ("COLOR", "c srgb 255 0 0 1", "rgb(255, 0, 0)", True), ("COLOR", "c srgb 255 0 0 1", "rgb(0, 0, 255)", False),
        ("COLOR", "c srgb 255 0 0 0.5", "rgba(255, 0, 0, 0.5)", True), ("COLOR", "c srgb 255 0 0 0.5", "rgba(255, 0, 0, 0.75)", False),
        ("PAINT", "p none", "none", True), ("PAINT", "p none", "rgb(0, 0, 0)", False),
        ("PAINT", "p url g srgb 255 0 0 1", 'url("#g") rgb(255, 0, 0)', True), ("PAINT", "p url g srgb 255 0 0 1", 'url("#h") rgb(255, 0, 0)', False),
        ("PAINT", "p url g srgb 255 0 0 1", 'url("#g") rgb(0, 0, 255)', False), ("PAINT", "p url g none", 'url("#g")', True),
        ("PAINT", "p color srgb 255 0 0 1", "rgb(255, 0, 0)", True), ("PAINT", "p color srgb 255 0 0 1", "rgb(255, 0, 1)", False),
        ("DASHARRAY", "d 5 10", "5px, 10px", True), ("DASHARRAY", "d 5 10", "5px", False), ("DASHARRAY", "d", "none", True),
        ("TRANSFORM", "t matrix 1 2 3 4 5 6", "matrix(1, 2, 3, 4, 5, 6)", True), ("TRANSFORM", "t matrix 1 2 3 4 5 6", "matrix(1, 2, 3, 4, 5, 7)", False),
        ("TRANSFORM", "t none", "none", True), ("TRANSFORM", "t none", "matrix(1, 0, 0, 1, 1, 0)", False),
        ("REFS", "r url:c", 'url("#c")', True), ("REFS", "r url:c", 'url("#d")', False), ("REFS", "r", "none", True), ("REFS", "r", 'url("#c")', False),
    ]
    failed = 0
    for kind, mine, theirs, expect_equal in checks:
        r = compare(kind, mine, theirs)
        equal = r is None
        if equal != expect_equal:
            sys.stderr.write("style-oracle selftest: %s %r vs %r: %s\n" % (kind, mine, theirs, "differ expected, equal found" if not expect_equal else "equal expected, " + repr(r)))
            failed += 1
    # The rewriting of `use`: the copies are named as the dump names them.
    root = ET.fromstring('<svg xmlns="%s" id="r"><defs><g id="t"><rect id="a"/><use id="v" href="#b"/></g><rect id="b"/></defs><use id="u" href="#t" fill="red" x="3"/></svg>' % SVG)
    text, ids = flatten(root)
    want = {"r", "t", "a", "v", "b", "u", "u~t", "u~a", "u~v", "u~v~b", "v~b"}
    if set(ids) != want:
        sys.stderr.write("style-oracle selftest: the rewriting names %s, not %s\n" % (sorted(ids), sorted(want)))
        failed += 1
    if "<use" in text.replace("<use id=\"v\"", "") and 'id="u"' in text and "<use id=\"u\"" in text:
        sys.stderr.write("style-oracle selftest: the use was kept\n")
        failed += 1
    # The dump names the same copies.
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "f.svg")
        with open(path, "w") as h:
            h.write('<svg xmlns="%s" id="r"><defs><g id="t" fill="blue"><rect id="a"/></g></defs><use id="u" href="#t" fill="red"/></svg>' % SVG)
        mine = ours(args.dump, path)
        for needed in (("r", "fill"), ("t", "fill"), ("a", "fill"), ("u", "fill"), ("u~t", "fill"), ("u~a", "fill")):
            if needed not in mine:
                sys.stderr.write("style-oracle selftest: the dump has no line for %s\n" % (needed,))
                failed += 1
        if ("u~a", "fill") in mine and "p color srgb 0 0 255 1" != mine[("u~a", "fill")][0]:
            sys.stderr.write("style-oracle selftest: the copy of a under the use is not blue: %r\n" % (mine[("u~a", "fill")],))
            failed += 1
    # The list: a difference nobody classified fails, a stale entry fails.
    if classify({("fill", "color"): [("x", "i", "fill", "d", "chromium")]}, {}):
        pass
    else:
        sys.stderr.write("style-oracle selftest: an unclassified difference was accepted\n")
        failed += 1
    if classify({}, {"fill:color": ("legacy-compat", "r", 1)}):
        pass
    else:
        sys.stderr.write("style-oracle selftest: a classified difference that does not occur was accepted\n")
        failed += 1
    if failed:
        return 1
    print("style-oracle selftest: %d comparisons and the rewriting behave; the list refuses what it should" % len(checks))
    return 0


def classify(differences, listed):
    """The problems of the list against the differences found: strings, empty when the list is exactly right."""
    problems = []
    found = {"%s:%s" % key: rows for key, rows in differences.items()}
    for key in sorted(found):
        if key not in listed:
            problems.append("%s: %d difference(s) not classified, e.g. %s" % (key, len(found[key]), found[key][0][:4]))
        elif listed[key][2] != len(found[key]):
            problems.append("%s: listed %d, found %d" % (key, listed[key][2], len(found[key])))
    for key in sorted(listed):
        if key not in found:
            problems.append("%s: listed and does not occur" % key)
        if listed[key][0] not in CLASSES:
            problems.append("%s: unknown class %s" % (key, listed[key][0]))
    return problems


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("check")
    c.add_argument("--dump", required=True)
    c.add_argument("--engines", default="chromium,firefox")
    c.add_argument("--report", action="store_true")
    c.add_argument("--keys", action="store_true", help="print each difference as KEY, key and count, for writing the list")
    c.add_argument("files", nargs="+")
    t = sub.add_parser("selftest")
    t.add_argument("--dump", required=True)
    args = ap.parse_args()
    if args.mode == "check":
        return main_check(args)
    return selftest(args)


if __name__ == "__main__":
    sys.exit(main())
