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
 * The load fuzzer: raw bytes, plain or gzip-wrapped, through the probe, the
 * load, the dump, the hash and both lowering entry points.
 *
 * The first byte is the options byte. Its low nibble names one GVEC_Limits
 * field to shrink (values past the last field shrink none), its high nibble
 * sets the shrunk value, and the second byte, when odd, reads the input
 * through a stream that returns at most (second byte >> 1) % 7 + 1 bytes per
 * read. So every limit is driven by the corpus, and short reads too.
 *
 * Invariants checked on every input (an abort is a finding), beside the typed document's (below):
 *   - the result is a GVEC_Result, and the output document is written exactly
 *     when the result is GVEC_OK;
 *   - every diagnostic has a code and severity in range and a message, and the
 *     list never holds more than the cap;
 *   - the probe agrees with the load: a load that succeeds was probed as svg;
 *   - a loaded document dumps and hashes the same twice;
 *   - both lowering entry points return GVEC_OK or a limit, refuse the same
 *     elements in the same order, and the list the edit lowering publishes
 *     equals the list the other one records (the equality gate); lowering the
 *     document into the published scene again changes nothing.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/shape-scene/edit.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/scene.h>
#include <ghoti.io/shape-scene/version.h>
#include <ghoti.io/vector/vector.h>

#include "../../src/stream/stream_internal.h"

static unsigned gvec_fuzz_seen;

/* What the load must leave in its output on failure. */
#define GVEC_FUZZ_SENTINEL ((uintptr_t) 64) /* a pointer value nothing allocates, never dereferenced */

/* The outcomes the committed corpus must reach. */
static const GVEC_Result gvec_fuzz_expected[] = {
    GVEC_OK, GVEC_ERR_FORMAT, GVEC_ERR_UNSUPPORTED, GVEC_ERR_LIMIT, GVEC_ERR_CORRUPT,
};

int gvec_fuzz_unreached(void);
int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

int gvec_fuzz_unreached(void) {
  int missing = 0;
  for (size_t i = 0; i < sizeof(gvec_fuzz_expected) / sizeof(gvec_fuzz_expected[0]); i++) {
    if (!(gvec_fuzz_seen & (1u << gvec_fuzz_expected[i]))) {
      fprintf(stderr, "fuzz_load: no input reached %s\n", gvec_result_string(gvec_fuzz_expected[i]));
      missing++;
    }
  }
  return missing;
}

static void gvec_fuzz_check(int condition, const char * what) {
  if (!condition) {
    fprintf(stderr, "fuzz_load: invariant broken: %s\n", what);
    abort();
  }
}

static void gvec_fuzz_check_diagnostics(const GVEC_Diagnostics * d) {
  gvec_fuzz_check(d->count <= d->cap || d->cap == 0, "more diagnostics than the cap");
  for (size_t i = 0; i < d->count; i++) {
    const GVEC_Diagnostic * e = &d->items[i];
    gvec_fuzz_check(e->code >= GVEC_DIAG_XML && e->code < GVEC_DIAG_CODE_COUNT, "diagnostic code in range");
    gvec_fuzz_check((unsigned) e->severity < GVEC_DIAG_SEVERITY_COUNT, "severity in range");
    gvec_fuzz_check(e->message != NULL, "diagnostic has a message");
  }
}

/* The refusals of a lowering: the reason and the line of each, in order. */
static void gvec_fuzz_check_refusals(const GVEC_Diagnostics * a, const GVEC_Diagnostics * b) {
  gvec_fuzz_check(a->count == b->count && a->dropped == b->dropped, "both lowerings report the same number of diagnostics");
  for (size_t i = 0; i < a->count; i++) {
    gvec_fuzz_check(a->items[i].code == b->items[i].code && a->items[i].detail == b->items[i].detail &&
            a->items[i].line == b->items[i].line && a->items[i].column == b->items[i].column,
        "both lowerings report the same diagnostic at the same place");
  }
}

/* Lower the document both ways and require the two to agree (see the file comment). */
static void gvec_fuzz_lowering(const GVEC_Doc * doc) {
  GSHSCENE_Scene * scene = NULL;
  GSHSCENE_List * list = NULL;
  GSHSCENE_Edit * edit = NULL;
  const GSHSCENE_Version * version = NULL;
  const GSHSCENE_List * emitted = NULL;
  GVEC_Diagnostics by_list, by_edit;
  GVEC_Lower_Options options;
  gvec_diagnostics_init(&by_list, NULL);
  gvec_diagnostics_init(&by_edit, NULL);
  gvec_fuzz_check(gshscene_scene_create(NULL, NULL, NULL, &scene) == GSHSCENE_OK, "a scene is made");
  GSHSCENE_Store * store = gshscene_scene_store(scene);
  gvec_fuzz_check(gshscene_list_create(NULL, NULL, store, &list) == GSHSCENE_OK, "a list is made");
  memset(&options, 0, sizeof options);
  options.store = store;
  GVEC_Result lr = gvec_doc_lower_list(doc, &options, list, &by_list);
  gvec_fuzz_check(lr == GVEC_OK, "lowering into a list is OK");
  gvec_fuzz_check_diagnostics(&by_list);
  gvec_fuzz_check(gshscene_list_close(list) == GSHSCENE_OK, "the list closes");
  gvec_fuzz_check(gshscene_scene_open_edit(scene, NULL, &edit) == GSHSCENE_OK, "an edit is opened");
  GVEC_Result er = gvec_doc_lower_edit(doc, NULL, edit, &by_edit);
  gvec_fuzz_check(er == GVEC_OK, "lowering into an edit is OK");
  gvec_fuzz_check_refusals(&by_list, &by_edit);
  gvec_fuzz_check(gshscene_scene_publish(scene, edit, NULL, &version) == GSHSCENE_OK, "the lowered edit publishes");
  gshscene_edit_destroy(edit);
  edit = NULL;
  gvec_fuzz_check(gshscene_version_list(version, &emitted) == GSHSCENE_OK, "the published version emits a list");
  gvec_fuzz_check(gshscene_list_equal(list, emitted) && gshscene_list_hash(list) == gshscene_list_hash(emitted),
      "the list the edit emits equals the list that was recorded");
  /* Lowering again into the scene as it now is touches nothing. */
  gvec_fuzz_check(gshscene_scene_open_edit(scene, NULL, &edit) == GSHSCENE_OK, "a second edit is opened");
  GVEC_Diagnostics again;
  gvec_diagnostics_init(&again, NULL);
  gvec_fuzz_check(gvec_doc_lower_edit(doc, NULL, edit, &again) == GVEC_OK, "lowering again is OK");
  const GSHSCENE_Version * second = NULL;
  gvec_fuzz_check(gshscene_scene_publish(scene, edit, NULL, &second) == GSHSCENE_OK, "the second edit publishes");
  gvec_fuzz_check(gshscene_version_serial(second) == gshscene_version_serial(version) &&
          gshscene_version_hash(second) == gshscene_version_hash(version),
      "lowering the same document again changes nothing");
  gshscene_version_release(second);
  gvec_diagnostics_destroy(&again);
  gshscene_edit_destroy(edit);
  gshscene_version_release(version);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
  gvec_diagnostics_destroy(&by_list);
  gvec_diagnostics_destroy(&by_edit);
}


/* The typed document's invariants: the tree is consistent, every attribute is in one of its classes with a value that
 * matches, every number is finite, the keys are distinct, the bounds held, and a reference points where it says. */
static int gvec_fuzz_key_compare(const void * a, const void * b) {
  uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int gvec_fuzz_finite(double v) {
  return v - v == 0.0;
}

static void gvec_fuzz_check_attribute(const GVEC_Svg_Attribute * a, const GVEC_Limits * limits) {
  gvec_fuzz_check(gvec_svg_attribute_name(a) != NULL && gvec_svg_attribute_raw(a, NULL) != NULL, "an attribute has a name and a value");
  GVEC_Svg_Attribute_Class cls = gvec_svg_attribute_class(a);
  gvec_fuzz_check((unsigned) cls < GVEC_SVG_ATTR_CLASS_COUNT, "the attribute class is in range");
  GVEC_Svg_Value_Kind kind = gvec_svg_attribute_value_kind(a);
  gvec_fuzz_check(cls == GVEC_SVG_ATTR_TYPED || kind == GVEC_SVG_VALUE_NONE, "only a typed attribute has a value");
  if (cls == GVEC_SVG_ATTR_INVALID) {
    gvec_fuzz_check(gvec_svg_attribute_error_reason(a) != NULL, "an invalid attribute says why");
    gvec_fuzz_check(gvec_svg_attribute_error_offset(a) <= (size_t) 1 << 40, "the error offset is plausible");
  }
  const double * items;
  const GVEC_Svg_Length * lengths;
  size_t count;
  double number;
  GVEC_Svg_Length length;
  if (gvec_svg_attribute_number(a, &number)) {
    gvec_fuzz_check(gvec_fuzz_finite(number), "a number is finite");
  }
  if (gvec_svg_attribute_length(a, &length)) {
    gvec_fuzz_check(gvec_fuzz_finite(length.value) && (unsigned) length.unit < GVEC_SVG_UNIT_COUNT, "a length is finite with a unit");
  }
  if (gvec_svg_attribute_numbers(a, &items, &count)) {
    gvec_fuzz_check(count <= limits->max_list_items, "a list is within its bound");
    for (size_t i = 0; i < count; i++) {
      gvec_fuzz_check(gvec_fuzz_finite(items[i]), "a list number is finite");
    }
  }
  if (gvec_svg_attribute_lengths(a, &lengths, &count)) {
    gvec_fuzz_check(count <= limits->max_list_items, "a length list is within its bound");
    for (size_t i = 0; i < count; i++) {
      gvec_fuzz_check(gvec_fuzz_finite(lengths[i].value), "a list length is finite");
    }
  }
  const GVEC_Svg_Path_Command * commands;
  const GVEC_Svg_Path_Segment * segments;
  size_t command_count, segment_count;
  if (gvec_svg_attribute_path(a, &commands, &command_count, &segments, &segment_count)) {
    gvec_fuzz_check(segment_count <= limits->max_path_segments && command_count == segment_count, "a path is within its bound");
    for (size_t i = 0; i < segment_count; i++) {
      gvec_fuzz_check((unsigned) segments[i].kind < GVEC_SVG_SEGMENT_COUNT, "a segment kind is in range");
      gvec_fuzz_check(gvec_fuzz_finite(segments[i].x) && gvec_fuzz_finite(segments[i].y) && gvec_fuzz_finite(segments[i].x1) &&
              gvec_fuzz_finite(segments[i].y1) && gvec_fuzz_finite(segments[i].x2) && gvec_fuzz_finite(segments[i].y2),
          "a segment is finite");
    }
  }
  GVEC_Svg_Reference ref;
  if (gvec_svg_attribute_reference(a, &ref) && ref.kind == GVEC_SVG_REF_DATA) {
    unsigned char out[64];
    size_t written = 0;
    GVEC_Result r = gvec_svg_data_uri_decode(&ref.data, out, sizeof out, &written);
    gvec_fuzz_check(r == GVEC_OK || r == GVEC_ERR_LIMIT || r == GVEC_ERR_FORMAT, "a data URI decodes or says why not");
  }
}

static void gvec_fuzz_check_document(const GVEC_Svg_Document * svg, const GVEC_Limits * limits) {
  size_t count = gvec_svg_element_count(svg);
  gvec_fuzz_check(count >= 1 && count <= limits->max_elements, "the element count is within its bound");
  uint64_t * keys = malloc(count * sizeof(uint64_t));
  gvec_fuzz_check(keys != NULL, "scratch memory");
  const GVEC_Svg_Element * root = gvec_svg_root(svg);
  gvec_fuzz_check(root && gvec_svg_element_kind(root) == GVEC_SVG_KIND_SVG && gvec_svg_element_parent(root) == NULL, "the root is svg");
  /* A pre-order walk along the sibling and parent links, with no recursion. */
  size_t visited = 0;
  const GVEC_Svg_Element * e = root;
  while (e) {
    gvec_fuzz_check(visited < count, "the walk visits no more elements than the count");
    keys[visited++] = gvec_svg_element_key(e);
    gvec_fuzz_check(gvec_svg_element_name(e) != NULL && (unsigned) gvec_svg_element_kind(e) < GVEC_SVG_KIND_COUNT, "an element has a name and a kind");
    for (size_t i = 0; i < gvec_svg_element_attribute_count(e); i++) {
      gvec_fuzz_check_attribute(gvec_svg_element_attribute_at(e, i), limits);
    }
    const char * id = gvec_svg_element_id(e);
    if (id) {
      const GVEC_Svg_Element * found = gvec_svg_find_by_id(svg, id);
      gvec_fuzz_check(found != NULL, "an element's id is found");
      gvec_fuzz_check(gvec_svg_element_id(found) && strcmp(gvec_svg_element_id(found), id) == 0, "the id index is right");
    }
    const GVEC_Svg_Element * target = gvec_svg_element_href_target(e);
    if (target) {
      const GVEC_Svg_Attribute * href = gvec_svg_element_find_attribute(e, "href");
      if (!href || gvec_svg_attribute_class(href) != GVEC_SVG_ATTR_TYPED) {
        href = gvec_svg_element_find_attribute(e, "xlink:href");
      }
      GVEC_Svg_Reference ref;
      gvec_fuzz_check(href && gvec_svg_attribute_reference(href, &ref) && ref.kind == GVEC_SVG_REF_INTERNAL, "a target comes from an internal href");
      gvec_fuzz_check(gvec_svg_element_id(target) && strcmp(gvec_svg_element_id(target), ref.fragment) == 0, "a target has the id the href names");
      gvec_fuzz_check(!gvec_svg_element_on_cycle(e) && !gvec_svg_element_expansion_limited(e), "a cut edge has no target");
    }
    const GVEC_Svg_Element * child = gvec_svg_element_first_child(e);
    if (child) {
      gvec_fuzz_check(gvec_svg_element_parent(child) == e, "a child's parent is the element");
      e = child;
      continue;
    }
    while (e && !gvec_svg_element_next_sibling(e)) {
      e = gvec_svg_element_parent(e);
    }
    if (e) {
      e = gvec_svg_element_next_sibling(e);
    }
  }
  gvec_fuzz_check(visited == count, "the walk visits every element");
  qsort(keys, count, sizeof(uint64_t), gvec_fuzz_key_compare);
  for (size_t i = 1; i < count; i++) {
    gvec_fuzz_check(keys[i] != keys[i - 1], "the keys are distinct");
  }
  free(keys);
}

/* The lowering of a document costs time for each element a `use` instantiates (a style is computed for each), so the fuzzer holds the
 * expansion to what lets it run thousands of inputs a minute; the default of a million is for the callers who want it. */
#define GVEC_FUZZ_EXPANSION 3000

static void gvec_fuzz_cap_expansion(GVEC_Limits * limits) {
  if (limits->max_reference_expansion == 0 || limits->max_reference_expansion > GVEC_FUZZ_EXPANSION) {
    limits->max_reference_expansion = GVEC_FUZZ_EXPANSION;
  }
}

static void gvec_fuzz_set_limit(GVEC_Limits * limits, unsigned field, unsigned scale) {
  size_t small = (size_t) scale + 1;
  switch (field) {
  case GVEC_LIMIT_INPUT_BYTES:
    limits->max_input_bytes = small * 64;
    break;
  case GVEC_LIMIT_INFLATED_BYTES:
    limits->max_inflated_bytes = small * 64;
    break;
  case GVEC_LIMIT_ELEMENTS:
    limits->max_elements = small;
    break;
  case GVEC_LIMIT_DEPTH:
    limits->max_depth = small;
    break;
  case GVEC_LIMIT_ATTRIBUTES:
    limits->max_attributes = small;
    break;
  case GVEC_LIMIT_ENTITY_EXPANDED_BYTES:
    limits->max_entity_expanded_bytes = small * 16;
    break;
  case GVEC_LIMIT_ENTITY_DEPTH:
    limits->max_entity_depth = small;
    break;
  case GVEC_LIMIT_ENTITY_REFERENCES:
    limits->max_entity_references = small;
    break;
  case GVEC_LIMIT_ENTITY_AMPLIFICATION:
    limits->max_entity_amplification = small;
    break;
  case GVEC_LIMIT_PATH_SEGMENTS:
    limits->max_path_segments = small;
    break;
  case GVEC_LIMIT_LIST_ITEMS:
    limits->max_list_items = small;
    break;
  case GVEC_LIMIT_REFERENCE_EXPANSION:
    limits->max_reference_expansion = small;
    break;
  case GVEC_LIMIT_TEXT_BYTES:
    limits->max_text_bytes = small;
    break;
  case GVEC_LIMIT_DIAGNOSTICS:
    limits->max_diagnostics = small;
    break;
  default:
    break;
  }
}

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GVEC_Limits limits;
  GVEC_Load_Options options;
  GVEC_Diagnostics diagnostics;
  GVEC_Stream * stream = NULL;
  GVEC_Stream * probe_stream = NULL;
  GVEC_Probe_Result probe;
  GVEC_Doc * doc = (GVEC_Doc *) GVEC_FUZZ_SENTINEL;
  GVEC_Result r;
  size_t chunk = 0;

  if (size < 2) {
    return 0;
  }
  memset(&limits, 0, sizeof(limits));
  unsigned data_options_field = data[0] & 0x0f, data_options_scale = data[0] >> 4;
  gvec_fuzz_set_limit(&limits, data_options_field, data_options_scale);
  gvec_fuzz_cap_expansion(&limits);
  if (data[1] & 1) {
    chunk = (size_t) ((data[1] >> 1) % 7) + 1;
  }
  data += 2;
  size -= 2;

  memset(&options, 0, sizeof(options));
  options.limits = &limits;
  gvec_diagnostics_init(&diagnostics, NULL);

  if (chunk) {
    r = gvec_stream_create_memory_chunked_with_allocator(NULL, data, size, chunk, &stream);
  }
  else {
    r = gvec_stream_create_memory(data, size, &stream);
  }
  gvec_fuzz_check(r == GVEC_OK, "a memory stream is created");

  r = gvec_stream_create_memory(data, size, &probe_stream);
  gvec_fuzz_check(r == GVEC_OK, "a probe stream is created");
  memset(&probe, 0, sizeof(probe));
  GVEC_Result probed = gvec_probe(probe_stream, &limits, &probe);
  gvec_stream_destroy(probe_stream);

  r = gvec_doc_load(stream, &options, &diagnostics, &doc);
  gvec_fuzz_check((unsigned) r < GVEC_RESULT_COUNT, "the result is a result");
  gvec_fuzz_seen |= 1u << r;
  gvec_fuzz_check_diagnostics(&diagnostics);

  if (r == GVEC_OK) {
    uint64_t h1 = 0;
    uint64_t h2 = 0;
    size_t before;
    gvec_fuzz_check(doc != (GVEC_Doc *) GVEC_FUZZ_SENTINEL && doc != NULL, "a document is written on success");
    gvec_fuzz_check(probed == GVEC_OK && probe.format_name && strcmp(probe.format_name, "svg") == 0,
        "a document that loads was probed as svg");
    {
      GVEC_Limits resolved;
      gvec_limits_default(&resolved);
      /* The bounds the load ran under: the defaults, with the one field the options byte shrank. */
      gvec_fuzz_set_limit(&resolved, data_options_field, data_options_scale);
      gvec_fuzz_cap_expansion(&resolved);
      gvec_fuzz_check(gvec_svg_document(doc) != NULL, "an SVG document has a typed document");
      gvec_fuzz_check_document(gvec_svg_document(doc), &resolved);
    }
    gvec_fuzz_check(gvec_doc_hash(doc, &h1) == GVEC_OK && gvec_doc_hash(doc, &h2) == GVEC_OK && h1 == h2,
        "the hash is the same twice");
    before = diagnostics.count + diagnostics.dropped;
    gvec_fuzz_lowering(doc);
    gvec_fuzz_check(diagnostics.count + diagnostics.dropped == before, "the load's diagnostics are not changed by lowering");
    gvec_doc_free(doc);
  }
  else {
    gvec_fuzz_check(doc == (GVEC_Doc *) GVEC_FUZZ_SENTINEL, "no document is written on failure");
  }

  gvec_diagnostics_destroy(&diagnostics);
  gvec_stream_destroy(stream);
  return 0;
}
