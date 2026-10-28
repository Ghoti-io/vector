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
 * @file svg_paint.c
 *
 * Paints and references (C9): `fill` and `stroke`, and `clip-path`, `mask`, `filter` and `marker-*`, resolved by id through the
 * document's index. Nothing is fetched: a `url()` that does not name an element of this document is a missing reference.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "svg_style.h"
#include "svg_used.h"

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/*
 * The element a `url()` names: a fragment, `#id`, with its percent-escapes decoded (RFC 3986). Anything else (another document, an
 * empty fragment) names nothing here.
 *
 * @param out_id Receives the decoded id, in the context's memory, when the url is a fragment (even if no element has that id).
 */
GVEC_Result gvec_svg_resolve_url(GVEC_Svg_Style_Context * context, const GTEXT_CSS_String * url, bool * out_local, const char ** out_id,
    const GVEC_Svg_Element ** out_target) {
  *out_local = false;
  *out_id = NULL;
  *out_target = NULL;
  if (url->length < 2 || url->data[0] != '#') {
    return GVEC_OK;
  }
  char * id = gvec_arena_alloc(&context->arena, url->length, 1);
  if (!id) {
    return GVEC_ERR_OOM;
  }
  size_t n = 0;
  for (size_t i = 1; i < url->length; i++) {
    char c = url->data[i];
    if (c == '%' && i + 2 < url->length && hex_value(url->data[i + 1]) >= 0 && hex_value(url->data[i + 2]) >= 0) {
      c = (char) (hex_value(url->data[i + 1]) * 16 + hex_value(url->data[i + 2]));
      i += 2;
    }
    if (c == 0) {
      return GVEC_OK; /* An id cannot hold a NUL. */
    }
    id[n++] = c;
  }
  id[n] = 0;
  *out_local = true;
  *out_id = id;
  *out_target = gvec_svg_find_by_id(context->document, id);
  return GVEC_OK;
}

static bool is_paint_server(const GVEC_Svg_Element * e) {
  switch (e->kind) {
  case GVEC_SVG_KIND_LINEAR_GRADIENT:
  case GVEC_SVG_KIND_RADIAL_GRADIENT:
  case GVEC_SVG_KIND_PATTERN:
    return true;
  default:
    return false;
  }
}

static bool keyword_is(const GTEXT_CSS_Value * v, const char * word) {
  if (v->kind != GTEXT_CSS_VAL_KEYWORD || v->text.length != strlen(word)) {
    return false;
  }
  for (size_t i = 0; i < v->text.length; i++) {
    char c = v->text.data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char) (c - 'A' + 'a');
    }
    if (c != word[i]) {
      return false;
    }
  }
  return true;
}

GVEC_Result gvec_svg_used_paint(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Value * v,
    GVEC_Svg_Paint * out) {
  memset(out, 0, sizeof *out);
  const GVEC_Svg_Element * where = instance->element;
  if (keyword_is(v, "none")) {
    out->kind = GVEC_SVG_PAINT_NONE;
    return GVEC_OK;
  }
  if (keyword_is(v, "context-fill")) {
    out->kind = GVEC_SVG_PAINT_CONTEXT_FILL;
    return GVEC_OK;
  }
  if (keyword_is(v, "context-stroke")) {
    out->kind = GVEC_SVG_PAINT_CONTEXT_STROKE;
    return GVEC_OK;
  }
  if (v->kind == GTEXT_CSS_VAL_COLOR) {
    out->kind = GVEC_SVG_PAINT_COLOR;
    return gvec_svg_used_color(context, instance, v, where, &out->color);
  }
  /* `url(#id)`, or `url(#id) <fallback>`: a list of the url and its fallback. */
  const GTEXT_CSS_Value * url = v;
  const GTEXT_CSS_Value * fallback = NULL;
  if (v->kind == GTEXT_CSS_VAL_LIST && v->child_count >= 1) {
    url = &v->children[0];
    if (v->child_count == 2) {
      fallback = &v->children[1];
    }
  }
  if (url->kind != GTEXT_CSS_VAL_URL) {
    return GVEC_ERR_UNSUPPORTED;
  }
  bool local;
  const char * id;
  const GVEC_Svg_Element * target;
  GVEC_Result r = gvec_svg_resolve_url(context, &url->text, &local, &id, &target);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Paint fallback_paint;
  memset(&fallback_paint, 0, sizeof fallback_paint);
  bool has_fallback = false;
  if (fallback) {
    has_fallback = true;
    if (keyword_is(fallback, "none")) {
      fallback_paint.kind = GVEC_SVG_PAINT_NONE;
    }
    else {
      fallback_paint.kind = GVEC_SVG_PAINT_COLOR;
      r = gvec_svg_used_color(context, instance, fallback, where, &fallback_paint.color);
      if (r != GVEC_OK) {
        return r;
      }
    }
  }
  bool cut = target && is_paint_server(target) && gvec_svg_reference_cut(context, where, target);
  if (cut) {
    /* The reference closes a cycle and was reported when the cycles were found: the paint that follows it, else none. */
    if (has_fallback) {
      *out = fallback_paint;
    }
    else {
      out->kind = GVEC_SVG_PAINT_NONE;
    }
    return GVEC_OK;
  }
  if (!target || !is_paint_server(target)) {
    if (gvec_svg_once_first(context, GVEC_ONCE_REFERENCE, where)) {
      GVEC_Result d = gvec_svg_style_diag(context, has_fallback ? GVEC_DIAG_NOTICE : GVEC_DIAG_WARNING, GVEC_DIAG_REFERENCE_MISSING, 0,
          where, NULL,
          target ? "a paint refers to an element that is no paint server; the fallback is used, else none"
                 : "a paint refers to an element the document does not have; the fallback is used, else none");
      if (d == GVEC_ERR_OOM) {
        return d;
      }
    }
    /* SVG 2, 13.2: the paint that follows the reference when the reference is not valid; otherwise the document is in error and
     * the browsers draw none. */
    if (has_fallback) {
      *out = fallback_paint;
    }
    else {
      out->kind = GVEC_SVG_PAINT_NONE;
    }
    return GVEC_OK;
  }
  out->kind = GVEC_SVG_PAINT_SERVER;
  out->server = target;
  out->server_id = id;
  out->has_fallback = has_fallback;
  out->fallback_kind = fallback_paint.kind;
  out->fallback_color = fallback_paint.color;
  return GVEC_OK;
}

/* One item of a reference list. */
static GVEC_Result reference_item(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * from, const GTEXT_CSS_Value * v,
    GVEC_Svg_Style_Reference * out) {
  memset(out, 0, sizeof *out);
  char * text = NULL;
  size_t length = 0;
  GTEXT_CSS_Status status = gtext_css_serialize_value_buffer(v, NULL, &text, &length);
  if (status != GTEXT_CSS_OK) {
    return status == GTEXT_CSS_E_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
  }
  out->text = gvec_svg_pool_string(context, text, length);
  gtext_css_serialize_buffer_free(NULL, text);
  if (!out->text) {
    return GVEC_ERR_OOM;
  }
  if (v->kind == GTEXT_CSS_VAL_URL) {
    out->is_url = true;
    GVEC_Result r = gvec_svg_resolve_url(context, &v->text, &out->local, &out->id, &out->target);
    if (r == GVEC_OK && out->target && gvec_svg_reference_cut(context, from, out->target)) {
      out->target = NULL;
      out->cut = true;
    }
    return r;
  }
  return GVEC_OK;
}

GVEC_Result gvec_svg_used_references(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Value * v,
    GVEC_Svg_Style_Value * out) {
  out->kind = GVEC_SVG_STYLE_REFERENCES;
  if (keyword_is(v, "none")) {
    return GVEC_OK;
  }
  /* A value is one item or a list of them (`filter: url(#a) blur(2px)`, a `mask-image` list). A function that holds a url
   * (`clip-path: url(...)`, never) is one item written as it is. */
  const GTEXT_CSS_Value * items = v;
  size_t count = 1;
  if (v->kind == GTEXT_CSS_VAL_LIST) {
    items = v->children;
    count = v->child_count;
  }
  GVEC_Svg_Style_Reference * refs = gvec_arena_array(&context->arena, count ? count : 1, sizeof *refs, 8);
  if (!refs) {
    return GVEC_ERR_OOM;
  }
  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (keyword_is(&items[i], "none")) {
      continue;
    }
    GVEC_Result r = reference_item(context, instance->element, &items[i], &refs[n]);
    if (r != GVEC_OK) {
      return r;
    }
    n++;
  }
  out->references = refs;
  out->reference_count = n;
  return GVEC_OK;
}
