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
 * @file lower_stroke.c
 *
 * Paints and strokes: the paint of a fill or a stroke with its opacity, the stroke style the `stroke-*` properties make, and
 * `paint-order`. A stroke is an item of the shape waist; nothing is outlined, dashed or joined here, so the geometry stays the
 * shape family's.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/shape-scene/stroke.h>

#include "lower_internal.h"

GVEC_Result gvec_lw_paint_of(gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, const GVEC_Svg_Paint * paint, double opacity,
    GSHSCENE_Paint * out, gvec_lw_paint_decision * decision) {
  *decision = GVEC_LW_PAINT_REFUSED;
  switch (paint->kind) {
  case GVEC_SVG_PAINT_NONE:
    *decision = GVEC_LW_PAINT_NONE;
    return GVEC_OK;
  case GVEC_SVG_PAINT_COLOR: {
    memset(out, 0, sizeof *out);
    double alpha = paint->color.color.rgba[3] * opacity;
    if (paint->color.kind == GVEC_SVG_COLOR_FOREGROUND) {
      out->kind = GSHSCENE_PAINT_FOREGROUND;
      out->rgba[3] = alpha;
    }
    else {
      out->kind = GSHSCENE_PAINT_SOLID;
      out->rgba[0] = paint->color.color.rgba[0];
      out->rgba[1] = paint->color.color.rgba[1];
      out->rgba[2] = paint->color.color.rgba[2];
      out->rgba[3] = alpha;
      out->space = paint->color.color.space;
    }
    *decision = alpha > 0 ? GVEC_LW_PAINT_DRAW : GVEC_LW_PAINT_NONE;
    return GVEC_OK;
  }
  case GVEC_SVG_PAINT_SERVER: {
    bool pattern = gvec_svg_element_kind(paint->server) == GVEC_SVG_KIND_PATTERN;
    return gvec_lw_refuse(ctx, element, pattern ? GVEC_LOWER_PATTERN : GVEC_LOWER_GRADIENT,
        pattern ? "a pattern paint is not lowered yet" : "a gradient paint is not lowered yet");
  }
  case GVEC_SVG_PAINT_CONTEXT_FILL:
  case GVEC_SVG_PAINT_CONTEXT_STROKE:
    if (!ctx->context_set) {
      return gvec_lw_refuse(ctx, element, GVEC_LOWER_CONTEXT_PAINT, "context-fill and context-stroke are lowered only inside a marker");
    }
    /* The paint of the element the marker is drawn for, which was resolved when the marker was entered, so it is never itself a
     * context paint. The opacity is the content's own. */
    return gvec_lw_paint_of(
        ctx, element, paint->kind == GVEC_SVG_PAINT_CONTEXT_FILL ? &ctx->context_fill : &ctx->context_stroke, opacity, out, decision);
  default:
    return gvec_lw_refuse(ctx, element, GVEC_LOWER_CONTEXT_PAINT, "this paint is not lowered");
  }
}

/* A keyword property's value, as one of `count` words; false when it is none of them. */
static bool keyword_index(const GVEC_Svg_Style_Value * v, const char * const * words, size_t count, size_t * out) {
  for (size_t i = 0; i < count; i++) {
    if (gvec_lw_keyword_is(v, words[i])) {
      *out = i;
      return true;
    }
  }
  return false;
}

/* Whether a `vector-effect` value is the one that matters here: `none`, `non-scaling-stroke`, or something else (refused). */
typedef enum effect { EFFECT_NONE = 0, EFFECT_NON_SCALING_STROKE, EFFECT_OTHER } effect;

static effect effect_of(const GVEC_Svg_Style_Value * v) {
  if (gvec_lw_keyword_is(v, "none")) {
    return EFFECT_NONE;
  }
  if (gvec_lw_keyword_is(v, "non-scaling-stroke")) {
    return EFFECT_NON_SCALING_STROKE;
  }
  return EFFECT_OTHER;
}

GVEC_Result gvec_lw_stroke_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_stroke * out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  GVEC_Svg_Style_Value v, w;
  bool usable;
  GVEC_Result r;
  memset(out, 0, sizeof *out);
  out->decision = GVEC_LW_PAINT_REFUSED;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (v.paint.kind == GVEC_SVG_PAINT_NONE) {
    out->decision = GVEC_LW_PAINT_NONE;
    return GVEC_OK;
  }
  GVEC_Svg_Paint paint = v.paint;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_OPACITY, GVEC_LOWER_STYLE, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  double opacity = w.kind == GVEC_SVG_STYLE_NUMBER ? w.number : 1;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_WIDTH, GVEC_LOWER_STYLE, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  double width = w.kind == GVEC_SVG_STYLE_LENGTH ? w.number : 1;
  /* A stroke that cannot be seen draws nothing, exactly, whatever else it says. */
  if (!(width > 0) || !(opacity > 0)) {
    out->decision = GVEC_LW_PAINT_NONE;
    return GVEC_OK;
  }
  r = gvec_lw_paint_of(ctx, e, &paint, opacity, &out->paint, &out->decision);
  if (r != GVEC_OK || out->decision != GVEC_LW_PAINT_DRAW) {
    return r;
  }
  out->decision = GVEC_LW_PAINT_REFUSED;

  GSHSCENE_Stroke_Style style;
  gshscene_stroke_style_init(&style);
  style.width = width;
  static const char * const caps[] = {"butt", "round", "square"};
  static const char * const joins[] = {"miter", "round", "bevel", "miter-clip", "arcs"};
  size_t index;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_LINECAP, GVEC_LOWER_STYLE, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!keyword_index(&w, caps, 3, &index)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_STYLE, "stroke-linecap has a value that cannot be drawn");
  }
  style.cap = (uint32_t) index;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_LINEJOIN, GVEC_LOWER_STYLE, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!keyword_index(&w, joins, 5, &index)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_STYLE, "stroke-linejoin has a value that cannot be drawn");
  }
  style.join = (uint32_t) index;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_MITERLIMIT, GVEC_LOWER_STYLE, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (w.kind == GVEC_SVG_STYLE_NUMBER) {
    /* A value below 1 is an error in SVG 2 (13.5.6) and the engine drops it, so none arrives here; a limit past the waist's
     * bound is clamped to it, as the edge a join of that sharpness cannot be told from a bevel. */
    style.miter_limit = w.number > GSHSCENE_STROKE_MITER_LIMIT_MAX ? GSHSCENE_STROKE_MITER_LIMIT_MAX : w.number;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_DASHARRAY, GVEC_LOWER_STYLE, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (w.kind == GVEC_SVG_STYLE_NUMBER_LIST && w.number_count > 0) {
    style.dashes = w.numbers;
    style.dash_count = w.number_count;
    GVEC_Svg_Style_Value o;
    r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_DASHOFFSET, GVEC_LOWER_STYLE, &o, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    style.dash_offset = o.kind == GVEC_SVG_STYLE_LENGTH ? o.number : 0;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_VECTOR_EFFECT, GVEC_LOWER_STYLE, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  effect fx = effect_of(&w);
  if (fx == EFFECT_OTHER) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_VECTOR_EFFECT, "this vector-effect is not part of the static rendering model");
  }
  if (fx == EFFECT_NON_SCALING_STROKE) {
    style.width_space = GSHSCENE_STROKE_WIDTH_DEVICE;
  }
  /* SVG 2 draws a subpath of no length with a round or square cap as the cap (13.5.4); the waist's default is that. */
  style.degenerate = GSHSCENE_STROKE_DEGENERATE_DRAW;

  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result g = gshscene_store_intern_stroke_style(ctx->store, &style, &resource);
  if (g == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (g != GSHSCENE_OK) {
    /* A width, a dash or an offset outside the waist's band, or more dashes than it holds. */
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a stroke width, dash or dash offset is past the range the waist can carry");
  }
  r = gvec_lw_hold(ctx, resource);
  if (r != GVEC_OK) {
    return r;
  }
  out->style = resource;
  out->decision = GVEC_LW_PAINT_DRAW;
  return GVEC_OK;
}

GVEC_Result gvec_lw_paint_order(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_part order[3], bool * usable) {
  GVEC_Svg_Style_Value v;
  order[0] = GVEC_LW_PART_FILL;
  order[1] = GVEC_LW_PART_STROKE;
  order[2] = GVEC_LW_PART_MARKERS;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_PAINT_ORDER, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable || v.kind != GVEC_SVG_STYLE_KEYWORD || !v.keyword) {
    return r;
  }
  /* `normal`, or the keywords listed in order, the ones left out following in the order fill, stroke, markers (SVG 2, 13.6). */
  static const char * const words[3] = {"fill", "stroke", "markers"};
  bool seen[3] = {false, false, false};
  size_t count = 0;
  const char * p = v.keyword;
  while (*p) {
    while (*p == ' ') {
      p++;
    }
    const char * start = p;
    while (*p && *p != ' ') {
      p++;
    }
    size_t n = (size_t) (p - start);
    for (int i = 0; i < 3; i++) {
      if (!seen[i] && n == strlen(words[i]) && strncmp(start, words[i], n) == 0) {
        seen[i] = true;
        order[count++] = (gvec_lw_part) i;
      }
    }
  }
  for (int i = 0; i < 3; i++) {
    if (!seen[i] && count < 3) {
      order[count++] = (gvec_lw_part) i;
    }
  }
  return GVEC_OK;
}
