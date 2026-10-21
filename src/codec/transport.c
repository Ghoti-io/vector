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
 * @file transport.c
 *
 * The gzip transport (`.svgz`), inflated through compress.
 *
 * A gzip file is a series of members (RFC 1952, section 2.2), so the members
 * are inflated one after another and their outputs concatenated. Bytes after
 * the last member that do not begin another are ignored with a warning, as
 * gzip(1) does. Inflation stops at GVEC_Limits.max_inflated_bytes: the output
 * buffer is never larger than the cap plus one byte, so a small member that
 * would inflate past the cap is stopped there, not after. One level only:
 * inflated bytes that are gzip again are refused.
 *
 * The members are walked here, one decoder each, rather than by compress's
 * own concatenation, because only then is the input position after the last
 * member known.
 */

#include <ghoti.io/vector/macros.h>

#include <stdint.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>

#include "../core/core_internal.h"
#include "codec_internal.h"

bool gvec_transport_is_gzip(const unsigned char * data, size_t size) {
  return size >= 3 && data[0] == 0x1f && data[1] == 0x8b && data[2] == 0x08;
}

typedef struct {
  const GVEC_Allocator * allocator;
  unsigned char * data;
  size_t size;
  size_t capacity;
  size_t max; /* the cap; capacity never exceeds max + 1 */
} gvec_inflated;

/* Room for more output: grow by doubling, never past max + 1. */
static GVEC_Result gvec_inflated_room(gvec_inflated * out) {
  size_t grown;
  unsigned char * data;
  if (out->size < out->capacity) {
    return GVEC_OK;
  }
  if (out->capacity > out->max) {
    return GVEC_ERR_LIMIT;
  }
  grown = out->capacity ? out->capacity * 2 : 65536;
  if (grown > out->max + 1) {
    grown = out->max + 1;
  }
  data = gvec_realloc(out->allocator, out->data, grown);
  if (!data) {
    return GVEC_ERR_OOM;
  }
  out->data = data;
  out->capacity = grown;
  return GVEC_OK;
}

static GVEC_Result gvec_status_result(gcomp_status_t status) {
  switch (status) {
  case GCOMP_OK:
    return GVEC_OK;
  case GCOMP_ERR_MEMORY:
    return GVEC_ERR_OOM;
  default:
    return GVEC_ERR_CORRUPT;
  }
}

/* Inflate one member at data[0 .. size), appending to `out`; `*used` is the
 * member's length. */
static GVEC_Result gvec_inflate_member(const unsigned char * data, size_t size, gvec_inflated * out, size_t * used,
    gcomp_status_t * out_status) {
  gcomp_options_t * options = NULL;
  gcomp_decoder_t * decoder = NULL;
  gcomp_buffer_t in = {data, size, 0};
  gcomp_status_t status;
  GVEC_Result r = GVEC_OK;

  *out_status = GCOMP_OK;
  status = gcomp_options_create(&options);
  /* The cap is enforced by the buffer above; compress's own output limit is
   * set past it only so that compress never stops first. */
  if (status == GCOMP_OK) {
    status = gcomp_options_set_uint64(options, "limits.max_output_bytes", (uint64_t) out->max + 2);
  }
  if (status == GCOMP_OK) {
    status = gcomp_decoder_create(gcomp_registry_default(), "gzip", options, &decoder);
  }
  while (status == GCOMP_OK) {
    gcomp_buffer_t chunk;
    size_t before = in.used;
    r = gvec_inflated_room(out);
    if (r != GVEC_OK) {
      break;
    }
    chunk.data = out->data + out->size;
    chunk.size = out->capacity - out->size;
    chunk.used = 0;
    status = gcomp_decoder_update(decoder, &in, &chunk);
    out->size += chunk.used;
    if (status == GCOMP_OK && chunk.used == 0 && in.used == before) {
      break;
    }
  }
  while (status == GCOMP_OK && r == GVEC_OK) {
    gcomp_buffer_t chunk;
    r = gvec_inflated_room(out);
    if (r != GVEC_OK) {
      break;
    }
    chunk.data = out->data + out->size;
    chunk.size = out->capacity - out->size;
    chunk.used = 0;
    status = gcomp_decoder_finish(decoder, &chunk);
    out->size += chunk.used;
    if (status == GCOMP_ERR_LIMIT && chunk.used > 0) {
      status = GCOMP_OK; /* more room is wanted */
      continue;
    }
    break;
  }
  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(options);
  if (r == GVEC_OK && out->size > out->max) {
    r = GVEC_ERR_LIMIT;
  }
  if (r == GVEC_OK && status != GCOMP_OK) {
    *out_status = status;
    r = gvec_status_result(status);
  }
  *used = in.used;
  return r;
}

GVEC_Result gvec_transport_decode(const unsigned char * data, size_t size, const GVEC_Limits * limits,
    GVEC_Diagnostics * diagnostics, const GVEC_Allocator * allocator, const unsigned char ** out_data,
    size_t * out_size, GVEC_Transport * out_transport) {
  gvec_inflated out = {allocator, NULL, 0, 0, limits->max_inflated_bytes};
  size_t pos = 0;
  GVEC_Result r = GVEC_OK;

  if (!gvec_transport_is_gzip(data, size)) {
    *out_data = data;
    *out_size = size;
    *out_transport = GVEC_TRANSPORT_NONE;
    return GVEC_OK;
  }

  while (pos < size) {
    size_t used = 0;
    gcomp_status_t status = GCOMP_OK;
    if (!gvec_transport_is_gzip(data + pos, size - pos)) {
      gvec_diag_add(diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_GZIP, 0, 0, 0, pos, NULL,
          "bytes after the last gzip member were ignored");
      break;
    }
    r = gvec_inflate_member(data + pos, size - pos, &out, &used, &status);
    if (r == GVEC_ERR_LIMIT) {
      gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, GVEC_LIMIT_INFLATED_BYTES, 0, 0, pos, NULL,
          "the gzip transport inflates past max_inflated_bytes");
      break;
    }
    if (r == GVEC_ERR_OOM) {
      gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_GZIP, (uint32_t) status, 0, 0, pos, NULL,
          "out of memory inflating the gzip transport");
      break;
    }
    if (r != GVEC_OK || used == 0) {
      r = GVEC_ERR_CORRUPT;
      gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_GZIP, (uint32_t) status, 0, 0, pos, NULL,
          "the gzip transport is damaged");
      break;
    }
    pos += used;
  }
  if (r != GVEC_OK) {
    gvec_free(allocator, out.data);
    return r;
  }

  if (gvec_transport_is_gzip(out.data, out.size)) {
    gvec_free(allocator, out.data);
    gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_GZIP, 0, 0, 0, 0, NULL,
        "a gzip transport inside a gzip transport is not read");
    return GVEC_ERR_UNSUPPORTED;
  }

  /* An empty inflation still yields a buffer to free, so the caller's free
   * is the same on every path. */
  if (!out.data) {
    out.data = gvec_alloc(allocator, 1);
    if (!out.data) {
      return GVEC_ERR_OOM;
    }
  }
  *out_data = out.data;
  *out_size = out.size;
  *out_transport = GVEC_TRANSPORT_GZIP;
  return GVEC_OK;
}

void gvec_transport_free(const unsigned char * data, GVEC_Transport transport, const GVEC_Allocator * allocator) {
  if (transport == GVEC_TRANSPORT_GZIP) {
    gvec_free(allocator, (void *) data);
  }
}
