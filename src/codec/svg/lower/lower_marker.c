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
 * @file lower_marker.c
 *
 * Markers (SVG 2, 11.6). The vertices of a shape's path and the direction at each come from the geometry library, whole: nothing
 * here computes a tangent. A marker's content is a picture, drawn once for each vertex under the map of the vertex, the direction,
 * the scale and the reference point, and clipped to the marker's viewport unless `overflow` says otherwise. The content inherits
 * from the marker's own place in the document and reads `context-fill` and `context-stroke` from the shape the marker is for.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/shape-geometry/vertices.h>
#include <ghoti.io/shape-scene/resource.h>

#include "lower_internal.h"

/* A marker, read once for a shape: its viewport and everything the placement of an instance needs. */
typedef struct marker {
  const GVEC_Svg_Element * element;
  const GVEC_Svg_Instance * instance;  ///< The marker at its own place: its children inherit from it.
  GSHGEOM_Map view;                    ///< The viewBox map (the identity without a viewBox).
  GSHGEOM_Map fixed;                   ///< What every instance shares: the scale, then the move of the reference point to the origin.
  GVEC_Svg_Orient orient;
  const GSHSCENE_Resource * clip;      ///< The viewport as a path, or NULL when the marker does not clip.
} marker;

typedef enum prepared { PREPARED_DRAWS = 0, PREPARED_NOTHING, PREPARED_REFUSED } prepared;

static GSHGEOM_Point point(double x, double y) {
  GSHGEOM_Point p;
  p.x = x;
  p.y = y;
  p.z = 0;
  return p;
}

static uint64_t hash3(uint64_t a, uint64_t b, uint64_t c) {
  uint8_t buf[24];
  uint64_t parts[3] = {a, b, c};
  for (int p = 0; p < 3; p++) {
    for (int i = 0; i < 8; i++) {
      buf[p * 8 + i] = (uint8_t) (parts[p] >> (8 * i));
    }
  }
  return gvec_svg_hash64(buf, sizeof buf);
}

//-----------------------------------------------------------------------------
// The marker at its own place
//-----------------------------------------------------------------------------

/* The instance of `element` as the document draws it: made down from the root through its ancestors, so that it inherits from them. */
static GVEC_Result place_instance(gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, const GVEC_Svg_Instance ** out) {
  size_t depth = 0;
  for (const GVEC_Svg_Element * a = element; a; a = gvec_svg_element_parent(a)) {
    depth++;
  }
  const GVEC_Svg_Element ** chain = gvec_arena_alloc(&ctx->arena, depth * sizeof *chain, sizeof(void *));
  if (!chain) {
    return GVEC_ERR_OOM;
  }
  size_t i = depth;
  for (const GVEC_Svg_Element * a = element; a; a = gvec_svg_element_parent(a)) {
    chain[--i] = a;
  }
  const GVEC_Svg_Instance * inst = gvec_svg_instance_root(ctx->style);
  if (!inst || gvec_svg_instance_element(inst) != chain[0]) {
    return inst ? GVEC_ERR_INTERNAL : GVEC_ERR_OOM;
  }
  for (i = 1; i < depth; i++) {
    const GVEC_Svg_Instance * next = NULL;
    GVEC_Result r = gvec_svg_instance_child(ctx->style, inst, chain[i], &next);
    if (r != GVEC_OK) {
      return r;
    }
    inst = next;
  }
  *out = inst;
  return GVEC_OK;
}

/* A length attribute of the marker as user units; `*refuse` when its unit needs a basis the lowering does not read. */
static bool length_attribute(const GVEC_Svg_Element * e, const char * name, double fallback, double * out, bool * percent, bool * refuse) {
  const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(e, name);
  GVEC_Svg_Length length;
  *out = fallback;
  *percent = false;
  if (!attribute || gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_length(attribute, &length)) {
    return true;
  }
  if (length.unit == GVEC_SVG_UNIT_PERCENT) {
    *percent = true;
    *out = length.value;
    return true;
  }
  if (!gvec_lw_absolute_length(&length, out)) {
    *refuse = true;
    return false;
  }
  return true;
}

static GVEC_Result disabled(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const char * message) {
  return gvec_diag_add(
      ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_RENDERING_DISABLED, 0, e->node.line, e->node.column, e->node.offset, e->name, message);
}

static GVEC_Result prepare_marker(
    gvec_lw_ctx * ctx, const GVEC_Svg_Element * shape, const GVEC_Svg_Element * element, marker * m, prepared * status) {
  *status = PREPARED_REFUSED;
  memset(m, 0, sizeof *m);
  m->element = element;
  bool refuse = false, percent;
  double w, h;
  if (!length_attribute(element, "markerWidth", 3, &w, &percent, &refuse) || percent ||
      !length_attribute(element, "markerHeight", 3, &h, &percent, &refuse) || percent) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_STYLE, "a marker size in a unit that needs the cascade is not lowered");
  }
  GVEC_Svg_View_Box box;
  bool has_box = false;
  const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(element, "viewBox");
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED) {
    has_box = gvec_svg_attribute_view_box(attribute, &box);
  }
  if (!(w > 0) || !(h > 0) || (has_box && (!(box.width > 0) || !(box.height > 0)))) {
    *status = PREPARED_NOTHING;
    return disabled(ctx, element, "a marker or viewBox of no size disables the rendering of the marker");
  }
  GSHGEOM_Map view;
  if (has_box) {
    GVEC_Svg_Preserve_Aspect_Ratio par = {GVEC_SVG_ALIGN_X_MID_Y_MID, 0, 0};
    const GVEC_Svg_Attribute * p = gvec_svg_element_find_attribute(element, "preserveAspectRatio");
    if (p && gvec_svg_attribute_class(p) == GVEC_SVG_ATTR_TYPED) {
      gvec_svg_attribute_preserve_aspect_ratio(p, &par);
    }
    if (!gvec_lw_viewbox_map(&box, &par, w, h, &view)) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the marker's viewBox is past the range the waist can carry");
    }
  }
  else if (gshgeom_map_identity(2, &view) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  m->view = view;
  /* The reference point is in the content's coordinates; a percentage (and the keywords, which are 0%, 50% and 100%) is of the
   * viewBox, or of the viewport without one (SVG 2, 11.6.2). */
  double ref[2];
  for (int axis = 0; axis < 2; axis++) {
    double v;
    if (!length_attribute(element, axis ? "refY" : "refX", 0, &v, &percent, &refuse)) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_STYLE, "a marker reference point in a unit that needs the cascade is not lowered");
    }
    if (percent) {
      double origin = has_box ? (axis ? box.y : box.x) : 0;
      double extent = has_box ? (axis ? box.height : box.width) : (axis ? h : w);
      v = origin + extent * v / 100.0;
    }
    ref[axis] = v;
  }
  GSHGEOM_Point at;
  if (gshgeom_map_apply_point(&view, point(ref[0], ref[1]), &at) != GSHGEOM_OK) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the marker's reference point is past the range the waist can carry");
  }
  m->orient.mode = GVEC_SVG_ORIENT_ANGLE;
  m->orient.angle = 0;
  attribute = gvec_svg_element_find_attribute(element, "orient");
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED) {
    gvec_svg_attribute_orient(attribute, &m->orient);
  }
  GSHGEOM_Map move;
  if (gshgeom_map_translate(2, -at.x, -at.y, 0, &move) != GSHGEOM_OK) {
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the marker's reference point is past the range the waist can carry");
  }
  m->fixed = move;
  GVEC_Result r = place_instance(ctx, element, &m->instance);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Style_Value overflow;
  bool usable;
  r = gvec_lw_read_property(ctx, m->instance, GVEC_SVG_PROP_OVERFLOW, GVEC_LOWER_STYLE, &overflow, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!(gvec_lw_keyword_is(&overflow, "visible") || gvec_lw_keyword_is(&overflow, "auto"))) {
    gvec_lw_geometry geometry;
    r = gvec_lw_rect_path(ctx, 0, 0, w, h, &geometry, &m->clip);
    if (r != GVEC_OK) {
      return r;
    }
    if (geometry != GVEC_LW_GEOMETRY_PATH) {
      return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the marker's viewport is past the range the waist can carry");
    }
  }
  *status = PREPARED_DRAWS;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// The instances
//-----------------------------------------------------------------------------

enum { SLOT_START = 0, SLOT_MID = 1, SLOT_END = 2 };

typedef struct walk {
  gvec_lw_ctx * ctx;
  const GVEC_Svg_Element * shape;
  const GSHGEOM_Map * local;
  marker * markers[3];             ///< The marker of each slot, or NULL.
  double scale[3];                 ///< The scale of each slot's instances (the stroke width, or 1).
  GVEC_Svg_Paint fill, stroke;     ///< The paints `context-fill` and `context-stroke` name.
  gvec_lw_node ** nodes;
  size_t count;
  size_t hits;
  bool counting;
  GVEC_Result result;
  bool refused;
} walk;

/* The map of one instance: the shape's map, the vertex, the rotation, the scale and the reference point. */
static bool placement(const walk * w, const marker * m, int slot, const GSHGEOM_Vertex * v, double scale, GSHGEOM_Map * out) {
  GSHGEOM_Map rotation, shift, size, step;
  if (m->orient.mode == GVEC_SVG_ORIENT_ANGLE) {
    if (gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, m->orient.angle, &rotation) != GSHGEOM_OK) {
      return false;
    }
  }
  else {
    double sign = m->orient.mode == GVEC_SVG_ORIENT_AUTO_START_REVERSE && slot == SLOT_START ? -1 : 1;
    double dx = sign * v->direction.x, dy = sign * v->direction.y;
    const double linear[4] = {dx, -dy, dy, dx};
    const double zero[2] = {0, 0};
    if (gshgeom_map_from_matrix(2, linear, zero, &rotation) != GSHGEOM_OK) {
      return false;
    }
  }
  return gshgeom_map_translate(2, v->point.x, v->point.y, 0, &shift) == GSHGEOM_OK &&
         gshgeom_map_scale(2, scale, scale, 1, &size) == GSHGEOM_OK && gvec_lw_compose(&shift, &rotation, &step) &&
         gvec_lw_compose(&step, &size, &step) && gvec_lw_compose(&step, &m->fixed, &step) && gvec_lw_compose(w->local, &step, out);
}

/* One instance: a picture of the marker's content, placed by `map`. */
static GVEC_Result instance_node(walk * w, const marker * m, uint64_t ordinal, const GSHGEOM_Map * map, gvec_lw_node ** out) {
  gvec_lw_ctx * ctx = w->ctx;
  const GVEC_Svg_Element * shape = w->shape;
  *out = NULL;
  gvec_lw_node * use = gvec_lw_new_node(ctx, GVEC_LW_USE);
  if (!use) {
    return GVEC_ERR_OOM;
  }
  uint64_t saved_chain = ctx->chain;
  uint64_t chain = hash3(saved_chain, m->element->key, ordinal);
  use->map = *map;
  use->key = gvec_lw_key(shape->key, chain, GVEC_LW_ROLE_INSTANCE);
  use->container_key = gvec_lw_key(shape->key, chain, GVEC_LW_ROLE_CONTAINER);
  use->site = gvec_lw_new_site(ctx, shape, use->key);
  use->container_site = gvec_lw_new_site(ctx, shape, use->container_key);
  if (use->site == GVEC_LW_NO_SITE || use->container_site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  /* The instance is a level and its target another, and the picture is a list of its own. */
  if (ctx->levels + 3 > ctx->caps.node_depth || ctx->picture_depth + 1 > ctx->caps.picture_depth) {
    w->refused = true;
    return gvec_lw_refuse(ctx, shape, GVEC_LOWER_LIMIT, "the marker nests pictures deeper than the scene's limits allow");
  }
  GVEC_Result r = GVEC_OK;
  size_t saved_open = ctx->open;
  bool saved_set = ctx->context_set;
  GVEC_Svg_Paint saved_fill = ctx->context_fill, saved_stroke = ctx->context_stroke;
  ctx->levels += 2;
  ctx->picture_depth++;
  ctx->open = 0;
  ctx->use_depth++;
  ctx->chain = hash3(chain, shape->key, 0);
  ctx->context_set = true;
  ctx->context_fill = w->fill;
  ctx->context_stroke = w->stroke;
  ctx->marker_instances++;
  gvec_lw_node * clip = NULL;
  if (m->clip) {
    clip = gvec_lw_new_node(ctx, GVEC_LW_CLIP);
    if (!clip) {
      r = GVEC_ERR_OOM;
    }
    else {
      clip->resource = m->clip;
      clip->rule = GSHGEOM_FILL_NONZERO;
      clip->key = gvec_lw_key(m->element->key, ctx->chain, GVEC_LW_ROLE_CLIP);
      clip->site = gvec_lw_new_site(ctx, m->element, clip->key);
      if (clip->site == GVEC_LW_NO_SITE) {
        r = GVEC_ERR_OOM;
      }
    }
  }
  if (r == GVEC_OK) {
    bool entered = true;
    if (clip) {
      r = gvec_lw_wrap_enter(ctx, m->element, &entered);
    }
    if (r == GVEC_OK && entered) {
      r = gvec_lw_children(ctx, m->instance, &m->view, clip ? clip : use);
      if (clip) {
        gvec_lw_wrap_leave(ctx);
        if (r == GVEC_OK) {
          r = gvec_lw_attach(ctx, use, clip);
        }
      }
    }
  }
  ctx->context_set = saved_set;
  ctx->context_fill = saved_fill;
  ctx->context_stroke = saved_stroke;
  ctx->chain = saved_chain;
  ctx->use_depth--;
  ctx->open = saved_open;
  ctx->picture_depth--;
  ctx->levels -= 2;
  if (r == GVEC_OK && use->content) {
    *out = use;
  }
  return r;
}

static GSHGEOM_Result visit(const GSHGEOM_Vertex * v, void * data) {
  walk * w = data;
  bool start = (v->flags & GSHGEOM_VERTEX_PATH_START) != 0, end = (v->flags & GSHGEOM_VERTEX_PATH_END) != 0;
  /* A vertex is the start, the end, both (a path of one vertex), or a mid. */
  int slots[2], n = 0;
  if (start) {
    slots[n++] = SLOT_START;
  }
  if (end) {
    slots[n++] = SLOT_END;
  }
  if (!start && !end) {
    slots[n++] = SLOT_MID;
  }
  for (int i = 0; i < n; i++) {
    int slot = slots[i];
    const marker * m = w->markers[slot];
    if (!m) {
      continue;
    }
    if (w->counting) {
      w->hits++;
      continue;
    }
    GSHGEOM_Map map;
    if (!(w->scale[slot] > 0)) {
      continue; /* A scale of nothing (a stroke width of 0 with markerUnits strokeWidth) draws nothing. */
    }
    if (!placement(w, m, slot, v, w->scale[slot], &map)) {
      w->refused = true;
      w->result = gvec_lw_refuse(w->ctx, w->shape, GVEC_LOWER_LIMIT, "a marker is placed past the range the waist can carry");
      return GSHGEOM_ERR_LIMIT;
    }
    gvec_lw_node * node = NULL;
    w->result = instance_node(w, m, ((uint64_t) v->element << 2) | (uint64_t) slot, &map, &node);
    if (w->result != GVEC_OK || w->refused) {
      return GSHGEOM_ERR_LIMIT;
    }
    if (node) {
      w->nodes[w->count++] = node;
    }
  }
  return GSHGEOM_OK;
}

//-----------------------------------------------------------------------------
// The markers of a shape
//-----------------------------------------------------------------------------

/* The marker a property names, or NULL: a missing element draws none (the style layer has said so), and one that is not a marker is
 * said here. */
static GVEC_Result marker_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, const GVEC_Svg_Element ** out,
    bool * usable) {
  GVEC_Svg_Style_Value v;
  *out = NULL;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, property, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  for (size_t i = 0; i < v.reference_count; i++) {
    const GVEC_Svg_Style_Reference * ref = &v.references[i];
    if (!ref->is_url || !ref->local || ref->cut || !ref->target) {
      continue;
    }
    if (gvec_svg_element_kind(ref->target) == GVEC_SVG_KIND_MARKER) {
      *out = ref->target;
    }
    else {
      const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
      return gvec_diag_add(ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_REFERENCE_MISSING, 0, e->node.line, e->node.column,
          e->node.offset, e->name, "a marker property names an element that is not a marker");
    }
    break;
  }
  return GVEC_OK;
}

/* The paint a context paint names for the markers of `inst`: its own, or (inside a marker) the one it takes from its context. */
static GVEC_Result context_paint(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, GVEC_Svg_Paint * out,
    bool * usable) {
  GVEC_Svg_Style_Value v;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, property, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  *out = v.paint;
  if (ctx->context_set && (v.paint.kind == GVEC_SVG_PAINT_CONTEXT_FILL || v.paint.kind == GVEC_SVG_PAINT_CONTEXT_STROKE)) {
    *out = v.paint.kind == GVEC_SVG_PAINT_CONTEXT_FILL ? ctx->context_fill : ctx->context_stroke;
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_markers(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * local, gvec_lw_marker_set * out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  static const GVEC_Svg_Property properties[3] = {GVEC_SVG_PROP_MARKER_START, GVEC_SVG_PROP_MARKER_MID, GVEC_SVG_PROP_MARKER_END};
  out->nodes = NULL;
  out->count = 0;
  out->refused = false;
  const GVEC_Svg_Element * named[3];
  bool usable, any = false;
  GVEC_Result r;
  for (int slot = 0; slot < 3; slot++) {
    r = marker_of(ctx, inst, properties[slot], &named[slot], &usable);
    if (r != GVEC_OK || !usable) {
      out->refused = !usable;
      return r;
    }
    any = any || named[slot];
  }
  if (!any) {
    return GVEC_OK;
  }
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * centerline = NULL;
  r = gvec_lw_centerline_of(ctx, inst, &geometry, &centerline, &usable);
  if (r != GVEC_OK || !usable) {
    out->refused = !usable;
    return r;
  }
  if (geometry == GVEC_LW_GEOMETRY_ARC || geometry == GVEC_LW_GEOMETRY_LIMIT) {
    out->refused = true;
    return geometry == GVEC_LW_GEOMETRY_ARC
               ? gvec_lw_refuse(ctx, e, GVEC_LOWER_ARC, "an elliptical arc waits for the shape family's exact primitive")
               : gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH) {
    return GVEC_OK;
  }
  const GSHGEOM_Path * path = gshscene_resource_path(centerline);
  walk w;
  memset(&w, 0, sizeof w);
  w.ctx = ctx;
  w.shape = e;
  w.local = local;
  /* The scale of `markerUnits="strokeWidth"` is the used stroke width, whether or not the stroke is painted. */
  GVEC_Svg_Style_Value width;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_STROKE_WIDTH, GVEC_LOWER_STYLE, &width, &usable);
  if (r != GVEC_OK || !usable) {
    out->refused = !usable;
    return r;
  }
  double stroke_width = width.kind == GVEC_SVG_STYLE_LENGTH ? width.number : 1;
  marker templates[3];
  for (int slot = 0; slot < 3; slot++) {
    if (!named[slot]) {
      continue;
    }
    prepared status;
    r = prepare_marker(ctx, e, named[slot], &templates[slot], &status);
    if (r != GVEC_OK || status == PREPARED_REFUSED) {
      out->refused = status == PREPARED_REFUSED;
      return r;
    }
    if (status == PREPARED_DRAWS) {
      w.markers[slot] = &templates[slot];
      const GVEC_Svg_Attribute * units = gvec_svg_element_find_attribute(named[slot], "markerUnits");
      size_t index = 1;
      const char * word;
      if (units && gvec_svg_attribute_class(units) == GVEC_SVG_ATTR_TYPED) {
        gvec_svg_attribute_keyword(units, &index, &word);
      }
      w.scale[slot] = index == 0 ? 1 : stroke_width;
    }
  }
  r = context_paint(ctx, inst, GVEC_SVG_PROP_FILL, &w.fill, &usable);
  if (r == GVEC_OK && usable) {
    r = context_paint(ctx, inst, GVEC_SVG_PROP_STROKE, &w.stroke, &usable);
  }
  if (r != GVEC_OK || !usable) {
    out->refused = !usable;
    return r;
  }
  w.counting = true;
  if (gshgeom_path_vertices(path, visit, &w) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  if (!w.hits) {
    return GVEC_OK;
  }
  if (w.hits > ctx->caps.marker_instances - (ctx->marker_instances < ctx->caps.marker_instances ? ctx->marker_instances : ctx->caps.marker_instances)) {
    out->refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element has more marker instances than the limit allows");
  }
  w.counting = false;
  w.nodes = gvec_arena_alloc(&ctx->arena, w.hits * sizeof *w.nodes, sizeof(void *));
  if (!w.nodes) {
    return GVEC_ERR_OOM;
  }
  GSHGEOM_Result g = gshgeom_path_vertices(path, visit, &w);
  if (w.result != GVEC_OK) {
    return w.result;
  }
  if (w.refused) {
    out->refused = true;
    return GVEC_OK;
  }
  if (g != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  for (int slot = 0; slot < 3; slot++) {
    if (w.markers[slot]) {
      gvec_svg_instance_release(ctx->style, w.markers[slot]->instance);
    }
  }
  out->nodes = w.nodes;
  out->count = w.count;
  return GVEC_OK;
}
