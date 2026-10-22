#!/usr/bin/env python3
#
# Plant one defect at a time in a rule of the load (L1 to L11 of
# documentation/design.md) and require the tests to catch it.
#
# For each plant, the one source file it touches is copied, the defect is
# written into the copy (the text it replaces must occur exactly once, so an
# edit that silently matches nothing cannot pass), the copy is compiled with
# the library's own flags, and the library's archive is rebuilt with it in
# place of the original. Every unit test, and the fuzz replay over the
# committed corpus, is then linked against that archive and run. The plant is
# caught when any of them fails or dies by a signal. Before any plant, the
# unmodified control is built the same way and must pass, so that a harness
# that fails everything cannot pass this check.
#
# Environment, supplied by `make check-planted`:
#   PLANT_CC, PLANT_CFLAGS      compiler and the library's compile flags with includes
#   PLANT_CCFLAGS               flags for the fuzz replay (CFLAGS and includes)
#   PLANT_CXX                   C++ compiler and flags
#   PLANT_OBJ_DIR               the library's object directory
#   PLANT_TEST_OBJS             the unit tests' objects
#   PLANT_LDFLAGS               link flags (dependencies, gtest)
#   PLANT_LD_PATH               LD_LIBRARY_PATH for running
#   PLANT_WORK                  a scratch directory

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (rule, source file, text, replacement, what the defect is)
PLANTS = [
    ("L1", "src/core/core.c", 'return "input or output failed";', 'return "ok";',
     "two results share a name"),
    ("L2", "src/stream/stream.c", "    if (n < size && s->source_pos == s->size) {\n      s->eof = true;\n    }\n", "",
     "a short read that reaches the end does not set eof"),
    ("L2", "src/stream/stream.c", "return stream->source_pos - stream->look_len;", "return stream->source_pos;",
     "tell counts peeked bytes as consumed"),
    ("L2", "src/stream/stream.c", "  if (stream->kind == GVEC_STREAM_OUTPUT || !stream->seekable) {",
     "  if (stream->kind == GVEC_STREAM_OUTPUT) {", "a stream that cannot seek seeks"),
    ("L3", "src/core/core.c", "    if (*value == 0) {\n      *value = gvec_limit_defaults[f];",
     "    if (*value == 0) {\n      *value = SIZE_MAX;", "zero means unlimited"),
    ("L3", "src/core/core.c", "else if (*value > gvec_limits_ceiling((GVEC_Limit_Field) f)) {",
     "else if (*value >= gvec_limits_ceiling((GVEC_Limit_Field) f)) {", "a limit at its ceiling is refused"),
    ("L3", "src/core/core.c", "    if (resolved._reserved[i] != 0) {", "    if (0) {", "a reserved byte is accepted"),
    ("L4", "src/core/core.c", "if (diagnostics->count >= cap) {", "if (diagnostics->count > cap) {",
     "one entry past the cap is kept"),
    ("L4", "src/core/core.c", "  entry->element = name;", "  entry->element = element;",
     "the element name is borrowed, not copied"),
    ("L4", "src/core/core.c", "  if (diagnostics->count >= cap) {\n    diagnostics->dropped++;",
     "  if (diagnostics->count >= cap) {\n    ;", "entries past the cap are not counted"),
    ("L5", "src/codec/codec.c", "    if (strcmp(gvec_codecs[i]->name, name) == 0) {",
     "    if (strcmp(gvec_codecs[i]->name, name) != 0) {", "lookup by name returns the wrong codec"),
    ("L6", "src/codec/transport.c", "  if (gvec_transport_is_gzip(out.data, out.size)) {", "  if (0) {",
     "gzip inside gzip is read"),
    ("L6", "src/codec/transport.c", "      gvec_diag_add(diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_GZIP, 0, 0, 0, pos, NULL,\n"
     "          \"bytes after the last gzip member were ignored\");\n", "",
     "trailing bytes are ignored silently"),
    ("L6", "src/codec/transport.c", "  return size >= 3 && data[0] == 0x1f && data[1] == 0x8b && data[2] == 0x08;",
     "  return size >= 2 && data[0] == 0x1f && data[1] == 0x8b;", "a gzip member with another method is taken for gzip"),
    ("L7", "src/codec/svg/svg_codec.c",
     "if (event.namespace_uri && strcmp(event.namespace_uri, GTEXT_MARKUP_NS_SVG) == 0 && event.local_name &&",
     "if ((!event.namespace_uri || strcmp(event.namespace_uri, GTEXT_MARKUP_NS_SVG) == 0) && event.local_name &&",
     "a root in no namespace is taken for SVG"),
    ("L7", "src/codec/svg/svg_codec.c", "    if (event.type == GTEXT_XML_EVENT_ELEMENT_START) {", "    if (1) {",
     "the probe decides at the first event, not the first start tag"),
    ("L7", "src/codec/svg/svg_codec.c", "        if (r == GVEC_ERR_OOM || r == GVEC_ERR_LIMIT) {", "        if (r == GVEC_ERR_OOM) {",
     "a limit reached before the root is reported as not SVG"),
    ("L7", "src/codec/codec.c", "        diagnostics->dropped += why_not.dropped;\n", "",
     "a reason the probe could not keep is lost"),
    ("L8", "src/codec/svg/svg_load.c", "      if (++elements > limits->max_elements) {", "      if (++elements >= limits->max_elements) {",
     "a document of exactly max_elements elements is refused"),
    ("L8", "src/codec/svg/svg_load.c", "        resource.kind != GTEXT_MARKUP_DTD_RESOURCE_GENERAL_ENTITY) {", "        1) {",
     "a skipped general entity is reported as an unread DTD"),
    ("L8", "src/codec/svg/svg_load.c", "      if (gvec_diag_add(diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_ENTITY_SKIPPED,",
     "      if (gvec_diag_add(NULL, GVEC_DIAG_WARNING, GVEC_DIAG_ENTITY_SKIPPED,",
     "a kept external entity is not reported"),
    ("L8", "src/codec/svg/svg_load.c", "  case GTEXT_XML_E_LIMIT_DEPTH:\n    *out = GVEC_LIMIT_DEPTH;",
     "  case GTEXT_XML_E_LIMIT_DEPTH:\n    *out = GVEC_LIMIT_ATTRIBUTES;", "the depth limit names the wrong field"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->dtd_mode = GTEXT_XML_DTD_PROCESS;", "  out->dtd_mode = GTEXT_XML_DTD_OFF;",
     "the internal subset is not processed"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->external_access = GTEXT_XML_EXTERNAL_ACCESS_DENY;",
     "  out->external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;", "external access is allowed"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->dtd_external_policy = GTEXT_XML_DTD_EXTERNAL_SKIP;",
     "  out->dtd_external_policy = GTEXT_XML_DTD_EXTERNAL_DENY;", "an external entity fails the load"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->max_depth = limits->max_depth;", "",
     "the depth limit is not passed to the parser"),
    ("L10", "src/codec/codec.c", '{"none", "gzip"}', '{"gzip", "none"}', "the dump names the wrong transport"),
    ("L10", "src/codec/codec.c", "  for (int i = 7; i >= 0; i--) {", "  for (int i = 0; i < 8; i++) {",
     "the hash is read big-endian"),
    ("L11", "src/codec/svg/svg_codec.c", "      svg->root_line, svg->root_column, svg->root_offset,",
     "      0, svg->root_column, svg->root_offset,", "the refusal has no line"),
    ("L11", "src/codec/svg/svg_codec.c", "  return gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LOWER_REFUSED,",
     "  return gvec_diag_add(diagnostics ? NULL : NULL, GVEC_DIAG_ERROR, GVEC_DIAG_LOWER_REFUSED,", "the root is not refused"),

    # The typed document (story 5): grammars, build, graph, conditional processing, keys, dump.
    ("T-number", "src/codec/svg/svg_number.c", "  if (flags & GMATH_DECIMAL_OVERFLOW) {", "  if (0) {",
     "a number past the largest double is accepted"),
    ("T-number", "src/codec/svg/svg_number.c", "    if (j == len) {\n      *unit = k_units[i].unit;", "    if (j == len && 0) {\n      *unit = k_units[i].unit;",
     "no unit is recognised"),
    ("T-number", "src/codec/svg/svg_number.c", "    while (j < len && lower((unsigned char) s[start + j]) == (unsigned char) k_units[i].name[j]) {",
     "    while (j < len && (unsigned char) s[start + j] == (unsigned char) k_units[i].name[j]) {",
     "units are case-sensitive"),
    ("T-list", "src/codec/svg/svg_number.c", "    if (comma) {\n      fail(err, comma_at, \"a list may not end in a comma\");",
     "    if (0 && comma) {\n      fail(err, comma_at, \"a list may not end in a comma\");", "a list may end in a comma"),
    ("T-list", "src/codec/svg/svg_number.c", "  size_t keep = count - (count % multiple);", "  size_t keep = count;",
     "an odd point list keeps its dangling coordinate"),
    ("T-list", "src/codec/svg/svg_number.c", "    if (adjacent_negative && s[p] == '-') {", "    if (!adjacent_negative && s[p] == '-') {",
     "a negative coordinate directly after a number is refused in points"),
    ("T-path", "src/codec/svg/svg_path.c", "seg.x1 = last_kind == 'C' ? 2 * cx - lcx : cx;", "seg.x1 = 2 * cx - lcx;",
     "a smooth cubic reflects a control point that is not a cubic's"),
    ("T-path", "src/codec/svg/svg_path.c", "seg.x1 = last_kind == 'Q' ? 2 * cx - lcx : cx;", "seg.x1 = last_kind != 0 ? 2 * cx - lcx : cx;",
     "a smooth quadratic reflects a cubic's control point"),
    ("T-path", "src/codec/svg/svg_path.c", "        seg.x = a[5] + ox;", "        seg.x = a[5];", "an arc's relative end point is not made absolute"),
    ("T-path", "src/codec/svg/svg_path.c", "        seg.kind = GVEC_SVG_SEGMENT_CLOSE;\n        seg.x = sx;",
     "        seg.kind = GVEC_SVG_SEGMENT_CLOSE;\n        seg.x = cx + (sx - sx);", "closepath goes back to the current point"),
    ("T-path", "src/codec/svg/svg_path.c", "    if (first && upper != 'M') {", "    if (0) {", "path data may begin with a lineto"),
    ("T-path", "src/codec/svg/svg_path.c", "if (pos < n && (s[pos] == '0' || s[pos] == '1')) {",
     "if (pos < n && s[pos] >= '0' && s[pos] <= '9') {", "an arc flag may be any digit"),
    ("T-path", "src/codec/svg/svg_path.c", "      if (b.segment_count >= ctx->max_path_segments) {", "      if (b.segment_count > ctx->max_path_segments) {",
     "one segment past the bound is kept"),
    ("T-path", "src/codec/svg/svg_path.c", "        current_letter = relative ? 'l' : 'L';", "        current_letter = 'L';",
     "an implicit repeat after a relative moveto is an absolute lineto"),
    ("T-misc", "src/codec/svg/svg_misc.c", "  if (v[2] < 0 || v[3] < 0) {", "  if (0) {", "a viewBox may have a negative size"),
    ("T-misc", "src/codec/svg/svg_misc.c", "      value.slice = 0;\n      stage = 3;", "      value.slice = 0;\n      stage = 2;",
     "preserveAspectRatio takes meet and slice both"),
    ("T-misc", "src/codec/svg/svg_misc.c", "if (v[i] == '%' && i + 2 < len &&", "if (v[i] == '%' && 0 &&",
     "a fragment is not percent-decoded"),
    ("T-misc", "src/codec/svg/svg_misc.c", "      base64 = 1;\n      head -= 7;", "      base64 = 1;",
     "the base64 marker stays in the parameters"),
    ("T-doc", "src/codec/svg/svg_build.c", "    if (!is_text_reader(e->kind) && all_wsp(data, length)) {", "    if (0) {",
     "white space between the elements of a group is kept"),
    ("T-doc", "src/codec/svg/svg_build.c", "    if (first_time(ctx, \"a:\", a->name)) {", "    if (1) {", "an unknown attribute is noted every time"),
    ("T-doc", "src/codec/svg/svg_build.c", "  if (presentation && strcmp(a->name, \"d\") != 0) {", "  if (0) {",
     "a presentation attribute is parsed as a typed attribute"),
    ("T-doc", "src/codec/svg/svg_build.c", "  if (strcmp(a->name, \"style\") == 0) {", "  if (0) {", "style is a typed string"),
    ("T-doc", "src/codec/svg/svg_build.c", "    if ((view.ns && strcmp(view.ns, GVEC_XMLNS_NS) == 0) || (!view.ns && strcmp(view.local, \"xmlns\") == 0)) {",
     "    if (0) {", "a namespace declaration is kept as an attribute"),
    ("T-doc", "src/codec/svg/svg_build.c", "total > ctx->limits->max_text_bytes", "total >= ctx->limits->max_text_bytes",
     "character data exactly at the bound is refused"),
    ("T-doc", "src/codec/svg/svg_build.c", "  return a->element->order < b->element->order ? -1 : a->element->order > b->element->order ? 1 : 0;",
     "  return a->element->order < b->element->order ? 1 : a->element->order > b->element->order ? -1 : 0;",
     "the last element with an id wins"),
    ("T-graph", "src/codec/svg/svg_graph.c", "      if (reports_missing(e->kind)) {", "      if (0) {", "a missing reference is not reported"),
    ("T-graph", "src/codec/svg/svg_graph.c", "        mark_cycle(diag, path[k]);", "        (void) diag;", "a gradient chain cycle is not marked"),
    ("T-graph", "src/codec/svg/svg_graph.c", "total = sat_add(t->subtree_size, segtree_query(&expansion_sum, lo[u], hi[u]));",
     "total = sat_add(1, segtree_query(&expansion_sum, lo[u], hi[u]));",
     "a use counts one element for its referent's whole subtree"),
    ("T-graph", "src/codec/svg/svg_graph.c", "total = sat_add(t->subtree_size, segtree_query(&expansion_sum, lo[u], hi[u]));",
     "total = sat_add(t->subtree_size, 0 * segtree_query(&expansion_sum, lo[u], hi[u]));",
     "a use does not count what the uses inside its referent instantiate"),
    ("T-graph", "src/codec/svg/svg_graph.c", "    if (u->href_target && u->expansion > limits->max_reference_expansion) {",
     "    if (u->href_target && u->expansion >= limits->max_reference_expansion) {", "a use exactly at the bound is refused"),
    ("T-graph", "src/codec/svg/svg_graph.c", " || inside > 0) {", " || inside < 0) {", "a use inside another use's referent is counted twice in the total"),
    ("T-graph", "src/codec/svg/svg_graph.c", "          if (loop) {\n            cyclic[m] = 1;\n          }", "          (void) loop;",
     "the members of a cycle of several uses are not marked"),
    ("T-graph", "src/codec/svg/svg_graph.c", "        bool loop = members > 1 || cyclic[u];", "        bool loop = cyclic[u];",
     "a component of several uses is not a cycle"),
    ("T-graph", "src/codec/svg/svg_graph.c", "        cyclic[i] = 1; /* a use of itself, or of an ancestor */", "        ;",
     "a use of its own ancestor is not a cycle"),
    ("T-cond", "src/codec/svg/svg_conditional.c", "  return ul == tl || tag[ul] == '-';", "  return 1;",
     "a language tag matches by any prefix"),
    ("T-cond", "src/codec/svg/svg_conditional.c", "        ok = false; /* no extension is supported, and an empty list is false */", "        ;",
     "an extension the library does not support evaluates true"),
    ("T-cond", "src/codec/svg/svg_conditional.c", "      if (is_switch_candidate(c->kind) && !(c->flags & GVEC_SVG_ELEMENT_COND_FALSE)) {",
     "      if (is_switch_candidate(c->kind)) {", "a switch selects a child whose conditions are false"),
    ("T-key", "src/codec/svg/svg_key.c", "  if (!id) {\n    for (int i = 0; i < 4; i++) {", "  if (0) {\n    for (int i = 0; i < 4; i++) {",
     "siblings of one kind get the same key"),
    ("T-dump", "src/codec/svg/svg_dump.c", "if (c >= 0x20 && c <= 0x7e && c != '\\\\' && c != '\"') {", "if (c >= 0x20 && c <= 0x7e) {",
     "the dump does not escape a quote or a backslash"),
    ("T-dump", "src/codec/svg/svg_dump.c", "  if (d.negative) {\n    put(w, \"-\", 1);\n  }", "",
     "the dump drops the sign of a number"),
]

# The root check of L8 is unreachable while the probe is correct (the probe
# refuses every document whose root it would refuse), so a plant that removes
# it alone cannot be caught by any test. It is planted together with the probe
# defect it guards against, and must then be caught.
PAIRED = [
    ("L8", [("src/codec/svg/svg_load.c", '  if (!root || !gtext_markup_element_is(root, GTEXT_MARKUP_NS_SVG, "svg")) {',
             "  if (!root) {"),
            ("src/codec/svg/svg_codec.c",
             "if (event.namespace_uri && strcmp(event.namespace_uri, GTEXT_MARKUP_NS_SVG) == 0 && event.local_name &&",
             "if (event.local_name &&")],
     "the root check removed, behind a probe that accepts any svg"),
    # The inflation cap is enforced twice, where the buffer grows and on the
    # final size, each covering the other; loosening one alone is caught by
    # neither test. Loosened together (by 4 KiB), the cap must be seen to fail.
    ("L6", [("src/codec/transport.c", "  if (out->capacity > out->max) {", "  if (out->capacity > out->max + 4096) {"),
            ("src/codec/transport.c", "  if (grown > out->max + 1) {\n    grown = out->max + 1;",
             "  if (grown > out->max + 4096) {\n    grown = out->max + 4096;"),
            ("src/codec/transport.c", "  if (r == GVEC_OK && out->size > out->max) {", "  if (r == GVEC_OK && out->size > out->max + 4096) {")],
     "the inflation cap is 4 KiB too loose"),
]


def env(name):
    value = os.environ.get(name)
    if value is None:
        sys.exit("check-planted: %s is not set; run this through make check-planted" % name)
    return value


def run(cmd, **kw):
    return subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def build_and_test(work, edits):
    """Build the library with `edits` (list of (file, old, new)) and run everything.

    Returns ("pass" | "fail" | "broken", log). A build that does not compile or
    link is "broken": it says nothing about the tests, so it is never counted
    as a plant caught."""
    os.makedirs(work, exist_ok=True)
    cc, cflags = env("PLANT_CC"), env("PLANT_CFLAGS")
    obj_dir = env("PLANT_OBJ_DIR")
    replaced = {}
    for path, old, new in edits:
        source = replaced.get(path)
        if source is None:
            with open(os.path.join(ROOT, path)) as h:
                source = h.read()
        count = source.count(old)
        if count != 1:
            sys.exit("check-planted: the text to plant in %s occurs %d times, not once:\n%s" % (path, count, old))
        replaced[path] = source.replace(old, new)
    objects = []
    for root, _, files in os.walk(obj_dir):
        if "/tests" in root[len(obj_dir):]:
            continue
        for f in files:
            if f.endswith(".o"):
                objects.append(os.path.join(root, f))
    swapped = {}
    for path, source in replaced.items():
        rel = os.path.relpath(path, "src")
        planted_src = os.path.join(work, rel)
        os.makedirs(os.path.dirname(planted_src), exist_ok=True)
        with open(planted_src, "w") as h:
            h.write(source)
        obj = os.path.join(work, rel[:-2] + ".o")
        # Compile from the original directory's view of relative includes.
        inc = "-I" + os.path.join(ROOT, os.path.dirname(path))
        r = run("%s %s %s -c %s -o %s" % (cc, cflags, inc, planted_src, obj), cwd=ROOT)
        if r.returncode != 0:
            return "broken", "compile failed:\n" + r.stdout
        swapped[os.path.normpath(os.path.join(obj_dir, rel[:-2] + ".o"))] = obj
    objects = [swapped.get(os.path.normpath(o), o) for o in objects]
    archive = os.path.join(work, "libplanted.a")
    if os.path.exists(archive):
        os.remove(archive)
    r = run("ar rcs %s %s" % (archive, " ".join(objects)))
    if r.returncode != 0:
        return "broken", "ar failed:\n" + r.stdout
    cxx, ldflags = env("PLANT_CXX"), env("PLANT_LDFLAGS")
    ld_path = env("PLANT_LD_PATH")
    log = []
    failed = False
    for t in env("PLANT_TEST_OBJS").split():
        exe = os.path.join(work, os.path.basename(t)[:-2])
        r = run("%s -o %s %s -Wl,--whole-archive %s -Wl,--no-whole-archive %s" % (cxx, exe, t, archive, ldflags),
                cwd=ROOT)
        if r.returncode != 0:
            return "broken", "link failed:\n" + r.stdout
        r = run("LD_LIBRARY_PATH='%s' %s --gtest_brief=1" % (ld_path, exe), cwd=ROOT, timeout=600)
        log.append("%s: exit %d\n%s" % (os.path.basename(exe), r.returncode, r.stdout[-3000:]))
        if r.returncode != 0:
            failed = True
    replay = os.path.join(work, "replay_load")
    r = run("%s %s -Itests/fuzz -o %s tests/fuzz/fuzz_load.c tests/fuzz/replay_main.c %s %s"
            % (env("PLANT_CC"), env("PLANT_CCFLAGS"), replay, archive, ldflags), cwd=ROOT)
    if r.returncode != 0:
        return "broken", "replay link failed:\n" + r.stdout
    corpus = sorted(os.path.join("tests/fuzz/corpus/load", f) for f in os.listdir(os.path.join(ROOT, "tests/fuzz/corpus/load")))
    r = run("LD_LIBRARY_PATH='%s' %s %s" % (ld_path, replay, " ".join(corpus)), cwd=ROOT, timeout=600)
    log.append("replay: exit %d\n%s" % (r.returncode, r.stdout[-2000:]))
    if r.returncode != 0:
        failed = True
    return ("fail" if failed else "pass"), "\n".join(log)


def main():
    work = env("PLANT_WORK")
    shutil.rmtree(work, ignore_errors=True)
    status, log = build_and_test(os.path.join(work, "control"), [])
    if status != "pass":
        print("check-planted: the control (no plant) fails, so no plant can be judged:\n" + log, file=sys.stderr)
        return 1
    print("  ok   control: the unplanted library passes every test and the replay")
    missed = 0
    cases = [(rule, [(path, old, new)], what) for rule, path, old, new, what in PLANTS] + PAIRED
    for n, (rule, edits, what) in enumerate(cases):
        status, log = build_and_test(os.path.join(work, "plant-%02d" % n), edits)
        if status == "broken":
            print("check-planted: the plant %s (%s) does not build:\n%s" % (rule, what, log), file=sys.stderr)
            return 1
        if status == "pass":
            missed += 1
            print("check-planted: MISSED %s: %s" % (rule, what), file=sys.stderr)
        else:
            print("  ok   %-4s %s: caught" % (rule, what))
    if missed:
        print("check-planted: %d of %d plants were not caught" % (missed, len(cases)), file=sys.stderr)
        return 1
    print("check-planted: all %d plants caught, control passes" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
