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
 * @file lower_clip.c
 *
 * `clip-path` naming a `clipPath` of one shape (H9, H10): one clip node of the waist around what the element draws, never an alpha
 * mask, and a clip that is itself clipped (on the clipPath or on its shape) a nested node, whose region is the intersection. What
 * needs several shapes in one clip is the union clip of the shape family and is refused until that exists; so is a basic shape and a
 * `use` child.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/shape-geometry/path.h>

#include "lower_internal.h"

static bool is_shape(GVEC_Svg_Kind kind) {
  return kind == GVEC_SVG_KIND_PATH || kind == GVEC_SVG_KIND_POLYGON || kind == GVEC_SVG_KIND_POLYLINE || kind == GVEC_SVG_KIND_LINE ||
      kind == GVEC_SVG_KIND_RECT || kind == GVEC_SVG_KIND_CIRCLE || kind == GVEC_SVG_KIND_ELLIPSE;
}

/* The map of an element's own `transform`; false (the element refused) when it depends on a box this lowering does not have. */
static GVEC_Result own_transform(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Element * refused, GSHGEOM_Map * out,
    bool * usable) {
  GVEC_Svg_Style_Value v;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  if (v.transform.depends_on_box) {
    *usable = false;
    return gvec_lw_refuse(ctx, refused, GVEC_LOWER_TRANSFORM_BOX, "a transform of a bounding box in a clip is not lowered yet");
  }
  *out = v.transform.map;
  return GVEC_OK;
}

/* Whether the `clip-path` property of `inst` names something: 0 no (also a reference a cycle cut, which is read as none), 1 a
 * clipPath's reference (`*target`), -1 something this does not lower. */
static GVEC_Result clip_reference(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, int * kind, const GVEC_Svg_Element ** target) {
  GVEC_Svg_Style_Value v;
  bool usable;
  *kind = 0;
  *target = NULL;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_CLIP_PATH, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    *kind = usable ? 0 : -1;
    return r;
  }
  for (size_t i = 0; i < v.reference_count; i++) {
    const GVEC_Svg_Style_Reference * ref = &v.references[i];
    if (!ref->is_url || !ref->local) {
      *kind = -1; /* A basic shape, a geometry box or a reference outside the document. */
      return GVEC_OK;
    }
  }
  if (v.reference_count > 0) {
    /* A reference the style layer cut because it closes a cycle still names its element: the chain is followed until it comes
     * back to a clip it has already made, and stops there. */
    const GVEC_Svg_Style_Reference * ref = &v.references[0];
    const GVEC_Svg_Element * t = ref->target;
    if (!t && ref->cut && ref->id) {
      t = gvec_svg_find_by_id(ctx->document, ref->id);
    }
    if (t) {
      *kind = 1;
      *target = t;
    }
  }
  return GVEC_OK;
}

typedef struct chain {
  gvec_lw_ctx * ctx;
  const GVEC_Svg_Element * element; ///< The element that is clipped: what is refused, and whose box the units name.
  const GVEC_Svg_Instance * inst;
  gvec_lw_node ** nodes;
  size_t count;
  const GVEC_Svg_Element * seen[GVEC_LW_CLIP_MAX + 1]; ///< The clipPaths of the chain so far: one that comes again closes a cycle.
  size_t seen_count;
} chain;

static bool seen_before(const chain * c, const GVEC_Svg_Element * clip_path) {
  for (size_t i = 0; i < c->seen_count; i++) {
    if (c->seen[i] == clip_path) {
      return true;
    }
  }
  return false;
}

/* Add the clip nodes of `clip_path`, in `space`: *state is NODE when they were added, ALL when nothing gets through, REFUSED when the
 * element is. */
static GVEC_Result add_clip(chain * c, const GVEC_Svg_Element * clip_path, const GSHGEOM_Map * space, size_t depth, gvec_lw_clip_state * state) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  *state = GVEC_LW_CLIP_REFUSED;
  if (depth >= GVEC_LW_CLIP_MAX || c->count >= GVEC_LW_CLIP_MAX) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "clips are nested deeper than the lowering allows");
  }
  c->seen[c->seen_count++] = clip_path;
  const GVEC_Svg_Instance * clip_inst;
  GVEC_Result r = gvec_lw_instance_of(ctx, clip_path, &clip_inst);
  if (r != GVEC_OK) {
    return r;
  }
  /* The clipPath's own clip-path: an intersection, in the same space. */
  const GVEC_Svg_Element * nested_target;
  int nested;
  r = clip_reference(ctx, clip_inst, &nested, &nested_target);
  if (r != GVEC_OK) {
    return r;
  }
  if (nested == -1) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clip that is clipped by a basic shape is not lowered yet");
  }
  if (nested == 1 && gvec_svg_element_kind(nested_target) == GVEC_SVG_KIND_CLIP_PATH && !seen_before(c, nested_target)) {
    gvec_lw_clip_state inner;
    r = add_clip(c, nested_target, space, depth + 1, &inner);
    if (r != GVEC_OK || inner != GVEC_LW_CLIP_NODE) {
      *state = inner;
      return r;
    }
  }
  /* Its one shape: the children that are shapes and are drawn; text is refused, and anything else is not part of a clip. */
  const GVEC_Svg_Element * shape = NULL;
  const GVEC_Svg_Instance * shape_inst = NULL;
  size_t shapes = 0;
  for (const GVEC_Svg_Element * child = gvec_svg_element_first_child(clip_path); child; child = gvec_svg_element_next_sibling(child)) {
    GVEC_Svg_Kind ck = gvec_svg_element_kind(child);
    if (ck == GVEC_SVG_KIND_TEXT) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "text in a clipPath is not lowered yet");
    }
    if (ck == GVEC_SVG_KIND_USE) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a use in a clipPath is not lowered yet");
    }
    if (!is_shape(ck)) {
      continue;
    }
    const GVEC_Svg_Instance * ci;
    r = gvec_lw_instance_of(ctx, child, &ci);
    if (r != GVEC_OK) {
      return r;
    }
    GVEC_Svg_Style_Value v;
    bool usable;
    r = gvec_lw_read_property(ctx, ci, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    bool drawn = !gvec_lw_keyword_is(&v, "none") && !gvec_svg_element_conditional_false(child);
    r = gvec_lw_read_property(ctx, ci, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    drawn = drawn && !(gvec_lw_keyword_is(&v, "hidden") || gvec_lw_keyword_is(&v, "collapse"));
    if (drawn) {
      shape = child;
      shape_inst = ci;
      shapes++;
    }
  }
  if (shapes > 1) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clipPath of several shapes needs the union clip, which is not lowered yet");
  }
  if (shapes == 0) {
    *state = GVEC_LW_CLIP_ALL; /* A clip of nothing lets nothing through. */
    return GVEC_OK;
  }
  /* The clip's map: the space, the clipPath's transform, the bounding box when the units say so, the shape's transform. */
  GSHGEOM_Map t, map = *space;
  bool usable;
  r = own_transform(ctx, clip_inst, e, &t, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!gvec_lw_compose(&map, &t, &map)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip's transform leaves the range the waist can carry");
  }
  const GVEC_Svg_Attribute * units = gvec_svg_element_find_attribute(clip_path, "clipPathUnits");
  size_t index;
  const char * word;
  if (units && gvec_svg_attribute_class(units) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_keyword(units, &index, &word) && index == 1) {
    if (!is_shape(gvec_svg_element_kind(e))) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clip in the units of the bounding box of a group is not lowered yet");
    }
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * element_path = NULL;
    r = gvec_lw_centerline_of(ctx, c->inst, &geometry, &element_path, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    GSHGEOM_Bounds box;
    if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
    }
    if (geometry != GVEC_LW_GEOMETRY_PATH || gshgeom_path_bounds(gshscene_resource_path(element_path), &box) != GSHGEOM_OK || box.empty ||
        !(box.max.x > box.min.x) || !(box.max.y > box.min.y)) {
      *state = GVEC_LW_CLIP_ALL; /* A box of no area: the clip cannot be placed, and the element is not rendered. */
      return GVEC_OK;
    }
    GSHGEOM_Map shift, scale, placed;
    if (gshgeom_map_translate(2, box.min.x, box.min.y, 0, &shift) != GSHGEOM_OK ||
        gshgeom_map_scale(2, box.max.x - box.min.x, box.max.y - box.min.y, 1, &scale) != GSHGEOM_OK || !gvec_lw_compose(&shift, &scale, &placed) ||
        !gvec_lw_compose(&map, &placed, &map)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip's map is past the range the waist can carry");
    }
  }
  r = own_transform(ctx, shape_inst, e, &t, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!gvec_lw_compose(&map, &t, &map)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip shape's transform leaves the range the waist can carry");
  }
  /* The shape's own clip-path is a clip in its own space (its transform included). */
  const GVEC_Svg_Element * own_target;
  int own;
  r = clip_reference(ctx, shape_inst, &own, &own_target);
  if (r != GVEC_OK) {
    return r;
  }
  if (own == -1) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clip shape that is clipped by a basic shape is not lowered yet");
  }
  if (own == 1 && gvec_svg_element_kind(own_target) == GVEC_SVG_KIND_CLIP_PATH && !seen_before(c, own_target)) {
    gvec_lw_clip_state inner;
    r = add_clip(c, own_target, &map, depth + 1, &inner);
    if (r != GVEC_OK || inner != GVEC_LW_CLIP_NODE) {
      *state = inner;
      return r;
    }
  }
  /* The shape's geometry, in its own space, and its clip-rule. */
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * path = NULL;
  GVEC_Svg_Kind sk = gvec_svg_element_kind(shape);
  if (sk == GVEC_SVG_KIND_RECT || sk == GVEC_SVG_KIND_CIRCLE || sk == GVEC_SVG_KIND_ELLIPSE) {
    r = gvec_lw_primitive_of(ctx, shape_inst, &geometry, &path, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
  }
  else {
    r = gvec_lw_path_of(ctx, shape, false, &geometry, &path);
    if (r != GVEC_OK) {
      return r;
    }
  }
  if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip shape's geometry is past the range the waist can carry");
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH) {
    *state = GVEC_LW_CLIP_ALL; /* A shape with no area clips everything away. */
    return GVEC_OK;
  }
  GVEC_Svg_Style_Value rule;
  r = gvec_lw_read_property(ctx, shape_inst, GVEC_SVG_PROP_CLIP_RULE, GVEC_LOWER_STYLE, &rule, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  gvec_lw_node * node = gvec_lw_new_node(ctx, GVEC_LW_CLIP);
  if (!node) {
    return GVEC_ERR_OOM;
  }
  node->map = map;
  node->resource = path;
  node->rule = gvec_lw_keyword_is(&rule, "evenodd") ? GSHGEOM_FILL_EVEN_ODD : GSHGEOM_FILL_NONZERO;
  node->key = gvec_lw_key(e->key, ctx->chain ^ (uint64_t) (c->count + 1) * UINT64_C(0x9E3779B97F4A7C15), GVEC_LW_ROLE_CLIP_PATH);
  node->site = gvec_lw_new_site(ctx, e, node->key);
  if (node->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  c->nodes[c->count++] = node;
  *state = GVEC_LW_CLIP_NODE;
  return GVEC_OK;
}

GVEC_Result gvec_lw_clip_begin(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_clip_state * state,
    gvec_lw_node ** out, size_t * count) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *state = GVEC_LW_CLIP_NONE;
  *count = 0;
  int kind;
  const GVEC_Svg_Element * clip_path;
  GVEC_Result r = clip_reference(ctx, inst, &kind, &clip_path);
  if (r != GVEC_OK) {
    return r;
  }
  if (kind == -1) {
    *state = GVEC_LW_CLIP_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clip-path that is a basic shape or names another document is not lowered yet");
  }
  if (kind == 0 || gvec_svg_element_kind(clip_path) != GVEC_SVG_KIND_CLIP_PATH) {
    return GVEC_OK; /* A reference to nothing, or to what is no clipPath, draws the element unclipped (CSS Masking). */
  }
  /* The space is the element's own: its frame and its transform. */
  GSHGEOM_Map t, local;
  bool usable;
  *state = GVEC_LW_CLIP_REFUSED;
  r = own_transform(ctx, inst, e, &t, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!gvec_lw_compose(frame, &t, &local)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's transform leaves the range the waist can carry");
  }
  chain c;
  memset(&c, 0, sizeof c);
  c.ctx = ctx;
  c.element = e;
  c.inst = inst;
  c.nodes = out;
  r = add_clip(&c, clip_path, &local, 0, state);
  *count = c.count;
  return r;
}
