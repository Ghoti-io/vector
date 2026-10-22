#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""Keep include/ghoti.io/vector/namespace.h in step with the declarations.

    gen-namespace.py [--check]

Every public type, public function and internal function (GVEC_INTERNAL_API, which the tests
link) is renamed into the version namespace by one line of namespace.h. This script finds the
declarations in include/ and src/ and rewrites the three blocks of the file, adding what is
missing and keeping what is there; --check fails when the file would change.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PATH = os.path.join(ROOT, "include/ghoti.io/vector/namespace.h")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def headers(dirname):
    for base, _dirs, files in os.walk(os.path.join(ROOT, dirname)):
        for name in files:
            if name.endswith(".h") and name not in ("namespace.h", "libver.h", "libver_gen.h", "macros.h"):
                yield os.path.join(base, name)


def scan():
    types, public, internal = set(), set(), set()
    for dirname, bucket in (("include", public), ("src", internal)):
        for path in headers(dirname):
            text = read(path)
            text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            for m in re.finditer(r"\}\s*(GVEC_\w+)\s*;", text):
                types.add(m.group(1))
            for m in re.finditer(r"typedef\s+struct\s+(GVEC_\w+)\s+(GVEC_\w+)\s*;", text):
                types.add(m.group(2))
            for m in re.finditer(r"typedef\s+\w[\w\s\*]*\(\*(GVEC_\w+)\)", text):
                types.add(m.group(1))
            for m in re.finditer(r"\bGVEC_API\b[^;{]*?\b(gvec_\w+)\s*\(", text):
                bucket.add(m.group(1)) if dirname == "include" else None
            for m in re.finditer(r"\bGVEC_INTERNAL_API\b[^;{]*?\b(gvec_\w+)\s*\(", text):
                internal.add(m.group(1))
            for m in re.finditer(r"\bGVEC_INTERNAL_API\b\s+extern\b[^;]*?\b(gvec_\w+)\s*;", text):
                internal.add(m.group(1))
            for m in re.finditer(r"\bextern\s+const\s+\w+\s+(gvec_\w+)\s*;", text):
                internal.add(m.group(1))
    return types, public, internal


def block(names):
    return "".join("#define %s GHOTIIO_VECTOR(%s)\n" % (n, n) for n in sorted(names))


def main(argv):
    text = read(PATH)
    types, public, internal = scan()
    sections = [("/* Public types, with their tags. */\n", "\n/* Internal functions"),
                ("/* Internal functions and objects (the src internal headers), for the tests. */\n", "\n/* Public functions. */"),
                ("/* Public functions. */\n", "\n/// @endcond")]
    wanted = [types, internal, public]
    out = text
    for (start, end), names in zip(sections, wanted):
        a = out.index(start) + len(start)
        b = out.index(end)
        existing = set(re.findall(r"#define (\w+) GHOTIIO_VECTOR", out[a:b]))
        out = out[:a] + block(existing | names) + out[b:]
    if "--check" in argv:
        if out != text:
            sys.stderr.write("gen-namespace: namespace.h is out of step with the declarations\n")
            return 1
        return 0
    with open(PATH, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
