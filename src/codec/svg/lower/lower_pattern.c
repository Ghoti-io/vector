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
/*
 * The pattern paint server (SVG 2, 13.3): a tile of the pattern's content on a lattice, lowered to the waist's pattern resource.
 *
 * The scene's pattern is a picture of the tile in pattern space, the cell of that space, the lattice steps (here the cell's
 * size), and the map from pattern space to the user space of the element that is painted. So `x`, `y`, `width` and `height` are the
 * cell, `patternTransform` is the map, and the picture holds the content already placed: the translation to the cell, then the
 * viewBox (or the scale of the bounding box, for `patternContentUnits` of `objectBoundingBox`). The tile is made while the element
 * is walked, as a picture of the content, the way a marker's instance is.
 */

#include <string.h>

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/pattern.h>

#include "lower_internal.h"

/* A reference chain is followed for this many patterns and no further, as a gradient's is. */
#define CHAIN_LIMIT 32

static bool is_pattern(const GVEC_Svg_Element * e) {
  return e && gvec_svg_element_kind(e) == GVEC_SVG_KIND_PATTERN;
}

/* The first attribute `name` that a pattern of the chain has, or NULL. */
static const GVEC_Svg_Attribute * chain_attribute(const GVEC_Svg_Element * start, const char * name) {
  const GVEC_Svg_Element * e = start;
  for (int hops = 0; e && hops < CHAIN_LIMIT && is_pattern(e); hops++, e = gvec_svg_element_href_target_uncut(e)) {
    const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(e, name);
    if (attribute && gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_RAW) {
      return attribute;
    }
  }
  return NULL;
}

/* The first pattern of the chain that has a child element: the content of the paint server (SVG 2, 13.3.1). */
static const GVEC_Svg_Element * content_source(const GVEC_Svg_Element * start) {
  const GVEC_Svg_Element * e = start;
  for (int hops = 0; e && hops < CHAIN_LIMIT && is_pattern(e); hops++, e = gvec_svg_element_href_target_uncut(e)) {
    if (gvec_svg_element_first_child(e)) {
      return e;
    }
  }
  return NULL;
}

static bool keyword_is_box(const GVEC_Svg_Attribute * attribute, bool fallback) {
  size_t index;
  const char * word;
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_keyword(attribute, &index, &word)) {
    return index == 1;
  }
  return fallback;
}

/* A coordinate of the cell: a fraction of the box (a percentage is that over 100), or user units, a percentage being of `basis`. */
static bool coordinate(const GVEC_Svg_Attribute * attribute, bool bounding_box, double basis, double * out) {
  GVEC_Svg_Length length = {0, GVEC_SVG_UNIT_NONE};
  *out = 0;
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

static uint64_t hash2(uint64_t a, uint64_t b) {
  uint8_t buf[16];
  uint64_t parts[2] = {a, b};
  for (int p = 0; p < 2; p++) {
    for (int i = 0; i < 8; i++) {
      buf[p * 8 + i] = (uint8_t) (parts[p] >> (8 * i));
    }
  }
  return gvec_svg_hash64(buf, sizeof buf);
}

GVEC_Result gvec_lw_pattern_paint(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint, double opacity,
    GSHSCENE_Paint * out, gvec_lw_paint_decision * decision) {
  return gvec_lw_pattern_paint_in(ctx, inst, paint, opacity, NULL, NULL, out, decision);
}

GVEC_Result gvec_lw_pattern_paint_in(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint, double opacity,
    const GSHGEOM_Map * into, const GSHGEOM_Bounds * given, GSHSCENE_Paint * out, gvec_lw_paint_decision * decision) {
  const GVEC_Svg_Element * shape = gvec_svg_instance_element(inst);
  const GVEC_Svg_Element * server = paint->server;
  *decision = GVEC_LW_PAINT_REFUSED;
  GVEC_Result r;

  /* A pattern whose content (at any depth of nesting) paints with the pattern itself has no tile: it is an error in the document, and
   * the element is not drawn with it. */
  for (size_t i = 0; i < ctx->pattern_depth; i++) {
    if (ctx->pattern_stack[i] == server) {
      return not_rendered(ctx, server, "a pattern that paints its own content with itself is not rendered", decision);
    }
  }
  if (ctx->pattern_depth >= GVEC_LW_PATTERN_NESTING || ctx->picture_depth + 1 > ctx->caps.picture_depth || ctx->levels + 3 > ctx->caps.node_depth) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the patterns nest deeper than the scene's limits allow");
  }

  bool units_box = keyword_is_box(chain_attribute(server, "patternUnits"), true);
  bool content_box = keyword_is_box(chain_attribute(server, "patternContentUnits"), false);

  /* The map from pattern space to the element's user space. */
  GSHGEOM_Map transform;
  if (gshgeom_map_identity(2, &transform) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  const GVEC_Svg_Element * e = server;
  for (int hops = 0; e && hops < CHAIN_LIMIT && is_pattern(e); hops++, e = gvec_svg_element_href_target_uncut(e)) {
    if (!gvec_svg_element_find_attribute(e, "patternTransform")) {
      continue;
    }
    const GVEC_Svg_Instance * at;
    r = gvec_lw_instance_of(ctx, e, &at);
    if (r != GVEC_OK) {
      return r;
    }
    GVEC_Svg_Style_Value v;
    bool usable;
    r = gvec_lw_read_property(ctx, at, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    if (v.transform.depends_on_box) {
      return gvec_lw_refuse(ctx, server, GVEC_LOWER_TRANSFORM_BOX, "a pattern transform of a bounding box is not lowered yet");
    }
    transform = v.transform.map;
    break;
  }
  double det = transform.m[0][0] * transform.m[1][1] - transform.m[0][1] * transform.m[1][0];
  if (!(det != 0) || det - det != 0) {
    return not_rendered(ctx, server, "a patternTransform with no inverse disables the paint server", decision);
  }

  /* The box of the element: the fraction and the scale of the box units. */
  GSHGEOM_Bounds box;
  box.empty = 1;
  if (units_box || content_box) {
    if (given) {
      box = *given;
    }
    else {
      gvec_lw_geometry geometry;
      const GSHSCENE_Resource * resource = NULL;
      bool usable;
      r = gvec_lw_centerline_of(ctx, inst, &geometry, &resource, &usable);
      if (r != GVEC_OK || !usable) {
        return r;
      }
      if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
        return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
      }
      if (geometry != GVEC_LW_GEOMETRY_PATH || gshgeom_path_bounds(gshscene_resource_path(resource), &box) != GSHGEOM_OK) {
        box.empty = 1;
      }
    }
    if (box.empty || !(box.max.x > box.min.x) || !(box.max.y > box.min.y)) {
      if (paint->has_fallback) {
        GVEC_Svg_Paint fallback;
        memset(&fallback, 0, sizeof fallback);
        fallback.kind = paint->fallback_kind;
        fallback.color = paint->fallback_color;
        return gvec_lw_paint_of(ctx, inst, &fallback, opacity, out, decision);
      }
      return not_rendered(ctx, server, "an element with no width or no height has no bounding box for a pattern to be placed in", decision);
    }
  }
  double bw = box.empty ? 1 : box.max.x - box.min.x;
  double bh = box.empty ? 1 : box.max.y - box.min.y;

  double vw, vh;
  gvec_svg_instance_viewport(inst, &vw, &vh);
  double x, y, w, h;
  if (!coordinate(chain_attribute(server, "x"), units_box, vw, &x) || !coordinate(chain_attribute(server, "y"), units_box, vh, &y) ||
      !coordinate(chain_attribute(server, "width"), units_box, vw, &w) || !coordinate(chain_attribute(server, "height"), units_box, vh, &h)) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_STYLE, "a pattern length that needs the font or the window is not lowered");
  }
  if (units_box) {
    x = box.min.x + x * bw;
    y = box.min.y + y * bh;
    w *= bw;
    h *= bh;
  }
  if (!(w > 0) || !(h > 0)) {
    return not_rendered(ctx, server, "a pattern with no width or no height disables the paint server", decision);
  }
  if (x - x != 0 || y - y != 0 || w - w != 0 || h - h != 0) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the pattern's cell is past the range the waist can carry");
  }

  /* The content's frame: the viewBox into the cell, or the box's scale, or nothing. */
  GSHGEOM_Map frame;
  GVEC_Svg_View_Box vb;
  bool has_view_box = false;
  const GVEC_Svg_Attribute * attribute = chain_attribute(server, "viewBox");
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED) {
    has_view_box = gvec_svg_attribute_view_box(attribute, &vb);
  }
  if (has_view_box) {
    if (!(vb.width > 0) || !(vb.height > 0)) {
      return not_rendered(ctx, server, "a pattern viewBox of no size disables the paint server", decision);
    }
    GVEC_Svg_Preserve_Aspect_Ratio par = {GVEC_SVG_ALIGN_X_MID_Y_MID, 0, 0};
    const GVEC_Svg_Attribute * p = chain_attribute(server, "preserveAspectRatio");
    if (p && gvec_svg_attribute_class(p) == GVEC_SVG_ATTR_TYPED) {
      gvec_svg_attribute_preserve_aspect_ratio(p, &par);
    }
    if (!gvec_lw_viewbox_map(&vb, &par, w, h, &frame)) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the pattern's viewBox is past the range the waist can carry");
    }
  }
  else if (content_box) {
    if (gshgeom_map_scale(2, bw, bh, 1, &frame) != GSHGEOM_OK) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the pattern's content scale is past the range the waist can carry");
    }
  }
  else if (gshgeom_map_identity(2, &frame) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  GSHGEOM_Map move, placed;
  if (gshgeom_map_translate(2, x, y, 0, &move) != GSHGEOM_OK || !gvec_lw_compose(&move, &frame, &placed)) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the pattern's cell is past the range the waist can carry");
  }

  const GVEC_Svg_Element * source = content_source(server);
  if (!source) {
    *decision = GVEC_LW_PAINT_NONE; /* No content: nothing is painted. */
    return GVEC_OK;
  }
  const GVEC_Svg_Instance * source_inst;
  r = gvec_lw_instance_of(ctx, source, &source_inst);
  if (r != GVEC_OK) {
    return r;
  }
  bool overflow_visible = false;
  {
    GVEC_Svg_Style_Value overflow;
    bool usable;
    const GVEC_Svg_Instance * own;
    r = gvec_lw_instance_of(ctx, server, &own);
    if (r != GVEC_OK) {
      return r;
    }
    r = gvec_lw_read_property(ctx, own, GVEC_SVG_PROP_OVERFLOW, GVEC_LOWER_STYLE, &overflow, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    overflow_visible = gvec_lw_keyword_is(&overflow, "visible") || gvec_lw_keyword_is(&overflow, "auto");
  }

  /* The tile: the content walked as the content of a picture, under the frame. */
  gvec_lw_node * holder = gvec_lw_new_node(ctx, GVEC_LW_USE);
  if (!holder) {
    return GVEC_ERR_OOM;
  }
  holder->key = gvec_lw_key(server->key, ctx->chain, GVEC_LW_ROLE_CONTAINER);
  holder->site = gvec_lw_new_site(ctx, shape, holder->key); /* A limit reached recording the tile refuses the element that uses it. */
  if (holder->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  uint64_t saved_chain = ctx->chain;
  size_t saved_open = ctx->open;
  size_t saved_levels = ctx->levels;
  bool saved_set = ctx->context_set;
  const GVEC_Svg_Instance * saved_context_inst = ctx->context_inst;
  GSHGEOM_Map saved_item_local = ctx->item_local;
  bool saved_item_crisp = ctx->item_crisp;
  ctx->levels += 2;
  ctx->picture_depth++;
  ctx->open = 0;
  ctx->use_depth++;
  ctx->chain = hash2(saved_chain, server->key);
  ctx->context_set = false;
  ctx->context_inst = NULL;
  ctx->pattern_stack[ctx->pattern_depth++] = server;
  r = gvec_lw_children(ctx, source_inst, &placed, holder);
  ctx->pattern_depth--;
  ctx->context_set = saved_set;
  ctx->context_inst = saved_context_inst;
  ctx->item_local = saved_item_local;
  ctx->item_crisp = saved_item_crisp;
  ctx->chain = saved_chain;
  ctx->use_depth--;
  ctx->open = saved_open;
  ctx->picture_depth--;
  ctx->levels = saved_levels;
  if (r != GVEC_OK) {
    return r;
  }
  if (!holder->content) {
    *decision = GVEC_LW_PAINT_NONE; /* An empty tile paints nothing. */
    return GVEC_OK;
  }
  const GSHSCENE_Resource * tile = NULL;
  r = gvec_lw_emit_picture(ctx, holder, &tile);
  if (r != GVEC_OK) {
    return r;
  }
  if (!tile) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the pattern's tile is past what the waist can carry");
  }
  GSHSCENE_Pattern pattern;
  gshscene_pattern_init(&pattern);
  pattern.tile = tile;
  pattern.x = x;
  pattern.y = y;
  pattern.w = w;
  pattern.h = h;
  pattern.x_step = w;
  pattern.y_step = h;
  pattern.repeat_x = 1;
  pattern.repeat_y = 1;
  if (into && !gvec_lw_compose(into, &transform, &transform)) {
    gshscene_resource_release(tile);
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the pattern's map is past the range the waist can carry");
  }
  pattern.map = transform;
  pattern.overflow = overflow_visible ? 1 : 0;
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result sr = gshscene_store_intern_pattern(ctx->store, &pattern, &resource);
  gshscene_resource_release(tile);
  if (sr == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (sr != GSHSCENE_OK) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the pattern is past what the waist can carry");
  }
  r = gvec_lw_hold(ctx, resource);
  if (r != GVEC_OK) {
    return r;
  }
  memset(out, 0, sizeof *out);
  out->kind = GSHSCENE_PAINT_PATTERN;
  out->resource = resource;
  out->rgba[3] = opacity;
  *decision = opacity > 0 ? GVEC_LW_PAINT_DRAW : GVEC_LW_PAINT_NONE;
  return GVEC_OK;
}
