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
 * The resolver fuzzer: a document that names resources, and a resolver whose every answer is the fuzzer's, lowered both ways.
 *
 * The first byte chooses the limits of the resolver (its low nibble the most resources minus one, its high nibble the deepest
 * nesting minus one) and the second byte the most `use` instantiations (1 to 64). The rest is a document, a NUL byte, and a script:
 * records of one status byte (the low three bits choose OK, REFUSED, NOT_FOUND, FAILED, an answer longer than the limit, a size with
 * no bytes, or an unknown status; the high bits whether the media type is given and which), a length byte and that many bytes.
 * The k-th distinct request is answered by the k-th record (the script repeats), so one input decides what every reference is.
 *
 * Invariants (an abort is a finding): both entry points return GVEC_OK, and the resolver is asked no more than `max_resources` times,
 * no request is deeper than `max_resource_depth`, no (kind, url) is asked twice in one lowering, no request has a size limit of zero,
 * and the diagnostics of the two entry points are the same, in the same order.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/scene.h>
#include <ghoti.io/vector/vector.h>

int gvec_fuzz_unreached(void);
int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

#define SEEN_ASKED 1u
#define SEEN_DRAWN 2u
#define SEEN_REFUSED 4u
#define SEEN_STYLE 8u
#define SEEN_DOCUMENT 16u
#define SEEN_IMAGE 32u
#define SEEN_FAILED 64u
#define SEEN_LIMIT 128u

static unsigned seen_bits;

int gvec_fuzz_unreached(void) {
  static const struct {
    unsigned bit;
    const char * name;
  } expected[] = {{SEEN_ASKED, "a request"}, {SEEN_DRAWN, "a drawing"}, {SEEN_REFUSED, "a refusal"}, {SEEN_STYLE, "a style sheet asked for"},
      {SEEN_DOCUMENT, "a document asked for"}, {SEEN_IMAGE, "an image asked for"}, {SEEN_FAILED, "a status that is not found"},
      {SEEN_LIMIT, "a refusal for a limit"}};
  int missing = 0;
  for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
    if (!(seen_bits & expected[i].bit)) {
      fprintf(stderr, "fuzz_resolver: no input reached %s\n", expected[i].name);
      missing++;
    }
  }
  return missing;
}

static void check(int condition, const char * what) {
  if (!condition) {
    fprintf(stderr, "fuzz_resolver: invariant broken: %s\n", what);
    abort();
  }
}

typedef struct request_record {
  int kind;
  char * url;
} request_record;

typedef struct script {
  const uint8_t * data;
  size_t size;
  size_t at;              /* where the next record is read from */
  size_t max_resources, max_depth;
  request_record * asked;
  size_t asked_count, asked_capacity;
  uint8_t * answer;       /* what the last reply points at: overwritten by the next, which shows that the library copied */
  size_t answer_size;
  char media[16];
} script;

static GVEC_Resource_Status resolve(void * user, const GVEC_Resource_Request * request, GVEC_Resource_Reply * reply) {
  script * s = user;
  seen_bits |= SEEN_ASKED;
  seen_bits |= request->kind == GVEC_RESOURCE_STYLE ? SEEN_STYLE : request->kind == GVEC_RESOURCE_DOCUMENT ? SEEN_DOCUMENT : SEEN_IMAGE;
  check(request->url != NULL && request->reference != NULL, "a request names its reference and its url");
  check(request->max_bytes > 0, "a request has a size limit");
  check(request->depth >= 1 && request->depth <= s->max_depth, "a request is no deeper than max_resource_depth");
  for (size_t i = 0; i < s->asked_count; i++) {
    check(!(s->asked[i].kind == (int) request->kind && strcmp(s->asked[i].url, request->url) == 0), "a (kind, url) is asked once in a lowering");
  }
  check(s->asked_count < s->max_resources, "the resolver is asked no more than max_resources times");
  if (s->asked_count == s->asked_capacity) {
    size_t capacity = s->asked_capacity ? s->asked_capacity * 2 : 16;
    request_record * fresh = realloc(s->asked, capacity * sizeof *fresh);
    check(fresh != NULL, "memory for the fuzzer's own list");
    s->asked = fresh;
    s->asked_capacity = capacity;
  }
  s->asked[s->asked_count].kind = (int) request->kind;
  size_t url_length = strlen(request->url) + 1;
  s->asked[s->asked_count].url = malloc(url_length);
  check(s->asked[s->asked_count].url != NULL, "memory for the fuzzer's own url");
  memcpy(s->asked[s->asked_count].url, request->url, url_length);
  s->asked_count++;
  if (s->at + 2 > s->size) {
    s->at = 0;
    if (s->size < 2) {
      return GVEC_RESOURCE_NOT_FOUND;
    }
  }
  uint8_t status = s->data[s->at], length = s->data[s->at + 1];
  s->at += 2;
  if (length > s->size - s->at) {
    length = (uint8_t) (s->size - s->at);
  }
  const uint8_t * body = s->data + s->at;
  s->at += length;
  switch (status & 7) {
  case 0:
  case 1: {
    free(s->answer);
    s->answer = malloc(length ? length : 1);
    check(s->answer != NULL, "memory for the fuzzer's own answer");
    memcpy(s->answer, body, length);
    s->answer_size = length;
    reply->bytes = s->answer;
    reply->size = length;
    if (status & 0x80) {
      static const char * const types[] = {"image/svg+xml", "image/png", "text/css", "application/octet-stream"};
      reply->media_type = types[(status >> 3) & 3];
    }
    return GVEC_RESOURCE_OK;
  }
  case 2:
    seen_bits |= SEEN_FAILED;
    return GVEC_RESOURCE_REFUSED;
  case 3:
    return GVEC_RESOURCE_NOT_FOUND;
  case 4:
    seen_bits |= SEEN_FAILED;
    return GVEC_RESOURCE_FAILED;
  case 5:
    /* A size over any limit, at an address that must never be read. */
    reply->bytes = (const void *) (uintptr_t) 8;
    reply->size = request->max_bytes + 1;
    return GVEC_RESOURCE_OK;
  case 6:
    reply->bytes = NULL;
    reply->size = length + 1u;
    return GVEC_RESOURCE_OK;
  default:
    return (GVEC_Resource_Status) (status >> 3) + 100;  /* a status the enum does not have */
  }
}

static void free_script(script * s) {
  for (size_t i = 0; i < s->asked_count; i++) {
    free(s->asked[i].url);
  }
  free(s->asked);
  free(s->answer);
  memset(s, 0, sizeof *s);
}

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 3 || size > (1u << 20)) {
    return 0;
  }
  size_t resources = (size_t) (data[0] & 15) + 1, depth = (size_t) (data[0] >> 4) + 1, instances = (size_t) (data[1] & 63) + 1;
  const uint8_t * body = data + 2;
  size_t body_size = size - 2;
  const uint8_t * nul = memchr(body, 0, body_size);
  size_t document_size = nul ? (size_t) (nul - body) : body_size;
  const uint8_t * records = nul ? nul + 1 : body + body_size;
  size_t records_size = nul ? body_size - document_size - 1 : 0;

  GVEC_Stream * stream = NULL;
  if (gvec_stream_create_memory(body, document_size, &stream) != GVEC_OK) {
    return 0;
  }
  GVEC_Diagnostics loaded;
  gvec_diagnostics_init(&loaded, NULL);
  GVEC_Doc * doc = NULL;
  GVEC_Result r = gvec_doc_load(stream, NULL, &loaded, &doc);
  gvec_stream_destroy(stream);
  gvec_diagnostics_destroy(&loaded);
  if (r != GVEC_OK || !doc) {
    gvec_doc_free(doc);
    return 0;
  }
  GSHSCENE_Scene * scene = NULL;
  GSHSCENE_List * list = NULL;
  GSHSCENE_Edit * edit = NULL;
  GVEC_Diagnostics by_list, by_edit;
  gvec_diagnostics_init(&by_list, NULL);
  gvec_diagnostics_init(&by_edit, NULL);
  script s;
  memset(&s, 0, sizeof s);
  check(gshscene_scene_create(NULL, NULL, NULL, &scene) == GSHSCENE_OK, "a scene is made");
  check(gshscene_list_create(NULL, NULL, gshscene_scene_store(scene), &list) == GSHSCENE_OK, "a list is made");
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.store = gshscene_scene_store(scene);
  options.resolver = resolve;
  options.resolver_user = &s;
  options.base_uri = "fuzz/dir/document.svg";
  options.max_resources = resources;
  options.max_resource_depth = depth;
  options.max_resource_instances = instances;
  s.data = records;
  s.size = records_size;
  s.max_resources = resources;
  s.max_depth = depth;
  r = gvec_doc_lower_list(doc, &options, list, &by_list);
  check(r == GVEC_OK, "the list lowering returns OK whatever the resolver answers");
  free_script(&s);
  s.data = records;
  s.size = records_size;
  s.max_resources = resources;
  s.max_depth = depth;
  check(gshscene_scene_open_edit(scene, NULL, &edit) == GSHSCENE_OK, "an edit is opened");
  r = gvec_doc_lower_edit(doc, &options, edit, &by_edit);
  check(r == GVEC_OK, "the edit lowering returns OK whatever the resolver answers");
  check(by_list.count == by_edit.count, "both entry points report the same number of diagnostics");
  for (size_t i = 0; i < by_list.count; i++) {
    check(by_list.items[i].code == by_edit.items[i].code && by_list.items[i].detail == by_edit.items[i].detail,
        "both entry points report the same diagnostics in the same order");
    if (by_list.items[i].code == GVEC_DIAG_LOWER_REFUSED) {
      seen_bits |= SEEN_REFUSED;
      if (by_list.items[i].detail == (uint32_t) GVEC_LOWER_LIMIT) {
        seen_bits |= SEEN_LIMIT;
      }
    }
  }
  if (gshscene_list_count(list) > 0) {
    seen_bits |= SEEN_DRAWN;
  }
  free_script(&s);
  gshscene_edit_destroy(edit);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
  gvec_diagnostics_destroy(&by_list);
  gvec_diagnostics_destroy(&by_edit);
  gvec_doc_free(doc);
  return 0;
}
