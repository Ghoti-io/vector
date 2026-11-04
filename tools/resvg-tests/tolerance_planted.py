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
"""Show that the tolerance in TOLERANCE rejects a drawing that is slightly wrong.

    tolerance_planted.py --corpus DIR --runner EXE --out DIR [--cases F]

A tolerance that accepts anything near the reference proves nothing, so the pair is
tested the way the other gates are: a defect is planted and the pair must refuse it.
The pixel oracle `resvg` renders each in-scope case that it already draws within the
tolerance. Each such case is then rendered again with its whole content wrapped in a
group that is slightly wrong (a shift of a tenth of a user unit and more, a growth of
0.2% and more, a fade of 2% and more), or with one small patch laid over it, and that render is compared with the same
reference. A defect only counts where it changed the oracle's own picture by more
than one level; the pair has to refuse every one of those, in every required kind,
and every kind needs enough cases to mean something. The smallest shift (a tenth of
a pixel) is reported and not required: on a smooth gradient it moves a pixel by one
or two levels, which no pixel tolerance can tell from rounding, so it measures where
the pair stops seeing and is not a defect the pair promises to refuse.

Exit status 0 when every planted defect was refused, 1 when one was accepted or a
kind had too few cases, 2 when the oracle cannot be reached.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "oracle"))
import oracle_env  # noqa: E402
import oracle_tolerance as ot  # noqa: E402
import score  # noqa: E402

# Wrappers for the content of the root element; the amounts are in user units, which
# the references scale (they are 2.5 times the size of the viewBox of most cases).
DEFECTS = (
    ("shift-0.04", '<g transform="translate(0.04 0.04)">', False),
    ("shift-0.08", '<g transform="translate(0.08 0.08)">', True),
    ("shift-0.2", '<g transform="translate(0.2 0.2)">', True),
    ("grow-0.2pct", '<g transform="translate(-0.2 -0.2) scale(1.002)">', True),
    ("grow-0.5pct", '<g transform="translate(-0.5 -0.5) scale(1.005)">', True),
    ("fade-2pct", '<g opacity="0.98">', True),
    ("fade-5pct", '<g opacity="0.95">', True),
    # A local defect is one patch laid over the picture, in user units: a patch of 4 by 4 is about 100 pixels of a
    # 500 by 500 reference and one of 16 by 16 about 1,600. The first is wrong by more than the largest difference
    # allows, the second by little over more pixels than the count allows; the third is under both limits, which is
    # what the pair accepts by design, so it is reported and not required. A patch counts only where it moved the oracle's
    # picture by more than twice the limit it is meant to break (so that the oracle's own distance from the reference,
    # which the pair allows, cannot account for the difference): over twice the largest difference, or over twice the
    # count of differing pixels. The patch on a dark gradient moves it by little, and is no defect there.
    ("patch-strong", '+<rect x="50" y="50" width="4" height="4" fill="#000" opacity="0.3"/>', True),
    ("patch-wide", '+<rect x="50" y="50" width="16" height="16" fill="#000" opacity="0.03"/>', True),
    ("patch-faint", '+<rect x="50" y="50" width="4" height="4" fill="#000" opacity="0.03"/>', False),
)
MIN_CASES = 20

RENDER = ('while read -r c w h; do mkdir -p "$2/defout/$(dirname "$c")"; '
          'timeout 120 resvg -w "$w" -h "$h" "$2/def/$c" "$2/defout/$c.png" 2>/dev/null || '
          'echo "$c" >> "$2/def.failed"; done < "$2/deflist"')


def wrap(source, opening):
    start = re.search(r"<svg\b[^>]*[^/]>", source)
    end = source.rfind("</svg>")
    if not start or end < 0 or re.search(r'href\s*=\s*"(?!#)', source):
        return None
    if opening.startswith("+"):  # an element laid over the content, not a group around it
        return source[:end] + opening[1:] + source[end:]
    return source[:start.end()] + opening + source[start.end():end] + "</g>" + source[end:]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cases", default=os.path.join(HERE, "CASES"))
    args = ap.parse_args(argv)
    try:
        versions = os.path.join(HERE, "VERSIONS")
        cases = score.population(args.corpus, int(score.read_record(versions, "expected-cases")))
        excluded = score.parse_cases(args.cases, cases)
        to_run = [c for c in cases if c not in excluded]
        max_diff, max_ppm, _ = score.parse_tolerance(os.path.join(HERE, "TOLERANCE"))
        oracle_env.check_pin("resvg")
        os.makedirs(args.out, exist_ok=True)
        scratch = os.path.abspath(os.path.join(args.out, "renders"))
        os.makedirs(scratch, exist_ok=True)
        failed = ot.render_all("resvg", args.corpus, to_run, scratch)
    except oracle_env.OracleUnavailable as err:
        sys.stderr.write("tolerance-planted: %s\n" % err)
        return 2
    except score.ScoreError as err:
        sys.stderr.write("tolerance-planted: %s\n" % err)
        return 1

    def within(got):
        return isinstance(got, tuple) and got[0] <= max_diff and got[1] * 1000000 // got[2] <= max_ppm

    ref = lambda c: os.path.join(args.corpus, "tests", c[:-4] + ".png")
    clean = {}
    for case in to_run:
        got = ot.compare(args.runner, os.path.join(scratch, "resvg", case + ".png"), ref(case))
        if case not in failed and within(got):
            clean[case] = os.path.join(scratch, "resvg", case + ".png")

    lines = []
    for kind, opening, _ in DEFECTS:
        for case in clean:
            with open(os.path.join(args.corpus, "tests", case), encoding="utf-8", errors="replace") as handle:
                text = wrap(handle.read(), opening)
            if text is None:
                continue
            path = os.path.join(scratch, "def", kind, case)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
            w, h = ot.png_size(ref(case))
            lines.append("%s/%s %d %d\n" % (kind, case, w, h))
    with open(os.path.join(scratch, "deflist"), "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(lines)
    stale = os.path.join(scratch, "def.failed")
    if os.path.exists(stale):
        os.remove(stale)
    cmd = [sys.executable, os.path.join(os.path.dirname(HERE), "oracle", "oracle-exec"),
           "--ro", os.path.abspath(args.corpus), "--scratch", scratch, "resvg", "--", "sh", "-c", RENDER,
           "sh", os.path.abspath(args.corpus), scratch]
    if subprocess.run(cmd, stdin=subprocess.DEVNULL).returncode == 2:
        sys.stderr.write("tolerance-planted: oracle-exec could not run resvg\n")
        return 2

    bad = False
    out = ["tolerance max-diff=%d max-bad-ppm=%d" % (max_diff, max_ppm),
           "cases the oracle draws within it: %d of %d in scope" % (len(clean), len(to_run)), "",
           "%-12s %6s %9s %9s" % ("defect", "cases", "refused", "accepted")]
    for kind, _, required in DEFECTS:
        counted = accepted = 0
        for case, clean_png in clean.items():
            png = os.path.join(scratch, "defout", kind, case + ".png")
            if not os.path.isfile(png):
                continue
            moved = ot.compare(args.runner, png, clean_png)
            if not isinstance(moved, tuple) or moved[0] <= 1:
                continue
            if kind == "patch-strong" and moved[0] <= 2 * max_diff:
                continue
            if kind == "patch-wide" and moved[1] * 1000000 // moved[2] <= 2 * max_ppm:
                continue
            counted += 1
            if within(ot.compare(args.runner, png, ref(case))):
                accepted += 1
                out.append("  accepted: %s %s" % (kind, case))
        out.append("%-12s %6d %9d %9d%s" % (kind, counted, counted - accepted, accepted, "" if required else "  (reported, not required)"))
        if required and (accepted or counted < MIN_CASES):
            bad = True
    out.append("")
    out.append("FAIL: a defect was accepted, or a kind had under %d cases" % MIN_CASES if bad else
               "ok: every required planted defect was refused")
    text = "\n".join(out) + "\n"
    with open(os.path.join(args.out, "tolerance-planted.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    sys.stdout.write(text)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
