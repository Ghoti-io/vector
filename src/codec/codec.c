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
 * @file codec.c
 *
 * The registry, the probe, loading, the dump and hash, and the lowering
 * dispatch.
 *
 * The registry is a `const` table of the codecs built into the library, in
 * their probe order. It needs no registration step, so it is complete before
 * any code runs and whichever way the library is linked (a constructor in a
 * static archive is dropped by the linker unless the caller links the whole
 * archive).
 */

#include <ghoti.io/vector/macros.h>

#include <stdio.h>
#include <string.h>

#include <ghoti.io/cutil/string.h>

#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/lower.h>

#include "../core/core_internal.h"
#include "../stream/stream_internal.h"
#include "codec_internal.h"
#include "svg/svg_internal.h"

static const GVEC_Codec * const gvec_codecs[] = {
    &gvec_svg_codec,
};

size_t gvec_codec_count(void) {
  return sizeof(gvec_codecs) / sizeof(gvec_codecs[0]);
}

const GVEC_Codec * gvec_codec_by_index(size_t index) {
  return index < gvec_codec_count() ? gvec_codecs[index] : NULL;
}

const GVEC_Codec * gvec_codec_by_name(const char * name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < gvec_codec_count(); i++) {
    if (strcmp(gvec_codecs[i]->name, name) == 0) {
      return gvec_codecs[i];
    }
  }
  return NULL;
}

const char * gvec_codec_name(const GVEC_Codec * codec) {
  return codec ? codec->name : NULL;
}

unsigned gvec_codec_capabilities(const GVEC_Codec * codec) {
  return codec ? codec->capabilities : 0;
}

/* The first codec that claims the bytes, in `*out` (NULL when none does, and
 * then each codec's reason is appended to `why_not`, which may be NULL).
 * GVEC_ERR_OOM when a probe could not decide. */
static GVEC_Result gvec_codec_find(const unsigned char * data, size_t size, const GVEC_Limits * limits,
    const GVEC_Allocator * allocator, GVEC_Diagnostics * why_not, const GVEC_Codec ** out) {
  *out = NULL;
  for (size_t i = 0; i < gvec_codec_count(); i++) {
    const GVEC_Codec * codec = gvec_codecs[i];
    int confidence = codec->probe ? codec->probe(data, size, limits, allocator, why_not) : 0;
    if (confidence < 0) {
      return (GVEC_Result) -confidence;
    }
    if (confidence == 100) {
      *out = codec;
      return GVEC_OK;
    }
  }
  return GVEC_OK;
}

/* Buffer the whole input, within the input limit. */
static GVEC_Result gvec_read_input(GVEC_Stream * stream, const GVEC_Limits * limits, GVEC_Diagnostics * diagnostics,
    const unsigned char ** out_data, size_t * out_size) {
  bool over = false;
  GVEC_Result r = gvec_stream_lookahead_all(stream, limits->max_input_bytes, out_data, out_size, &over);
  if (r != GVEC_OK) {
    return r;
  }
  if (over) {
    gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, GVEC_LIMIT_INPUT_BYTES, 0, 0, 0, NULL,
        "the input is longer than max_input_bytes");
    return GVEC_ERR_LIMIT;
  }
  return GVEC_OK;
}

GVEC_Result gvec_probe(GVEC_Stream * stream, const GVEC_Limits * limits, GVEC_Probe_Result * out_result) {
  GVEC_Limits resolved;
  const unsigned char * data;
  const unsigned char * bytes;
  size_t size;
  size_t bytes_size;
  GVEC_Transport transport;
  const GVEC_Codec * codec;
  GVEC_Result r;

  if (!stream || !out_result) {
    return GVEC_ERR_INVALID;
  }
  r = gvec_limits_resolve(limits, &resolved, NULL);
  if (r != GVEC_OK) {
    return r;
  }
  r = gvec_read_input(stream, &resolved, NULL, &data, &size);
  if (r != GVEC_OK) {
    return r;
  }
  r = gvec_transport_decode(data, size, &resolved, NULL, NULL, &bytes, &bytes_size, &transport);
  if (r != GVEC_OK) {
    return r;
  }
  r = gvec_codec_find(bytes, bytes_size, &resolved, NULL, NULL, &codec);
  gvec_transport_free(bytes, transport, NULL);
  if (r != GVEC_OK) {
    return r;
  }
  memset(out_result, 0, sizeof(*out_result));
  out_result->format_name = codec ? codec->name : NULL;
  out_result->confidence = codec ? 100 : 0;
  out_result->transport = transport;
  return GVEC_OK;
}

GVEC_Result gvec_doc_load(
    GVEC_Stream * stream, const GVEC_Load_Options * options, GVEC_Diagnostics * diagnostics, GVEC_Doc ** out_doc) {
  GVEC_Limits resolved;
  GVEC_Limit_Field field = GVEC_LIMIT_INPUT_BYTES;
  const GVEC_Allocator * allocator = options ? options->allocator : NULL;
  const unsigned char * data;
  const unsigned char * bytes;
  size_t size;
  size_t bytes_size;
  GVEC_Transport transport;
  const GVEC_Codec * codec;
  GVEC_Doc * doc;
  GVEC_Result r;

  if (!stream || !out_doc) {
    return GVEC_ERR_INVALID;
  }
  if (options) {
    for (size_t i = 0; i < sizeof(options->_reserved); i++) {
      if (options->_reserved[i] != 0) {
        return GVEC_ERR_INVALID;
      }
    }
  }
  if (options) {
    /* Written to reject NaN too: a comparison with NaN is false. */
    if (!(options->viewport_width >= 0 && options->viewport_width <= 1e9) ||
        !(options->viewport_height >= 0 && options->viewport_height <= 1e9) ||
        (unsigned) options->color_scheme >= GVEC_COLOR_SCHEME_COUNT) {
      return GVEC_ERR_INVALID;
    }
  }
  r = gvec_limits_resolve(options ? options->limits : NULL, &resolved, &field);
  if (r != GVEC_OK) {
    gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, field, 0, 0, 0, NULL,
        "a limit is above its ceiling, or a reserved byte is not zero");
    return r;
  }
  gvec_diag_set_cap(diagnostics, resolved.max_diagnostics);

  r = gvec_read_input(stream, &resolved, diagnostics, &data, &size);
  if (r != GVEC_OK) {
    return r;
  }
  r = gvec_transport_decode(data, size, &resolved, diagnostics, allocator, &bytes, &bytes_size, &transport);
  if (r != GVEC_OK) {
    gvec_stream_consume(stream, size);
    return r;
  }

  {
    GVEC_Diagnostics why_not;
    gvec_diagnostics_init(&why_not, allocator);
    gvec_diag_set_cap(&why_not, resolved.max_diagnostics);
    r = gvec_codec_find(bytes, bytes_size, &resolved, allocator, &why_not, &codec);
    if (!codec) {
      for (size_t i = 0; i < why_not.count; i++) {
        const GVEC_Diagnostic * e = &why_not.items[i];
        gvec_diag_add(diagnostics, e->severity, e->code, e->detail, e->line, e->column, e->offset, e->element,
            e->message);
      }
      if (diagnostics) {
        diagnostics->dropped += why_not.dropped;
      }
    }
    gvec_diagnostics_destroy(&why_not);
  }
  if (r != GVEC_OK) {
    gvec_transport_free(bytes, transport, allocator);
    gvec_stream_consume(stream, size);
    return r;
  }
  if (!codec) {
    gvec_transport_free(bytes, transport, allocator);
    gvec_stream_consume(stream, size);
    return GVEC_ERR_UNSUPPORTED;
  }

  doc = gvec_alloc(allocator, sizeof(*doc));
  if (!doc) {
    gvec_transport_free(bytes, transport, allocator);
    gvec_stream_consume(stream, size);
    return GVEC_ERR_OOM;
  }
  memset(doc, 0, sizeof(*doc));
  doc->allocator = allocator;
  doc->codec = codec;
  doc->transport = transport;

  r = codec->load(doc, bytes, bytes_size, &resolved, options, diagnostics);
  gvec_transport_free(bytes, transport, allocator);
  gvec_stream_consume(stream, size);
  if (r != GVEC_OK) {
    gvec_free(allocator, doc);
    return r;
  }
  *out_doc = doc;
  return GVEC_OK;
}

void gvec_doc_free(GVEC_Doc * doc) {
  if (!doc) {
    return;
  }
  if (doc->codec && doc->codec->free_private) {
    doc->codec->free_private(doc);
  }
  gvec_free(doc->allocator, doc);
}

const GVEC_Codec * gvec_doc_codec(const GVEC_Doc * doc) {
  return doc ? doc->codec : NULL;
}

const char * gvec_doc_format(const GVEC_Doc * doc) {
  return doc ? doc->codec->name : NULL;
}

GVEC_Transport gvec_doc_transport(const GVEC_Doc * doc) {
  return doc ? doc->transport : GVEC_TRANSPORT_NONE;
}

GVEC_Result gvec_dump_text(GVEC_Stream * out, const char * text) {
  return gvec_stream_write(out, text, strlen(text));
}

GVEC_Result gvec_doc_dump(const GVEC_Doc * doc, GVEC_Stream * out) {
  static const char * const transports[GVEC_TRANSPORT_COUNT] = {"none", "gzip"};
  char line[96];
  GVEC_Result r;
  if (!doc || !out) {
    return GVEC_ERR_INVALID;
  }
  snprintf(line, sizeof(line), "vector-doc 1\nformat %s\ntransport %s\n", doc->codec->name,
      transports[doc->transport]);
  r = gvec_dump_text(out, line);
  if (r != GVEC_OK) {
    return GVEC_ERR_IO;
  }
  if (doc->codec->dump) {
    r = doc->codec->dump(doc, out);
    if (r != GVEC_OK) {
      return GVEC_ERR_IO;
    }
  }
  return GVEC_OK;
}

GVEC_Result gvec_doc_hash(const GVEC_Doc * doc, uint64_t * out_hash) {
  GVEC_Stream * out = NULL;
  const void * bytes;
  size_t size;
  unsigned char digest[16];
  uint64_t hash = 0;
  GVEC_Result r;
  if (!doc || !out_hash) {
    return GVEC_ERR_INVALID;
  }
  r = gvec_stream_create_memory_output_with_allocator(doc->allocator, &out);
  if (r != GVEC_OK) {
    return r;
  }
  r = gvec_doc_dump(doc, out);
  if (r != GVEC_OK) {
    gvec_stream_destroy(out);
    return r;
  }
  gvec_stream_output_buffer(out, &bytes, &size);
  gcu_string_murmur3_x64_128(bytes ? bytes : "", size, 0, digest);
  gvec_stream_destroy(out);
  for (int i = 7; i >= 0; i--) {
    hash = (hash << 8) | digest[i];
  }
  *out_hash = hash;
  return GVEC_OK;
}

GVEC_Result gvec_doc_lower_edit(
    const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_Edit * edit, GVEC_Diagnostics * diagnostics) {
  if (!doc || !edit) {
    return GVEC_ERR_INVALID;
  }
  if (!doc->codec->lower_edit) {
    return GVEC_ERR_UNSUPPORTED;
  }
  return doc->codec->lower_edit(doc, options, edit, diagnostics);
}

GVEC_Result gvec_doc_lower_list(
    const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list, GVEC_Diagnostics * diagnostics) {
  if (!doc || !list) {
    return GVEC_ERR_INVALID;
  }
  if (!doc->codec->lower_list) {
    return GVEC_ERR_UNSUPPORTED;
  }
  return doc->codec->lower_list(doc, options, list, diagnostics);
}

GVEC_Result gvec_doc_size(const GVEC_Doc * doc, double * width, double * height) {
  if (!doc || !width || !height) {
    return GVEC_ERR_INVALID;
  }
  if (!doc->codec->size) {
    return GVEC_ERR_UNSUPPORTED;
  }
  return doc->codec->size(doc, width, height);
}
