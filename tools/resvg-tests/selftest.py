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
"""The conformance harness, shown to work and shown to fail.

    selftest.py --runner EXE --work DIR

A scorer that has never produced a PASS, or never been seen to reject a bad
case table, is an untested scorer. This builds a synthetic corpus (SVG files
with PNG references, 16 by 16) and checks, each by its outcome and its numbers:

  * the runner's outcomes, one case each: PASS (the planted lowering that draws
    the reference exactly), REFUSED (one that draws it exactly and also refuses
    an element), FAIL (one row too tall), NOT_LOADED, ERROR (a reference that
    does not decode), EXCLUDED, and the sink's report, and what the library itself gives;
  * the tolerance: the same off-by-one drawing passes under a tolerance that
    admits it and fails under zero, and a tolerance that is nonzero with no
    evidence is refused;
  * every check of the scorer, each on its plant: a wrong case count, a case
    with no reference, each malformed line of the case table;
  * a runner that reports a case twice, never, or one it was not asked about;
  * that two runs give the same bytes;
  * the oracle wrapper: exit 2 for a missing engine, an unknown oracle and a
    version mismatch, and the oracle's own status passed through. Checks that
    need a container engine and the built images are SKIPPED, counted, and fail
    instead under GHOTI_ORACLE_REQUIRED=1.

Exit status is nonzero on the first failed check.
"""

import argparse
import contextlib
import io
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import score  # noqa: E402

ORACLE_EXEC = os.path.join(os.path.dirname(HERE), "oracle", "oracle-exec")
SVG = (b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">'
       b'<path d="M2 2H10V10H2Z" fill="#ff0000"/></svg>\n')

# No size of its own: it is laid out in the viewport the runner gives it, the size of the reference.
SVG_SIZED_BY_VIEWPORT = (b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">'
                         b'<path d="M2 2H10V10H2Z" fill="#ff0000"/></svg>\n')

passed = 0
skipped = 0


def check(condition, what):
    global passed
    if not condition:
        sys.stderr.write("check-conformance: FAILED: %s\n" % what)
        sys.exit(1)
    passed += 1


def skip(what, why):
    global skipped
    if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1":
        sys.stderr.write("check-conformance: FAILED (GHOTI_ORACLE_REQUIRED=1): %s needs %s\n" % (what, why))
        sys.exit(1)
    skipped += 1
    print("check-conformance: SKIPPED %s (%s)" % (what, why))


def png(width, height, pixels):
    """A PNG file of straight RGBA8 `pixels` (rows of width*4 bytes)."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def rect_reference(transparent_colour=False):
    px = bytearray(16 * 16 * 4)
    if transparent_colour:
        # Fully transparent pixels whose colour channels are not zero: the same picture.
        px[(14 * 16 + 14) * 4:(14 * 16 + 14) * 4 + 4] = bytes([5, 6, 7, 0])
    for y in range(2, 10):
        for x in range(2, 10):
            px[(y * 16 + x) * 4:(y * 16 + x) * 4 + 4] = bytes([255, 0, 0, 255])
    return png(16, 16, bytes(px))


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


def build_corpus(root):
    ref = rect_reference()
    for case, body, reference in (
            ("s/drawn.svg", SVG, ref),
            ("s/transp.svg", SVG_SIZED_BY_VIEWPORT, rect_reference(True)),
            ("s/notsvg.svg", b"this is not xml\n", ref),
            ("s/badref.svg", SVG, b"not a png"),
            ("x/skipped.svg", SVG, ref)):
        write(os.path.join(root, "tests", case), body)
        write(os.path.join(root, "tests", case[:-4] + ".png"), reference)


def run_score(runner, corpus, out, cases, stub=None, tolerance=None, expected=5, expect_ok=True):
    argv = ["--corpus", corpus, "--runner", runner, "--out", out, "--cases", cases, "--accepted", "/nonexistent/ACCEPTED", "--expected", str(expected),
            "--tolerance", tolerance or os.path.join(HERE, "TOLERANCE")]
    if stub:
        argv += ["--stub", stub]
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        code = score.main(argv)
    if expect_ok:
        check(code == 0, "score.py ran (stub %s)" % stub)
    return code


def table(out):
    rows = {}
    with open(os.path.join(out, "cases.tsv"), encoding="utf-8") as handle:
        for line in handle:
            case, outcome, detail = line.rstrip("\n").split("\t")
            rows[case] = (outcome, detail)
    return rows


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args(argv)
    work = os.path.abspath(args.work)
    shutil.rmtree(work, ignore_errors=True)
    corpus = os.path.join(work, "corpus")
    build_corpus(corpus)
    cases_file = os.path.join(work, "CASES")
    write(cases_file, b"x/*\tOUT_OF_SCOPE\tplanted exclusion for the self test\n")
    runner = os.path.abspath(args.runner)
    zero = os.path.join(HERE, "TOLERANCE")

    # --- the runner's outcomes, one planted lowering at a time -----------------
    out = os.path.join(work, "out-exact")
    run_score(runner, corpus, out, cases_file, stub="exact")
    rows = table(out)
    check(rows["s/drawn.svg"] == ("PASS", "maxdiff=0 bad=0/256"), "the exact drawing is a PASS: %r" % (rows["s/drawn.svg"],))
    check(rows["s/transp.svg"][0] == "PASS", "two fully transparent pixels are equal whatever their colour channels hold")
    check(rows["s/notsvg.svg"][0] == "NOT_LOADED", "a file that is not SVG is NOT_LOADED")
    check(rows["s/badref.svg"][0] == "ERROR", "a reference that does not decode is an ERROR")
    check(rows["x/skipped.svg"][0] == "EXCLUDED", "a case a rule names is EXCLUDED, never run")

    out = os.path.join(work, "out-refused")
    run_score(runner, corpus, out, cases_file, stub="exact-refused")
    rows = table(out)
    check(rows["s/drawn.svg"][0] == "REFUSED" and rows["s/drawn.svg"][1].startswith("refusals=1 first=rect:1"),
          "a drawing that is exactly right and also refuses an element is REFUSED, not PASS: %r" % (rows["s/drawn.svg"],))

    out = os.path.join(work, "out-dropped")
    run_score(runner, corpus, out, cases_file, stub="exact-dropped")
    check(table(out)["s/drawn.svg"][0] == "REFUSED", "a lowering that dropped diagnostics past the cap is REFUSED, not trusted")

    near = os.path.join(work, "TOLERANCE-near")
    write(near, b"max-diff\t2\nmax-bad-ppm\t0\nevidence\tthe self test's own planted row\n")
    out = os.path.join(work, "out-minus1")
    run_score(runner, corpus, out, cases_file, stub="minus1", tolerance=near)
    check(table(out)["s/drawn.svg"] == ("PASS", "maxdiff=1 bad=0/256"), "a difference of one level is not a bad pixel: %r" % (table(out)["s/drawn.svg"],))
    out = os.path.join(work, "out-minus2")
    run_score(runner, corpus, out, cases_file, stub="minus2", tolerance=near)
    check(table(out)["s/drawn.svg"] == ("FAIL", "maxdiff=2 bad=64/256"), "a difference of two levels is a bad pixel: %r" % (table(out)["s/drawn.svg"],))

    out = os.path.join(work, "out-off")
    run_score(runner, corpus, out, cases_file, stub="off-by-one")
    rows = table(out)
    check(rows["s/drawn.svg"] == ("FAIL", "maxdiff=255 bad=8/256"), "a drawing one row too tall is a FAIL with its numbers: %r" % (rows["s/drawn.svg"],))

    out = os.path.join(work, "out-sink")
    run_score(runner, corpus, out, cases_file, stub="sink-refused")
    rows = table(out)
    check(rows["s/drawn.svg"] == ("REFUSED", "sink-report=1 first=unplaced"),
          "a drawing the sink reports it did not draw is REFUSED, not PASS, even when the pixels agree: %r" % (rows["s/drawn.svg"],))

    out = os.path.join(work, "out-real")
    run_score(runner, corpus, out, cases_file)
    rows = table(out)
    check(rows["s/drawn.svg"] == ("PASS", "maxdiff=0 bad=0/256"),
          "the library's own lowering draws the planted document exactly: %r" % (rows["s/drawn.svg"],))
    check(rows["s/transp.svg"][0] == "PASS",
          "and the document with no size, which is laid out in the reference's size, whose reference has a transparent pixel with colour")
    with open(os.path.join(out, "score"), encoding="utf-8") as handle:
        text = handle.read()
    check("over all cases) 2/5 40.00%" in text, "the headline is over every case: 2/5")
    check("not the score: determined correct over all cases but the excluded 2/4" in text, "the second ratio is printed, labelled")

    # --- the reviewed rows: ACCEPTED ----------------------------------------------
    off_seen = "maxdiff=255 bad=8/256"
    row = "s/drawn.svg\tUNDEFINED\tthe planted evidence of the self test\tthe planted reason of the self test\t%s\n"
    accepted_file = os.path.join(work, "ACCEPTED")
    write(accepted_file, (row % off_seen).encode())

    def run_accepted(stub, rows_text, expect_ok=True, name="out-acc"):
        write(accepted_file, rows_text.encode())
        argv = ["--corpus", corpus, "--runner", runner, "--out", os.path.join(work, name), "--cases", cases_file,
                "--accepted", accepted_file, "--expected", "5"]
        if stub:
            argv += ["--stub", stub]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = score.main(argv)
        if expect_ok:
            check(code == 0, "score.py ran with an ACCEPTED file (stub %s)" % stub)
        return code

    run_accepted("off-by-one", row % off_seen)
    check(table(os.path.join(work, "out-acc"))["s/drawn.svg"][0] == "ACCEPTED", "a failing case with a reviewed row is ACCEPTED")
    with open(os.path.join(work, "out-acc", "score"), encoding="utf-8") as handle:
        text = handle.read()
    check("determined correct, PASS + ACCEPTED, over all cases) 1/5 20.00%" in text, "the headline counts the accepted: 1/5")
    check("of which PASS (within the tolerance of the reference) 0/5 0.00%" in text and "of which ACCEPTED (a reviewed row, in ACCEPTED) 1/5 20.00%" in text,
          "PASS and ACCEPTED are printed apart")
    check(run_accepted("exact", row % "maxdiff=0 bad=0/256", expect_ok=False, name="o6") == 1, "a row for a case that passes is refused")
    check(run_accepted("off-by-one", row % "maxdiff=254 bad=8/256", expect_ok=False, name="o6") == 1,
          "a row whose report no longer matches the drawing is refused")
    check(run_accepted("off-by-one", (row % off_seen) + (row % off_seen), expect_ok=False, name="o6") == 1, "a case listed twice is refused")
    for name, body in {
            "four fields": "s/drawn.svg\tUNDEFINED\tthe planted evidence of the self test\tthe planted reason of the self test\n",
            "unknown kind": (row % off_seen).replace("UNDEFINED", "MAYBE"),
            "short evidence": (row % off_seen).replace("the planted evidence of the self test", "too short"),
            "short reason": (row % off_seen).replace("the planted reason of the self test", "too short"),
            "no report": (row % off_seen).replace(off_seen, " "),
            "a glob": (row % off_seen).replace("s/drawn.svg", "s/*"),
            "a missing case": (row % off_seen).replace("s/drawn.svg", "s/gone.svg"),
            "an excluded case": (row % off_seen).replace("s/drawn.svg", "x/skipped.svg")}.items():
        check(run_accepted("off-by-one", body, expect_ok=False, name="o6") == 1, "an ACCEPTED row with %s is refused" % name)
    check(run_accepted(None, "", name="o7") == 0, "an empty ACCEPTED file is fine")
    os.remove(accepted_file)

    # --- the tolerance ----------------------------------------------------------
    loose = os.path.join(work, "TOLERANCE-loose")
    write(loose, b"max-diff\t255\nmax-bad-ppm\t31250\nevidence\tthe self test's own planted row\n")
    out = os.path.join(work, "out-loose")
    run_score(runner, corpus, out, cases_file, stub="off-by-one", tolerance=loose)
    check(table(out)["s/drawn.svg"][0] == "PASS", "the off-by-one drawing passes a tolerance that admits it (8 bad of 256)")
    below = os.path.join(work, "TOLERANCE-below")
    write(below, b"max-diff\t254\nmax-bad-ppm\t1000000\nevidence\tthe self test's own planted row\n")
    out = os.path.join(work, "out-below")
    run_score(runner, corpus, out, cases_file, stub="off-by-one", tolerance=below)
    check(table(out)["s/drawn.svg"][0] == "FAIL", "a largest difference of 255 fails a max-diff of 254, whatever the bad pixel share")
    tight = os.path.join(work, "TOLERANCE-tight")
    write(tight, b"max-diff\t255\nmax-bad-ppm\t31249\nevidence\tthe self test's own planted row\n")
    out = os.path.join(work, "out-tight")
    run_score(runner, corpus, out, cases_file, stub="off-by-one", tolerance=tight)
    check(table(out)["s/drawn.svg"][0] == "FAIL", "and fails one part per million short of admitting it")
    noevidence = os.path.join(work, "TOLERANCE-noevidence")
    write(noevidence, b"max-diff\t1\nmax-bad-ppm\t0\nevidence\tnone\n")
    check(run_score(runner, corpus, os.path.join(work, "out-ne"), cases_file, tolerance=noevidence, expect_ok=False) == 1,
          "a nonzero tolerance with no evidence is refused")

    # --- the scorer's checks, each on its plant --------------------------------
    check(run_score(runner, corpus, os.path.join(work, "o1"), cases_file, expected=6, expect_ok=False) == 1,
          "a case count different from the pin is refused")
    os.remove(os.path.join(corpus, "tests", "s", "drawn.png"))
    check(run_score(runner, corpus, os.path.join(work, "o2"), cases_file, expect_ok=False) == 1,
          "a case with no reference PNG is refused")
    write(os.path.join(corpus, "tests", "s", "drawn.png"), rect_reference())
    plants = {
        "two fields": b"x/*\tOUT_OF_SCOPE\n",
        "unknown class": b"x/*\tMAYBE\tplanted exclusion for the self test\n",
        "short reason": b"x/*\tOUT_OF_SCOPE\ttoo short\n",
        "empty glob": b"nothing/*\tOUT_OF_SCOPE\tplanted exclusion for the self test\n",
        "missing case": b"x/gone.svg\tENVIRONMENT\tplanted exclusion for the self test\n",
        "glob outside OUT_OF_SCOPE": b"x/*\tENVIRONMENT\tplanted exclusion for the self test\n",
        "duplicate rule": b"x/*\tOUT_OF_SCOPE\tplanted exclusion for the self test\nx/*\tOUT_OF_SCOPE\tplanted exclusion for the self test\n",
        "a case in two rules": b"x/*\tOUT_OF_SCOPE\tplanted exclusion for the self test\nx/skipped.svg\tENVIRONMENT\tplanted exclusion for the self test\n",
    }
    for name, body in plants.items():
        bad = os.path.join(work, "CASES-bad")
        write(bad, body)
        check(run_score(runner, corpus, os.path.join(work, "o3"), bad, expect_ok=False) == 1,
              "a case table with %s is refused" % name)

    # --- a runner that misreports ----------------------------------------------
    echo_all = "#!/bin/sh\nwhile read -r c; do printf '%s\\tPASS\\tx\\n' \"$c\"; done < \"$4\"\n"
    echo_but_first = "#!/bin/sh\nn=0\nwhile read -r c; do n=$((n+1)); [ $n -gt 1 ] && printf '%s\\tPASS\\tx\\n' \"$c\"; done < \"$4\"\n"
    for name, body in (
            ("twice", echo_all + "head -n 1 \"$4\" | while read -r c; do printf '%s\\tPASS\\tx\\n' \"$c\"; done\n"),
            ("never", echo_but_first),
            ("unasked", echo_all + "printf 'zz/other.svg\\tPASS\\tx\\n'\n"),
            ("unknown outcome", "#!/bin/sh\nprintf 's/drawn.svg\\tEXCLUDED\\tx\\n'\n")):
        fake = os.path.join(work, "fake-runner")
        write(fake, body.encode())
        os.chmod(fake, 0o755)
        check(run_score(fake, corpus, os.path.join(work, "o4"), cases_file, expect_ok=False) == 1,
              "a runner that reports a case %s is refused" % name)
    fake = os.path.join(work, "fake-runner")
    write(fake, echo_all.encode())
    check(run_score(fake, corpus, os.path.join(work, "o5"), cases_file) == 0, "a runner that reports every case once is accepted")

    # --- repeatability ----------------------------------------------------------
    a, b = os.path.join(work, "rep-a"), os.path.join(work, "rep-b")
    run_score(runner, corpus, a, cases_file, stub="exact")
    run_score(runner, corpus, b, cases_file, stub="exact")
    for name in ("score", "cases.tsv"):
        check(open(os.path.join(a, name), "rb").read() == open(os.path.join(b, name), "rb").read(),
              "two runs of the same tree give the same %s bytes" % name)

    # --- --compare --------------------------------------------------------------
    one = os.path.join(work, "one.png")
    two = os.path.join(work, "two.png")
    write(one, rect_reference())
    px = bytearray(16 * 16 * 4)
    px[0:4] = bytes([10, 20, 30, 255])
    write(two, png(16, 16, bytes(px)))
    done = subprocess.run([runner, "--compare", one, one], capture_output=True, text=True)
    check(done.stdout.strip() == "maxdiff=0 bad=0 total=256", "--compare of a file with itself: %r" % done.stdout)
    done = subprocess.run([runner, "--compare", one, two], capture_output=True, text=True)
    check(done.stdout.strip() == "maxdiff=255 bad=65 total=256", "--compare of two different files: %r" % done.stdout)

    # --- the oracle wrapper -----------------------------------------------------
    env = dict(os.environ)
    env["GHOTI_CONTAINER_ENGINE"] = "no-such-engine-here"
    done = subprocess.run([sys.executable, ORACLE_EXEC, "resvg", "--", "true"], env=env, capture_output=True, text=True)
    check(done.returncode == 2 and "no-such-engine-here" in done.stderr, "a missing engine is exit 2 with the reason")
    done = subprocess.run([sys.executable, ORACLE_EXEC, "no-such-oracle", "--", "true"], capture_output=True, text=True)
    check(done.returncode == 2, "an oracle with no pin is exit 2")
    engine = os.environ.get("GHOTI_CONTAINER_ENGINE", "podman")
    have = False
    if shutil.which(engine):
        have = subprocess.run([sys.executable, ORACLE_EXEC, "--check", "resvg"], capture_output=True).returncode == 0
    if not have:
        skip("the oracle wrapper's checks against a built image", "a container engine and `make oracle-images`")
    else:
        done = subprocess.run([sys.executable, ORACLE_EXEC, "resvg", "--", "false"], capture_output=True, text=True)
        check(done.returncode == 1, "an oracle that runs and returns non-zero is its own status, not exit 2 (got %d)" % done.returncode)
        mismatch = os.path.join(work, "IMAGES-mismatch")
        with open(os.path.join(os.path.dirname(HERE), "oracle", "containers", "IMAGES"), encoding="utf-8") as handle:
            lines = handle.read().replace("resvg 0.45.1", "resvg 9.9.9")
        write(mismatch, lines.encode())
        env = dict(os.environ)
        env["GHOTI_ORACLE_IMAGES"] = mismatch
        done = subprocess.run([sys.executable, ORACLE_EXEC, "resvg", "--", "true"], env=env, capture_output=True, text=True)
        check(done.returncode == 2 and "9.9.9" in done.stderr, "a version that does not match IMAGES is exit 2 naming both")
    print("check-conformance: %d checks passed, %d skipped" % (passed, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
