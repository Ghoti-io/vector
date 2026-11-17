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
 * @file lower_external.c
 *
 * A `use` of an element of another document (the Resources section of design.md): the document is asked of the caller's resolver, loaded once
 * for the lowering, and the element is lowered by a context of its own for that document, so that its ids, its references and its style
 * sheets are the other document's, while what it inherits is what the `use` gives it, as for any `use`.
 *
 * Nothing here opens anything: the bytes are what the resolver handed over (svg_resource.c). A document that names itself again through
 * others closes a cycle, found by the `use` elements the contexts above were made for; the resolver's depth, its count and the number of
 * documents instantiated bound the rest.
 */

#include <ghoti.io/vector/macros.h>
#include <ghoti.io/vector/codec.h>

#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include "../style/svg_style.h"
#include "lower_internal.h"

static GVEC_Result missing(gvec_lw_ctx * ctx, const GVEC_Svg_Element * use, const char * message) {
  return gvec_diag_add(ctx->diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_REFERENCE_MISSING, 0, use->node.line, use->node.column, use->node.offset,
      use->name, message);
}

static int hex(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The id a fragment names: percent-escapes decoded. NULL for an escape that is not one, or a NUL; `*oom` when out of memory. */
static char * fragment_id(const GVEC_Allocator * a, const char * fragment, bool * oom) {
  size_t n = strlen(fragment);
  char * id = gvec_alloc(a, n + 1);
  if (!id) {
    *oom = true;
    return NULL;
  }
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char) fragment[i];
    if (c == '%') {
      int h = i + 1 < n ? hex(fragment[i + 1]) : -1, l = i + 2 < n ? hex(fragment[i + 2]) : -1;
      if (h < 0 || l < 0) {
        gvec_free(a, id);
        return NULL;
      }
      c = (unsigned char) (h * 16 + l);
      i += 2;
      if (c == 0) {
        gvec_free(a, id);
        return NULL;
      }
    }
    id[o++] = (char) c;
  }
  id[o] = 0;
  return id;
}

GVEC_Result gvec_lw_external_target(gvec_lw_ctx * ctx, const GVEC_Svg_Element * use, const char * reference, gvec_lw_ctx ** nested,
    const GVEC_Svg_Element ** target, bool * ready) {
  *ready = false;
  *nested = NULL;
  *target = NULL;
  size_t before = 0;
  const char * fragment = gvec_res_fragment(reference, &before);
  if (!fragment || !fragment[0]) {
    return missing(ctx, use, "the use names a document and no element of it (no #id): nothing is drawn");
  }
  if (before == 0) {
    return missing(ctx, use, "the use names no document before the #id");
  }
  gvec_res_state * res = ctx->res;
  const GVEC_Limits * limits = &ctx->document->limits;
  gvec_res_outcome outcome;
  gvec_res_entry * entry = NULL;
  GVEC_Result r = gvec_res_fetch(res, GVEC_RESOURCE_DOCUMENT, ctx->base, reference, limits->max_input_bytes, ctx->res_depth + 1, &outcome, &entry);
  if (r != GVEC_OK) {
    return r;
  }
  switch (outcome) {
  case GVEC_RES_OK:
    break;
  case GVEC_RES_NO_RESOLVER:
  case GVEC_RES_REFUSED:
    return gvec_lw_refuse(ctx, use, GVEC_LOWER_EXTERNAL_REFERENCE, "the resolver refuses the document the use names");
  case GVEC_RES_NOT_FOUND:
    return missing(ctx, use, "the resolver finds no document at the reference of the use: nothing is drawn");
  case GVEC_RES_FAILED:
    return missing(ctx, use, "the resolver failed to give the document the use names: nothing is drawn");
  case GVEC_RES_TOO_BIG:
    return gvec_lw_refuse(ctx, use, GVEC_LOWER_LIMIT, "the document the use names is larger than max_input_bytes or max_resource_bytes allows");
  case GVEC_RES_LIMIT_COUNT:
    return gvec_lw_refuse(ctx, use, GVEC_LOWER_LIMIT, "the use would pass max_resources");
  case GVEC_RES_LIMIT_DEPTH:
    return gvec_lw_refuse(ctx, use, GVEC_LOWER_LIMIT, "the use names documents nested deeper than max_resource_depth allows");
  }
  if (res->instantiations >= res->max_instances) {
    return gvec_lw_refuse(ctx, use, GVEC_LOWER_LIMIT, "the lowering has instantiated as many references into other documents as max_resource_instances allows");
  }
  /* The same `use` instantiated inside itself: the documents name one another in a cycle. */
  for (const gvec_lw_ctx * up = ctx; up; up = up->parent) {
    if (up->external_use == use) {
      return gvec_lw_refuse(ctx, use, GVEC_LOWER_EXTERNAL_REFERENCE, "the use closes a cycle of documents that name one another");
    }
  }
  if (!entry->document && !entry->document_tried) {
    entry->document_tried = true;
    GVEC_Doc * doc = NULL;
    GVEC_Result problem;
    /* Laid out in the viewport of the document that names it: the element is drawn in that document's picture. */
    r = gvec_lw_load_document(ctx, entry->bytes, entry->size, ctx->document->viewport_width, ctx->document->viewport_height, &doc, &problem);
    if (r != GVEC_OK) {
      return r;
    }
    entry->document = doc;
  }
  if (!entry->document) {
    return missing(ctx, use, "the document the use names is not an SVG document the library reads: nothing is drawn");
  }
  const GVEC_Doc * doc = entry->document;
  const GVEC_Svg_Document * svg = gvec_svg_document(doc);
  if (!svg) {
    return missing(ctx, use, "the document the use names is not an SVG document the library reads: nothing is drawn");
  }
  bool oom = false;
  char * id = fragment_id(ctx->allocator, fragment, &oom);
  if (oom) {
    return GVEC_ERR_OOM;
  }
  const GVEC_Svg_Element * found = id ? gvec_svg_find_by_id(svg, id) : NULL;
  gvec_free(ctx->allocator, id);
  if (!found) {
    return missing(ctx, use, "the document the use names has no element with the id of the reference: nothing is drawn");
  }
  /* The context for the document, kept (with its nodes) until the lowering ends, as for an SVG image. */
  gvec_lw_ctx * n = gvec_alloc(ctx->allocator, sizeof *n);
  if (!n) {
    return GVEC_ERR_OOM;
  }
  if (ctx->nested_count == ctx->nested_capacity) {
    size_t capacity = ctx->nested_capacity ? ctx->nested_capacity * 2 : 4, bytes;
    void * fresh = gcu_safe_mul_size(capacity, sizeof *ctx->nested, &bytes) ? gvec_realloc(ctx->allocator, ctx->nested, bytes) : NULL;
    if (!fresh) {
      gvec_free(ctx->allocator, n);
      return GVEC_ERR_OOM;
    }
    ctx->nested = fresh;
    ctx->nested_capacity = capacity;
  }
  r = gvec_lw_begin_in(n, doc, ctx->options, ctx->store, ctx->diagnostics, ctx, entry->url);
  if (r != GVEC_OK) {
    gvec_free(ctx->allocator, n);
    return r;
  }
  ctx->nested[ctx->nested_count++] = n;
  res->instantiations++;
  n->parent = ctx;
  n->parent_element = use;
  n->external_doc = doc;
  n->external_use = use;
  n->image_nesting = ctx->image_nesting;
  /* Its own style sheets apply to its elements, as the document's do. */
  r = gvec_svg_style_sheets(n->style);
  if (r != GVEC_OK) {
    return r;
  }
  const char * why = NULL;
  const GVEC_Svg_Element * refused = gvec_svg_style_sheet_refused(n->style, &why);
  if (refused) {
    r = gvec_lw_refuse(n, refused, GVEC_LOWER_STYLESHEET, why);
    if (r != GVEC_OK) {
      return r;
    }
  }
  *nested = n;
  *target = found;
  *ready = true;
  return GVEC_OK;
}

void gvec_lw_external_sync(gvec_lw_ctx * ctx, gvec_lw_ctx * nested) {
  nested->chain = ctx->chain;
  nested->use_depth = ctx->use_depth;
  nested->levels = ctx->levels;
  nested->open = ctx->open;
  nested->picture_depth = ctx->picture_depth;
  nested->depth = ctx->depth;
  nested->context_set = ctx->context_set;
  nested->context_fill = ctx->context_fill;
  nested->context_stroke = ctx->context_stroke;
  nested->context_inst = NULL;
  nested->context_box_set = false;
  nested->image_decoded = ctx->image_decoded;
  nested->marker_instances = ctx->marker_instances;
}

void gvec_lw_external_done(gvec_lw_ctx * ctx, gvec_lw_ctx * nested) {
  ctx->image_decoded = nested->image_decoded;
  ctx->marker_instances = nested->marker_instances;
}
