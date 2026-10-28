#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""Show that the table generator's checks fail on their plants.

    selftest.py [--input DIR]

Needs the pinned pages (tools/svg2/fetch.sh). Without them it prints SKIPPED, counted, and
exits 1 under GHOTI_ORACLE_REQUIRED=1 (a required gate fails rather than skips).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GEN = os.path.join(HERE, "gen-tables.py")


def run(args):
    return subprocess.run([sys.executable, GEN] + args, capture_output=True, text=True)


def main(argv):
    inputs = argv[1] if len(argv) > 1 and argv[0] == "--input" else os.path.join(ROOT, "third_party/svg2")
    if not os.path.isfile(os.path.join(inputs, "attindex.html")):
        if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1":
            sys.stderr.write("check-tables: the pinned pages are missing and GHOTI_ORACLE_REQUIRED=1\n")
            return 1
        print("check-tables: SKIPPED (run tools/svg2/fetch.sh); nothing was checked")
        return 0
    passed = 0

    def expect(done, fragment, what):
        nonlocal passed
        if done.returncode == 0 or fragment not in done.stderr:
            sys.stderr.write("check-tables: FAILED: %s (exit %d, stderr %r)\n" % (what, done.returncode, done.stderr[-300:]))
            sys.exit(1)
        passed += 1

    done = run(["--input", inputs, "--check"])
    if done.returncode != 0:
        sys.stderr.write("check-tables: the control failed:\n%s" % done.stderr)
        return 1
    passed += 1
    with tempfile.TemporaryDirectory() as tmp:
        def pages(mutate):
            d = tempfile.mkdtemp(dir=tmp)
            for name in os.listdir(inputs):
                shutil.copy(os.path.join(inputs, name), os.path.join(d, name))
            mutate(d)
            return d

        def edit(name, fn):
            def go(d):
                path = os.path.join(d, name)
                text = open(path, encoding="utf-8").read()
                new = fn(text)
                assert new != text, "the plant did not change %s" % name
                open(path, "w", encoding="utf-8").write(new)
            return go

        fake = ('<tr><th><span class="attr-name"><a href="x"><span>zzNewAttribute</span></a></span></th><td>'
                '<span class="element-name">‘<a href="y"><span>rect</span></a>’</span></td><td></td></tr>')
        expect(run(["--input", pages(edit("attindex.html", lambda t: t.replace("</tbody>", fake + "</tbody>", 1))), "--check"]),
               "no grammar rule", "an attribute of the index that no rule maps")
        import re
        expect(run(["--input", pages(edit("attindex.html", lambda t: re.sub(r'<tr><th><span class="attr-name"><a[^>]*><span>zoomAndPan</span>.*?</tr>', "", t))), "--check"]),
               "maps no pair", "a rule that maps no pair of the index")
        expect(run(["--input", pages(edit("eltindex.html", lambda t: re.sub(r"<li><span class=\"element-name\">‘<a [^>]*><span>rect</span></a>’</span></li>", "", t))), "--check"]),
               "lacks", "an element of the attribute index that the element index lacks")
        rules = os.path.join(tmp, "rules.tsv")
        text = open(os.path.join(HERE, "grammars.tsv"), encoding="utf-8").read()
        open(rules, "w", encoding="utf-8").write(text + "zzNoSuchAttribute\t*\tNUMBER\tplanted\n")
        expect(run(["--input", inputs, "--check", "--rules", rules]), "maps no pair", "a stale rule")
        open(rules, "w", encoding="utf-8").write(text.replace("RAW_ARIA", "NO_SUCH_GRAMMAR", 1))
        expect(run(["--input", inputs, "--check", "--rules", rules]), "unknown grammar", "a rule naming a grammar the library lacks")
        # The property table: a property of the CR's indexes that the table lacks, a page that grows a property, and malformed rows.
        props = os.path.join(tmp, "properties.tsv")
        ptext = open(os.path.join(HERE, "properties.tsv"), encoding="utf-8").read()
        open(props, "w", encoding="utf-8").write("\n".join(l for l in ptext.split("\n") if not l.startswith("stroke-linecap\t")))
        expect(run(["--input", inputs, "--check", "--properties", props]), "lacks stroke-linecap", "a property of the index that the table lacks")
        fake_property = '<tr><td><a class="property" href="x">zz-new-property</a></td><td>none</td></tr>'
        expect(run(["--input", pages(edit("propidx.html", lambda t: t.replace("</table>", fake_property + "</table>", 1))), "--check"]),
               "lacks zz-new-property", "a property the index grows")
        open(props, "w", encoding="utf-8").write(ptext.replace("\tKEYWORD\t", "\tNOSUCHTYPE\t", 1))
        expect(run(["--input", inputs, "--check", "--properties", props]), "unknown type", "a property of a type the library lacks")
        open(props, "w", encoding="utf-8").write(ptext.replace("alignment-baseline\tKEYWORD", "alignment-baseline\tKEYWORD\textra", 1))
        expect(run(["--input", inputs, "--check", "--properties", props]), "not nine", "a row of the wrong width")
        open(props, "w", encoding="utf-8").write(ptext.replace("color-rendering\tKEYWORD\t-\t\tR\tyes\tauto\tauto | optimizeSpeed | optimizeQuality\t", "color-rendering\tKEYWORD\t-\t\tR\t\t\t\t", 1))
        expect(run(["--input", inputs, "--check", "--properties", props]), "registered or overridden", "a registered property with no definition")
        open(props, "w", encoding="utf-8").write(ptext.replace("\nalignment-baseline\t", "\nzz-first\tKEYWORD\t-\t\tG\t\t\t\t\nalignment-baseline\t", 1))
        expect(run(["--input", inputs, "--check", "--properties", props]), "byte order", "a table out of order")
        root = os.path.join(tmp, "root")
        for rel in ("include/ghoti.io/vector/svg_kind.h", "include/ghoti.io/vector/svg_prop.h", "src/codec/svg/svg_tables.h", "src/codec/svg/svg_tables.c",
                    "src/codec/svg/style/svg_prop_tables.h", "src/codec/svg/style/svg_prop_tables.c", "documentation/elements.md", "documentation/properties.md"):
            os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
            shutil.copy(os.path.join(ROOT, rel), os.path.join(root, rel))
        path = os.path.join(root, "src/codec/svg/svg_tables.c")
        text = open(path, encoding="utf-8").read()
        open(path, "w", encoding="utf-8").write(text.replace('{"circle"', '{"circlf"', 1))
        expect(run(["--input", inputs, "--check", "--root", root]), "is not what the generator writes", "a committed table that differs")
    print("check-tables: %d checks passed" % passed)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
