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
 * @file load.c
 *
 * Load an SVG file, print what the load reported, and lower it into a
 * shape-scene list.
 *
 *     load [FILE]
 *
 * With no FILE it loads a document built in below, which names an external
 * entity: the load keeps the reference and reports it, and nothing is
 * fetched. Each diagnostic is printed as "line N, <element>: message". The
 * program checks what it shows and exits 1 when that does not hold.
 */

#include <stdio.h>
#include <string.h>

#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/resource.h>
#include <ghoti.io/vector/vector.h>

static const char builtin[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE svg [\n"
    "  <!ENTITY logo SYSTEM \"logo.xml\">\n"
    "]>\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\">\n"
    "  <g>&logo;</g>\n"
    "</svg>\n";

static void print_diagnostics(const GVEC_Diagnostics * d) {
  static const char * const severities[] = {"notice", "warning", "error"};
  for (size_t i = 0; i < d->count; i++) {
    const GVEC_Diagnostic * e = &d->items[i];
    if (e->line) {
      printf("line %zu", e->line);
    }
    else {
      printf("document");
    }
    if (e->element) {
      printf(", <%s>", e->element);
    }
    printf(": %s: %s\n", severities[e->severity], e->message);
  }
  if (d->dropped) {
    printf("(%zu more not kept)\n", d->dropped);
  }
}

int main(int argc, char ** argv) {
  GVEC_Stream * stream = NULL;
  GVEC_Diagnostics diagnostics;
  GVEC_Doc * doc = NULL;
  GSHSCENE_Store * store = NULL;
  GSHSCENE_List * list = NULL;
  GVEC_Result r;
  int status = 0;

  if (argc > 1) {
    r = gvec_stream_create_file(argv[1], &stream);
  }
  else {
    r = gvec_stream_create_memory(builtin, sizeof(builtin) - 1, &stream);
  }
  if (r != GVEC_OK) {
    fprintf(stderr, "load: cannot open: %s\n", gvec_result_string(r));
    return 1;
  }

  gvec_diagnostics_init(&diagnostics, NULL);
  r = gvec_doc_load(stream, NULL, &diagnostics, &doc);
  gvec_stream_destroy(stream);
  printf("load: %s\n", gvec_result_string(r));
  print_diagnostics(&diagnostics);
  if (r != GVEC_OK) {
    gvec_diagnostics_destroy(&diagnostics);
    return 1;
  }
  printf("format %s, transport %s\n", gvec_doc_format(doc),
      gvec_doc_transport(doc) == GVEC_TRANSPORT_GZIP ? "gzip" : "none");

  if (argc == 1 && gvec_diagnostics_count_code(&diagnostics, GVEC_DIAG_ENTITY_SKIPPED) != 1) {
    fprintf(stderr, "load: expected the external entity to be reported once\n");
    status = 1;
  }

  /* Lower into an immediate-mode list over a store. What cannot be drawn is
   * reported, one refusal for each element, with its line. */
  gvec_diagnostics_clear(&diagnostics);
  if (gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK || gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
    fprintf(stderr, "load: cannot create a shape-scene list\n");
    status = 1;
  }
  else {
    GVEC_Lower_Options options;
    memset(&options, 0, sizeof options);
    options.store = store;
    r = gvec_doc_lower_list(doc, &options, list, &diagnostics);
    printf("lower: %s, %zu items, %zu refused\n", gvec_result_string(r), gshscene_list_count(list),
        gvec_diagnostics_count_code(&diagnostics, GVEC_DIAG_LOWER_REFUSED));
    print_diagnostics(&diagnostics);
    if (r != GVEC_OK || (argc == 1 && gvec_diagnostics_count_code(&diagnostics, GVEC_DIAG_LOWER_REFUSED) != 0)) {
      fprintf(stderr, "load: expected the document to lower without a refusal\n");
      status = 1;
    }
  }

  gshscene_list_destroy(list);
  gshscene_store_destroy(store);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&diagnostics);
  return status;
}
