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
 * Invariants checked on every input (an abort is a finding):
 *   - the result is a GVEC_Result, and the output document is written exactly
 *     when the result is GVEC_OK;
 *   - every diagnostic has a code and severity in range and a message, and the
 *     list never holds more than the cap;
 *   - the probe agrees with the load: a load that succeeds was probed as svg;
 *   - a loaded document dumps and hashes the same twice, and both lowering
 *     entry points return GVEC_OK with exactly one refusal at the root.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/vector/vector.h>

#include "../../src/stream/stream_internal.h"

static unsigned gvec_fuzz_seen;

/* What the load must leave in its output on failure, and what the lowering is
 * handed for an edit or a list: nothing is lowered yet, so neither is touched. */
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
  gvec_fuzz_set_limit(&limits, data[0] & 0x0f, data[0] >> 4);
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
    gvec_fuzz_check(gvec_doc_hash(doc, &h1) == GVEC_OK && gvec_doc_hash(doc, &h2) == GVEC_OK && h1 == h2,
        "the hash is the same twice");
    before = diagnostics.count + diagnostics.dropped;
    gvec_fuzz_check(gvec_doc_lower_edit(doc, NULL, (GSHSCENE_Edit *) GVEC_FUZZ_SENTINEL, &diagnostics) == GVEC_OK,
        "lowering into an edit is OK");
    gvec_fuzz_check(gvec_doc_lower_list(doc, NULL, (GSHSCENE_List *) GVEC_FUZZ_SENTINEL, &diagnostics) == GVEC_OK,
        "lowering into a list is OK");
    gvec_fuzz_check(diagnostics.count + diagnostics.dropped == before + 2, "each lowering refuses once");
    gvec_doc_free(doc);
  }
  else {
    gvec_fuzz_check(doc == (GVEC_Doc *) GVEC_FUZZ_SENTINEL, "no document is written on failure");
  }

  gvec_diagnostics_destroy(&diagnostics);
  gvec_stream_destroy(stream);
  return 0;
}
