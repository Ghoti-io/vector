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
 * The lowering fuzzer: any document the loader accepts, lowered both ways under the scene's structural limits shrunk by the
 * input, and the report of a sink mapped back to the document.
 *
 * The first byte names one structural limit of the scene to shrink (the low nibble: the levels of nodes, the clips and groups
 * open in one list, the pictures nested in pictures, the items of a list, the expansion, the nodes of a version; the values
 * past the sixth shrink none) and the high nibble sets how far. The second byte's low bit asks for a foreground, and its
 * next two bits choose the size of the viewport the document is loaded in. The rest is the document.
 *
 * Invariants checked on every input (an abort is a finding):
 *   - both entry points return GVEC_OK (a limit refuses an element, it does not fail the call) and report the same
 *     diagnostics in the same order;
 *   - the edit publishes, whatever the limits: the walk holds the drawing to the scene's limits, so a publish is never the
 *     place a hostile document is found;
 *   - the list the published version emits equals the recorded list, bit for bit, and has the same hash;
 *   - lowering the same document into the published scene again changes nothing;
 *   - every entry of a sink's report maps to a site or is returned as unplaced, never as an error, for the list and for
 *     the version, and a placed site names an element and a line.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/shape-scene/dump_sink.h>
#include <ghoti.io/shape-scene/edit.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/report.h>
#include <ghoti.io/shape-scene/scene.h>
#include <ghoti.io/shape-scene/sink.h>
#include <ghoti.io/shape-scene/version.h>
#include <ghoti.io/vector/vector.h>

static unsigned gvec_fuzz_seen;

int gvec_fuzz_unreached(void);
int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

#define SEEN_NO_DOC 1u
#define SEEN_DRAWN 2u
#define SEEN_REFUSED 4u
#define SEEN_LIMIT 8u
#define SEEN_PICTURE 16u
#define SEEN_PLACED 32u
#define SEEN_SINK_REFUSED 64u

int gvec_fuzz_unreached(void) {
  static const struct {
    unsigned bit;
    const char * name;
  } k_expected[] = {{SEEN_NO_DOC, "a document that does not load"}, {SEEN_DRAWN, "a document that draws something"},
      {SEEN_REFUSED, "an element refused"}, {SEEN_LIMIT, "an element refused for the scene's limits"},
      {SEEN_PICTURE, "a use that is a picture"}, {SEEN_PLACED, "a report entry placed at an element"},
      {SEEN_SINK_REFUSED, "an item the sink refused"}};
  int missing = 0;
  for (size_t i = 0; i < sizeof k_expected / sizeof k_expected[0]; i++) {
    if (!(gvec_fuzz_seen & k_expected[i].bit)) {
      fprintf(stderr, "fuzz_lower: no input reached %s\n", k_expected[i].name);
      missing++;
    }
  }
  return missing;
}

static void check(int condition, const char * what) {
  if (!condition) {
    fprintf(stderr, "fuzz_lower: invariant broken: %s\n", what);
    abort();
  }
}

static GSHSCENE_Result discard(void * ctx, const void * bytes, size_t count) {
  (void) ctx;
  (void) bytes;
  (void) count;
  return GSHSCENE_OK;
}

/* Map every entry of a report; return how many were placed. */
static size_t map_report(const GVEC_Lower_Trace * trace, const GSHSCENE_Version * version, const GSHSCENE_Report * report) {
  size_t placed = 0;
  for (size_t i = 0; i < gshscene_report_count(report); i++) {
    GVEC_Lower_Site site;
    check(gvec_lower_trace_element(trace, version, report, i, &site) == GVEC_OK, "an entry of a report is mapped or unplaced");
    if (site.placed) {
      check(site.element != NULL && site.line >= 1 && site.column >= 1, "a placed site names an element and its place");
      placed++;
    }
  }
  return placed;
}

static void render_and_map(const GSHSCENE_List * list, const GVEC_Lower_Trace * trace, const GSHSCENE_Version * version) {
  GSHSCENE_Dump_Sink_Config config;
  gshscene_dump_sink_config_default(&config);
  config.max_extent = 3.0; /* so that most fills are refused while drawing and there is a report to map */
  GSHSCENE_Sink * sink = NULL;
  GSHSCENE_Report * report = NULL;
  check(gshscene_dump_sink_create(NULL, NULL, &config, &sink) == GSHSCENE_OK, "a sink is made");
  check(gshscene_report_create(NULL, NULL, &report) == GSHSCENE_OK, "a report is made");
  GSHSCENE_Writer writer;
  writer.write = discard;
  writer.ctx = NULL;
  GSHSCENE_Target target;
  memset(&target, 0, sizeof target);
  target.kind = GSHSCENE_TARGET_STREAM;
  target.surface = &writer;
  target.unit = GSHSCENE_UNIT_MILLIMETER;
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = list;
  request.target = &target;
  request.report = report;
  GSHSCENE_Result r = gshscene_sink_render(sink, &request);
  if (r == GSHSCENE_OK) {
    GSHSCENE_Report_Summary summary;
    check(gshscene_report_summary(report, &summary) == GSHSCENE_OK, "a summary is read");
    if (summary.by_class[GSHSCENE_ENTRY_REFUSED] > 0) {
      gvec_fuzz_seen |= SEEN_SINK_REFUSED;
    }
    if (map_report(trace, version, report) > 0) {
      gvec_fuzz_seen |= SEEN_PLACED;
    }
  }
  gshscene_report_destroy(report);
  gshscene_sink_destroy(sink);
}

static void set_limit(GSHSCENE_Limits * limits, unsigned which, size_t value) {
  switch (which) {
  case 1:
    limits->max_node_depth = value + 2;
    break;
  case 2:
    limits->max_depth = value;
    break;
  case 3:
    limits->max_picture_depth = value;
    break;
  case 4:
    limits->max_list_items = value;
    break;
  case 5:
    limits->max_expansion = value * 2 + 2;
    break;
  case 6:
    limits->max_nodes = value + 2;
    break;
  default:
    break;
  }
}

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }
  unsigned which = data[0] & 0x0f;
  size_t value = (size_t) (data[0] >> 4) + 1;
  uint8_t flags = data[1];
  const uint8_t * document = data + 2;
  size_t document_size = size - 2;

  GVEC_Stream * stream = NULL;
  GVEC_Doc * doc = NULL;
  GVEC_Diagnostics load_diagnostics, by_list, by_edit, again;
  gvec_diagnostics_init(&load_diagnostics, NULL);
  gvec_diagnostics_init(&by_list, NULL);
  gvec_diagnostics_init(&by_edit, NULL);
  gvec_diagnostics_init(&again, NULL);
  check(gvec_stream_create_memory(document, document_size, &stream) == GVEC_OK, "a stream is made");
  /* A style is computed for every element a `use` instantiates, so the expansion is held to what lets thousands of inputs run a minute. */
  GVEC_Limits document_limits;
  memset(&document_limits, 0, sizeof document_limits);
  document_limits.max_reference_expansion = 3000;
  GVEC_Load_Options load;
  memset(&load, 0, sizeof load);
  load.limits = &document_limits;
  static const double sizes[4][2] = {{0, 0}, {7, 5}, {64, 64}, {1000, 20}};
  load.viewport_width = sizes[(flags >> 1) & 3][0];
  load.viewport_height = sizes[(flags >> 1) & 3][1];
  GVEC_Result r = gvec_doc_load(stream, &load, &load_diagnostics, &doc);
  gvec_stream_destroy(stream);
  if (r != GVEC_OK) {
    gvec_fuzz_seen |= SEEN_NO_DOC;
    gvec_diagnostics_destroy(&load_diagnostics);
    gvec_diagnostics_destroy(&by_list);
    gvec_diagnostics_destroy(&by_edit);
    gvec_diagnostics_destroy(&again);
    return 0;
  }

  GSHSCENE_Limits limits;
  gshscene_limits_default(&limits);
  set_limit(&limits, which, value);
  GSHSCENE_Scene * scene = NULL;
  GSHSCENE_List * list = NULL;
  GSHSCENE_Edit * edit = NULL;
  const GSHSCENE_Version * version = NULL;
  const GSHSCENE_Version * second = NULL;
  const GSHSCENE_List * emitted = NULL;
  GVEC_Lower_Trace * list_trace = NULL;
  GVEC_Lower_Trace * edit_trace = NULL;
  check(gshscene_scene_create(NULL, &limits, NULL, &scene) == GSHSCENE_OK, "a scene is made");
  GSHSCENE_Store * store = gshscene_scene_store(scene);
  check(gshscene_list_create(NULL, &limits, store, &list) == GSHSCENE_OK, "a list is made");
  check(gvec_lower_trace_create(NULL, &list_trace) == GVEC_OK && gvec_lower_trace_create(NULL, &edit_trace) == GVEC_OK,
      "traces are made");

  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.store = store;
  options.scene_limits = &limits;
  if (flags & 1) {
    options.flags = GVEC_LOWER_SET_FOREGROUND;
    gshscene_color_init(&options.foreground);
    options.foreground.rgba[0] = 0.25;
  }
  options.trace = list_trace;
  check(gvec_doc_lower_list(doc, &options, list, &by_list) == GVEC_OK, "lowering into a list is OK");
  check(gshscene_list_close(list) == GSHSCENE_OK, "the list closes");
  options.trace = edit_trace;
  check(gshscene_scene_open_edit(scene, NULL, &edit) == GSHSCENE_OK, "an edit is opened");
  check(gvec_doc_lower_edit(doc, &options, edit, &by_edit) == GVEC_OK, "lowering into an edit is OK");
  check(by_list.count == by_edit.count && by_list.dropped == by_edit.dropped, "both report the same number of diagnostics");
  for (size_t i = 0; i < by_list.count; i++) {
    check(by_list.items[i].code == by_edit.items[i].code && by_list.items[i].detail == by_edit.items[i].detail &&
            by_list.items[i].line == by_edit.items[i].line,
        "both report the same diagnostic at the same place");
    if (by_list.items[i].code == GVEC_DIAG_LOWER_REFUSED) {
      gvec_fuzz_seen |= SEEN_REFUSED;
      if (by_list.items[i].detail == (uint32_t) GVEC_LOWER_LIMIT) {
        gvec_fuzz_seen |= SEEN_LIMIT;
      }
    }
  }
  GSHSCENE_Result published = gshscene_scene_publish(scene, edit, NULL, &version);
  check(published == GSHSCENE_OK, "the lowered edit publishes under any limits");
  gshscene_edit_destroy(edit);
  edit = NULL;
  check(gshscene_version_list(version, &emitted) == GSHSCENE_OK, "the version emits a list");
  check(gshscene_list_equal(list, emitted) && gshscene_list_hash(list) == gshscene_list_hash(emitted),
      "the list the edit emits equals the list that was recorded");
  if (gshscene_list_count(list) > 0) {
    gvec_fuzz_seen |= SEEN_DRAWN;
  }
  for (size_t i = 0; i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * item = NULL;
    check(gshscene_list_item(list, i, &item) == GSHSCENE_OK, "an item is read");
    if (item->kind == GSHSCENE_ITEM_INSTANCE) {
      gvec_fuzz_seen |= SEEN_PICTURE;
    }
  }
  render_and_map(list, list_trace, NULL);
  render_and_map(emitted, edit_trace, version);

  /* Again: nothing changes. */
  check(gshscene_scene_open_edit(scene, NULL, &edit) == GSHSCENE_OK, "a second edit is opened");
  options.trace = NULL;
  check(gvec_doc_lower_edit(doc, &options, edit, &again) == GVEC_OK, "lowering again is OK");
  check(gshscene_scene_publish(scene, edit, NULL, &second) == GSHSCENE_OK, "the second edit publishes");
  check(gshscene_version_serial(second) == gshscene_version_serial(version) &&
          gshscene_version_hash(second) == gshscene_version_hash(version),
      "lowering the same document again changes nothing");

  gshscene_version_release(second);
  gshscene_edit_destroy(edit);
  gshscene_version_release(version);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
  gvec_lower_trace_free(list_trace);
  gvec_lower_trace_free(edit_trace);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&load_diagnostics);
  gvec_diagnostics_destroy(&by_list);
  gvec_diagnostics_destroy(&by_edit);
  gvec_diagnostics_destroy(&again);
  return 0;
}
