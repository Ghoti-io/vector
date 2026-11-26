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
"""The animation oracle: the computed style of a document at a time, this library against two browsers paused at that time.

    anim_oracle.py check --dump PROGRAM [--engines chromium,firefox] [--keys] FILE...
    anim_oracle.py selftest --dump PROGRAM FILE

Each FILE (tests/data/svg-anim) names the times it is read at in a comment, `<!-- times: 0 250 1000 -->` (milliseconds). For each time
this library's style dump (tools/doc-oracle/style_dump.c with `--time`) gives the computed values of every element with an id, and the
pinned Chromium and Firefox (tools/oracle/containers/browser, the style oracle's image) give `getComputedStyle()` of the same elements
once the document is paused at that time (tools/doc-oracle/anim_driver.mjs). The comparison, the treatment of `use` and the list of
properties are the style oracle's own (style_oracle.py): this script adds the time and nothing else.

Every difference is a key `property:what` that tools/doc-oracle/anim_divergences.txt must classify, by the style oracle's rules: a
difference that is not listed, a listed key that does not occur and a count that is not the listed one all fail.

What is not a computed style is not compared here: `animateMotion` (a transform outside the element's, not a property), an attribute that
is not a presentation attribute (`points`, `viewBox`, `href`) and `discard`. The unit tests check those against the values both browsers
were seen to give (tests/unit/test_lower_animate.cpp).

`selftest` shows that the check can fail: our side read 100 ms away from the browsers' time must differ.

The oracle is the pinned image; without it `check` prints SKIPPED, and fails under GHOTI_ORACLE_REQUIRED=1.
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "oracle"))

import oracle_env  # noqa: E402
import style_oracle as so  # noqa: E402

LIST = os.path.join(HERE, "anim_divergences.txt")
DRIVER = os.path.join(HERE, "anim_driver.mjs")
STYLE_DRIVER_DIR = os.path.join(os.path.dirname(HERE), "oracle", "containers", "browser")


def times_of(path):
    with open(path, encoding="utf-8") as handle:
        m = re.search(r"<!--\s*times:\s*([0-9.\s]+)-->", handle.read())
    if not m:
        raise SystemExit("anim_oracle: %s names no times (<!-- times: ... -->)" % path)
    return [float(t) for t in m.group(1).split()]


def ours(program, path, time):
    w, h = so.VIEWPORT.split("x")
    done = subprocess.run([program, path, w, h, "--time", repr(time)], capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit("anim_oracle: %s failed on %s at %s: %s" % (program, path, time, done.stderr.strip()))
    out = {}
    for line in done.stdout.split("\n"):
        if line:
            f = line.split("\t")
            out[(f[0], f[1])] = (f[2], f[3] if len(f) > 3 else "")
    return out


def ask(engine, docs):
    argv = ["sh", "-c", "cd /opt/browser && BROWSER_VIEWPORT=%s exec node %s %s light" % (so.VIEWPORT, DRIVER, engine)]
    cmd = oracle_env.command("browser", argv, ro=[HERE, STYLE_DRIVER_DIR])
    payload = "\n".join(json.dumps(d) for d in docs) + "\n"
    done = subprocess.run(cmd, input=payload, capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit("anim_oracle: the browser driver failed (%s): %s" % (engine, done.stderr[-500:]))
    lines = [l for l in done.stdout.split("\n") if l]
    answers = [json.loads(l) for l in lines[1:]]
    if len(answers) != len(docs):
        raise SystemExit("anim_oracle: %d answers for %d documents" % (len(answers), len(docs)))
    return answers


def run(program, files, engines, props, shift=0.0):
    """{engine: {(fixture, time, id, property): (verdict, detail)}}."""
    names = [p for p in props if p not in so.SKIPPED]
    jobs = []
    for path in files:
        flat_text, ids = so.flatten(ET.parse(path).getroot())
        queries = [[i, sorted({so.browser_name(p, props[p][1]) for p in names})] for i in ids]
        for t in times_of(path):
            jobs.append((path, t, ids, {"mode": "x", "text": flat_text, "time": t, "queries": queries}))
    results = {}
    for engine in engines:
        answers = ask(engine, [j[3] for j in jobs])
        verdicts = {}
        for (path, t, ids, _), answer in zip(jobs, answers):
            mine = ours(program, path, t + shift)
            browser = {(i, p): v for i, p, v in answer}
            for i in ids:
                for p in names:
                    key = (os.path.basename(path), t, i, p)
                    if (i, p) not in mine:
                        verdicts[key] = ("missing", "the dump has no line")
                        continue
                    got = browser.get((i, so.browser_name(p, props[p][1])))
                    if not got or got.startswith("!"):
                        verdicts[key] = ("skip", "browser-no-value")
                        continue
                    r = so.compare(props[p][0], mine[(i, p)][0], got)
                    if p == "transform" and r is None:
                        r = so.compare_origin(mine[(i, p)][0], browser.get((i, "transform-origin"), ""))
                    if r is None:
                        verdicts[key] = ("equal", "")
                    elif isinstance(r, tuple):
                        verdicts[key] = ("skip", r[1])
                    else:
                        verdicts[key] = ("differ", r)
        results[engine] = verdicts
    return results


def classified():
    out = {}
    if not os.path.exists(LIST):
        return out
    with open(LIST, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            if not line.strip() or line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) != 4:
                raise SystemExit("anim_oracle: %s:%d: not four tab-separated fields" % (LIST, number))
            out[f[0]] = (f[1], f[2], int(f[3]))
    return out


def unavailable():
    try:
        oracle_env.check_pin("browser")
    except oracle_env.OracleUnavailable as err:
        if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1":
            sys.stderr.write("anim-oracle: the browser oracle is required and unavailable: %s\n" % err)
            return 1
        print("anim-oracle: SKIPPED (%s); nothing was checked" % err)
        return 0
    return None


def main_check(args):
    skip = unavailable()
    if skip is not None:
        return skip
    props = so.properties()
    engines = args.engines.split(",")
    results = run(args.dump, args.files, engines, props)
    summary = collections.Counter()
    differences = collections.defaultdict(list)
    for engine, verdicts in results.items():
        for (name, t, i, p), (verdict, detail) in verdicts.items():
            summary[(engine, verdict)] += 1
            if verdict in ("differ", "missing"):
                differences[(p, so.abstract(detail))].append((name, t, i, detail, engine))
    for engine in engines:
        print("anim-oracle %s: %s" % (engine, ", ".join("%d %s" % (summary[(engine, v)], v) for v in ("equal", "differ", "skip", "missing"))))
    if args.keys:
        for key in sorted(differences):
            rows = differences[key]
            print("KEY\t%s:%s\t%d\te.g. %s" % (key[0], key[1], len(rows), rows[0]))
    listed = classified()
    problems = so.classify(differences, listed)
    for problem in problems:
        sys.stderr.write("anim-oracle: %s\n" % problem)
    ours_listed = [k for k, v in listed.items() if v[0] == "ours"]
    for key in ours_listed:
        sys.stderr.write("anim-oracle: %s is listed as a defect of this library\n" % key)
    if not problems and not ours_listed:
        print("anim-oracle: every difference is classified (%d keys, %d classified differences)" % (len(listed), sum(v[2] for v in listed.values())))
    return 1 if problems or ours_listed else 0


def selftest(args):
    skip = unavailable()
    if skip is not None:
        return skip
    props = so.properties()
    results = run(args.dump, [args.file], ["chromium"], props, shift=100.0)
    differ = sum(1 for v in results["chromium"].values() if v[0] == "differ")
    if differ == 0:
        sys.stderr.write("anim-oracle: selftest: our side read 100 ms away from the browsers' time agrees with them; the check cannot see time\n")
        return 1
    print("anim-oracle: selftest: our side read 100 ms away differs in %d values" % differ)
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("check")
    c.add_argument("--dump", required=True)
    c.add_argument("--engines", default="chromium,firefox")
    c.add_argument("--keys", action="store_true")
    c.add_argument("files", nargs="+")
    t = sub.add_parser("selftest")
    t.add_argument("--dump", required=True)
    t.add_argument("file")
    args = ap.parse_args()
    return main_check(args) if args.mode == "check" else selftest(args)


if __name__ == "__main__":
    sys.exit(main())
