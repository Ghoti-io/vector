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
 * @file lower_gradient.c
 *
 * `linearGradient` and `radialGradient` as the shape waist's gradient resource (H1 to H8): the units, the transform, the spread,
 * the stops and what a reference chain inherits. Nothing is sampled or rasterized here; the resource says where the colors are
 * and a sink draws them.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/paint.h>

#include "lower_internal.h"

/* A reference chain is followed for this many gradients and no further: a loop repeats elements, so cutting it anywhere uses what
 * the chain has already given. */
#define CHAIN_LIMIT 32

/* What of a paint server does not depend on the element that uses it: its stops, transform, units and spread. */
typedef struct gradient_cache {
  struct gradient_cache * next;
  const GVEC_Svg_Element * server;
  bool usable;                       ///< False when a property of a stop or of the gradient was refused (already reported).
  bool bounding_box;                 ///< `gradientUnits` is `objectBoundingBox`.
  uint32_t extend;
  GSHGEOM_Map transform;             ///< `gradientTransform`.
  bool singular;                     ///< The transform has no inverse: the paint server is not rendered.
  bool linear_rgb;                   ///< `color-interpolation` is `linearRGB`.
  GSHSCENE_Gradient_Stop * stops;
  size_t stop_count;
} gradient_cache;

static bool is_gradient(const GVEC_Svg_Element * e) {
  GVEC_Svg_Kind kind = e ? gvec_svg_element_kind(e) : GVEC_SVG_KIND_COUNT;
  return kind == GVEC_SVG_KIND_LINEAR_GRADIENT || kind == GVEC_SVG_KIND_RADIAL_GRADIENT;
}

/* The first attribute `name` that a gradient of the chain has, or NULL. A name only one kind has is read from gradients of that
 * kind (SVG 2, 13.2.2: the geometry of a `linearGradient` is not that of a `radialGradient` that names it). */
static const GVEC_Svg_Attribute * chain_attribute(const GVEC_Svg_Element * start, const char * name, bool same_kind) {
  GVEC_Svg_Kind kind = gvec_svg_element_kind(start);
  const GVEC_Svg_Element * e = start;
  for (int hops = 0; e && hops < CHAIN_LIMIT && is_gradient(e); hops++, e = gvec_svg_element_href_target_uncut(e)) {
    if (same_kind && gvec_svg_element_kind(e) != kind) {
      continue;
    }
    const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(e, name);
    if (attribute && gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_RAW) {
      return attribute;
    }
  }
  return NULL;
}

/* The instance of an element of the document: the chain from the root, so that a property inherits as it does in the tree. */
GVEC_Result gvec_lw_instance_of(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const GVEC_Svg_Instance ** out) {
  const GVEC_Svg_Element * parent = gvec_svg_element_parent(e);
  if (!parent) {
    *out = gvec_svg_instance_root(ctx->style);
    return GVEC_OK;
  }
  const GVEC_Svg_Instance * above;
  GVEC_Result r = gvec_lw_instance_of(ctx, parent, &above);
  if (r != GVEC_OK) {
    return r;
  }
  return gvec_svg_instance_child(ctx->style, above, e, out);
}

static double clamp01(double v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* One stop's paint: its color with `stop-opacity`, in the waist's terms. */
static GVEC_Result stop_paint(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GSHSCENE_Paint * out, bool * usable) {
  GVEC_Svg_Style_Value color, opacity;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STOP_COLOR, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &color, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STOP_OPACITY, GVEC_LOWER_STYLE, &opacity, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  double factor = opacity.kind == GVEC_SVG_STYLE_NUMBER ? clamp01(opacity.number) : 1;
  memset(out, 0, sizeof *out);
  const GVEC_Svg_Color * c = color.kind == GVEC_SVG_STYLE_COLOR ? &color.color : color.kind == GVEC_SVG_STYLE_PAINT ? &color.paint.color : NULL;
  if (!c) {
    *usable = false;
    return gvec_lw_refuse(ctx, gvec_svg_instance_element(inst), GVEC_LOWER_STYLE, "a stop color that is not a color is not lowered");
  }
  if (c->kind == GVEC_SVG_COLOR_FOREGROUND) {
    out->kind = GSHSCENE_PAINT_FOREGROUND;
    out->rgba[3] = c->color.rgba[3] * factor;
  }
  else {
    out->kind = GSHSCENE_PAINT_SOLID;
    out->rgba[0] = c->color.rgba[0];
    out->rgba[1] = c->color.rgba[1];
    out->rgba[2] = c->color.rgba[2];
    out->rgba[3] = c->color.rgba[3] * factor;
    out->space = c->color.space;
  }
  return GVEC_OK;
}

/* The stops of the first gradient of the chain that has any (SVG 2, 13.2.3), offsets clamped to [0, 1] and never below the one before. */
static GVEC_Result chain_stops(gvec_lw_ctx * ctx, const GVEC_Svg_Element * start, gradient_cache * cache) {
  const GVEC_Svg_Element * source = NULL;
  const GVEC_Svg_Element * e = start;
  for (int hops = 0; e && hops < CHAIN_LIMIT && is_gradient(e) && !source; hops++, e = gvec_svg_element_href_target_uncut(e)) {
    for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(e); c; c = gvec_svg_element_next_sibling(c)) {
      if (gvec_svg_element_kind(c) == GVEC_SVG_KIND_STOP) {
        source = e;
        break;
      }
    }
  }
  cache->stops = NULL;
  cache->stop_count = 0;
  if (!source) {
    return GVEC_OK;
  }
  size_t count = 0;
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(source); c; c = gvec_svg_element_next_sibling(c)) {
    count += gvec_svg_element_kind(c) == GVEC_SVG_KIND_STOP;
  }
  size_t limit = 65535;
  if (count > limit) {
    cache->usable = false;
    return gvec_lw_refuse(ctx, start, GVEC_LOWER_LIMIT, "the gradient has more stops than the waist allows");
  }
  GSHSCENE_Gradient_Stop * stops = gvec_arena_alloc(&ctx->arena, count * sizeof *stops, sizeof(double));
  if (!stops) {
    return GVEC_ERR_OOM;
  }
  double previous = 0;
  size_t n = 0;
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(source); c; c = gvec_svg_element_next_sibling(c)) {
    if (gvec_svg_element_kind(c) != GVEC_SVG_KIND_STOP) {
      continue;
    }
    const GVEC_Svg_Instance * inst;
    GVEC_Result r = gvec_lw_instance_of(ctx, c, &inst);
    if (r != GVEC_OK) {
      return r;
    }
    bool usable;
    r = stop_paint(ctx, inst, &stops[n].color, &usable);
    if (r != GVEC_OK || !usable) {
      cache->usable = false;
      return r;
    }
    double offset = 0;
    const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(c, "offset");
    if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED) {
      gvec_svg_attribute_number(attribute, &offset);
    }
    offset = clamp01(offset);
    if (offset < previous) {
      offset = previous;
    }
    previous = offset;
    stops[n].offset = offset;
    n++;
  }
  cache->stops = stops;
  cache->stop_count = n;
  return GVEC_OK;
}

static gradient_cache * cache_of(gvec_lw_ctx * ctx, const GVEC_Svg_Element * server, GVEC_Result * result) {
  *result = GVEC_OK;
  for (gradient_cache * c = ctx->gradients; c; c = c->next) {
    if (c->server == server) {
      return c;
    }
  }
  gradient_cache * c = gvec_arena_alloc(&ctx->arena, sizeof *c, sizeof(double));
  if (!c) {
    *result = GVEC_ERR_OOM;
    return NULL;
  }
  memset(c, 0, sizeof *c);
  c->server = server;
  c->usable = true;
  c->bounding_box = true;
  c->extend = GSHSCENE_EXTEND_PAD;
  if (gshgeom_map_identity(2, &c->transform) != GSHGEOM_OK) {
    *result = GVEC_ERR_INTERNAL;
    return NULL;
  }
  c->next = ctx->gradients;
  ctx->gradients = c;
  const GVEC_Svg_Attribute * attribute = chain_attribute(server, "gradientUnits", false);
  size_t index;
  const char * word;
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_keyword(attribute, &index, &word)) {
    c->bounding_box = index == 1;
  }
  attribute = chain_attribute(server, "spreadMethod", false);
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_keyword(attribute, &index, &word)) {
    c->extend = index == 1 ? GSHSCENE_EXTEND_REFLECT : index == 2 ? GSHSCENE_EXTEND_REPEAT : GSHSCENE_EXTEND_PAD;
  }
  /* The transform: the one of the first gradient of the chain that gives one, read as the transform of that element. */
  const GVEC_Svg_Element * e = server;
  for (int hops = 0; e && hops < CHAIN_LIMIT && is_gradient(e); hops++, e = gvec_svg_element_href_target_uncut(e)) {
    if (!gvec_svg_element_find_attribute(e, "gradientTransform")) {
      continue;
    }
    const GVEC_Svg_Instance * inst;
    GVEC_Result r = gvec_lw_instance_of(ctx, e, &inst);
    if (r != GVEC_OK) {
      *result = r;
      return NULL;
    }
    GVEC_Svg_Style_Value v;
    bool usable;
    r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      c->usable = false;
      *result = r;
      return c;
    }
    if (v.transform.depends_on_box) {
      c->usable = false;
      *result = gvec_lw_refuse(ctx, server, GVEC_LOWER_TRANSFORM_BOX, "a gradient transform of a bounding box is not lowered yet");
      return c;
    }
    c->transform = v.transform.map;
    break;
  }
  double det = c->transform.m[0][0] * c->transform.m[1][1] - c->transform.m[0][1] * c->transform.m[1][0];
  c->singular = !(det != 0) || det - det != 0;
  const GVEC_Svg_Instance * inst;
  GVEC_Result r = gvec_lw_instance_of(ctx, server, &inst);
  if (r != GVEC_OK) {
    *result = r;
    return NULL;
  }
  GVEC_Svg_Style_Value v;
  bool usable;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_COLOR_INTERPOLATION, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    c->usable = false;
    *result = r;
    return c;
  }
  c->linear_rgb = gvec_lw_keyword_is(&v, "linearrgb") || gvec_lw_keyword_is(&v, "linearRGB");
  *result = chain_stops(ctx, server, c);
  return c;
}

/* A coordinate of the gradient: a fraction of the box, or user units. `fallback` is the default, in percent. */
static bool coordinate(const GVEC_Svg_Attribute * attribute, double fallback, bool bounding_box, double basis, double * out) {
  GVEC_Svg_Length length = {fallback, GVEC_SVG_UNIT_PERCENT};
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED) {
    gvec_svg_attribute_length(attribute, &length);
  }
  if (length.unit == GVEC_SVG_UNIT_PERCENT) {
    *out = bounding_box ? length.value / 100.0 : length.value * basis / 100.0;
    return true;
  }
  return gvec_lw_absolute_length(&length, out);
}

static GVEC_Result not_rendered(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const char * message, gvec_lw_paint_decision * decision) {
  *decision = GVEC_LW_PAINT_NONE;
  return gvec_diag_add(
      ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_RENDERING_DISABLED, 0, e->node.line, e->node.column, e->node.offset, e->name, message);
}

/* A paint of the stop of the last offset: what a gradient with nothing to interpolate over is. */
static void solid_of(const GSHSCENE_Gradient_Stop * stop, double opacity, GSHSCENE_Paint * out, gvec_lw_paint_decision * decision) {
  *out = stop->color;
  out->rgba[3] *= opacity;
  *decision = out->rgba[3] > 0 ? GVEC_LW_PAINT_DRAW : GVEC_LW_PAINT_NONE;
}

GVEC_Result gvec_lw_gradient_paint(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint, double opacity,
    GSHSCENE_Paint * out, gvec_lw_paint_decision * decision) {
  const GVEC_Svg_Element * shape = gvec_svg_instance_element(inst);
  const GVEC_Svg_Element * server = paint->server;
  *decision = GVEC_LW_PAINT_REFUSED;
  GVEC_Result r;
  gradient_cache * cache = cache_of(ctx, server, &r);
  if (r != GVEC_OK || !cache || !cache->usable) {
    return r != GVEC_OK ? r : gvec_lw_refuse(ctx, shape, GVEC_LOWER_GRADIENT, "a gradient whose stops or transform cannot be used");
  }
  if (cache->linear_rgb) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_GRADIENT, "color-interpolation: linearRGB is not lowered yet");
  }
  if (cache->singular) {
    return not_rendered(ctx, server, "a gradientTransform with no inverse disables the paint server", decision);
  }
  if (cache->stop_count == 0) {
    *decision = GVEC_LW_PAINT_NONE; /* No stops: the paint is none (SVG 2, 13.2.3). */
    return GVEC_OK;
  }
  if (cache->stop_count == 1) {
    solid_of(&cache->stops[0], opacity, out, decision);
    return GVEC_OK;
  }
  /* The map from gradient space to the element's local space. */
  GSHGEOM_Map map = cache->transform;
  double vw, vh;
  gvec_svg_instance_viewport(inst, &vw, &vh);
  if (cache->bounding_box) {
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * resource = NULL;
    bool usable;
    r = gvec_lw_centerline_of(ctx, inst, &geometry, &resource, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    GSHGEOM_Bounds box;
    if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
    }
    if (geometry != GVEC_LW_GEOMETRY_PATH || gshgeom_path_bounds(gshscene_resource_path(resource), &box) != GSHGEOM_OK || box.empty ||
        !(box.max.x > box.min.x) || !(box.max.y > box.min.y)) {
      return not_rendered(ctx, server, "an element with no width or no height has no bounding box for a gradient to be placed in", decision);
    }
    GSHGEOM_Map shift, scale, placed;
    if (gshgeom_map_translate(2, box.min.x, box.min.y, 0, &shift) != GSHGEOM_OK ||
        gshgeom_map_scale(2, box.max.x - box.min.x, box.max.y - box.min.y, 1, &scale) != GSHGEOM_OK || !gvec_lw_compose(&shift, &scale, &placed) ||
        !gvec_lw_compose(&placed, &cache->transform, &map)) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the gradient's map is past the range the waist can carry");
    }
  }
  bool bb = cache->bounding_box;
  GSHSCENE_Gradient g;
  memset(&g, 0, sizeof g);
  g.map = map;
  g.extend = cache->extend;
  g.stop_count = cache->stop_count;
  g.stops = cache->stops;
  if (gvec_svg_element_kind(server) == GVEC_SVG_KIND_LINEAR_GRADIENT) {
    double x1, y1, x2, y2;
    if (!coordinate(chain_attribute(server, "x1", true), 0, bb, vw, &x1) || !coordinate(chain_attribute(server, "y1", true), 0, bb, vh, &y1) ||
        !coordinate(chain_attribute(server, "x2", true), 100, bb, vw, &x2) || !coordinate(chain_attribute(server, "y2", true), 0, bb, vh, &y2)) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_STYLE, "a gradient length that needs the font or the window is not lowered");
    }
    if (x1 == x2 && y1 == y2) {
      solid_of(&cache->stops[cache->stop_count - 1], opacity, out, decision); /* One color: the last stop's (SVG 2, 13.2.2). */
      return GVEC_OK;
    }
    g.kind = GSHSCENE_GRADIENT_LINEAR;
    g.p0[0] = x1;
    g.p0[1] = y1;
    g.p1[0] = x2;
    g.p1[1] = y2;
  }
  else {
    double cx, cy, rr, fx, fy, fr;
    double diagonal = gmath_sqrt((vw * vw + vh * vh) / 2.0);
    if (!coordinate(chain_attribute(server, "cx", true), 50, bb, vw, &cx) || !coordinate(chain_attribute(server, "cy", true), 50, bb, vh, &cy) ||
        !coordinate(chain_attribute(server, "r", true), 50, bb, diagonal, &rr) || !coordinate(chain_attribute(server, "fr", true), 0, bb, diagonal, &fr)) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_STYLE, "a gradient length that needs the font or the window is not lowered");
    }
    const GVEC_Svg_Attribute * afx = chain_attribute(server, "fx", true);
    const GVEC_Svg_Attribute * afy = chain_attribute(server, "fy", true);
    if (afx) {
      if (!coordinate(afx, 50, bb, vw, &fx)) {
        return gvec_lw_refuse(ctx, shape, GVEC_LOWER_STYLE, "a gradient length that needs the font or the window is not lowered");
      }
    }
    else {
      fx = cx;
    }
    if (afy) {
      if (!coordinate(afy, 50, bb, vh, &fy)) {
        return gvec_lw_refuse(ctx, shape, GVEC_LOWER_STYLE, "a gradient length that needs the font or the window is not lowered");
      }
    }
    else {
      fy = cy;
    }
    if (rr < 0 || fr < 0) {
      return not_rendered(ctx, server, "a negative radius is an error that disables the paint server", decision);
    }
    if (rr == 0) {
      solid_of(&cache->stops[cache->stop_count - 1], opacity, out, decision); /* A radius of zero is the last stop's color. */
      return GVEC_OK;
    }
    g.kind = GSHSCENE_GRADIENT_RADIAL;
    g.c0[0] = fx;
    g.c0[1] = fy;
    g.r0 = fr;
    g.c1[0] = cx;
    g.c1[1] = cy;
    g.r1 = rr;
  }
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result sr = gshscene_store_intern_gradient(ctx->store, &g, &resource);
  if (sr == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (sr != GSHSCENE_OK) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the gradient is past what the waist can carry");
  }
  r = gvec_lw_hold(ctx, resource);
  if (r != GVEC_OK) {
    return r;
  }
  memset(out, 0, sizeof *out);
  out->kind = GSHSCENE_PAINT_GRADIENT;
  out->resource = resource;
  out->rgba[3] = opacity;
  *decision = opacity > 0 ? GVEC_LW_PAINT_DRAW : GVEC_LW_PAINT_NONE;
  return GVEC_OK;
}
