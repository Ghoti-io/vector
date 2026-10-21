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
"""Measure how far the pixel oracles are from the corpus's references and from each other.

    oracle_tolerance.py --corpus DIR --runner EXE --out DIR [--cases F] [--oracles resvg,inkscape]

The in-scope cases (every case no rule of CASES excludes) are rendered by each
oracle, in its pinned image, at the size of the case's reference; then each
render is compared with the reference, and the oracles with each other, by the
runner's --compare (the largest per-channel difference, and the number of
pixels differing by more than 1/255 in any channel).

This is the floor under every tolerance: no tolerance tighter than the distance
two independent renderers keep from each other can tell a correct drawing from
an incorrect one. The tolerance for the drawing this library produces is set from
this table and from the distance of its own renders, never before, and the row
that supports it is named in TOLERANCE.

Output, in --out: table.tsv (one line per case and pair: case, pair, maxdiff,
bad, total, or a status), and summary.txt (per pair: the count, the share
exact, and the percentiles of maxdiff and of the bad pixels in parts per
million), headed by the oracles' versions and the corpus pin.

Exit status 2 when an oracle cannot be reached; it never records "no oracle"
as a measurement.
"""

import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "oracle"))
import oracle_env  # noqa: E402
import score  # noqa: E402

PAIRS = (("resvg", "reference"), ("inkscape", "reference"), ("resvg", "inkscape"))

# One script per oracle, run inside its image: for each "case width height" line,
# render tests/<case> to <scratch>/<oracle>/<case>.png at that size, 120 s at most.
SCRIPTS = {
    "resvg": 'while read -r c w h; do mkdir -p "$2/resvg/$(dirname "$c")"; '
             'timeout 120 resvg -w "$w" -h "$h" "$1/tests/$c" "$2/resvg/$c.png" 2>/dev/null || '
             'echo "$c" >> "$2/resvg.failed"; done < "$2/list"',
    "inkscape": 'while read -r c w h; do mkdir -p "$2/inkscape/$(dirname "$c")"; '
                'timeout 120 inkscape --export-type=png --export-filename="$2/inkscape/$c.png" '
                '-w "$w" -h "$h" "$1/tests/$c" >/dev/null 2>&1 || '
                'echo "$c" >> "$2/inkscape.failed"; done < "$2/list"',
}


def png_size(path):
    with open(path, "rb") as handle:
        head = handle.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise score.ScoreError("%s is not a PNG" % path)
    return struct.unpack(">II", head[16:24])


def percentile(values, p):
    values = sorted(values)
    if not values:
        return "n/a"
    index = (len(values) - 1) * p // 100
    return str(values[index])


def render_all(oracle, corpus, to_run, scratch):
    listing = os.path.join(scratch, "list")
    with open(listing, "w", encoding="utf-8", newline="\n") as handle:
        for case in to_run:
            w, h = png_size(os.path.join(corpus, "tests", case[:-4] + ".png"))
            handle.write("%s %d %d\n" % (case, w, h))
    for stale in (os.path.join(scratch, oracle + ".failed"),):
        if os.path.exists(stale):
            os.remove(stale)
    cmd = [sys.executable, os.path.join(os.path.dirname(HERE), "oracle", "oracle-exec"),
           "--ro", os.path.abspath(corpus), "--scratch", scratch, oracle, "--", "sh", "-c", SCRIPTS[oracle],
           "sh", os.path.abspath(corpus), scratch]
    done = subprocess.run(cmd, stdin=subprocess.DEVNULL)
    if done.returncode == 2:
        raise oracle_env.OracleUnavailable("oracle-exec could not run %s" % oracle)
    failed = os.path.join(scratch, oracle + ".failed")
    if os.path.exists(failed):
        with open(failed, encoding="utf-8") as handle:
            return set(handle.read().split())
    return set()


def compare(runner, a, b):
    if not (os.path.isfile(a) and os.path.isfile(b)):
        return None
    done = subprocess.run([runner, "--compare", a, b], capture_output=True, text=True)
    out = done.stdout.strip()
    if done.returncode == 0 and out.startswith("maxdiff="):
        fields = dict(f.split("=") for f in out.split())
        return int(fields["maxdiff"]), int(fields["bad"]), int(fields["total"])
    return out or "error"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cases", default=os.path.join(HERE, "CASES"))
    ap.add_argument("--oracles", default="resvg,inkscape")
    ap.add_argument("--limit", type=int, default=0, help="render only the first N in-scope cases (a trial)")
    args = ap.parse_args(argv)
    oracles = args.oracles.split(",")
    try:
        versions_file = os.path.join(HERE, "VERSIONS")
        expected = int(score.read_record(versions_file, "expected-cases"))
        pin = score.read_record(versions_file, "resvg-test-suite-commit")
        cases = score.population(args.corpus, expected)
        excluded = score.parse_cases(args.cases, cases)
        to_run = [c for c in cases if c not in excluded]
        if args.limit:
            to_run = to_run[:args.limit]
        found = {name: oracle_env.check_pin(name) for name in oracles}
        os.makedirs(args.out, exist_ok=True)
        scratch = os.path.abspath(os.path.join(args.out, "renders"))
        os.makedirs(scratch, exist_ok=True)
        failed = {name: render_all(name, args.corpus, to_run, scratch) for name in oracles}
    except oracle_env.OracleUnavailable as err:
        sys.stderr.write("oracle-tolerance: %s\n" % err)
        return 2
    except score.ScoreError as err:
        sys.stderr.write("oracle-tolerance: %s\n" % err)
        return 1

    def path_of(side, case):
        if side == "reference":
            return os.path.join(args.corpus, "tests", case[:-4] + ".png")
        return os.path.join(scratch, side, case + ".png")

    rows = []
    stats = {pair: [] for pair in PAIRS if pair[0] in oracles and (pair[1] == "reference" or pair[1] in oracles)}
    bad_status = {pair: 0 for pair in stats}
    for case in to_run:
        for pair in stats:
            if case in failed.get(pair[0], ()) or case in failed.get(pair[1], ()):
                rows.append((case, pair, "oracle-error"))
                bad_status[pair] += 1
                continue
            got = compare(args.runner, path_of(pair[0], case), path_of(pair[1], case))
            if isinstance(got, tuple):
                rows.append((case, pair, got))
                stats[pair].append(got)
            else:
                rows.append((case, pair, got or "missing"))
                bad_status[pair] += 1
    with open(os.path.join(args.out, "table.tsv"), "w", encoding="utf-8", newline="\n") as handle:
        for case, pair, got in rows:
            label = "%s-vs-%s" % pair
            if isinstance(got, tuple):
                handle.write("%s\t%s\t%d\t%d\t%d\n" % (case, label, got[0], got[1], got[2]))
            else:
                handle.write("%s\t%s\t%s\n" % (case, label, got))
    lines = ["corpus %s" % pin, "in-scope cases measured %d" % len(to_run)]
    for name in oracles:
        lines.append("oracle %s: %s" % (name, found[name]))
    lines.append("")
    lines.append("%-22s %5s %6s %7s %5s %5s %5s %5s | %8s %8s %8s %8s" % (
        "pair", "n", "status", "exact", "p50", "p90", "p99", "max", "ppm p50", "ppm p90", "ppm p99", "ppm max"))
    for pair, data in stats.items():
        diffs = [d[0] for d in data]
        ppms = [d[1] * 1000000 // d[2] for d in data]
        exact = sum(1 for d in data if d[0] == 0)
        lines.append("%-22s %5d %6d %7d %5s %5s %5s %5s | %8s %8s %8s %8s" % (
            "%s-vs-%s" % pair, len(data), bad_status[pair], exact,
            percentile(diffs, 50), percentile(diffs, 90), percentile(diffs, 99), percentile(diffs, 100),
            percentile(ppms, 50), percentile(ppms, 90), percentile(ppms, 99), percentile(ppms, 100)))
    lines.append("")
    lines.append("n: cases compared; status: cases with no comparison (oracle error, missing render, size mismatch);")
    lines.append("exact: cases with maxdiff 0; p50/p90/p99/max: percentiles of the largest per-channel difference;")
    lines.append("ppm: pixels differing by more than 1/255 in any channel, in parts per million of the pixels.")
    text = "\n".join(lines) + "\n"
    with open(os.path.join(args.out, "summary.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
