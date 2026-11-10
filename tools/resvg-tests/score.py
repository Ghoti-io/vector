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
"""Score vector against the resvg test suite.

    score.py --corpus DIR --runner EXE --out DIR
             [--versions F] [--cases F] [--accepted F] [--tolerance F] [--expected N] [--stub NAME]

The population is every tests/**/*.svg in the corpus, read from the tree. A
case is given exactly one outcome:

    PASS        loaded, lowered with no refusal at all, render within the tolerance
    FAIL        loaded, lowered with no refusal, rendered, outside the tolerance
    REFUSED     loaded, and the lowering reported one or more refusals
    NOT_LOADED  the load failed
    ACCEPTED    rendered outside the tolerance, and a reviewed row of ACCEPTED
                says why the library's drawing is the correct one
    EXCLUDED    a rule of CASES names it (never run, and so never a PASS)
    ERROR       the harness failed on it (decode, render, crash, timeout)

The score is "determined correct": PASS plus ACCEPTED plus the FAILs classed EDGE or ORACLE_AGREES (below), over *all* cases,
EXCLUDED included. The reference is one oracle among several, so a case whose
behavior is undefined, or on which the oracles disagree, or which the
specification's text decides against the reference, is correct when a reviewed
row says so and gives the evidence; PASS and ACCEPTED are always printed apart.
The same ratio over all but the excluded is printed beside it, labelled, and is
never the headline.

An ACCEPTED row is one line of five tab-separated fields: the case, a kind
(UNDEFINED, DISAGREEMENT or SPECIFICATION), the evidence, the reason, and what
the runner reported for the case when it was reviewed. It names a case exactly
(a glob names no case, so it is refused), and it is refused when the case is excluded, when it now passes or is
refused or errors, or when the runner's report has changed since the review: a
row cannot go stale unseen.

The scorer fails (exit 1, the line named) on: a population different from the
pinned count, a case with no reference PNG, a CASES line that is malformed, has
an unknown class, a reason of fewer than three words, a glob outside
OUT_OF_SCOPE, a rule matching no case, a case matched by two rules; a
TOLERANCE that is nonzero with no evidence; and a runner that reports a case
twice, not at all, or that was not asked about.

Every FAIL is explained, or the scorer fails. The runner reports, beside the strict
figures, how many pixels differ by more than the tolerance's level once both rasters
are composited on white ("white-over"). A FAIL within the tolerance on that figure is
EDGE: it differs from the reference along antialiased edges, or in the color of pixels
that are fully transparent in one raster, which the strict comparison of straight RGBA
counts and a viewer cannot see. Every other FAIL needs a reviewed row of FAILS, one
line of four tab-separated fields: the case, a kind (ORACLE_AGREES: a fresh render by a
pinned oracle is within the tolerance of ours; ORACLE_SPLIT: the oracles disagree
among themselves; DEFECT: ours, open), the evidence (the distances, in pixels over
the tolerance's level composited on white, that the kind rests on), and the runner's
report as reviewed. A row for a case that is not a FAIL, that is EDGE, or whose report
has changed is refused, as for ACCEPTED. EDGE and ORACLE_AGREES FAILs count as determined correct
(printed apart); ORACLE_SPLIT and DEFECT never do.

The score file has no timestamp and no path, and lists cases in sorted order,
so two runs of the same tree give the same bytes.
"""

import argparse
import fnmatch
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CLASSES = ("OUT_OF_SCOPE", "ENVIRONMENT", "REFERENCE")
KINDS = ("UNDEFINED", "DISAGREEMENT", "SPECIFICATION")
FAIL_KINDS = ("ORACLE_AGREES", "ORACLE_SPLIT", "DEFECT")
OUTCOMES = ("PASS", "ACCEPTED", "FAIL", "REFUSED", "NOT_LOADED", "EXCLUDED", "ERROR")
RUNNER_OUTCOMES = ("PASS", "FAIL", "REFUSED", "NOT_LOADED", "ERROR")


class ScoreError(Exception):
    """A violation of the scorer's own checks; the message names the line."""


def read_record(path, name):
    """The value of `name` in a VERSIONS-style file (first match wins)."""
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) >= 2 and parts[0] == name and not line.lstrip().startswith("#"):
                return parts[1]
    raise ScoreError("%s: no record %r" % (path, name))


def population(corpus, expected):
    """Every case (a path under tests/, with its .svg), sorted; checked."""
    root = os.path.join(corpus, "tests")
    if not os.path.isdir(root):
        raise ScoreError("%s: no tests/ directory (run tools/resvg-tests/fetch.sh)" % corpus)
    cases = []
    for base, _dirs, files in os.walk(root):
        for name in files:
            if name.endswith(".svg"):
                cases.append(os.path.relpath(os.path.join(base, name), root))
    cases.sort()
    if expected is not None and len(cases) != expected:
        raise ScoreError("the corpus holds %d cases and VERSIONS expects %d; a changed pin is a "
                         "reviewed change, so update expected-cases with it" % (len(cases), expected))
    for case in cases:
        if not os.path.isfile(os.path.join(root, case[:-4] + ".png")):
            raise ScoreError("%s: no reference PNG" % case)
    return cases


def parse_cases(path, cases):
    """The case table: returns {case: (class, reason)} for the excluded cases."""
    known = set(cases)
    excluded = {}
    seen_rules = set()
    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            where = "%s:%d" % (os.path.basename(path), number)
            parts = line.split("\t")
            if len(parts) != 3:
                raise ScoreError("%s: not three tab-separated fields" % where)
            rule, klass, reason = parts
            if klass not in CLASSES:
                raise ScoreError("%s: class %r is not one of %s" % (where, klass, ", ".join(CLASSES)))
            if len(reason.split()) < 3:
                raise ScoreError("%s: a reason of fewer than three words" % where)
            if rule in seen_rules:
                raise ScoreError("%s: rule %r is listed twice" % (where, rule))
            seen_rules.add(rule)
            has_glob = any(c in rule for c in "*?[")
            if has_glob and klass != "OUT_OF_SCOPE":
                raise ScoreError("%s: a glob is allowed for OUT_OF_SCOPE only" % where)
            if has_glob:
                matched = [c for c in cases if fnmatch.fnmatchcase(c, rule)]
            else:
                matched = [rule] if rule in known else []
            if not matched:
                raise ScoreError("%s: rule %r matches no case" % (where, rule))
            for case in matched:
                if case in excluded:
                    raise ScoreError("%s: case %s is already excluded by another rule" % (where, case))
                excluded[case] = (klass, reason)
    return excluded


def parse_accepted(path, cases, excluded):
    """The reviewed rows: {case: (kind, evidence, reason, seen)}."""
    known = set(cases)
    accepted = {}
    if not os.path.isfile(path):
        return accepted
    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            where = "%s:%d" % (os.path.basename(path), number)
            parts = line.split("\t")
            if len(parts) != 5:
                raise ScoreError("%s: not five tab-separated fields" % where)
            case, kind, evidence, reason, seen = parts
            if kind not in KINDS:
                raise ScoreError("%s: kind %r is not one of %s" % (where, kind, ", ".join(KINDS)))
            if len(evidence.split()) < 3:
                raise ScoreError("%s: evidence of fewer than three words" % where)
            if len(reason.split()) < 3:
                raise ScoreError("%s: a reason of fewer than three words" % where)
            if not seen.strip():
                raise ScoreError("%s: no report of the runner to hold the row to" % where)
            if case not in known:
                raise ScoreError("%s: %r matches no case" % (where, case))
            if case in excluded:
                raise ScoreError("%s: %s is excluded, so it is never run and cannot be accepted" % (where, case))
            if case in accepted:
                raise ScoreError("%s: %s is listed twice" % (where, case))
            accepted[case] = (kind, evidence, reason, seen)
    return accepted


def parse_fails(path, cases, excluded, accepted):
    """The reviewed rows for FAILs that are not edge-only: {case: (kind, evidence, seen)}."""
    known = set(cases)
    rows = {}
    if not os.path.isfile(path):
        return rows
    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            where = "%s:%d" % (os.path.basename(path), number)
            parts = line.split("\t")
            if len(parts) != 4:
                raise ScoreError("%s: not four tab-separated fields" % where)
            case, kind, evidence, seen = parts
            if kind not in FAIL_KINDS:
                raise ScoreError("%s: kind %r is not one of %s" % (where, kind, ", ".join(FAIL_KINDS)))
            if len(evidence.split()) < 3:
                raise ScoreError("%s: evidence of fewer than three words" % where)
            if not seen.strip():
                raise ScoreError("%s: no report of the runner to hold the row to" % where)
            if case not in known:
                raise ScoreError("%s: %r matches no case" % (where, case))
            if case in excluded or case in accepted:
                raise ScoreError("%s: %s is excluded or accepted, so it is not a FAIL to explain" % (where, case))
            if case in rows:
                raise ScoreError("%s: %s is listed twice" % (where, case))
            rows[case] = (kind, evidence, seen)
    return rows


def edge_only(detail, ppm):
    """True when a FAIL's report says the composited difference is within the tolerance's share of pixels."""
    over = re.search(r"white-over=(\d+)", detail)
    total = re.search(r"/(\d+)", detail)
    if not over or not total:
        return False
    return int(over.group(1)) * 1000000 <= ppm * int(total.group(1))


def explain_fails(merged, fails, tolerance):
    """Give every FAIL its kind: EDGE by measurement, else a reviewed row. Returns {case: kind}."""
    kinds = {}
    for case, (outcome, detail) in merged.items():
        row = fails.get(case)
        if outcome != "FAIL":
            if row:
                raise ScoreError("%s has a row in FAILS but is %s, so remove the row" % (case, outcome))
            continue
        if edge_only(detail, tolerance[1]):
            if row:
                raise ScoreError("%s has a row in FAILS but differs only at edges (%s), so remove the row" % (case, detail))
            kinds[case] = "EDGE"
            continue
        if not row:
            raise ScoreError("%s is a FAIL (%s) that is not edge-only and has no row in FAILS: review it" % (case, detail))
        if row[2] != detail:
            raise ScoreError("%s is reviewed as %r but the runner now reports %r: the drawing changed since "
                             "the review, so review the row again" % (case, row[2], detail))
        kinds[case] = row[0]
    return kinds


def parse_tolerance(path):
    values = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 2:
                raise ScoreError("%s: not two tab-separated fields: %r" % (os.path.basename(path), line))
            values[parts[0]] = parts[1]
    for key in ("max-diff", "max-bad-ppm", "evidence"):
        if key not in values:
            raise ScoreError("%s: no %s" % (os.path.basename(path), key))
    try:
        diff, ppm = int(values["max-diff"]), int(values["max-bad-ppm"])
    except ValueError:
        raise ScoreError("%s: max-diff and max-bad-ppm are integers" % os.path.basename(path))
    if not (0 <= diff <= 255) or ppm < 0 or ppm > 1000000:
        raise ScoreError("%s: max-diff is 0 to 255 and max-bad-ppm 0 to 1000000" % os.path.basename(path))
    if (diff or ppm) and values["evidence"] in ("", "none"):
        raise ScoreError("%s: a nonzero tolerance needs evidence: the row of the oracle table that "
                         "supports it" % os.path.basename(path))
    return diff, ppm, values["evidence"]


def run_runner(runner, corpus, to_run, tolerance, stub, work, timeout):
    """Run the runner over `to_run`; returns {case: (outcome, detail)}."""
    os.makedirs(work, exist_ok=True)
    listing = os.path.join(work, "run-list")
    with open(listing, "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(c + "\n" for c in to_run)
    argv = [runner, "--corpus", corpus, "--list", listing, "--max-diff", str(tolerance[0]),
            "--max-bad-ppm", str(tolerance[1]), "--timeout", str(timeout)]
    if stub:
        argv += ["--stub", stub]
    done = subprocess.run(argv, stdout=subprocess.PIPE, text=True, stdin=subprocess.DEVNULL)
    if done.returncode != 0:
        raise ScoreError("the runner exited %d" % done.returncode)
    asked = set(to_run)
    got = {}
    for line in done.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) != 3 or parts[1] not in RUNNER_OUTCOMES:
            raise ScoreError("the runner gave a malformed line: %r" % line)
        case, outcome, detail = parts
        if case not in asked:
            raise ScoreError("the runner reported %s, which it was not asked about" % case)
        if case in got:
            raise ScoreError("the runner reported %s twice" % case)
        got[case] = (outcome, detail)
    missing = [c for c in to_run if c not in got]
    if missing:
        raise ScoreError("the runner did not report %d case(s), the first %s" % (len(missing), missing[0]))
    return got


def score(cases, excluded, accepted, results):
    """Merge the exclusions, the reviewed rows and the runner's results into {case: (outcome, detail)}."""
    merged = {}
    for case in cases:
        if case in excluded:
            merged[case] = ("EXCLUDED", "%s: %s" % excluded[case])
        elif case in accepted:
            kind, _evidence, reason, seen = accepted[case]
            outcome, detail = results[case]
            if outcome != "FAIL":
                raise ScoreError("%s is accepted but the runner reports %s (%s): only a drawing outside the "
                                 "tolerance is accepted, so remove the row" % (case, outcome, detail))
            if detail != seen:
                raise ScoreError("%s is accepted as %r but the runner now reports %r: the drawing changed "
                                 "since the review, so review the row again" % (case, seen, detail))
            merged[case] = ("ACCEPTED", "%s: %s: %s" % (kind, reason, detail))
        else:
            merged[case] = results[case]
    return merged


def render(pin, merged, tolerance, kinds):
    """The score file's text."""
    counts = {o: 0 for o in OUTCOMES}
    by_dir = {}
    for case, (outcome, _detail) in merged.items():
        counts[outcome] += 1
        top = "/".join(case.split("/")[:2])
        row = by_dir.setdefault(top, {o: 0 for o in OUTCOMES})
        row[outcome] += 1
    total = len(merged)
    run = total - counts["EXCLUDED"]
    tally = {k: 0 for k in ("EDGE",) + FAIL_KINDS}
    for kind in kinds.values():
        tally[kind] += 1
    correct = counts["PASS"] + counts["ACCEPTED"] + tally["EDGE"] + tally["ORACLE_AGREES"]
    out = ["resvg-test-suite %s" % pin, "cases %d" % total]
    for outcome in OUTCOMES:
        out.append("%-10s %d" % (outcome, counts[outcome]))
    out.append("tolerance max-diff=%d max-bad-ppm=%d evidence=%s" % tolerance)
    out.append("score (determined correct, PASS + ACCEPTED + EDGE + ORACLE_AGREES, over all cases) %d/%d %s" % (correct, total, pct(correct, total)))
    out.append("  of which PASS (within the tolerance of the reference) %d/%d %s" % (counts["PASS"], total, pct(counts["PASS"], total)))
    out.append("  of which ACCEPTED (a reviewed row, in ACCEPTED) %d/%d %s" % (counts["ACCEPTED"], total, pct(counts["ACCEPTED"], total)))
    out.append("  of which EDGE (a FAIL that differs only at antialiased edges, measured) %d/%d %s" % (tally["EDGE"], total, pct(tally["EDGE"], total)))
    out.append("  of which ORACLE_AGREES (a FAIL a fresh oracle render agrees with, reviewed in FAILS) %d/%d %s" % (tally["ORACLE_AGREES"], total, pct(tally["ORACLE_AGREES"], total)))
    if counts["FAIL"]:
        out.append("FAIL, all %d explained: %s" % (counts["FAIL"], ", ".join("%s %d" % (k, tally[k]) for k in tally)))
    out.append("not the score: determined correct over all cases but the excluded %d/%d %s" % (correct, run, pct(correct, run)))
    out.append("")
    out.append("%-34s %6s %6s %6s %6s %6s %6s %6s %6s" % (("directory", "cases") + OUTCOMES))
    for top in sorted(by_dir):
        row = by_dir[top]
        out.append("%-34s %6d %s" % (top, sum(row.values()), " ".join("%6d" % row[o] for o in OUTCOMES)))
    return "\n".join(out) + "\n"


def pct(n, d):
    """n/d as a percentage with two places, from integers only."""
    if d == 0:
        return "n/a"
    hundredths = (n * 10000 * 2 + d) // (d * 2)
    return "%d.%02d%%" % (hundredths // 100, hundredths % 100)


def write_outputs(out, text, merged):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "score"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    with open(os.path.join(out, "cases.tsv"), "w", encoding="utf-8", newline="\n") as handle:
        for case in sorted(merged):
            handle.write("%s\t%s\t%s\n" % (case, merged[case][0], merged[case][1]))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--versions", default=os.path.join(HERE, "VERSIONS"))
    ap.add_argument("--cases", default=os.path.join(HERE, "CASES"))
    ap.add_argument("--accepted", default=os.path.join(HERE, "ACCEPTED"))
    ap.add_argument("--fails", default=os.path.join(HERE, "FAILS"))
    ap.add_argument("--tolerance", default=os.path.join(HERE, "TOLERANCE"))
    ap.add_argument("--expected", type=int, default=None)
    ap.add_argument("--stub", default=None)
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args(argv)
    started = time.monotonic()
    try:
        expected = args.expected if args.expected is not None else int(read_record(args.versions, "expected-cases"))
        pin = read_record(args.versions, "resvg-test-suite-commit")
        cases = population(args.corpus, expected)
        excluded = parse_cases(args.cases, cases)
        accepted = parse_accepted(args.accepted, cases, excluded)
        tolerance = parse_tolerance(args.tolerance)
        to_run = [c for c in cases if c not in excluded]
        results = run_runner(args.runner, args.corpus, to_run, tolerance[:2], args.stub,
                             os.path.join(args.out, "work"), args.timeout)
        merged = score(cases, excluded, accepted, results)
        fails = parse_fails(args.fails, cases, excluded, accepted)
        kinds = explain_fails(merged, fails, tolerance)
        for case, kind in kinds.items():
            merged[case] = ("FAIL", "%s: %s" % (kind, merged[case][1]))
    except ScoreError as err:
        sys.stderr.write("score: %s\n" % err)
        return 1
    text = render(pin, merged, tolerance, kinds)
    write_outputs(args.out, text, merged)
    sys.stdout.write(text)
    sys.stderr.write("score: %d cases in %.1f s\n" % (len(cases), time.monotonic() - started))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
