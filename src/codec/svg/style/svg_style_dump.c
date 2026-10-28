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
 * @file svg_style_dump.c
 *
 * The dump and the hash of styles (C13): the engine's canonical text of an instance's computed style, and one number for the styles of
 * the whole document. The document's own dump (svg_dump.c) does not change when the cascade does.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "svg_style.h"

typedef struct buffer {
  const GVEC_Allocator * allocator;
  char * data;
  size_t length, capacity;
  bool failed;
} buffer;

static GTEXT_Markup_Status buffer_write(void * user, const char * bytes, size_t length) {
  buffer * b = user;
  if (b->length + length > b->capacity) {
    size_t capacity = b->capacity ? b->capacity : 4096;
    while (capacity < b->length + length) {
      capacity *= 2;
    }
    char * grown = gvec_realloc(b->allocator, b->data, capacity);
    if (!grown) {
      b->failed = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = grown;
    b->capacity = capacity;
  }
  memcpy(b->data + b->length, bytes, length);
  b->length += length;
  return GTEXT_MARKUP_OK;
}

typedef struct stream_sink {
  GVEC_Stream * stream;
  bool failed;
} stream_sink;

static GTEXT_Markup_Status stream_write(void * user, const char * bytes, size_t length) {
  stream_sink * s = user;
  if (gvec_stream_write(s->stream, bytes, length) != GVEC_OK) {
    s->failed = true;
    return GTEXT_MARKUP_E_WRITE;
  }
  return GTEXT_MARKUP_OK;
}

GVEC_Result gvec_svg_style_dump(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Stream * out) {
  if (!context || !instance || !out) {
    return GVEC_ERR_INVALID;
  }
  const GTEXT_CSS_Computed_Style * style = NULL;
  GVEC_Result r = gvec_svg_instance_style(context, instance, &style);
  if (r != GVEC_OK) {
    return r;
  }
  stream_sink state = {out, false};
  GTEXT_Markup_Sink sink = {stream_write, &state};
  GTEXT_CSS_Status status = gtext_css_computed_dump(style, &sink);
  if (status == GTEXT_CSS_OK) {
    return GVEC_OK;
  }
  return state.failed ? GVEC_ERR_IO : status == GTEXT_CSS_E_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
}

/* The style of every element in document order. */
GVEC_Result gvec_svg_style_hash(GVEC_Svg_Style_Context * context, uint64_t * out_hash) {
  if (!context || !out_hash) {
    return GVEC_ERR_INVALID;
  }
  const GVEC_Svg_Document * doc = context->document;
  size_t n = doc->element_count;
  const GVEC_Svg_Instance * const * instances = NULL;
  GVEC_Result r = gvec_svg_document_instances(context, &instances);
  if (r != GVEC_OK) {
    return r;
  }
  uint8_t * pairs = gvec_alloc(context->allocator, (n ? n : 1) * 16);
  buffer text = {context->allocator, NULL, 0, 0, false};
  if (!pairs) {
    r = GVEC_ERR_OOM;
  }
  for (size_t i = 0; r == GVEC_OK && i < n; i++) {
    const GTEXT_CSS_Computed_Style * style = NULL;
    r = gvec_svg_instance_style(context, instances[i], &style);
    if (r != GVEC_OK) {
      break;
    }
    text.length = 0;
    GTEXT_Markup_Sink sink = {buffer_write, &text};
    GTEXT_CSS_Status status = gtext_css_computed_dump(style, &sink);
    if (status != GTEXT_CSS_OK) {
      /* The sink refuses output only when it cannot grow its buffer. */
      r = status == GTEXT_CSS_E_OOM || text.failed ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
      break;
    }
    uint64_t key = doc->elements[i]->key;
    uint64_t h = gvec_svg_hash64(text.data, text.length);
    for (int k = 0; k < 8; k++) {
      pairs[16 * i + k] = (uint8_t) (key >> (8 * k));
      pairs[16 * i + 8 + k] = (uint8_t) (h >> (8 * k));
    }
  }
  if (r == GVEC_OK) {
    *out_hash = gvec_svg_hash64(pairs, n * 16);
  }
  gvec_free(context->allocator, text.data);
  gvec_free(context->allocator, pairs);
  return r;
}
