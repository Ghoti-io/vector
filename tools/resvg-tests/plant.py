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
"""Plant a defect in the harness and require its self-test to fail.

    plant.py --build 'CC ... -o {out} {sources} ...' --work DIR

A self-test that has never been seen to fail proves nothing. Each plant is an
exact replacement in a copy of the runner's or the scorer's source, with its
anchor asserted to match (an edit that did not apply is not a plant). The
copy is built, the self-test is run against it, and the plant counts as
caught only when the self-test exits non-zero; a plant whose copy does not
build is an error, never a catch. The control, the unmodified copy, must pass.

--build is the command that compiles the runner, with {out} and {sources}
standing for the output file and the source files.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# (name, file, anchor, replacement)
PLANTS = [
    ("a refusal is ignored", "tests/conformance/runner.c",
     "if (refusals > 0 || diag.dropped > 0) {", "if (0) {"),
    ("dropped diagnostics are not a refusal", "tests/conformance/runner.c",
     "refusals > 0 || diag.dropped > 0", "refusals > 0"),
    ("the largest difference is not compared", "tests/conformance/runner.c",
     "d.max_diff <= o->max_diff && d.bad", "d.bad"),
    ("the bad pixel share is not compared", "tests/conformance/runner.c",
     "d.bad * 1000000u <= o->max_bad_ppm * d.total", "1"),
    ("transparent pixels are compared by colour", "tests/conformance/runner.c",
     "pa[3] == 0 && pb[3] == 0", "0"),
    ("a difference of one is bad", "tests/conformance/runner.c",
     "if (worst > 1) {", "if (worst > 0) {"),
    ("a sink's refusal is ignored", "tests/conformance/runner.c",
     "if (unsound > 0) {", "if (0) {"),
    ("the viewport is not the reference's size", "tests/conformance/runner.c",
     "load.viewport_width = (double) gimg_raster_width(reference);", "load.viewport_width = 0;"),
    ("the viewport's height is not the reference's", "tests/conformance/runner.c",
     "load.viewport_height = (double) gimg_raster_height(reference);", "load.viewport_height = 0;"),
    ("a failed load is not reported", "tests/conformance/runner.c",
     'say(v, "NOT_LOADED"', 'say(v, "PASS"'),
    ("the headline denominator drops the excluded", "tools/resvg-tests/score.py",
     'out.append("score (determined correct, PASS + ACCEPTED + EDGE + ORACLE_AGREES, over all cases) %d/%d %s" % (correct, total, pct(correct, total)))',
     'out.append("score (determined correct, PASS + ACCEPTED + EDGE + ORACLE_AGREES, over all cases) %d/%d %s" % (correct, run, pct(correct, run)))'),
    ("a DEFECT counts as correct", "tools/resvg-tests/score.py",
     'tally["EDGE"] + tally["ORACLE_AGREES"]', 'tally["EDGE"] + tally["ORACLE_AGREES"] + tally["DEFECT"]'),
    ("an edge-only FAIL is not counted", "tools/resvg-tests/score.py",
     'counts["ACCEPTED"] + tally["EDGE"]', 'counts["ACCEPTED"] + 0 * tally["EDGE"]'),
    ("a passing case stays accepted", "tools/resvg-tests/score.py",
     'outcome, detail = results[case]\n            if outcome != "FAIL":', 'outcome, detail = results[case]\n            if False:'),
    ("a changed drawing stays accepted", "tools/resvg-tests/score.py",
     "if detail != seen:", "if False:"),
    ("an excluded case can be accepted", "tools/resvg-tests/score.py",
     "            if case in excluded:\n                raise ScoreError(\"%s: %s is excluded, so",
     "            if False:\n                raise ScoreError(\"%s: %s is excluded, so"),
    ("an accepted row needs no reason", "tools/resvg-tests/score.py",
     'if len(reason.split()) < 3:\n                raise ScoreError("%s: a reason of fewer than three words" % where)\n            if not seen.strip():',
     'if False:\n                raise ScoreError("%s: a reason of fewer than three words" % where)\n            if not seen.strip():'),
    ("an unexplained FAIL is allowed", "tools/resvg-tests/score.py",
     'if not row:\n            raise ScoreError("%s is a FAIL', 'if False:\n            raise ScoreError("%s is a FAIL'),
    ("a stale review of a FAIL is kept", "tools/resvg-tests/score.py", 'if row[2] != detail:', "if False:"),
    ("a row for an edge-only FAIL is kept", "tools/resvg-tests/score.py",
     'if row:\n                raise ScoreError("%s has a row in FAILS but differs', 'if False:\n                raise ScoreError("%s has a row in FAILS but differs'),
    ("a row for a case that does not fail is kept", "tools/resvg-tests/score.py",
     'if outcome != "FAIL":\n            if row:', 'if outcome != "FAIL":\n            if False:'),
    ("edge-only ignores the tolerance", "tools/resvg-tests/score.py",
     'return int(over.group(1)) * 1000000 <= ppm * int(total.group(1))', "return True"),
    ("a FAILS row needs no evidence", "tools/resvg-tests/score.py",
     'if len(evidence.split()) < 3:\n                raise ScoreError("%s: evidence of fewer than three words" % where)\n            if not seen.strip():',
     'if False:\n                raise ScoreError("%s: evidence of fewer than three words" % where)\n            if not seen.strip():'),
    ("a kind of FAILS is not checked", "tools/resvg-tests/score.py", 'if kind not in FAIL_KINDS:', "if False:"),
    ("a case listed twice in FAILS is kept", "tools/resvg-tests/score.py", 'if case in rows:', "if False:"),
    ("an accepted row needs no evidence", "tools/resvg-tests/score.py",
     'if len(evidence.split()) < 3:\n                raise ScoreError("%s: evidence of fewer than three words" % where)\n            if len(reason.split()) < 3:\n                raise ScoreError("%s: a reason of fewer than three words" % where)\n            if not seen.strip():\n                raise ScoreError("%s: no report of the runner to hold the row to" % where)\n            if case not in known:\n                raise ScoreError("%s: %r matches no case" % (where, case))\n            if case in excluded:',
     'if False:\n                raise ScoreError("%s: evidence of fewer than three words" % where)\n            if len(reason.split()) < 3:\n                raise ScoreError("%s: a reason of fewer than three words" % where)\n            if not seen.strip():\n                raise ScoreError("%s: no report of the runner to hold the row to" % where)\n            if case not in known:\n                raise ScoreError("%s: %r matches no case" % (where, case))\n            if case in excluded:'),
    ("accepted counts as a pass", "tools/resvg-tests/score.py",
     'correct = counts["PASS"] + counts["ACCEPTED"]', 'correct = counts["PASS"]'),
    ("the pinned count is not checked", "tools/resvg-tests/score.py",
     "if expected is not None and len(cases) != expected:", "if False:"),
    ("a missing reference is not checked", "tools/resvg-tests/score.py",
     'if not os.path.isfile(os.path.join(root, case[:-4] + ".png")):', "if False:"),
    ("a short reason is accepted", "tools/resvg-tests/score.py",
     'if len(reason.split()) < 3:\n                raise ScoreError("%s: a reason of fewer than three words" % where)\n            if rule in seen_rules:',
     'if False:\n                raise ScoreError("%s: a reason of fewer than three words" % where)\n            if rule in seen_rules:'),
    ("a glob is allowed in any class", "tools/resvg-tests/score.py",
     'if has_glob and klass != "OUT_OF_SCOPE":', "if False:"),
    ("a rule that matches nothing is accepted", "tools/resvg-tests/score.py",
     "if not matched:", "if False:"),
    ("a case in two rules is accepted", "tools/resvg-tests/score.py",
     "            for case in matched:\n                if case in excluded:",
     "            for case in matched:\n                if False:"),
    ("a nonzero tolerance needs no evidence", "tools/resvg-tests/score.py",
     'if (diff or ppm) and values["evidence"] in ("", "none"):', "if False:"),
    ("a case reported twice is accepted", "tools/resvg-tests/score.py",
     "if case in got:", "if False:"),
    ("a case never reported is accepted", "tools/resvg-tests/score.py",
     "if missing:", "if False:"),
    ("a case nobody asked about is accepted", "tools/resvg-tests/score.py",
     "if case not in asked:", "if False:"),
    ("an excluded case is scored by the runner", "tools/resvg-tests/score.py",
     'merged[case] = ("EXCLUDED", "%s: %s" % excluded[case])', "merged[case] = results.get(case, ('PASS', 'x'))"),
    ("the oracle wrapper conflates its status", "tools/oracle/oracle-exec",
     "return subprocess.run(cmd, stdin=subprocess.DEVNULL).returncode",
     "return 2 if subprocess.run(cmd, stdin=subprocess.DEVNULL).returncode else 0"),
    ("the oracle version is not checked", "tools/oracle/oracle_env.py",
     "if want not in got:", "if False:"),
]


NEEDS_ORACLE = ("the oracle wrapper conflates its status", "the oracle version is not checked")


def oracle_present():
    done = subprocess.run([sys.executable, os.path.join(ROOT, "tools/oracle/oracle-exec"), "--check", "resvg"],
                          capture_output=True)
    return done.returncode == 0


def copy_tree(dest):
    shutil.rmtree(dest, ignore_errors=True)
    for rel in ("tools/resvg-tests", "tools/oracle", "tests/conformance"):
        shutil.copytree(os.path.join(ROOT, rel), os.path.join(dest, rel),
                        ignore=shutil.ignore_patterns("__pycache__", "renders"))
    # runner.c includes ../../src/core/core_internal.h by relative path.
    shutil.copytree(os.path.join(ROOT, "src"), os.path.join(dest, "src"))
    shutil.copytree(os.path.join(ROOT, "include"), os.path.join(dest, "include"))
    gen = os.path.join(ROOT, "build")
    return gen


def build(template, dest, out):
    sources = " ".join(sorted(os.path.join(dest, "tests/conformance", f)
                              for f in os.listdir(os.path.join(dest, "tests/conformance")) if f.endswith(".c")))
    cmd = template.replace("{out}", out).replace("{sources}", sources).replace("{dir}", dest)
    return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True)


def selftest(dest, runner, work):
    env = dict(os.environ)
    return subprocess.run([sys.executable, os.path.join(dest, "tools/resvg-tests/selftest.py"),
                           "--runner", runner, "--work", work], capture_output=True, text=True, env=env)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args(argv)
    work = os.path.abspath(args.work)
    dest = os.path.join(work, "tree")
    uncaught, errors = [], []

    def attempt(label, edit):
        copy_tree(dest)
        if edit:
            _name, rel, anchor, repl = edit
            path = os.path.join(dest, rel)
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
            if text.count(anchor) != 1:
                errors.append("%s: the anchor matches %d times in %s" % (label, text.count(anchor), rel))
                return None
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text.replace(anchor, repl, 1))
        runner = os.path.join(work, "runner")
        if os.path.exists(runner):
            os.remove(runner)
        built = build(args.build, dest, runner)
        if built.returncode != 0:
            errors.append("%s: the runner did not build:\n%s" % (label, built.stderr[-600:]))
            return None
        return selftest(dest, runner, os.path.join(work, "selftest"))

    control = attempt("control", None)
    if control is None or control.returncode != 0:
        sys.stderr.write("plant: the control (no plant) must pass:\n%s\n" % (
            (control.stderr + control.stdout)[-800:] if control else "\n".join(errors)))
        return 1
    caught = 0
    skipped = 0
    have_oracle = oracle_present()
    for plant in PLANTS:
        if plant[0] in NEEDS_ORACLE and not have_oracle:
            if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1":
                sys.stderr.write("plant: %s needs a container engine and the built oracle images "
                                 "(GHOTI_ORACLE_REQUIRED=1)\n" % plant[0])
                return 1
            skipped += 1
            print("plant: SKIPPED %s (no oracle image)" % plant[0])
            continue
        result = attempt(plant[0], plant)
        if result is None:
            continue
        if result.returncode == 0:
            uncaught.append(plant[0])
        else:
            caught += 1
    for line in errors:
        sys.stderr.write("plant: ERROR %s\n" % line)
    for name in uncaught:
        sys.stderr.write("plant: NOT CAUGHT: %s\n" % name)
    print("plant: %d of %d planted defects caught by the self-test (%d skipped); the control passes"
          % (caught, len(PLANTS) - skipped, skipped))
    return 1 if (uncaught or errors) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
