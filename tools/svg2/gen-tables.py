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
"""Generate the SVG element and attribute tables from the SVG 2 Candidate Recommendation.

    gen-tables.py [--input DIR] [--check]

Reads the pinned pages in DIR (default third_party/svg2; tools/svg2/fetch.sh makes it):
the element index, the attribute index, the property index and the styling
chapter. Writes

    include/ghoti.io/vector/svg_kind.h      the element kinds (public)
    src/codec/svg/svg_tables.h              the grammars and the lookups (internal)
    src/codec/svg/svg_tables.c              the tables
    documentation/elements.md               the coverage of the indexes, by element and attribute
    include/ghoti.io/vector/svg_prop.h      the properties the style layer knows by name (public)
    src/codec/svg/style/svg_prop_tables.[ch] their types, bases and the definitions vector registers
    documentation/properties.md             where each property's definition comes from

The tables are committed so that a stranger can build without the network.
--check regenerates them and fails when a committed file differs, which is how
the library notices that the specification pages or the grammar rules changed.

The generator fails (exit 1, the pair named) on:
  - an (element, attribute) pair of the attribute index that no rule of
    grammars.tsv maps, or that maps to a grammar the library does not have;
  - a rule that maps no pair of the index (a stale rule);
  - an element of the element index that gets no kind;
  - a presentation attribute row it cannot parse;
  - a property of the property index or the presentation attribute list that tools/svg2/properties.tsv lacks,
    or a row of it that is malformed.
"""

import argparse
import fnmatch
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Names the element index lists that are not element kinds of their own: `unknown`
# is the CR's placeholder for any element it does not name.
NOT_KINDS = {"unknown"}

GRAMMARS = [
    "NONE", "RAW_ARIA", "EVENT", "RAW_ANIM", "RAW_FILTER", "LEGACY", "STRING", "ID", "TABINDEX",
    "ENUM_XML_SPACE", "HREF", "SYSTEM_LANGUAGE", "REQUIRED_EXTENSIONS", "VIEWBOX", "PAR", "NUMBER",
    "LENGTH", "COORD_LIST", "NUMBER_LIST", "POINTS", "PATH_DATA", "ORIENT", "OFFSET", "REF_COORD",
    "TRANSFORM", "ENUM_UNITS", "ENUM_SPREAD", "ENUM_MARKER_UNITS", "ENUM_LENGTH_ADJUST", "ENUM_METHOD",
    "ENUM_SIDE", "ENUM_SPACING", "ENUM_ZOOM_PAN",
]


def fail(message):
    sys.stderr.write("gen-tables: %s\n" % message)
    sys.exit(1)


def read(dirname, name):
    path = os.path.join(dirname, name)
    if not os.path.isfile(path):
        fail("%s is missing; run tools/svg2/fetch.sh" % path)
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def element_names(index_html):
    names = re.findall(r'element-name">‘<a [^>]*><span>(\w+)</span>', index_html)
    if len(names) < 60:
        fail("the element index parsed to %d names; the page changed" % len(names))
    return sorted(set(names) - NOT_KINDS)


def attribute_rows(index_html):
    rows = re.findall(r'<tr><th><span class="attr-name">(.*?)</span></th><td>(.*?)</td><td>', index_html)
    if len(rows) < 300:
        fail("the attribute index parsed to %d rows; the page changed" % len(rows))
    pairs = {}
    for name_html, elements_html in rows:
        name = re.sub(r"<[^>]+>", "", name_html)
        for element in re.findall(r"<span>([\w:-]+)</span>", elements_html):
            pairs.setdefault(name, set()).add(element)
    return pairs


def presentation(styling_html, kinds):
    """Returns {name: set(kind names or 'ALL')} for the presentation attributes."""
    start = styling_html.find('id="PresentationAttributes"')
    end = styling_html.find('id="RequiredProperties"')
    if start < 0 or end < start:
        fail("the presentation attribute section was not found")
    section = styling_html[start:end]
    table = section[section.find("<table"):section.find("</table>")]
    out = {}
    for row in re.findall(r"<tr>(.*?)</tr>", table, re.S):
        cells = re.findall(r"<td>(.*?)</td>", row, re.S)
        if len(cells) != 2:
            continue
        names = re.findall(r'class="property"[^>]*>([\w-]+)</a>', cells[0])
        if not names:
            names = re.findall(r"<code>([\w-]+)</code>", cells[0])
        text = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", cells[1]))
        elements = re.findall(r"<span>(\w+)</span>", cells[1])
        if "Any element in the SVG namespace" in text and not elements:
            where = {"ALL"}
            if "except for animation elements" in text:
                where = {"ALL_EXCEPT_ANIMATION"}
        elif "Any element in the SVG namespace" in text:
            where = {"ALL"}
            if "except for animation elements" in text:
                where = {"ALL_EXCEPT_ANIMATION"}
        else:
            where = set(elements)
        if not names:
            fail("a presentation attribute row names no property: %r" % row[:120])
        for name in names:
            out.setdefault(name, set()).update(where)
    # The transform row's cell nests a list of three element groups; the page states the exception in prose.
    if "transform" not in out:
        out["transform"] = {"ALL_EXCEPT_TRANSFORM_PAINT_SERVERS"}
    if len(out) < 55:
        fail("the presentation attributes parsed to %d names; the page changed" % len(out))
    return out


RULES_PATH = os.path.join(HERE, "grammars.tsv")
PROPERTIES_PATH = os.path.join(HERE, "properties.tsv")
PROPERTY_TYPES = ["KEYWORD", "NUMBER", "LENGTH", "COLOR", "PAINT", "DASHARRAY", "TRANSFORM", "REFS", "SHORTHAND"]
PROPERTY_BASES = ["-", "W", "H", "D"]
PROPERTY_SOURCES = ["G", "R", "O"]
LEGACY_PATH = os.path.join(HERE, "legacy.txt")


def load_rules(path):
    rules = []
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 4:
                fail("grammars.tsv:%d: not four tab-separated fields" % number)
            name, elements, grammar, note = parts
            if grammar not in GRAMMARS:
                fail("grammars.tsv:%d: unknown grammar %s" % (number, grammar))
            rules.append({"name": name, "elements": None if elements == "*" else set(elements.split(",")),
                          "grammar": grammar, "note": note, "line": number, "used": False})
    return rules


def page_properties(propidx_html, styling_html):
    """The property names of the CR's property index and of its presentation attribute list."""
    index = set(re.findall(r'class="property"[^>]*>(?:<span[^>]*>)?([\w-]+)', propidx_html))
    start = styling_html.find('id="PresentationAttributes"')
    end = styling_html.find('id="RequiredProperties"')
    if start < 0 or end < start:
        fail("the presentation attribute section was not found")
    section = styling_html[start:end]
    table = section[section.find("<table"):section.find("</table>")]
    listed = set(re.findall(r'class="property"[^>]*>([\w-]+)</a>', table)) | set(re.findall(r"<code>([\w-]+)</code>", table))
    if len(index) < 40 or len(listed) < 50:
        fail("the property index parsed to %d names and the presentation list to %d; the page changed" % (len(index), len(listed)))
    return index, listed


def load_properties(path, index, listed):
    rows = []
    seen = set()
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 9:
                fail("properties.tsv:%d: %d tab-separated fields, not nine" % (number, len(parts)))
            name, kind, basis, reads, source, inherits, initial, grammar, note = parts
            if name in seen:
                fail("properties.tsv:%d: %s twice" % (number, name))
            seen.add(name)
            if kind not in PROPERTY_TYPES:
                fail("properties.tsv:%d: unknown type %s" % (number, kind))
            if basis not in PROPERTY_BASES:
                fail("properties.tsv:%d: unknown basis %s" % (number, basis))
            if source not in PROPERTY_SOURCES:
                fail("properties.tsv:%d: unknown source %s" % (number, source))
            if basis != "-" and kind not in ("LENGTH", "DASHARRAY"):
                fail("properties.tsv:%d: %s has a percentage basis but is not a length" % (number, name))
            if source == "G" and (inherits or initial or grammar or note):
                fail("properties.tsv:%d: %s is the pin's, so it has no definition to give" % (number, name))
            if source != "G" and not (inherits in ("yes", "no") and initial and grammar and note):
                fail("properties.tsv:%d: %s is registered or overridden, so it gives inherits, initial, grammar and note" % (number, name))
            rows.append({"name": name, "type": kind, "basis": basis, "reads": reads or name, "source": source,
                         "inherits": inherits, "initial": initial, "grammar": grammar, "note": note})
    names = [r["name"] for r in rows]
    if names != sorted(names):
        fail("properties.tsv is not in the byte order of the names")
    missing = sorted((index | listed) - seen)
    if missing:
        fail("properties.tsv lacks %s of the property index or the presentation attribute list" % ", ".join(missing))
    return rows


def snake(name):
    out = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    return out.upper().replace("-", "_")


def build(inputs):
    elements = element_names(read(inputs, "eltindex.html"))
    pairs = attribute_rows(read(inputs, "attindex.html"))
    pres = presentation(read(inputs, "styling.html"), elements)
    rules = load_rules(RULES_PATH)
    legacy = [l.strip() for l in open(LEGACY_PATH, encoding="utf-8")
              if l.strip() and not l.startswith("#")]
    kind_index = {name: i + 2 for i, name in enumerate(elements)}  # 0 unknown, 1 foreign
    kind_index["unknown"] = 0  # the index lists `unknown` for the attributes an unrecognised SVG element may carry
    # Map every pair to a grammar, first rule wins.
    mapped = {}  # (name, grammar) -> set of element names
    unmapped = []
    for name in sorted(pairs):
        for element in sorted(pairs[name]):
            if element not in kind_index:
                fail("the attribute index names element %r which the element index lacks" % element)
            found = None
            for rule in rules:
                if not fnmatch.fnmatchcase(name, rule["name"]):
                    continue
                if rule["elements"] is not None and element not in rule["elements"]:
                    continue
                found = rule
                break
            if not found:
                unmapped.append("%s on %s" % (name, element))
                continue
            found["used"] = True
            mapped.setdefault((name, found["grammar"]), set()).add(element)
    if unmapped:
        fail("%d (attribute, element) pairs of the index have no grammar rule, the first %s" % (len(unmapped), unmapped[0]))
    stale = [r for r in rules if not r["used"]]
    if stale:
        fail("grammars.tsv:%d: the rule for %s maps no pair of the index" % (stale[0]["line"], stale[0]["name"]))
    return elements, kind_index, mapped, pres, legacy, pairs


def mask_of(element_names_, kind_index, add_unknown=False):
    lo = hi = 0
    for e in element_names_:
        bit = kind_index[e]
        if bit < 64:
            lo |= 1 << bit
        else:
            hi |= 1 << (bit - 64)
    if add_unknown:
        lo |= 1
    return lo, hi


def cstr(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_properties(inputs, lic):
    index, listed = page_properties(read(inputs, "propidx.html"), read(inputs, "styling.html"))
    rows = load_properties(PROPERTIES_PATH, index, listed)
    files = {}
    n = len(rows)
    pub = [lic, "\n/**\n * @file\n *\n * The properties the style layer knows by name (generated by tools/svg2/gen-tables.py from the SVG 2\n"
           " * Candidate Recommendation's property index and presentation attribute list and tools/svg2/properties.tsv; do not edit).\n */\n\n"
           "#ifndef GHOTI_IO_GVEC_SVG_PROP_H\n#define GHOTI_IO_GVEC_SVG_PROP_H\n\n#include <ghoti.io/vector/macros.h>\n\n"
           "#include <stdbool.h>\n#include <stddef.h>\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
           "/** A property of SVG 2 (or of a module it takes one from). The numbers are stable within a release; they follow the byte order of the names. */\n"
           "typedef enum GVEC_Svg_Property {\n"]
    for i, r in enumerate(rows):
        pub.append("  GVEC_SVG_PROP_%s = %d, ///< `%s`\n" % (snake(r["name"]), i, r["name"]))
    pub.append("  GVEC_SVG_PROP_COUNT = %d ///< Closes the enum.\n} GVEC_Svg_Property;\n\n"
               "/** The CSS name of a property; NULL for a value that is not one. Static. */\n"
               "GVEC_API const char * gvec_svg_property_name(GVEC_Svg_Property property);\n\n"
               "/** The property named `name` (ASCII case-sensitive, as CSS spells it); false when SVG has none of that name. */\n"
               "GVEC_API bool gvec_svg_property_find(const char * name, size_t length, GVEC_Svg_Property * out);\n\n"
               "#ifdef __cplusplus\n}\n#endif\n\n#endif // GHOTI_IO_GVEC_SVG_PROP_H\n" % n)
    files["include/ghoti.io/vector/svg_prop.h"] = "".join(pub)

    h = [lic, "\n/**\n * @file svg_prop_tables.h\n *\n * What the style layer knows of each property (generated by tools/svg2/gen-tables.py; do not edit).\n */\n\n"
         "#ifndef GHOTI_IO_GVEC_SVG_PROP_TABLES_H\n#define GHOTI_IO_GVEC_SVG_PROP_TABLES_H\n\n#include <ghoti.io/vector/macros.h>\n\n"
         "#include <stdbool.h>\n#include <stdint.h>\n\n#include <ghoti.io/vector/svg_prop.h>\n\n"
         "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n/** How gvec_svg_get() reads a property. */\ntypedef enum gvec_svg_prop_type {\n"]
    for i, t in enumerate(PROPERTY_TYPES):
        h.append("  GVEC_SVG_PT_%s = %d,\n" % (t, i))
    h.append("  GVEC_SVG_PT_COUNT = %d\n} gvec_svg_prop_type;\n\n/** What a percentage of a length is of. */\ntypedef enum gvec_svg_prop_basis {\n" % len(PROPERTY_TYPES))
    for i, b in enumerate(["NONE", "WIDTH", "HEIGHT", "DIAGONAL"]):
        h.append("  GVEC_SVG_BASIS_%s = %d,\n" % (b, i))
    h.append("  GVEC_SVG_BASIS_COUNT = 4\n} gvec_svg_prop_basis;\n\n/** Where the registry's definition of a property comes from. */\ntypedef enum gvec_svg_prop_source {\n"
             "  GVEC_SVG_SOURCE_GENERATED = 0, ///< The pin of `text` defines it and the definition stands.\n"
             "  GVEC_SVG_SOURCE_REGISTERED,    ///< The pin lacks it; this library registers it.\n"
             "  GVEC_SVG_SOURCE_OVERRIDDEN     ///< The pin defines it differently from the CR (or cannot decide a value); this library overrides it.\n"
             "} gvec_svg_prop_source;\n\n"
             "/** One property. */\ntypedef struct gvec_svg_prop_row {\n  const char * name;\n  uint8_t type;       ///< A gvec_svg_prop_type.\n"
             "  uint8_t basis;      ///< A gvec_svg_prop_basis.\n  uint8_t source;     ///< A gvec_svg_prop_source.\n  bool inherits;      ///< Registered and overridden properties only.\n"
             "  const char * reads; ///< The registry property whose computed value is read.\n  const char * initial;\n  const char * grammar;\n"
             "  const char * note;\n} gvec_svg_prop_row;\n\n"
             "/** The rows, indexed by GVEC_Svg_Property. */\nGVEC_INTERNAL_API extern const gvec_svg_prop_row gvec_svg_prop_rows[GVEC_SVG_PROP_COUNT];\n\n"
             "#ifdef __cplusplus\n}\n#endif\n\n#endif // GHOTI_IO_GVEC_SVG_PROP_TABLES_H\n")
    files["src/codec/svg/style/svg_prop_tables.h"] = "".join(h)

    basis_name = {"-": "NONE", "W": "WIDTH", "H": "HEIGHT", "D": "DIAGONAL"}
    source_name = {"G": "GENERATED", "R": "REGISTERED", "O": "OVERRIDDEN"}
    c = [lic, "\n/**\n * @file\n *\n * The property tables (generated by tools/svg2/gen-tables.py; do not edit).\n */\n\n"
         "#include \"svg_prop_tables.h\"\n\n#include <string.h>\n\n"
         "const gvec_svg_prop_row gvec_svg_prop_rows[GVEC_SVG_PROP_COUNT] = {\n"]
    for r in rows:
        c.append("  {%s, GVEC_SVG_PT_%s, GVEC_SVG_BASIS_%s, GVEC_SVG_SOURCE_%s, %s, %s, %s, %s, %s},\n" % (
            cstr(r["name"]), r["type"], basis_name[r["basis"]], source_name[r["source"]], "true" if r["inherits"] == "yes" else "false",
            cstr(r["reads"]), cstr(r["initial"]) if r["initial"] else "NULL", cstr(r["grammar"]) if r["grammar"] else "NULL",
            cstr(r["note"]) if r["note"] else "NULL"))
    c.append("};\n\nconst char * gvec_svg_property_name(GVEC_Svg_Property property) {\n"
             "  return (unsigned) property < GVEC_SVG_PROP_COUNT ? gvec_svg_prop_rows[property].name : NULL;\n}\n\n"
             "bool gvec_svg_property_find(const char * name, size_t length, GVEC_Svg_Property * out) {\n"
             "  size_t lo = 0, hi = GVEC_SVG_PROP_COUNT;\n  if (!name || !out) {\n    return false;\n  }\n"
             "  while (lo < hi) {\n    size_t mid = lo + (hi - lo) / 2;\n    const char * row = gvec_svg_prop_rows[mid].name;\n"
             "    int c = strncmp(name, row, length);\n    if (c == 0 && row[length] != 0) {\n      c = -1;\n    }\n"
             "    if (c == 0) {\n      *out = (GVEC_Svg_Property) mid;\n      return true;\n    }\n"
             "    if (c < 0) {\n      hi = mid;\n    } else {\n      lo = mid + 1;\n    }\n  }\n  return false;\n}\n")
    files["src/codec/svg/style/svg_prop_tables.c"] = "".join(c)

    d = ["# SVG properties\n\nGenerated by `tools/svg2/gen-tables.py` from `tools/svg2/properties.tsv` and the SVG 2 Candidate Recommendation's property "
         "index (Appendix H) and presentation attribute list (chapter 6.6); do not edit. The cascade, the inheritance and the computed values are "
         "`text`'s; this page says, for each of the %d properties the style layer names, where the registry's definition comes from, how "
         "`gvec_svg_get()` reads it, and what a percentage of it is of.\n\n"
         "The sources: **generated** is the definition of the pin the registry of `text` is made from, which stands; **registered** is a property "
         "the pin lacks, which this library adds; **overridden** is one the pin defines differently from the CR (or cannot decide a value for), "
         "which this library redefines, and every override is reported by `gvec_svg_style_context_override_at()`. A property of the CR's index or "
         "presentation attribute list that is in none of the three fails the generator, and a registered or overridden definition that the "
         "registry does not hold fails the property tests.\n\n"
         "Types: **keyword** is the computed value as text; **length** is in user units; a percentage of a length is of the **width** or **height** "
         "of the nearest viewport (its `viewBox` size when it has one) or of their **diagonal** `sqrt((w\u00b2 + h\u00b2) / 2)`; **shorthand** has no value "
         "of its own.\n\n| property | source | type | percentage of | reads |\n| --- | --- | --- | --- | --- |\n" % n]
    for r in rows:
        d.append("| `%s` | %s | %s | %s | %s |\n" % (r["name"], source_name[r["source"]].lower(), r["type"].lower(),
                 {"-": "", "W": "width", "H": "height", "D": "diagonal"}[r["basis"]], "" if r["reads"] == r["name"] else "`%s`" % r["reads"]))
    d.append("\n## Registered and overridden definitions\n\n")
    for r in rows:
        if r["source"] == "G":
            continue
        d.append("### `%s` (%s)\n\n- inherited: %s\n- initial value: `%s`\n- value: `%s`\n- why: %s\n\n" % (
            r["name"], source_name[r["source"]].lower(), r["inherits"], r["initial"], r["grammar"], r["note"]))
    files["documentation/properties.md"] = "".join(d)
    return files


def render(inputs):
    elements, kind_index, mapped, pres, legacy, pairs = build(inputs)
    total = len(elements) + 2
    if total > 128:
        fail("more than 126 element kinds; the masks need a third word")
    lic = open(os.path.join(ROOT, "src/core/core.c"), encoding="utf-8").read().split("*/", 1)[0] + "*/\n"
    files = {}

    # ---- the public kind enum ------------------------------------------------
    kind = [lic, "\n/**\n * @file\n *\n * The kinds of SVG element (generated by tools/svg2/gen-tables.py from the SVG 2 Candidate\n"
            " * Recommendation's element index; do not edit).\n */\n\n#ifndef GHOTI_IO_GVEC_SVG_KIND_H\n"
            "#define GHOTI_IO_GVEC_SVG_KIND_H\n\n#include <ghoti.io/vector/macros.h>\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
            "/** The kind of an element of the typed document. The numbers are stable. */\n"
            "typedef enum GVEC_Svg_Kind {\n"
            "  GVEC_SVG_KIND_UNKNOWN = 0, ///< In the SVG namespace, but not an element of SVG 2.\n"
            "  GVEC_SVG_KIND_FOREIGN = 1, ///< In another namespace.\n"]
    for name in elements:
        kind.append("  GVEC_SVG_KIND_%s = %d, ///< `%s`\n" % (snake(name), kind_index[name], name))
    kind.append("  GVEC_SVG_KIND_COUNT = %d ///< Closes the enum.\n} GVEC_Svg_Kind;\n\n"
                "/** The element's local name for a kind, or NULL for UNKNOWN and FOREIGN (whose names the element keeps). */\n"
                "GVEC_API const char * gvec_svg_kind_name(GVEC_Svg_Kind kind);\n\n"
                "#ifdef __cplusplus\n}\n#endif\n\n#endif // GHOTI_IO_GVEC_SVG_KIND_H\n" % total)
    files["include/ghoti.io/vector/svg_kind.h"] = "".join(kind)

    # ---- the internal header --------------------------------------------------
    h = [lic, "\n/**\n * @file svg_tables.h\n *\n * The SVG element and attribute tables (generated by tools/svg2/gen-tables.py; do not edit).\n */\n\n"
         "#ifndef GHOTI_IO_GVEC_SVG_TABLES_H\n#define GHOTI_IO_GVEC_SVG_TABLES_H\n\n#include <ghoti.io/vector/macros.h>\n\n"
         "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n\n#include <ghoti.io/vector/svg_kind.h>\n\n"
         "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n/** How an attribute is read. */\ntypedef enum GVEC_Svg_Grammar {\n"]
    for i, g in enumerate(GRAMMARS):
        h.append("  GVEC_SVG_GRAMMAR_%s = %d,\n" % (g, i))
    h.append("  GVEC_SVG_GRAMMAR_COUNT = %d\n} GVEC_Svg_Grammar;\n\n" % len(GRAMMARS))
    h.append("/** The element kind whose local name is `name` (in the SVG namespace), or GVEC_SVG_KIND_UNKNOWN. */\n"
             "GVEC_INTERNAL_API GVEC_Svg_Kind gvec_svg_kind_find(const char * name, size_t length);\n\n"
             "/**\n * The grammar of attribute `name` (no namespace, or `xlink:href` and the like spelled with their prefix)\n"
             " * on an element of `kind`; GVEC_SVG_GRAMMAR_NONE when the CR defines none for that element.\n */\n"
             "GVEC_INTERNAL_API GVEC_Svg_Grammar gvec_svg_grammar_for(GVEC_Svg_Kind kind, const char * name, size_t length);\n\n"
             "/** Whether `name` is a presentation attribute of an element of `kind` (SVG 2, chapter 6.6). */\n"
             "GVEC_INTERNAL_API bool gvec_svg_is_presentation(GVEC_Svg_Kind kind, const char * name, size_t length);\n\n"
             "/** Whether `name` is an SVG 1.1 attribute that SVG 2 removed. */\n"
             "GVEC_INTERNAL_API bool gvec_svg_is_legacy(const char * name, size_t length);\n\n"
             "/** A grammar's name, for the dump. */\n"
             "GVEC_INTERNAL_API const char * gvec_svg_grammar_name(GVEC_Svg_Grammar grammar);\n\n"
             "#ifdef __cplusplus\n}\n#endif\n\n#endif // GHOTI_IO_GVEC_SVG_TABLES_H\n")
    files["src/codec/svg/svg_tables.h"] = "".join(h)

    # ---- the tables -----------------------------------------------------------
    c = [lic, "\n/**\n * @file\n *\n * The SVG element and attribute tables (generated by tools/svg2/gen-tables.py from the SVG 2\n"
         " * Candidate Recommendation, 4 October 2018; do not edit).\n */\n\n#include \"svg_tables.h\"\n\n#include <string.h>\n\n"]
    c.append("static const struct { const char * name; GVEC_Svg_Kind kind; } k_elements[] = {\n")
    for name in elements:
        c.append("  {\"%s\", GVEC_SVG_KIND_%s},\n" % (name, snake(name)))
    c.append("};\n\n")
    c.append("const char * gvec_svg_kind_name(GVEC_Svg_Kind kind) {\n  for (size_t i = 0; i < sizeof k_elements / sizeof k_elements[0]; i++) {\n"
             "    if (k_elements[i].kind == kind) {\n      return k_elements[i].name;\n    }\n  }\n  return NULL;\n}\n\n")
    c.append("GVEC_Svg_Kind gvec_svg_kind_find(const char * name, size_t length) {\n  size_t lo = 0, hi = sizeof k_elements / sizeof k_elements[0];\n"
             "  while (lo < hi) {\n    size_t mid = lo + (hi - lo) / 2;\n    int c = strncmp(name, k_elements[mid].name, length);\n"
             "    if (c == 0 && k_elements[mid].name[length] != 0) {\n      c = -1;\n    }\n"
             "    if (c == 0) {\n      return k_elements[mid].kind;\n    }\n    if (c < 0) {\n      hi = mid;\n    } else {\n      lo = mid + 1;\n    }\n  }\n"
             "  return GVEC_SVG_KIND_UNKNOWN;\n}\n\n")
    rows = []
    for (name, grammar), els in sorted(mapped.items()):
        lo, hi = mask_of(els, kind_index)
        rows.append((name, grammar, lo, hi))
    c.append("static const struct { const char * name; uint8_t grammar; uint64_t lo, hi; } k_attributes[] = {\n")
    for name, grammar, lo, hi in rows:
        c.append("  {\"%s\", GVEC_SVG_GRAMMAR_%s, UINT64_C(0x%016x), UINT64_C(0x%016x)},\n" % (name, grammar, lo, hi))
    c.append("};\n\n")
    c.append("static bool mask_has(uint64_t lo, uint64_t hi, GVEC_Svg_Kind kind) {\n  unsigned bit = (unsigned) kind;\n"
             "  return bit < 64 ? ((lo >> bit) & 1) != 0 : ((hi >> (bit - 64)) & 1) != 0;\n}\n\n")
    c.append("/* Compare a table name with a name of `length` bytes that is not NUL-terminated. */\n"
             "static int name_cmp(const char * row, const char * name, size_t length) {\n"
             "  int c = strncmp(row, name, length);\n  if (c != 0) {\n    return c;\n  }\n  return row[length] == 0 ? 0 : 1;\n}\n\n")
    c.append("GVEC_Svg_Grammar gvec_svg_grammar_for(GVEC_Svg_Kind kind, const char * name, size_t length) {\n"
             "  size_t lo = 0, hi = sizeof k_attributes / sizeof k_attributes[0];\n"
             "  while (lo < hi) {\n    size_t mid = lo + (hi - lo) / 2;\n"
             "    if (name_cmp(k_attributes[mid].name, name, length) < 0) {\n      lo = mid + 1;\n    } else {\n      hi = mid;\n    }\n  }\n"
             "  for (size_t i = lo; i < sizeof k_attributes / sizeof k_attributes[0] && name_cmp(k_attributes[i].name, name, length) == 0; i++) {\n"
             "    if (mask_has(k_attributes[i].lo, k_attributes[i].hi, kind)) {\n      return (GVEC_Svg_Grammar) k_attributes[i].grammar;\n    }\n  }\n"
             "  return GVEC_SVG_GRAMMAR_NONE;\n}\n\n")
    # presentation: masks; ALL variants as masks over every SVG-namespace kind (UNKNOWN included).
    def pres_mask(where):
        every = [e for e in elements]
        if "ALL" in where:
            chosen = every
        elif "ALL_EXCEPT_ANIMATION" in where:
            chosen = [e for e in every if e not in ("animate", "animateMotion", "animateTransform", "set", "discard")]
        elif "ALL_EXCEPT_TRANSFORM_PAINT_SERVERS" in where:
            chosen = [e for e in every if e not in ("pattern", "linearGradient", "radialGradient")]
        else:
            chosen = [e for e in where]
            for e in chosen:
                if e not in kind_index:
                    fail("the presentation table names element %r which the element index lacks" % e)
            return mask_of(chosen, kind_index)
        return mask_of(chosen, kind_index, True)
    c.append("static const struct { const char * name; uint64_t lo, hi; } k_presentation[] = {\n")
    for name in sorted(pres):
        lo, hi = pres_mask(pres[name])
        c.append("  {\"%s\", UINT64_C(0x%016x), UINT64_C(0x%016x)},\n" % (name, lo, hi))
    c.append("};\n\n")
    c.append("bool gvec_svg_is_presentation(GVEC_Svg_Kind kind, const char * name, size_t length) {\n"
             "  size_t lo = 0, hi = sizeof k_presentation / sizeof k_presentation[0];\n"
             "  while (lo < hi) {\n    size_t mid = lo + (hi - lo) / 2;\n    int c = name_cmp(k_presentation[mid].name, name, length);\n"
             "    if (c == 0) {\n      return mask_has(k_presentation[mid].lo, k_presentation[mid].hi, kind);\n    }\n"
             "    if (c < 0) {\n      lo = mid + 1;\n    } else {\n      hi = mid;\n    }\n  }\n  return false;\n}\n\n")
    c.append("static const char * const k_legacy[] = {\n" + "".join("  \"%s\",\n" % l for l in legacy) + "};\n\n")
    c.append("bool gvec_svg_is_legacy(const char * name, size_t length) {\n  for (size_t i = 0; i < sizeof k_legacy / sizeof k_legacy[0]; i++) {\n"
             "    if (strlen(k_legacy[i]) == length && memcmp(k_legacy[i], name, length) == 0) {\n      return true;\n    }\n  }\n  return false;\n}\n\n")
    c.append("static const char * const k_grammar_names[] = {\n" + "".join("  \"%s\",\n" % g.lower() for g in GRAMMARS) + "};\n\n")
    c.append("const char * gvec_svg_grammar_name(GVEC_Svg_Grammar grammar) {\n  return (unsigned) grammar < GVEC_SVG_GRAMMAR_COUNT ? k_grammar_names[grammar] : \"unknown\";\n}\n")
    files["src/codec/svg/svg_tables.c"] = "".join(c)

    # ---- the coverage document ------------------------------------------------
    d = ["# SVG elements and attributes\n\nGenerated by `tools/svg2/gen-tables.py` from the SVG 2 Candidate Recommendation (4 October 2018) element, "
         "attribute and property indexes and chapter 6; do not edit. It states how every element and attribute of those "
         "indexes is read: typed by this library, held as written for the cascade (a CSS property or a presentation "
         "attribute), or kept as written because the phase that reads it has not landed.\n\n"
         "## Elements (%d)\n\nEach is a kind of the typed document. An element in the SVG namespace that is not in this list is "
         "`UNKNOWN`; one in another namespace is `FOREIGN`. Both are kept in place and not rendered.\n\n" % len(elements)]
    d.append(", ".join("`%s`" % e for e in elements) + "\n\n")
    byg = {}
    for (name, grammar), els in sorted(mapped.items()):
        byg.setdefault(grammar, []).append((name, sorted(els)))
    d.append("## Attributes of the index, by how they are read\n\n")
    meaning = {
        "RAW_ARIA": "kept as written (accessibility metadata; does not affect drawing)",
        "EVENT": "kept as written, never evaluated",
        "RAW_ANIM": "kept as written; animation is not applied",
        "RAW_FILTER": "kept as written; read when filters are (a later phase)",
        "STRING": "a string, kept as written",
        "TRANSFORM": "a transform list, typed by the cascade",
    }
    for grammar in GRAMMARS:
        if grammar not in byg:
            continue
        d.append("### %s\n\n%s\n\n" % (grammar, meaning.get(grammar, "typed by its own grammar")))
        d.append("| attribute | elements |\n| --- | --- |\n")
        for name, els in byg[grammar]:
            shown = ", ".join("`%s`" % e for e in els) if len(els) <= 8 else "%d elements" % len(els)
            d.append("| `%s` | %s |\n" % (name, shown))
        d.append("\n")
    d.append("## Presentation attributes (SVG 2, 6.6)\n\nHeld as written and typed by the cascade. The geometry properties are "
             "presentation attributes only on the elements named; the same names elsewhere are typed attributes above.\n\n"
             "| property | elements |\n| --- | --- |\n")
    for name in sorted(pres):
        where = pres[name]
        text = "every SVG element" if where == {"ALL"} else "every SVG element but the animation elements" if where == {"ALL_EXCEPT_ANIMATION"} \
            else "every SVG element but `pattern`, `linearGradient`, `radialGradient`" if where == {"ALL_EXCEPT_TRANSFORM_PAINT_SERVERS"} \
            else ", ".join("`%s`" % e for e in sorted(where))
        d.append("| `%s` | %s |\n" % (name, text))
    d.append("\n## SVG 1.1 attributes SVG 2 removed\n\nKept as written, without a notice: %s.\n" % ", ".join("`%s`" % l for l in legacy))
    files["documentation/elements.md"] = "".join(d)
    files.update(render_properties(inputs, lic))
    return files


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default=os.path.join(ROOT, "third_party/svg2"))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--rules", default=RULES_PATH)
    ap.add_argument("--properties", default=PROPERTIES_PATH)
    ap.add_argument("--root", default=ROOT, help="where the committed files are read from and written to")
    args = ap.parse_args(argv)
    globals()["RULES_PATH"] = args.rules
    globals()["PROPERTIES_PATH"] = args.properties
    files = render(args.input)
    bad = False
    for rel, text in files.items():
        path = os.path.join(args.root, rel)
        if args.check:
            have = open(path, encoding="utf-8").read() if os.path.isfile(path) else None
            if have != text:
                sys.stderr.write("gen-tables: %s is not what the generator writes\n" % rel)
                bad = True
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
    if args.check and not bad:
        print("gen-tables: the committed tables are what the generator writes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
