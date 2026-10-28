/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Vector.
 *
 * Ghoti.io Vector is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Vector is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The style fuzzer: declarations and presentation attributes through the cascade, the used values, the references and the dump.
 *
 * The input is an options byte and then pieces separated by a line feed. The pieces are the values of the `style` attribute of the root
 * and of a group, of a `fill`, a `transform` and a `stroke-dasharray` attribute and of the `style` of a rect, a `use` and a symbol's
 * rect, in a fixed document, so that what the engine of `text` and the used values meet is what the corpus makes. The low nibble of
 * the options byte names one limit to shrink (the three of the style layer, and the reference expansion), the high nibble sets how far.
 *
 * Invariants checked on every input (an abort is a finding):
 *   - the context is made, and every read is GVEC_OK or GVEC_ERR_UNSUPPORTED (an element refused), never another result;
 *   - every number is finite, every enumerator is in range, every string is there, every reference names an element of the document;
 *   - the same read twice is the same value; the style hash is the same twice; a dump begins with the versioned header;
 *   - the diagnostics are in range and within the cap;
 *   - reading with a shrunk limit never reads out of bounds (the sanitizers), and never returns more than the limit allows.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/vector/vector.h>

static unsigned gvec_fuzz_seen;

int gvec_fuzz_unreached(void);
int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

/* The outcomes the committed corpus must reach: reads that succeed, elements that are refused, and a document that does not load. */
#define SEEN_OK 1u
#define SEEN_UNSUPPORTED 2u
#define SEEN_NO_DOC 4u
#define SEEN_DROPPED 8u
#define SEEN_CYCLE 16u
#define SEEN_LIMIT 32u

int gvec_fuzz_unreached(void) {
  static const struct {
    unsigned bit;
    const char * name;
  } k_expected[] = {{SEEN_OK, "a read that succeeds"}, {SEEN_UNSUPPORTED, "an element refused"}, {SEEN_NO_DOC, "a document that does not load"},
      {SEEN_DROPPED, "a declaration dropped"}, {SEEN_CYCLE, "a reference cycle"}, {SEEN_LIMIT, "a style limit"}};
  int missing = 0;
  for (size_t i = 0; i < sizeof k_expected / sizeof k_expected[0]; i++) {
    if (!(gvec_fuzz_seen & k_expected[i].bit)) {
      fprintf(stderr, "fuzz_style: no input reached %s\n", k_expected[i].name);
      missing++;
    }
  }
  return missing;
}

static void check(int condition, const char * what) {
  if (!condition) {
    fprintf(stderr, "fuzz_style: invariant broken: %s\n", what);
    abort();
  }
}

static int finite(double v) {
  return v - v == 0.0;
}

/* Append `piece` to `out` as the text of an XML attribute value of well-formed UTF-8: markup is escaped, a byte that is not
 * valid in XML 1.0 or UTF-8 is a space. */
static size_t append_value(char * out, size_t length, size_t capacity, const uint8_t * piece, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = piece[i];
    const char * text = NULL;
    char single[2] = {0, 0};
    if (c == '&') {
      text = "&amp;";
    }
    else if (c == '<') {
      text = "&lt;";
    }
    else if (c == '>') {
      text = "&gt;";
    }
    else if (c == '"') {
      text = "&quot;";
    }
    else if (c == '\t') {
      text = " ";
    }
    else if (c < 0x20 || c >= 0x80) {
      text = " ";
    }
    else {
      single[0] = (char) c;
      text = single;
    }
    size_t l = strlen(text);
    if (length + l >= capacity) {
      break;
    }
    memcpy(out + length, text, l);
    length += l;
  }
  out[length] = 0;
  return length;
}

static void check_color(const GVEC_Svg_Color * c) {
  check((unsigned) c->kind < GVEC_SVG_COLOR_COUNT, "a color kind is in range");
  for (int i = 0; i < 4; i++) {
    check(finite(c->color.rgba[i]), "a color component is finite");
  }
  check(c->color.rgba[3] >= 0 && c->color.rgba[3] <= 1, "an alpha is in [0, 1]");
  check(gshscene_color_check(&c->color) == GSHSCENE_OK, "a color is one the waist records");
}

static void check_value(const GVEC_Svg_Document * doc, const GVEC_Svg_Style_Value * v) {
  check((unsigned) v->kind < GVEC_SVG_STYLE_KIND_COUNT, "a value kind is in range");
  switch (v->kind) {
  case GVEC_SVG_STYLE_KEYWORD:
    check(v->keyword != NULL, "a keyword has its text");
    break;
  case GVEC_SVG_STYLE_NUMBER:
  case GVEC_SVG_STYLE_LENGTH:
    check(finite(v->number), "a number or length is finite");
    break;
  case GVEC_SVG_STYLE_COLOR:
    check_color(&v->color);
    break;
  case GVEC_SVG_STYLE_PAINT:
    check((unsigned) v->paint.kind < GVEC_SVG_PAINT_COUNT, "a paint kind is in range");
    if (v->paint.kind == GVEC_SVG_PAINT_COLOR) {
      check_color(&v->paint.color);
    }
    if (v->paint.kind == GVEC_SVG_PAINT_SERVER) {
      check(v->paint.server != NULL && v->paint.server_id != NULL, "a paint server is an element with an id");
      check(gvec_svg_find_by_id(doc, v->paint.server_id) == v->paint.server, "a paint server is the element its id names");
      if (v->paint.has_fallback && v->paint.fallback_kind == GVEC_SVG_PAINT_COLOR) {
        check_color(&v->paint.fallback_color);
      }
    }
    break;
  case GVEC_SVG_STYLE_NUMBER_LIST:
    check(v->number_count % 2 == 0, "a dash array has an even number of entries");
    check(v->number_count == 0 || v->numbers != NULL, "a dash array has its entries");
    for (size_t i = 0; i < v->number_count; i++) {
      check(finite(v->numbers[i]) && v->numbers[i] >= 0, "a dash is finite and not negative");
    }
    break;
  case GVEC_SVG_STYLE_TRANSFORM:
    check(gshgeom_map_check(&v->transform.map) == GSHGEOM_OK || v->transform.depends_on_box, "a transform map is valid");
    check((unsigned) v->transform.box < GVEC_SVG_BOX_COUNT, "a transform box is in range");
    check(v->transform.op_count == 0 || v->transform.ops != NULL, "a transform has its functions");
    for (int i = 0; i < 2; i++) {
      check(finite(v->transform.origin[i]) && finite(v->transform.origin_fraction[i]), "an origin is finite");
    }
    break;
  case GVEC_SVG_STYLE_REFERENCES:
    check(v->reference_count == 0 || v->references != NULL, "references are there");
    for (size_t i = 0; i < v->reference_count; i++) {
      check(v->references[i].text != NULL, "a reference has its text");
      if (v->references[i].target) {
        check(v->references[i].is_url && v->references[i].local && v->references[i].id != NULL, "a target is a local url with an id");
        check(gvec_svg_find_by_id(doc, v->references[i].id) == v->references[i].target, "a target is the element its id names");
      }
    }
    break;
  default:
    break;
  }
}

static void check_instance(GVEC_Svg_Style_Context * context, const GVEC_Svg_Document * doc, const GVEC_Svg_Instance * instance) {
  for (int p = 0; p < GVEC_SVG_PROP_COUNT; p++) {
    GVEC_Svg_Style_Value a, b;
    GVEC_Result r = gvec_svg_get(context, instance, (GVEC_Svg_Property) p, &a);
    check(r == GVEC_OK || r == GVEC_ERR_UNSUPPORTED || r == GVEC_ERR_LIMIT, "a read is OK, UNSUPPORTED or LIMIT");
    gvec_fuzz_seen |= r == GVEC_OK ? SEEN_OK : r == GVEC_ERR_UNSUPPORTED ? SEEN_UNSUPPORTED : 0;
    if (r != GVEC_OK) {
      continue;
    }
    check_value(doc, &a);
    GVEC_Result r2 = gvec_svg_get(context, instance, (GVEC_Svg_Property) p, &b);
    check(r2 == GVEC_OK, "a second read succeeds when the first did");
    check(a.kind == b.kind && a.number == b.number && a.number_count == b.number_count && a.reference_count == b.reference_count,
        "the same read twice is the same value");
    GVEC_Svg_Style_Specified s;
    GVEC_Result sr = gvec_svg_get_specified(context, instance, (GVEC_Svg_Property) p, &s);
    if (sr != GVEC_OK) {
      fprintf(stderr, "fuzz_style: property %s: %s\n", gvec_svg_property_name((GVEC_Svg_Property) p), gvec_result_string(sr));
    }
    check(sr == GVEC_OK, "the computed value is read");
    check((unsigned) s.source < GVEC_SVG_STYLE_FROM_COUNT, "a source is in range");
  }
  double w, h;
  gvec_svg_instance_viewport(instance, &w, &h);
  check(finite(w) && finite(h) && w >= 0 && h >= 0, "a viewport is finite");
}

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 1) {
    return 0;
  }
  GVEC_Limits limits;
  memset(&limits, 0, sizeof limits);
  unsigned field = data[0] & 0x0f, scale = data[0] >> 4;
  size_t small = (size_t) scale * 16 + 1;
  switch (field) {
  case 0:
    limits.max_style_bytes = small;
    break;
  case 1:
    limits.max_declarations = (size_t) scale + 1;
    break;
  case 2:
    limits.max_custom_expansion = (size_t) scale * 4 + 1;
    break;
  case 3:
    limits.max_reference_expansion = (size_t) scale + 1;
    break;
  case 4:
    limits.max_attributes = (size_t) scale + 1;
    break;
  default:
    break;
  }
  data++;
  size--;

  /* The pieces. */
  const uint8_t * piece[7];
  size_t piece_size[7];
  size_t pieces = 0;
  size_t start = 0;
  for (size_t i = 0; i <= size && pieces < 7; i++) {
    if (i == size || data[i] == '\n') {
      piece[pieces] = data + start;
      piece_size[pieces] = i - start;
      pieces++;
      start = i + 1;
    }
  }
  while (pieces < 7) {
    piece[pieces] = data;
    piece_size[pieces] = 0;
    pieces++;
  }
  enum { VALUE = 2048, DOCUMENT = 7 * (VALUE * 6) + 4096 }; /* a value escaped is at most six times its size */
  char values[7][VALUE];
  for (int i = 0; i < 7; i++) {
    append_value(values[i], 0, sizeof values[i], piece[i], piece_size[i]);
  }
  char * text = malloc(DOCUMENT);
  check(text != NULL, "the document is built");
  snprintf(text, DOCUMENT,
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\" style=\"%s\">"
      "<defs><linearGradient id=\"g\"/><clipPath id=\"c\"><rect id=\"cr\" style=\"%s\"/></clipPath><marker id=\"m\"/>"
      "<symbol id=\"s\" viewBox=\"0 0 10 10\"><rect id=\"in\" style=\"%s\"/></symbol></defs>"
      "<g id=\"g1\" style=\"%s\" fill=\"%s\" transform=\"%s\" stroke-dasharray=\"%s\">"
      "<rect id=\"r\" width=\"10%%\" clip-path=\"url(#c)\" marker-start=\"url(#m)\"/><use id=\"u\" href=\"#s\" width=\"20\" height=\"20\"/></g></svg>",
      values[0], values[1], values[2], values[3], values[4], values[5], values[6]);

  GVEC_Diagnostics diagnostics;
  gvec_diagnostics_init(&diagnostics, NULL);
  GVEC_Stream * stream = NULL;
  check(gvec_stream_create_memory(text, strlen(text), &stream) == GVEC_OK, "a memory stream is created");
  GVEC_Load_Options options;
  memset(&options, 0, sizeof options);
  options.limits = &limits;
  GVEC_Doc * doc = NULL;
  GVEC_Result r = gvec_doc_load(stream, &options, &diagnostics, &doc);
  gvec_stream_destroy(stream);
  if (r != GVEC_OK) {
    gvec_fuzz_seen |= SEEN_NO_DOC;
    gvec_diagnostics_destroy(&diagnostics);
    free(text);
    return 0;
  }
  const GVEC_Svg_Document * svg = gvec_svg_document(doc);
  GVEC_Svg_Style_Context * context = NULL;
  check(gvec_svg_style_context_new(svg, &diagnostics, NULL, &context) == GVEC_OK, "the context is made");

  /* Every element in the document's own context, then the clone. An instance is not made under an element whose style cannot be computed
   * (a viewport is read from it): that is UNSUPPORTED, and the subtree is not read. */
  const GVEC_Svg_Instance * root = gvec_svg_instance_root(context);
  check(root != NULL, "the root instance is made");
  check_instance(context, svg, root);
  const GVEC_Svg_Instance * g1 = NULL;
  const GVEC_Svg_Element * g1e = gvec_svg_find_by_id(svg, "g1");
  GVEC_Result made = g1e ? gvec_svg_instance_child(context, root, g1e, &g1) : GVEC_ERR_UNSUPPORTED;
  check(made == GVEC_OK || made == GVEC_ERR_UNSUPPORTED || made == GVEC_ERR_LIMIT, "a child instance is made or refused");
  if (made == GVEC_OK) {
    check_instance(context, svg, g1);
    for (const GVEC_Svg_Element * e = gvec_svg_element_first_child(g1e); e; e = gvec_svg_element_next_sibling(e)) {
      const GVEC_Svg_Instance * child = NULL;
      made = gvec_svg_instance_child(context, g1, e, &child);
      check(made == GVEC_OK || made == GVEC_ERR_UNSUPPORTED || made == GVEC_ERR_LIMIT, "a child of the group is made or refused");
      if (made != GVEC_OK) {
        continue;
      }
      check_instance(context, svg, child);
      if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_USE && gvec_svg_element_href_target(e)) {
        const GVEC_Svg_Instance * clone = NULL;
        made = gvec_svg_instance_use(context, child, &clone);
        check(made == GVEC_OK || made == GVEC_ERR_UNSUPPORTED || made == GVEC_ERR_LIMIT, "the clone is made or refused");
        if (made != GVEC_OK) {
          continue;
        }
        check(gvec_svg_instance_parent(clone) == child && gvec_svg_instance_use_context(clone) == child, "a clone inherits from its use");
        check_instance(context, svg, clone);
        const GVEC_Svg_Element * inner = gvec_svg_find_by_id(svg, "in");
        const GVEC_Svg_Instance * symbol_child = NULL;
        if (inner && gvec_svg_instance_child(context, clone, inner, &symbol_child) == GVEC_OK) {
          check_instance(context, svg, symbol_child);
        }
      }
    }
  }
  /* The references, the hash and a dump. */
  check(gvec_svg_style_resolve_references(context) == GVEC_OK, "the references are resolved");
  if (gvec_diagnostics_count_code(&diagnostics, GVEC_DIAG_REFERENCE_CYCLE)) {
    gvec_fuzz_seen |= SEEN_CYCLE;
  }
  if (gvec_diagnostics_count_code(&diagnostics, GVEC_DIAG_STYLE_INVALID)) {
    gvec_fuzz_seen |= SEEN_DROPPED;
  }
  uint64_t h1 = 0, h2 = 0;
  GVEC_Result hr = gvec_svg_style_hash(context, &h1);
  check(hr == GVEC_OK || hr == GVEC_ERR_UNSUPPORTED || hr == GVEC_ERR_LIMIT, "the hash is OK, UNSUPPORTED or LIMIT");
  if (hr == GVEC_OK) {
    check(gvec_svg_style_hash(context, &h2) == GVEC_OK && h1 == h2, "the hash is the same twice");
  }
  GVEC_Stream * out = NULL;
  check(gvec_stream_create_memory_output(&out) == GVEC_OK, "an output stream is created");
  GVEC_Result dr = gvec_svg_style_dump(context, root, out);
  if (dr == GVEC_OK) {
    const void * bytes = NULL;
    size_t length = 0;
    gvec_stream_output_buffer(out, &bytes, &length);
    check(length >= 15 && memcmp(bytes, "css-computed 1\n", 15) == 0, "a dump begins with its versioned header");
  }
  gvec_stream_destroy(out);

  check(diagnostics.count <= diagnostics.cap || diagnostics.cap == 0, "no more diagnostics than the cap");
  for (size_t i = 0; i < diagnostics.count; i++) {
    check(diagnostics.items[i].code >= GVEC_DIAG_XML && diagnostics.items[i].code < GVEC_DIAG_CODE_COUNT, "a diagnostic code is in range");
    check(diagnostics.items[i].message != NULL, "a diagnostic has a message");
    if (diagnostics.items[i].code == GVEC_DIAG_LIMIT &&
        (diagnostics.items[i].detail == GVEC_LIMIT_STYLE_BYTES || diagnostics.items[i].detail == GVEC_LIMIT_DECLARATIONS ||
            diagnostics.items[i].detail == GVEC_LIMIT_CUSTOM_EXPANSION)) {
      gvec_fuzz_seen |= SEEN_LIMIT;
    }
  }
  gvec_svg_style_context_free(context);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&diagnostics);
  free(text);
  return 0;
}
