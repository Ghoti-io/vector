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
 * @file lower.c
 *
 * Lower a document into an immediate-mode list, render it, and print what a sink's report says about it in terms
 * of the document.
 *
 *     lower
 *
 * The document is built in. Its first shape is small and its second is large; the test sink the scene library
 * ships is given a resource limit that the large one passes, so the sink refuses that fill. The report entry is
 * mapped back, through the trace, to the element and the line it came from. (Lowering into an edit of a retained
 * scene is the same call with `gvec_doc_lower_edit`; publishing it is the application's, so no example does.) The
 * document also has a `path` filled with a gradient, which this library does not lower yet: that is a refusal of
 * the lowering itself, printed as "line N, <element>: refused, <reason>". The program checks what it shows and exits 1 when that does
 * not hold.
 *
 * An application that renders with a real sink reads its report the same way.
 */

#include <stdio.h>
#include <string.h>

#include <ghoti.io/shape-scene/dump_sink.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/report.h>
#include <ghoti.io/shape-scene/resource.h>
#include <ghoti.io/shape-scene/sink.h>
#include <ghoti.io/vector/vector.h>

static const char document[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 100 100\">\n"
    "<path d=\"M10 10H20V20H10Z\" fill=\"#c0392b\"/>\n"
    "<path d=\"M0 0H90V90H0Z\" fill=\"#2980b9\" opacity=\".5\"/>\n"
    "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#000\"/><stop offset=\"1\" stop-color=\"#fff\"/></linearGradient></defs>\n"
    "<path d=\"M5 5H15V15H5Z\" fill=\"url(#g)\"/>\n"
    "</svg>\n";

static GSHSCENE_Result discard(void * context, const void * bytes, size_t count) {
  (void) context;
  (void) bytes;
  (void) count;
  return GSHSCENE_OK;
}

int main(void) {
  GVEC_Stream * stream = NULL;
  GVEC_Doc * doc = NULL;
  GVEC_Diagnostics diagnostics;
  GVEC_Lower_Trace * trace = NULL;
  GSHSCENE_Store * store = NULL;
  GSHSCENE_List * list = NULL;
  GSHSCENE_Sink * sink = NULL;
  GSHSCENE_Report * report = NULL;
  int status = 1;
  gvec_diagnostics_init(&diagnostics, NULL);

  if (gvec_stream_create_memory(document, sizeof document - 1, &stream) != GVEC_OK ||
      gvec_doc_load(stream, NULL, &diagnostics, &doc) != GVEC_OK) {
    fprintf(stderr, "lower: the document does not load\n");
    goto done;
  }

  /* The list is recorded over a store, which the lowering is told of, and closed when it is done. */
  if (gvec_lower_trace_create(NULL, &trace) != GVEC_OK || gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK ||
      gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
    fprintf(stderr, "lower: cannot make a list\n");
    goto done;
  }
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.store = store;
  options.trace = trace;
  if (gvec_doc_lower_list(doc, &options, list, &diagnostics) != GVEC_OK || gshscene_list_close(list) != GSHSCENE_OK) {
    fprintf(stderr, "lower: the lowering failed\n");
    goto done;
  }

  /* What the lowering itself could not draw, each refusal with its place and its reason. */
  size_t refusals = 0;
  for (size_t i = 0; i < diagnostics.count; i++) {
    const GVEC_Diagnostic * d = &diagnostics.items[i];
    if (d->code == GVEC_DIAG_LOWER_REFUSED) {
      printf("line %zu, %s: refused, %s\n", d->line, d->element, gvec_lower_reason_string((GVEC_Lower_Reason) d->detail));
      refusals++;
    }
  }

  /* Render the list with a sink that refuses a fill wider than 30 units. */
  GSHSCENE_Dump_Sink_Config config;
  gshscene_dump_sink_config_default(&config);
  config.max_extent = 30.0;
  GSHSCENE_Writer writer;
  writer.write = discard;
  writer.ctx = NULL;
  GSHSCENE_Target target;
  memset(&target, 0, sizeof target);
  target.kind = GSHSCENE_TARGET_STREAM;
  target.surface = &writer;
  target.unit = GSHSCENE_UNIT_MILLIMETER;
  if (gshscene_dump_sink_create(NULL, NULL, &config, &sink) != GSHSCENE_OK ||
      gshscene_report_create(NULL, NULL, &report) != GSHSCENE_OK) {
    fprintf(stderr, "lower: cannot set up the render\n");
    goto done;
  }
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = list;
  request.target = &target;
  request.report = report;
  if (gshscene_sink_render(sink, &request) != GSHSCENE_OK) {
    fprintf(stderr, "lower: the render failed\n");
    goto done;
  }

  /* The report speaks of items; the trace says which element each came from (for an edit, it is given the published version too). */
  size_t placed = 0;
  for (size_t i = 0; i < gshscene_report_count(report); i++) {
    GSHSCENE_Report_Entry entry;
    GVEC_Lower_Site site;
    if (gshscene_report_entry(report, i, &entry) != GSHSCENE_OK ||
        gvec_lower_trace_element(trace, NULL, report, i, &site) != GVEC_OK) {
      fprintf(stderr, "lower: cannot read the report\n");
      goto done;
    }
    if (entry.entry_class == GSHSCENE_ENTRY_REFUSED && site.placed) {
      printf("line %zu, %s: refused by the sink, %s\n", site.line, site.element, gshscene_reason_string(entry.reason));
      placed++;
    }
  }
  if (refusals != 1 || placed != 1) {
    fprintf(stderr, "lower: expected one refusal by the lowering and one by the sink\n");
    goto done;
  }
  status = 0;

done:
  gshscene_report_destroy(report);
  gshscene_sink_destroy(sink);
  gshscene_list_destroy(list);
  gshscene_store_destroy(store);
  gvec_lower_trace_free(trace);
  gvec_doc_free(doc);
  gvec_stream_destroy(stream);
  gvec_diagnostics_destroy(&diagnostics);
  return status;
}
