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
"""Draft the FAILS rows of the cases under one directory prefix from fresh oracle renders.

    review_fails.py --corpus DIR --runner EXE --out DIR --prefix filters/ [--oracles resvg,inkscape]

Runs the runner over the in-scope cases under the prefix (its report is what a row is held to), renders the
FAILs that are not edge-only with each oracle in its pinned image, and writes `rows.tsv` in FAILS' format with
the kind chosen by the rule below and the distances measured (pixels differing by more than 1/255 in any channel
once composited on white, on the reference's canvas): ours against the reference, against each oracle, and each
oracle against the reference.  A person reads the rows and writes the sentence that follows the colon (the
draft says only which engines agree); nothing is added to FAILS by this script.

    ORACLE_AGREES  ours is within the tolerance's share of pixels of some oracle
    ORACLE_SPLIT   no oracle is within it of ours, and the oracles differ from each other by more than it
    DEFECT         no oracle is within it of ours, and the oracles agree with each other
"""

import argparse
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "oracle"))
import oracle_tolerance  # noqa: E402
import score  # noqa: E402


def load(path):
    img = Image.open(path).convert("RGBA")
    return np.asarray(img).astype(float)


def over_white(a):
    al = a[..., 3:4] / 255.0
    return a[..., :3] * al + 255.0 * (1.0 - al)


def pam(path, size):
    data = open(path, "rb").read().split(b"ENDHDR\n", 1)[1]
    return np.frombuffer(data, dtype=np.uint8).reshape(size[1], size[0], 4).astype(float)


def distance(a, b):
    d = np.abs(over_white(a) - over_white(b)).max(axis=2)
    return int((d > 1.0).sum())


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--oracles", default="resvg,inkscape")
    args = ap.parse_args(argv)
    oracles = args.oracles.split(",")
    versions = os.path.join(HERE, "VERSIONS")
    cases = score.population(args.corpus, int(score.read_record(versions, "expected-cases")))
    excluded = score.parse_cases(os.path.join(HERE, "CASES"), cases)
    tol = score.parse_tolerance(os.path.join(HERE, "TOLERANCE")) if hasattr(score, "parse_tolerance") else (32, 2000)
    accepted = score.parse_accepted(os.path.join(HERE, "ACCEPTED"), cases, excluded)
    to_run = [c for c in cases if c not in excluded and c not in accepted and c.startswith(args.prefix)]
    os.makedirs(args.out, exist_ok=True)
    results = score.run_runner(args.runner, args.corpus, to_run, tol[:2], None, os.path.join(args.out, "work"), 60)
    dump = os.path.join(args.out, "dump")
    os.makedirs(dump, exist_ok=True)
    fails = [c for c in to_run if results[c][0] == "FAIL" and not score.edge_only(results[c][1], tol[1])]
    listing = os.path.join(args.out, "fail-list")
    with open(listing, "w") as h:
        h.write("".join(c + "\n" for c in fails))
    subprocess.run([args.runner, "--corpus", args.corpus, "--list", listing, "--max-diff", str(tol[0]), "--max-bad-ppm", str(tol[1]),
                    "--dump", dump, "--timeout", "60"], stdout=subprocess.DEVNULL, stdin=subprocess.DEVNULL, check=True)
    scratch = os.path.abspath(os.path.join(args.out, "renders"))
    os.makedirs(scratch, exist_ok=True)
    failed = {o: oracle_tolerance.render_all(o, args.corpus, fails, scratch) for o in oracles}
    share = tol[1]
    rows = []
    for c in fails:
        ref_path = os.path.join(args.corpus, "tests", c[:-4] + ".png")
        ref = load(ref_path)
        size = (ref.shape[1], ref.shape[0])
        ours = pam(os.path.join(dump, c.replace("/", "_") + ".pam"), size)
        total = size[0] * size[1]
        d = {"ref": distance(ours, ref)}
        renders = {}
        for o in oracles:
            p = os.path.join(scratch, o, c + ".png")
            if c in failed[o] or not os.path.isfile(p):
                continue
            renders[o] = load(p)
            d[o] = distance(ours, renders[o])
        limit = share * total // 1000000
        close = [o for o in renders if d[o] <= limit]
        names = list(renders)
        apart = any(distance(renders[a], renders[b]) > limit for i, a in enumerate(names) for b in names[i + 1:])
        kind = "ORACLE_AGREES" if close else ("ORACLE_SPLIT" if apart else "DEFECT")
        ours_part = "ours/ref %d" % d["ref"] + "".join(", ours/%s %d" % (o[:3], d[o]) for o in renders)
        oracle_part = "; " + ", ".join("%s/ref %d" % (o[:3], distance(renders[o], ref)) for o in renders)
        verdict = ("ours is within the tolerance of " + " and ".join(close)) if close else (
            "the oracles differ among themselves" if apart else "the oracles agree with each other and not with ours")
        rows.append("%s\t%s\t%s%s: %s\t%s" % (c, kind, ours_part, oracle_part, verdict, results[c][1]))
    with open(os.path.join(args.out, "rows.tsv"), "w") as h:
        h.write("\n".join(rows) + "\n")
    counts = {}
    for r in rows:
        counts[r.split("\t")[1]] = counts.get(r.split("\t")[1], 0) + 1
    print("fails reviewed", len(fails), counts)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
