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
 * @file lower_mask.c
 *
 * `mask` naming a `mask` element: a group of the waist whose soft mask is the content of the `mask` element, drawn in the masked
 * element's own space (or in the units of its bounding box) and clipped to the mask's region, read as luminance or as alpha. A `mask`
 * on the `mask` element is a mask of the mask's picture, and one on a child of it a mask of that child; a mask that comes back to
 * itself through those is not applied where it comes back (the style layer has already reported the cycle).
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/mask.h>

#include "lower_internal.h"

static bool keyword_is_box(const GVEC_Svg_Attribute * attribute, bool fallback) {
  size_t index;
  const char * word;
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_keyword(attribute, &index, &word)) {
    return index == 1;
  }
  return fallback;
}

/* One side of the region: the attribute when it is a length, else `fallback` percent. In the units of the box a length is a fraction
 * of the box (a percentage is that over 100); in user units a percentage is of `basis`. */
static bool region_value(const GVEC_Svg_Element * mask, const char * name, bool bounding_box, double basis, double fallback, double * out) {
  GVEC_Svg_Length length = {fallback, GVEC_SVG_UNIT_PERCENT};
  const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(mask, name);
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED) {
    GVEC_Svg_Length given;
    if (gvec_svg_attribute_length(attribute, &given)) {
      length = given;
    }
  }
  if (length.unit == GVEC_SVG_UNIT_PERCENT) {
    *out = bounding_box ? length.value / 100.0 : length.value * basis / 100.0;
    return true;
  }
  return gvec_lw_absolute_length(&length, out);
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

/* The `mask` of `inst`: 0 when it names nothing a mask can be made of (nothing, a reference to what is no `mask`, or one a cycle cut and
 * that is already being made), 1 with the element in `*target`, -1 for what this does not lower. */
static GVEC_Result mask_reference(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, int * kind, const GVEC_Svg_Element ** target) {
  GVEC_Svg_Style_Value v;
  bool usable;
  *kind = 0;
  *target = NULL;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_MASK, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    *kind = usable ? 0 : -1;
    return r;
  }
  for (size_t i = 0; i < v.reference_count; i++) {
    if (!v.references[i].is_url || !v.references[i].local) {
      *kind = -1;
      return GVEC_OK;
    }
  }
  if (v.reference_count > 0) {
    const GVEC_Svg_Style_Reference * ref = &v.references[0];
    const GVEC_Svg_Element * t = ref->target;
    if (!t && ref->cut && ref->id) {
      t = gvec_svg_find_by_id(ctx->document, ref->id);
    }
    if (t && gvec_svg_element_kind(t) == GVEC_SVG_KIND_MASK) {
      *kind = 1;
      *target = t;
    }
  }
  return GVEC_OK;
}

static bool in_stack(const gvec_lw_ctx * ctx, const GVEC_Svg_Element * mask) {
  for (size_t i = 0; i < ctx->mask_depth; i++) {
    if (ctx->mask_stack[i] == mask) {
      return true;
    }
  }
  return false;
}

static GVEC_Result group_of(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const GSHSCENE_Resource * mask, uint64_t salt, gvec_lw_node ** out) {
  gvec_lw_node * g = gvec_lw_new_node(ctx, GVEC_LW_GROUP);
  if (!g) {
    return GVEC_ERR_OOM;
  }
  g->opacity = 1;
  g->mask = mask;
  g->key = gvec_lw_key(e->key, hash2(ctx->chain, salt), GVEC_LW_ROLE_MASK);
  g->site = gvec_lw_new_site(ctx, e, g->key);
  if (g->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  *out = g;
  return GVEC_OK;
}

/* Whether the `mask` properties of the mask elements, followed from `mask`, come back to `mask`: the masks of the loop have no mask of their
 * own (the browsers and `resvg` drop the `mask` of a `mask` that is on such a loop, so that the loop is not applied in either order). */
static GVEC_Result on_loop(gvec_lw_ctx * ctx, const GVEC_Svg_Element * mask, bool * loops) {
  const GVEC_Svg_Element * at = mask;
  *loops = false;
  for (size_t hops = 0; hops <= 2 * GVEC_LW_MASK_NESTING; hops++) {
    const GVEC_Svg_Instance * at_inst;
    GVEC_Result r = gvec_lw_instance_of(ctx, at, &at_inst);
    if (r != GVEC_OK) {
      return r;
    }
    int kind;
    const GVEC_Svg_Element * next;
    r = mask_reference(ctx, at_inst, &kind, &next);
    if (r != GVEC_OK) {
      return r;
    }
    if (kind != 1) {
      return GVEC_OK;
    }
    if (next == mask) {
      *loops = true;
      return GVEC_OK;
    }
    at = next;
  }
  *loops = true; /* A chain longer than the lowering follows is not applied either. */
  return GVEC_OK;
}

/* The mask resource of `mask` for the element `masked` (whose own space is `local`); *state is NODE when `*out` is set. */
static GVEC_Result make_mask(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * masked, const GSHGEOM_Map * local, const GVEC_Svg_Element * mask,
    gvec_lw_mask_state * state, const GSHSCENE_Resource ** out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(masked);
  *state = GVEC_LW_MASK_REFUSED;
  *out = NULL;
  if (ctx->mask_depth >= GVEC_LW_MASK_NESTING || ctx->picture_depth + 1 > ctx->caps.picture_depth || ctx->levels + 4 > ctx->caps.node_depth) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the masks nest deeper than the scene's limits allow");
  }
  const GVEC_Svg_Instance * mask_inst;
  GVEC_Result r = gvec_lw_instance_of(ctx, mask, &mask_inst);
  if (r != GVEC_OK) {
    return r;
  }
  bool units_box = keyword_is_box(gvec_svg_element_find_attribute(mask, "maskUnits"), true);
  bool content_box = keyword_is_box(gvec_svg_element_find_attribute(mask, "maskContentUnits"), false);

  GSHGEOM_Bounds box;
  box.empty = 1;
  if (units_box || content_box) {
    bool known;
    r = gvec_lw_inner_box(ctx, masked, &box, &known);
    if (r != GVEC_OK) {
      return r;
    }
    if (!known) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_MASK, "a mask in the units of the bounding box of an element that holds more than shapes is not lowered yet");
    }
    if (box.empty || !(box.max.x > box.min.x) || !(box.max.y > box.min.y)) {
      *state = GVEC_LW_MASK_ALL; /* The box cannot place the mask: the element is not rendered. */
      return GVEC_OK;
    }
  }
  double bw = box.empty ? 1 : box.max.x - box.min.x;
  double bh = box.empty ? 1 : box.max.y - box.min.y;
  double vw, vh;
  gvec_svg_instance_viewport(masked, &vw, &vh);
  double x, y, w, h;
  if (!region_value(mask, "x", units_box, vw, -10, &x) || !region_value(mask, "y", units_box, vh, -10, &y) ||
      !region_value(mask, "width", units_box, vw, 120, &w) || !region_value(mask, "height", units_box, vh, 120, &h)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_STYLE, "a mask length that needs the font or the window is not lowered");
  }
  if (units_box) {
    x = box.min.x + x * bw;
    y = box.min.y + y * bh;
    w *= bw;
    h *= bh;
  }
  if (!(w > 0) || !(h > 0)) {
    *state = GVEC_LW_MASK_ALL; /* A region of no size lets nothing of the element be seen. */
    return GVEC_OK;
  }
  if (x - x != 0 || y - y != 0 || w - w != 0 || h - h != 0) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask's region is past the range the waist can carry");
  }
  GSHGEOM_Map content = *local;
  if (content_box) {
    GSHGEOM_Map shift, scale, placed;
    if (gshgeom_map_translate(2, box.min.x, box.min.y, 0, &shift) != GSHGEOM_OK || gshgeom_map_scale(2, bw, bh, 1, &scale) != GSHGEOM_OK ||
        !gvec_lw_compose(&shift, &scale, &placed) || !gvec_lw_compose(local, &placed, &content)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask's content scale is past the range the waist can carry");
    }
  }

  /* The mask of the mask: a mask of the picture's content. */
  int nested_kind;
  const GVEC_Svg_Element * nested_target;
  r = mask_reference(ctx, mask_inst, &nested_kind, &nested_target);
  if (r != GVEC_OK) {
    return r;
  }
  if (nested_kind == -1) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_MASK, "a mask that names another document is not lowered");
  }
  const GSHSCENE_Resource * nested = NULL;
  bool loops = false;
  if (nested_kind == 1) {
    r = on_loop(ctx, mask, &loops);
    if (r != GVEC_OK) {
      return r;
    }
  }
  if (nested_kind == 1 && !loops && !in_stack(ctx, nested_target) && nested_target != mask) {
    ctx->mask_stack[ctx->mask_depth++] = mask;
    gvec_lw_mask_state inner;
    r = make_mask(ctx, masked, local, nested_target, &inner, &nested);
    ctx->mask_depth--;
    if (r != GVEC_OK || inner != GVEC_LW_MASK_NODE) {
      *state = inner;
      return r;
    }
  }

  /* The picture: the region's clip around the content, in a hidden holder, under the nested mask's group when there is one. */
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * rect = NULL;
  r = gvec_lw_rect_path(ctx, x, y, w, h, &geometry, &rect);
  if (r != GVEC_OK) {
    return r;
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask's region is past the range the waist can carry");
  }
  gvec_lw_node * holder = gvec_lw_new_node(ctx, GVEC_LW_USE);
  gvec_lw_node * clip = gvec_lw_new_node(ctx, GVEC_LW_CLIP);
  if (!holder || !clip) {
    return GVEC_ERR_OOM;
  }
  holder->key = gvec_lw_key(mask->key, ctx->chain, GVEC_LW_ROLE_MASK_CONTAINER);
  holder->site = gvec_lw_new_site(ctx, e, holder->key); /* A limit reached recording the picture refuses the element that is masked. */
  clip->map = *local;
  clip->resource = rect;
  clip->rule = GSHGEOM_FILL_NONZERO;
  clip->key = gvec_lw_key(mask->key, ctx->chain, GVEC_LW_ROLE_MASK_REGION);
  clip->site = gvec_lw_new_site(ctx, e, clip->key);
  if (holder->site == GVEC_LW_NO_SITE || clip->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  gvec_lw_node * outer = clip;
  if (nested) {
    r = group_of(ctx, e, nested, mask->key, &outer);
    if (r != GVEC_OK) {
      return r;
    }
  }

  uint64_t saved_chain = ctx->chain;
  size_t saved_open = ctx->open;
  size_t saved_levels = ctx->levels;
  bool saved_set = ctx->context_set;
  const GVEC_Svg_Instance * saved_context_inst = ctx->context_inst;
  GSHGEOM_Map saved_item_local = ctx->item_local;
  bool saved_item_crisp = ctx->item_crisp;
  ctx->levels += 3;
  ctx->picture_depth++;
  ctx->open = 1;
  ctx->use_depth++;
  ctx->chain = hash2(saved_chain, mask->key);
  ctx->context_set = false;
  ctx->context_inst = NULL;
  ctx->mask_stack[ctx->mask_depth++] = mask;
  r = gvec_lw_children(ctx, mask_inst, &content, clip);
  ctx->mask_depth--;
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
  if (!clip->content) {
    *state = GVEC_LW_MASK_ALL; /* A mask with nothing in it hides the element. */
    return GVEC_OK;
  }
  if (outer != clip) {
    r = gvec_lw_attach(ctx, outer, clip);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, holder, outer);
    }
  }
  else {
    r = gvec_lw_attach(ctx, holder, clip);
  }
  if (r != GVEC_OK) {
    return r;
  }
  const GSHSCENE_Resource * picture = NULL;
  r = gvec_lw_emit_picture(ctx, holder, &picture);
  if (r != GVEC_OK) {
    return r;
  }
  if (!picture) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask's picture is past what the waist can carry");
  }

  GVEC_Svg_Style_Value v;
  bool usable;
  GSHSCENE_Mask m;
  memset(&m, 0, sizeof m);
  m.picture = picture;
  r = gvec_lw_read_property(ctx, mask_inst, GVEC_SVG_PROP_MASK_TYPE, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    gshscene_resource_release(picture);
    return r;
  }
  m.mode = gvec_lw_keyword_is(&v, "alpha") ? GSHSCENE_MASK_ALPHA : GSHSCENE_MASK_LUMINANCE;
  if (m.mode == GSHSCENE_MASK_LUMINANCE) {
    r = gvec_lw_read_property(ctx, mask_inst, GVEC_SVG_PROP_COLOR_INTERPOLATION, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      gshscene_resource_release(picture);
      return r;
    }
    if (gvec_lw_keyword_is(&v, "linearrgb") || gvec_lw_keyword_is(&v, "linearRGB")) {
      m.space.primaries = GCOL_PRIMARIES_SRGB; /* The luminance is of the linear values; the picture is still blended as it is. */
      m.space.transfer = GCOL_TRANSFER_LINEAR;
    }
  }
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result sr = gshscene_store_intern_mask(ctx->store, &m, &resource);
  gshscene_resource_release(picture);
  if (sr == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (sr != GSHSCENE_OK) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask is past what the waist can carry");
  }
  r = gvec_lw_hold(ctx, resource);
  if (r != GVEC_OK) {
    return r;
  }
  *out = resource;
  *state = GVEC_LW_MASK_NODE;
  return GVEC_OK;
}

GVEC_Result gvec_lw_mask_begin(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_mask_state * state,
    gvec_lw_node ** out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *state = GVEC_LW_MASK_NONE;
  *out = NULL;
  int kind;
  const GVEC_Svg_Element * mask;
  GVEC_Result r = mask_reference(ctx, inst, &kind, &mask);
  if (r != GVEC_OK) {
    *state = GVEC_LW_MASK_REFUSED;
    return r;
  }
  if (kind == -1) {
    *state = GVEC_LW_MASK_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_MASK, "a mask that names another document is not lowered");
  }
  if (kind == 0 || in_stack(ctx, mask)) {
    return GVEC_OK; /* A reference to nothing, or one that comes back to a mask being made, draws the element unmasked. */
  }
  /* The space is the element's own: its frame and its transform. */
  GVEC_Svg_Style_Value v;
  bool usable;
  *state = GVEC_LW_MASK_REFUSED;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (v.transform.depends_on_box) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TRANSFORM_BOX, "a transform with percentages of a bounding box is not lowered yet");
  }
  GSHGEOM_Map local;
  if (!gvec_lw_compose(frame, &v.transform.map, &local)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's transform leaves the range the waist can carry");
  }
  const GSHSCENE_Resource * resource = NULL;
  r = make_mask(ctx, inst, &local, mask, state, &resource);
  if (r != GVEC_OK || *state != GVEC_LW_MASK_NODE) {
    return r;
  }
  return group_of(ctx, e, resource, 0, out);
}
