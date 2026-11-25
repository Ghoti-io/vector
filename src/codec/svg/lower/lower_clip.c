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
 * mask, and a clip that is itself clipped (on the clipPath or on its shape) a nested node, whose region is the intersection. Several
 * shapes are one union clip of the waist, each with its own transform and clip-rule; a `use` child that names a shape is that shape,
 * placed by the use's transform and `x` and `y`, and one that names anything else adds nothing; a clip-path on one shape of several
 * is the member's inner clips (the clip's region as a union of intersections, a member for each term). A basic shape (`circle()` and the others) is lowered by lower_basic_shape.c.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/elementary.h>
#include <ghoti.io/shape-geometry/arc.h>
#include <ghoti.io/shape-geometry/flatten.h>
#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-geometry/stroke.h>
#include <ghoti.io/shape-scene/clip_union.h>
#include <ghoti.io/shape-scene/glyph.h>
#include <ghoti.io/shape-scene/stroke.h>

#include "lower_internal.h"

static bool is_shape(GVEC_Svg_Kind kind) {
  return kind == GVEC_SVG_KIND_PATH || kind == GVEC_SVG_KIND_POLYGON || kind == GVEC_SVG_KIND_POLYLINE || kind == GVEC_SVG_KIND_LINE ||
      kind == GVEC_SVG_KIND_RECT || kind == GVEC_SVG_KIND_CIRCLE || kind == GVEC_SVG_KIND_ELLIPSE;
}

/* The map of an element's own `transform`; false (the element refused) when it depends on a box this lowering cannot make. */
static GVEC_Result own_transform(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Element * refused, GSHGEOM_Map * out,
    bool * usable) {
  GVEC_Svg_Style_Value v;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  if (v.transform.depends_on_box) {
    bool known;
    r = gvec_lw_box_transform(ctx, inst, &v.transform, out, &known);
    if (r != GVEC_OK || !known) {
      *usable = false;
      return r != GVEC_OK ? r : gvec_lw_refuse(ctx, refused, GVEC_LOWER_TRANSFORM_BOX, "a transform of a bounding box of an element that holds what the lowering does not box");
    }
    return GVEC_OK;
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
  struct region_memo * memo; ///< The regions worked out so far (a clipPath of several shapes would otherwise be worked out again for each).
  size_t memo_count;
  bool used_subject; ///< Set when a clip in the units of the box read the box of the subject: what was worked out then depends on the subject.
} chain;

static bool seen_before(const chain * c, const GVEC_Svg_Element * clip_path) {
  for (size_t i = 0; i < c->seen_count; i++) {
    if (c->seen[i] == clip_path) {
      return true;
    }
  }
  return false;
}

/* Whether a child of a clipPath is a shape that contributes: `display` is not none, it is not conditional-false and `visibility` is
 * not hidden or collapse. */
static GVEC_Result inst_drawn(gvec_lw_ctx * ctx, const GVEC_Svg_Element * child, const GVEC_Svg_Instance * inst, bool * drawn) {
  GVEC_Svg_Style_Value v;
  bool usable;
  *drawn = false;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  bool shown = !gvec_lw_keyword_is(&v, "none") && !gvec_svg_element_conditional_false(child);
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  *drawn = shown && !(gvec_lw_keyword_is(&v, "hidden") || gvec_lw_keyword_is(&v, "collapse"));
  return GVEC_OK;
}

static GVEC_Result child_drawn(gvec_lw_ctx * ctx, const GVEC_Svg_Element * child, const GVEC_Svg_Instance ** inst, bool * drawn) {
  GVEC_Result r = gvec_lw_instance_of(ctx, child, inst);
  *drawn = false;
  if (r != GVEC_OK) {
    return r;
  }
  return inst_drawn(ctx, child, *inst, drawn);
}

/* A shape that contributes to a clip: a child that is one, or the shape a `use` child names. `pre` places a `use` (its `transform`
 * and its `x` and `y`) and is the identity for a shape child; the shape's own `transform` comes after it. */
typedef struct clip_child {
  const GVEC_Svg_Element * shape;
  const GVEC_Svg_Instance * inst;
  GSHGEOM_Map pre;
  bool preset;                      ///< A piece of text: its region is `resource` under `pre`, by `rule`, and nothing is read from `shape`.
  const GSHSCENE_Resource * resource;
  GSHGEOM_Fill_Rule rule;
  const GVEC_Svg_Instance * use_inst; ///< The `use` child whose own `clip-path` (a clipPath or a basic shape) clips it, in the use's space (its transform, not its x and y), or NULL.
  GSHGEOM_Map use_space;             ///< The use's own transform.
  const GVEC_Svg_Instance * own_inst; ///< For a piece of text: the text element, whose own `clip-path` clips the pieces in `own_space` (the element's space); NULL when it has none.
  GSHGEOM_Map own_space;
} clip_child;

/* Room for one more entry of the list, which grows as the text in the clip brings pieces. */
static GVEC_Result grow_list(gvec_lw_ctx * ctx, clip_child ** list, size_t * capacity, size_t count) {
  if (count < *capacity) {
    return GVEC_OK;
  }
  size_t grown = *capacity ? *capacity * 2 : 8;
  clip_child * more = gvec_arena_alloc(&ctx->arena, grown * sizeof *more, sizeof(double));
  if (!more) {
    return GVEC_ERR_OOM;
  }
  if (count) {
    memcpy(more, *list, count * sizeof *more);
  }
  *list = more;
  *capacity = grown;
  return GVEC_OK;
}

/* The shapes of a clipPath that are drawn, in document order. A `use` child names its shape (SVG 2, 14.3.5): one that names anything
 * else (a group, a symbol, nothing) is not a valid child and adds nothing. Text is its glyphs. *refused is set when the element is. */
/* A clipPath child that is text, or a `use` of one: its glyphs' outlines (and its decorations' lines), each piece with the clip-rule of the
 * span it is of, placed by `outer` (the use's placement, or the identity) and the text's transform. The text's own `clip-path` clips the
 * pieces in the text's space; a `use`'s is `use_inst`, in `use_space`. */
static GVEC_Result add_text(chain * c, const GVEC_Svg_Element * child, const GVEC_Svg_Instance * ti, const GSHGEOM_Map * outer,
    const GVEC_Svg_Instance * use_inst, const GSHGEOM_Map * use_space, clip_child ** list, size_t * capacity, size_t * count, clip_child ** out,
    bool * refused) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  const GVEC_Svg_Element * own_target;
  int own;
  GVEC_Result r = clip_reference(ctx, ti, &own, &own_target);
  if (r != GVEC_OK) {
    return r;
  }
  const bool has_own = own == -1 || (own == 1 && gvec_svg_element_kind(own_target) == GVEC_SVG_KIND_CLIP_PATH);
  GSHGEOM_Map text_map, placed_text;
  bool usable;
  r = own_transform(ctx, ti, e, &text_map, &usable);
  if (r != GVEC_OK || !usable) {
    *refused = !usable;
    return r;
  }
  if (!gvec_lw_compose(outer, &text_map, &placed_text)) {
    *refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text in a clipPath is placed past the range the waist can carry");
  }
  gvec_lw_text laid;
  r = gvec_lw_text_layout(ctx, ti, &laid, &usable);
  if (r != GVEC_OK || !usable) {
    *refused = !usable;
    return r;
  }
  for (size_t k = 0; k < laid.count; k++) {
    const gvec_lw_text_piece * piece = &laid.pieces[k];
    const GSHSCENE_Resource * region = piece->run;
    if (!piece->decoration) {
      GSHGEOM_Path * outlines = NULL;
      GSHSCENE_Result gr = gshscene_glyph_run_centerline(piece->run, NULL, NULL, &outlines, NULL);
      if (gr == GSHSCENE_ERR_OOM) {
        return GVEC_ERR_OOM;
      }
      if (gr != GSHSCENE_OK) {
        *refused = true;
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "the outlines of the text in a clipPath could not be made");
      }
      GSHSCENE_Result ir = gshscene_store_intern_path(ctx->store, outlines, &region);
      gshgeom_path_destroy(outlines);
      if (ir == GSHSCENE_ERR_OOM) {
        return GVEC_ERR_OOM;
      }
      if (ir != GSHSCENE_OK) {
        *refused = true;
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the outlines of the text in a clipPath are past what the waist can carry");
      }
      r = gvec_lw_hold(ctx, region);
      if (r != GVEC_OK) {
        return r;
      }
    }
    GVEC_Svg_Style_Value rv;
    r = gvec_lw_read_property(ctx, piece->inst, GVEC_SVG_PROP_CLIP_RULE, GVEC_LOWER_STYLE, &rv, &usable);
    if (r != GVEC_OK || !usable) {
      *refused = !usable;
      return r;
    }
    r = grow_list(ctx, list, capacity, *count);
    if (r != GVEC_OK) {
      return r;
    }
    *out = *list;
    clip_child * member = &(*list)[(*count)++];
    member->shape = child;
    member->inst = ti;
    member->preset = true;
    member->use_inst = use_inst;
    member->use_space = *use_space;
    member->own_inst = has_own ? ti : NULL;
    member->own_space = placed_text;
    member->resource = region;
    member->rule = GSHGEOM_FILL_NONZERO;
    if (gvec_lw_keyword_is(&rv, "evenodd")) {
      member->rule = GSHGEOM_FILL_EVEN_ODD;
    }
    if (!gvec_lw_compose(&placed_text, &piece->map, &member->pre)) {
      *refused = true;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text in a clipPath is placed past the range the waist can carry");
    }
  }
  return GVEC_OK;
}

/* Whether `inst`'s own `clip-path` clips it: a clipPath it names, or a basic shape (a reference to what is no clipPath, or one a cycle cut, does not). */
static GVEC_Result has_clip(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, bool * has) {
  const GVEC_Svg_Element * target;
  int kind;
  GVEC_Result r = clip_reference(ctx, inst, &kind, &target);
  *has = r == GVEC_OK && (kind == -1 || (kind == 1 && gvec_svg_element_kind(target) == GVEC_SVG_KIND_CLIP_PATH));
  return r;
}

static GVEC_Result gather_children(chain * c, const GVEC_Svg_Element * clip_path, clip_child ** out, size_t * count, bool * refused) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  size_t n = 0;
  for (const GVEC_Svg_Element * child = gvec_svg_element_first_child(clip_path); child; child = gvec_svg_element_next_sibling(child)) {
    n++;
  }
  clip_child * list = gvec_arena_alloc(&ctx->arena, (n ? n : 1) * sizeof *list, sizeof(double));
  size_t capacity = n ? n : 1;
  if (!list) {
    return GVEC_ERR_OOM;
  }
  *out = list;
  *count = 0;
  *refused = false;
  GSHGEOM_Map identity;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  for (const GVEC_Svg_Element * child = gvec_svg_element_first_child(clip_path); child; child = gvec_svg_element_next_sibling(child)) {
    GVEC_Svg_Kind ck = gvec_svg_element_kind(child);
    const GVEC_Svg_Instance * ci;
    bool drawn;
    GVEC_Result r;
    if (ck == GVEC_SVG_KIND_TEXT) {
      r = child_drawn(ctx, child, &ci, &drawn);
      if (r != GVEC_OK) {
        return r;
      }
      if (!drawn) {
        continue;
      }
      r = add_text(c, child, ci, &identity, NULL, &identity, &list, &capacity, count, out, refused);
      if (r != GVEC_OK || *refused) {
        return r;
      }
      list = *out;
      continue;
    }
    if (ck == GVEC_SVG_KIND_USE) {
      r = child_drawn(ctx, child, &ci, &drawn);
      if (r != GVEC_OK) {
        return r;
      }
      if (!drawn) {
        continue;
      }
      const GVEC_Svg_Element * target = gvec_svg_element_href_target(child);
      if (!target) {
        continue; /* A reference to nothing, or on a cycle (reported when the document was loaded), names no shape. */
      }
      GVEC_Svg_Kind tk = gvec_svg_element_kind(target);
      if (tk != GVEC_SVG_KIND_TEXT && !is_shape(tk)) {
        continue;
      }
      const GVEC_Svg_Instance * clone;
      r = gvec_svg_instance_use(ctx->style, ci, &clone);
      if (r == GVEC_ERR_LIMIT) {
        *refused = true;
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_EXPANSION_LIMIT, "the use instantiates more elements than the limit allows");
      }
      if (r != GVEC_OK) {
        return r;
      }
      r = inst_drawn(ctx, target, clone, &drawn);
      if (r != GVEC_OK) {
        return r;
      }
      if (!drawn) {
        continue;
      }
      bool use_clipped;
      r = has_clip(ctx, ci, &use_clipped);
      if (r != GVEC_OK) {
        return r;
      }
      GSHGEOM_Map t, shift, placed;
      bool usable;
      r = own_transform(ctx, ci, e, &t, &usable);
      if (r != GVEC_OK || !usable) {
        *refused = !usable;
        return r;
      }
      double x = 0, y = 0;
      GVEC_Svg_Style_Value v;
      r = gvec_lw_read_property(ctx, ci, GVEC_SVG_PROP_X, GVEC_LOWER_STYLE, &v, &usable);
      if (r != GVEC_OK || !usable) {
        *refused = !usable;
        return r;
      }
      x = v.kind == GVEC_SVG_STYLE_LENGTH && !v.length_auto ? v.number : 0;
      r = gvec_lw_read_property(ctx, ci, GVEC_SVG_PROP_Y, GVEC_LOWER_STYLE, &v, &usable);
      if (r != GVEC_OK || !usable) {
        *refused = !usable;
        return r;
      }
      y = v.kind == GVEC_SVG_STYLE_LENGTH && !v.length_auto ? v.number : 0;
      if (gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&t, &shift, &placed)) {
        *refused = true;
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the use is placed past the range the waist can carry");
      }
      if (tk == GVEC_SVG_KIND_TEXT) {
        r = add_text(c, child, clone, &placed, use_clipped ? ci : NULL, &t, &list, &capacity, count, out, refused);
        if (r != GVEC_OK || *refused) {
          return r;
        }
        list = *out;
        continue;
      }
      r = grow_list(ctx, &list, &capacity, *count);
      if (r != GVEC_OK) {
        return r;
      }
      *out = list;
      list[*count].shape = target;
      list[*count].inst = clone;
      list[*count].pre = placed;
      list[*count].preset = false;
      list[*count].use_inst = use_clipped ? ci : NULL;
      list[*count].use_space = t;
      list[*count].own_inst = NULL;
      (*count)++;
      continue;
    }
    if (!is_shape(ck)) {
      continue;
    }
    r = child_drawn(ctx, child, &ci, &drawn);
    if (r != GVEC_OK) {
      return r;
    }
    if (!drawn) {
      continue;
    }
    r = grow_list(ctx, &list, &capacity, *count);
    if (r != GVEC_OK) {
      return r;
    }
    *out = list;
    list[*count].shape = child;
    list[*count].inst = ci;
    list[*count].preset = false;
    list[*count].use_inst = NULL;
    list[*count].own_inst = NULL;
    if (gshgeom_map_identity(2, &list[*count].pre) != GSHGEOM_OK || gshgeom_map_identity(2, &list[*count].use_space) != GSHGEOM_OK) {
      return GVEC_ERR_INTERNAL;
    }
    (*count)++;
  }
  return GVEC_OK;
}

/* The geometry of a clip shape in its own space, and its clip-rule. *geometry is not PATH when it has no area; a refusal is returned
 * through the refuse call and *geometry stays LIMIT. */
static GVEC_Result region_of(chain * c, const GVEC_Svg_Element * shape, const GVEC_Svg_Instance * shape_inst, gvec_lw_geometry * geometry,
    const GSHSCENE_Resource ** path, GSHGEOM_Fill_Rule * rule) {
  gvec_lw_ctx * ctx = c->ctx;
  bool usable;
  GVEC_Result r;
  GVEC_Svg_Kind sk = gvec_svg_element_kind(shape);
  *path = NULL;
  *geometry = GVEC_LW_GEOMETRY_LIMIT;
  if (sk == GVEC_SVG_KIND_RECT || sk == GVEC_SVG_KIND_CIRCLE || sk == GVEC_SVG_KIND_ELLIPSE) {
    r = gvec_lw_primitive_of(ctx, shape_inst, geometry, path, &usable);
    if (r != GVEC_OK || !usable) {
      *geometry = GVEC_LW_GEOMETRY_LIMIT;
      return r;
    }
  }
  else {
    r = gvec_lw_path_of(ctx, shape, shape_inst, false, geometry, path);
    if (r != GVEC_OK) {
      return r;
    }
  }
  if (*geometry == GVEC_LW_GEOMETRY_LIMIT) {
    return gvec_lw_refuse(ctx, c->element, GVEC_LOWER_LIMIT, "the clip shape's geometry is past the range the waist can carry");
  }
  GVEC_Svg_Style_Value v;
  r = gvec_lw_read_property(ctx, shape_inst, GVEC_SVG_PROP_CLIP_RULE, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    *geometry = GVEC_LW_GEOMETRY_LIMIT;
    return r;
  }
  *rule = gvec_lw_keyword_is(&v, "evenodd") ? GSHGEOM_FILL_EVEN_ODD : GSHGEOM_FILL_NONZERO;
  return GVEC_OK;
}

/* One clip node for a path or a union, appended to the chain. */
static GVEC_Result push_node(chain * c, const GSHSCENE_Resource * resource, const GSHGEOM_Map * map, GSHGEOM_Fill_Rule rule,
    gvec_lw_clip_state * state) {
  gvec_lw_ctx * ctx = c->ctx;
  gvec_lw_node * node = gvec_lw_new_node(ctx, GVEC_LW_CLIP);
  if (!node) {
    return GVEC_ERR_OOM;
  }
  node->map = *map;
  node->resource = resource;
  node->rule = rule;
  node->key = gvec_lw_key(c->element->key, ctx->chain ^ (uint64_t) (c->count + 1) * UINT64_C(0x9E3779B97F4A7C15), GVEC_LW_ROLE_CLIP_PATH);
  node->site = gvec_lw_new_site(ctx, c->element, node->key);
  if (node->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  c->nodes[c->count++] = node;
  *state = GVEC_LW_CLIP_NODE;
  return GVEC_OK;
}

/* The region of a clipPath as a union of intersections: each term is the inner clips (in the union's space) a member is cut by, a term
 * of no clips being the whole plane and no term being the empty region.  A clip is the intersection of its own clip-path's region and
 * the union over its shapes of the shape cut by the shape's own clip-path, so that distributing the intersection over the unions gives
 * a union of terms, each an intersection of single paths, which is what a member with inner clips says. */
typedef struct clip_term {
  GSHSCENE_Clip_Inner * clips;
  size_t count;
} clip_term;

typedef struct clip_dnf {
  clip_term * terms;
  size_t count, capacity;
} clip_dnf;

/* The most terms of one region, and of clips in them, before the lowering refuses: a product of unions grows as the product of their sizes. */
#define GVEC_LW_DNF_TERMS 4096u

static GVEC_Result dnf_push(chain * c, clip_dnf * d, clip_term term, bool * refused) {
  if (d->count >= GVEC_LW_DNF_TERMS) {
    *refused = true;
    return gvec_lw_refuse(c->ctx, c->element, GVEC_LOWER_LIMIT, "a clip that clips its own shapes is past what the waist's union holds");
  }
  if (d->count == d->capacity) {
    size_t grown = d->capacity ? d->capacity * 2 : 4;
    clip_term * more = gvec_arena_alloc(&c->ctx->arena, grown * sizeof *more, sizeof(double));
    if (!more) {
      return GVEC_ERR_OOM;
    }
    if (d->count) {
      memcpy(more, d->terms, d->count * sizeof *more);
    }
    d->terms = more;
    d->capacity = grown;
  }
  d->terms[d->count++] = term;
  return GVEC_OK;
}

/* a and b intersected: every term of one with every term of the other. */
static GVEC_Result dnf_product(chain * c, const clip_dnf * a, const clip_dnf * b, clip_dnf * out, bool * refused) {
  for (size_t i = 0; i < a->count; i++) {
    for (size_t j = 0; j < b->count; j++) {
      clip_term t;
      t.count = a->terms[i].count + b->terms[j].count;
      t.clips = NULL;
      if (t.count) {
        t.clips = gvec_arena_alloc(&c->ctx->arena, t.count * sizeof *t.clips, sizeof(double));
        if (!t.clips) {
          return GVEC_ERR_OOM;
        }
        if (a->terms[i].count) {
          memcpy(t.clips, a->terms[i].clips, a->terms[i].count * sizeof *t.clips);
        }
        if (b->terms[j].count) {
          memcpy(t.clips + a->terms[i].count, b->terms[j].clips, b->terms[j].count * sizeof *t.clips);
        }
      }
      GVEC_Result r = dnf_push(c, out, t, refused);
      if (r != GVEC_OK || *refused) {
        return r;
      }
    }
  }
  return GVEC_OK;
}

static GVEC_Result group_box(gvec_lw_ctx * ctx, const GVEC_Svg_Element * refused, const GVEC_Svg_Element * group, const GVEC_Svg_Instance * ginst,
    const GSHGEOM_Map * map, GSHGEOM_Bounds * box, bool * known, int depth, const GVEC_Svg_Element * only);
static GVEC_Result shape_text(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const char ** text);

static GVEC_Result clip_terms(chain * c, const GVEC_Svg_Element * clip_path, const GSHGEOM_Map * space, size_t depth, const GVEC_Svg_Instance * subject,
    clip_dnf * out, bool * refused);

/* Whether the clipPath's `clipPathUnits` is `objectBoundingBox`. */
static bool boxed_units(const GVEC_Svg_Element * clip_path) {
  const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(clip_path, "clipPathUnits");
  size_t which;
  const char * name;
  if (!attribute || gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_TYPED) {
    return false;
  }
  return gvec_svg_attribute_keyword(attribute, &which, &name) && which == 1;
}

/* The bounding box of `subject` (the element a clip is applied to), for a clip in the units of the box. *all is set when the box has no area
 * (the clip cannot be placed and the element is not rendered); *refused when the element is. */
static GVEC_Result subject_box(chain * c, const GVEC_Svg_Instance * subject, GSHGEOM_Bounds * box, bool * all, bool * refused) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  const GVEC_Svg_Element * se = gvec_svg_instance_element(subject);
  c->used_subject = true;
  *all = false;
  *refused = true;
  GVEC_Result r;
  if (!is_shape(gvec_svg_element_kind(se))) {
    GSHGEOM_Map identity;
    bool known = true;
    box->empty = 1;
    if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
      return GVEC_ERR_INTERNAL;
    }
    if (gvec_svg_element_kind(se) == GVEC_SVG_KIND_IMAGE) {
      r = gvec_lw_image_leaf_box(ctx, subject, &identity, box, &known); /* an image's box is its viewport */
    }
    else {
      r = group_box(ctx, e, se, subject, &identity, box, &known, 0, NULL);
    }
    if (r != GVEC_OK) {
      return r;
    }
    if (!known) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clip in the units of the bounding box of a group that holds more than shapes is not lowered yet");
    }
    if (box->empty || !(box->max.x > box->min.x) || !(box->max.y > box->min.y)) {
      *all = true;
      *refused = false;
      return GVEC_OK;
    }
  }
  else {
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * element_path = NULL;
    bool usable;
    r = gvec_lw_centerline_of(ctx, subject, &geometry, &element_path, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
    }
    if (geometry != GVEC_LW_GEOMETRY_PATH || gshgeom_path_bounds(gshscene_resource_path(element_path), box) != GSHGEOM_OK || box->empty ||
        !(box->max.x > box->min.x) || !(box->max.y > box->min.y)) {
      *all = true; /* A box of no area: the clip cannot be placed, and the element is not rendered. */
      *refused = false;
      return GVEC_OK;
    }
  }
  *refused = false;
  return GVEC_OK;
}

/* `map` placed on the unit square of `box`. False when it leaves the range the waist can carry. */
static bool place_on_box(const GSHGEOM_Map * map, const GSHGEOM_Bounds * box, GSHGEOM_Map * out) {
  GSHGEOM_Map shift, scale, placed;
  return gshgeom_map_translate(2, box->min.x, box->min.y, 0, &shift) == GSHGEOM_OK &&
      gshgeom_map_scale(2, box->max.x - box->min.x, box->max.y - box->min.y, 1, &scale) == GSHGEOM_OK && gvec_lw_compose(&shift, &scale, &placed) &&
      gvec_lw_compose(map, &placed, out);
}

/* The region of the `clip-path` of `inst` that is a basic shape, placed in `space` (the element's, its transform included) and measured against the
 * reference box of `box_inst` (`inst` itself, or for a clipPath's own clip-path the element the clipPath is applied to), as terms: one
 * term of the shape, the whole plane (one term of no clips) when the property is as if not specified, no term when nothing gets through. */
static GVEC_Result basic_terms(chain * c, const GVEC_Svg_Instance * inst, const GVEC_Svg_Instance * box_inst, const GSHGEOM_Map * space, clip_dnf * out,
    bool * refused) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  const GVEC_Svg_Element * ie = gvec_svg_instance_element(inst);
  const char * text;
  GVEC_Result r = shape_text(ctx, inst, &text);
  if (r != GVEC_OK) {
    *refused = true;
    return r;
  }
  if (!text) {
    *refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clip-path that names an element the resolver does not supply is not lowered");
  }
  gvec_lw_shape shape;
  r = gvec_lw_basic_shape(ctx, box_inst, text, &shape);
  if (r != GVEC_OK) {
    *refused = true;
    return r;
  }
  clip_term whole = {NULL, 0};
  switch (shape.kind) {
  case GVEC_LW_SHAPE_REFUSED:
    *refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, shape.why);
  case GVEC_LW_SHAPE_ERROR:
    *refused = true;
    return GVEC_OK; /* The element was refused where the reason arose. */
  case GVEC_LW_SHAPE_INVALID:
    r = gvec_diag_add(ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_STYLE_INVALID, 0, ie->node.line, ie->node.column, ie->node.offset, ie->name,
        "a clip-path that is not a shape or a reference is as if it were not specified");
    if (r != GVEC_OK) {
      return r;
    }
    return dnf_push(c, out, whole, refused);
  case GVEC_LW_SHAPE_NONE:
    return dnf_push(c, out, whole, refused);
  case GVEC_LW_SHAPE_ALL:
    return GVEC_OK;
  case GVEC_LW_SHAPE_PATH:
    break;
  }
  GSHSCENE_Clip_Inner * one = gvec_arena_alloc(&ctx->arena, sizeof *one, sizeof(double));
  GSHGEOM_Map moved;
  if (!one) {
    return GVEC_ERR_OOM;
  }
  if (gshgeom_map_translate(2, shape.ox, shape.oy, 0, &moved) != GSHGEOM_OK || !gvec_lw_compose(space, &moved, &one->map)) {
    *refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip shape's transform leaves the range the waist can carry");
  }
  one->resource = shape.path;
  one->rule = shape.rule;
  clip_term term = {one, 1};
  return dnf_push(c, out, term, refused);
}

/* A region that was worked out: for a clipPath, in a space, for a subject, at a depth, under the clipPaths that were open (the cycle check and the
 * depth limit read them), so that a clipPath that several shapes of a union are each cut by is worked out once, not once for each shape (a chain of
 * unions of six shapes did the work six times at every level before it reached the limit on the terms, which took gigabytes). The terms are read
 * and never changed, so they are shared. Only a region that was not refused is kept. */
#define GVEC_LW_MEMO_MAX 32
typedef struct region_memo {
  const GVEC_Svg_Element * clip_path;
  const GVEC_Svg_Instance * subject;
  size_t depth, seen_count;
  const GVEC_Svg_Element * seen[GVEC_LW_CLIP_MAX + 1];
  GSHGEOM_Map space;
  bool uses_subject; ///< Whether the region depends on the subject (a clip in the units of a box in it): if not, any subject will do.
  clip_dnf region;
} region_memo;

static const region_memo * memo_find(const chain * c, const GVEC_Svg_Element * clip_path, const GSHGEOM_Map * space, size_t depth,
    const GVEC_Svg_Instance * subject) {
  for (size_t i = 0; i < c->memo_count; i++) {
    const region_memo * m = &c->memo[i];
    if (m->clip_path == clip_path && (!m->uses_subject || m->subject == subject) && m->depth == depth && m->seen_count == c->seen_count &&
        memcmp(m->seen, c->seen, c->seen_count * sizeof m->seen[0]) == 0 && memcmp(&m->space, space, sizeof *space) == 0) {
      return m;
    }
  }
  return NULL;
}

static GVEC_Result memo_keep(chain * c, const GVEC_Svg_Element * clip_path, const GSHGEOM_Map * space, size_t depth, const GVEC_Svg_Instance * subject,
    const clip_dnf * region, bool uses_subject) {
  if (c->memo_count >= GVEC_LW_MEMO_MAX) {
    return GVEC_OK; /* full: later regions are worked out each time, as they always were */
  }
  if (!c->memo) {
    c->memo = gvec_arena_alloc(&c->ctx->arena, GVEC_LW_MEMO_MAX * sizeof *c->memo, sizeof(double));
    if (!c->memo) {
      return GVEC_ERR_OOM;
    }
  }
  region_memo * m = &c->memo[c->memo_count++];
  m->clip_path = clip_path;
  m->subject = subject;
  m->depth = depth;
  m->seen_count = c->seen_count;
  memcpy(m->seen, c->seen, c->seen_count * sizeof m->seen[0]);
  m->space = *space;
  m->uses_subject = uses_subject;
  m->region = *region;
  return GVEC_OK;
}

/* The region of the `clip-path` of `inst`, in `space` (the element's, its transform included): a clipPath it names, or a basic shape. *has is
 * false when it has none (a reference to nothing, to what is no clipPath, or one that closes a cycle of clips). `subject` is the element
 * that the clip's units refer to. */
static GVEC_Result inst_clip_dnf(chain * c, const GVEC_Svg_Instance * inst, const GVEC_Svg_Instance * box_inst, const GSHGEOM_Map * space, size_t depth,
    const GVEC_Svg_Instance * subject, clip_dnf * out, bool * has, bool * refused) {
  const GVEC_Svg_Element * target;
  int kind;
  out->terms = NULL;
  out->count = out->capacity = 0;
  *has = false;
  GVEC_Result r = clip_reference(c->ctx, inst, &kind, &target);
  if (r != GVEC_OK) {
    return r;
  }
  if (kind == -1) {
    *has = true;
    return basic_terms(c, inst, box_inst, space, out, refused);
  }
  if (kind == 1 && gvec_svg_element_kind(target) == GVEC_SVG_KIND_CLIP_PATH && !seen_before(c, target)) {
    *has = true;
    const region_memo * known = memo_find(c, target, space, depth + 1, subject);
    if (known) {
      *out = known->region;
      return GVEC_OK;
    }
    const size_t open = c->seen_count;
    GSHGEOM_Map kept_space = *space;
    const bool outer_used = c->used_subject;
    c->used_subject = false;
    r = clip_terms(c, target, space, depth + 1, subject, out, refused);
    const bool used = c->used_subject;
    c->used_subject = outer_used || used;
    if (r != GVEC_OK || *refused) {
      return r;
    }
    /* `seen` is as it was when the region was asked for (clip_terms puts it back) */
    return c->seen_count == open ? memo_keep(c, target, &kept_space, depth + 1, subject, out, used) : GVEC_OK;
  }
  return GVEC_OK;
}

/* The region of one child of a clipPath, placed by `map` (the clip's own), as terms whose first clip is the shape itself: the shape cut by
 * the clip-path of the shape (in its own space, its transform included) and, for a `use`, by the use's (in the use's space). A shape of no
 * area adds no term. */
static GVEC_Result child_terms(chain * c, const clip_child * ch, const GSHGEOM_Map * map, size_t depth, clip_dnf * out, bool * refused) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  const GVEC_Svg_Instance * own_inst = NULL;
  GSHGEOM_Map own_space;
  memset(&own_space, 0, sizeof own_space);
  out->terms = NULL;
  out->count = out->capacity = 0;
  GSHSCENE_Clip_Inner * one = gvec_arena_alloc(&ctx->arena, sizeof *one, sizeof(double));
  if (!one) {
    return GVEC_ERR_OOM;
  }
  clip_term base = {one, 1};
  GVEC_Result r;
  clip_dnf cur = {&base, 1, 1};
  if (ch->preset) {
    one->resource = ch->resource;
    one->rule = ch->rule;
    if (!gvec_lw_compose(map, &ch->pre, &one->map)) {
      *refused = true;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip shape's transform leaves the range the waist can carry");
    }
    own_inst = ch->own_inst;
    if (own_inst && !gvec_lw_compose(map, &ch->own_space, &own_space)) {
      *refused = true;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip shape's transform leaves the range the waist can carry");
    }
  }
  else {
    GSHGEOM_Map own_t, placed;
    bool usable;
    r = own_transform(ctx, ch->inst, e, &own_t, &usable);
    if (r != GVEC_OK || !usable) {
      *refused = !usable;
      return r;
    }
    if (!gvec_lw_compose(map, &ch->pre, &placed) || !gvec_lw_compose(&placed, &own_t, &one->map)) {
      *refused = true;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip shape's transform leaves the range the waist can carry");
    }
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * path;
    GSHGEOM_Fill_Rule rule;
    r = region_of(c, ch->shape, ch->inst, &geometry, &path, &rule);
    if (r != GVEC_OK || geometry == GVEC_LW_GEOMETRY_LIMIT) {
      *refused = geometry == GVEC_LW_GEOMETRY_LIMIT;
      return r;
    }
    if (geometry != GVEC_LW_GEOMETRY_PATH) {
      return GVEC_OK;
    }
    one->resource = path;
    one->rule = rule;
    own_inst = ch->inst;
    own_space = one->map;
  }
  /* the clip-path of the shape, in the shape's own space; then the use's, in the use's */
  for (int which = 0; which < 2; which++) {
    const GVEC_Svg_Instance * inst = which == 0 ? own_inst : ch->use_inst;
    if (!inst) {
      continue;
    }
    GSHGEOM_Map space;
    if (which == 0) {
      space = own_space;
    }
    else if (!gvec_lw_compose(map, &ch->use_space, &space)) {
      *refused = true;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip shape's transform leaves the range the waist can carry");
    }
    clip_dnf region;
    bool has;
    r = inst_clip_dnf(c, inst, inst, &space, depth, inst, &region, &has, refused);
    if (r != GVEC_OK || *refused) {
      return r;
    }
    if (!has) {
      continue;
    }
    clip_dnf next = {NULL, 0, 0};
    r = dnf_product(c, &cur, &region, &next, refused);
    if (r != GVEC_OK || *refused) {
      return r;
    }
    cur = next;
  }
  for (size_t k = 0; k < cur.count; k++) {
    r = dnf_push(c, out, cur.terms[k], refused);
    if (r != GVEC_OK || *refused) {
      return r;
    }
  }
  return GVEC_OK;
}

/* The region of `clip_path`, placed by `space` (the map from the clipped shape's own space to the union's), as terms. *refused is set
 * when the element is. `subject` is the element the clip is applied to, whose box a clip in the units of the box is placed on. */
static GVEC_Result clip_terms(chain * c, const GVEC_Svg_Element * clip_path, const GSHGEOM_Map * space, size_t depth, const GVEC_Svg_Instance * subject,
    clip_dnf * out, bool * refused) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  out->terms = NULL;
  out->count = out->capacity = 0;
  if (depth >= GVEC_LW_CLIP_MAX || c->seen_count >= GVEC_LW_CLIP_MAX) {
    *refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "clips are nested deeper than the lowering allows");
  }
  const size_t mark = c->seen_count;
  c->seen[c->seen_count++] = clip_path;
  const GVEC_Svg_Instance * clip_inst;
  GVEC_Result r = gvec_lw_instance_of(ctx, clip_path, &clip_inst);
  if (r != GVEC_OK) {
    return r;
  }
  /* the clipPath's own clip-path: an intersection, in the same space */
  clip_dnf nested = {NULL, 0, 0};
  bool nests;
  r = inst_clip_dnf(c, clip_inst, subject, space, depth, subject, &nested, &nests, refused);
  if (r != GVEC_OK || *refused) {
    return r;
  }
  clip_child * list;
  size_t shapes;
  r = gather_children(c, clip_path, &list, &shapes, refused);
  if (r != GVEC_OK || *refused) {
    return r;
  }
  /* the clip's map: the space, the clipPath's transform, and the box of the subject when the units say so */
  GSHGEOM_Map t, map;
  bool usable;
  r = own_transform(ctx, clip_inst, e, &t, &usable);
  if (r != GVEC_OK || !usable) {
    *refused = !usable;
    return r;
  }
  if (!gvec_lw_compose(space, &t, &map)) {
    *refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip's transform leaves the range the waist can carry");
  }
  if (boxed_units(clip_path)) {
    GSHGEOM_Bounds box;
    bool all;
    r = subject_box(c, subject, &box, &all, refused);
    if (r != GVEC_OK || *refused) {
      return r;
    }
    if (all) {
      c->seen_count = mark; /* a box of no area: nothing gets through */
      return GVEC_OK;
    }
    if (!place_on_box(&map, &box, &map)) {
      *refused = true;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip's map is past the range the waist can carry");
    }
  }
  clip_dnf shapes_dnf = {NULL, 0, 0};
  for (size_t i = 0; i < shapes; i++) {
    clip_dnf one = {NULL, 0, 0};
    r = child_terms(c, &list[i], &map, depth, &one, refused);
    if (r != GVEC_OK || *refused) {
      return r;
    }
    for (size_t k = 0; k < one.count; k++) {
      r = dnf_push(c, &shapes_dnf, one.terms[k], refused);
      if (r != GVEC_OK || *refused) {
        return r;
      }
    }
  }
  c->seen_count = mark;
  if (!nests) {
    *out = shapes_dnf;
    return GVEC_OK;
  }
  return dnf_product(c, &nested, &shapes_dnf, out, refused);
}

/* A clipPath of several shapes: one union of the shapes' regions, each with its own transform and clip-rule, placed by `base`. A
 * `clip-path` on one of the shapes clips that shape alone: the member carries the clip's region as inner clips (a member for each term of
 * the union of intersections the region is), and a use's own clip-path likewise. */
static GVEC_Result add_union(chain * c, const clip_child * list, const GSHGEOM_Map * base, size_t shapes, gvec_lw_clip_state * state) {
  gvec_lw_ctx * ctx = c->ctx;
  const GVEC_Svg_Element * e = c->element;
  size_t capacity = shapes ? shapes : 1;
  GSHSCENE_Clip_Member * members = gvec_arena_alloc(&ctx->arena, capacity * sizeof *members, sizeof(double));
  if (!members) {
    return GVEC_ERR_OOM;
  }
  size_t count = 0;
  GSHGEOM_Map identity;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  for (size_t i = 0; i < shapes; i++) {
    /* A shape with a clip-path of its own is a member with inner clips, one for each term of the region the clip is a union of. */
    clip_dnf terms;
    bool refused = false;
    GVEC_Result r = child_terms(c, &list[i], &identity, 1, &terms, &refused);
    if (r != GVEC_OK || refused) {
      return r;
    }
    if (count + terms.count > capacity) {
      size_t grown = capacity;
      while (count + terms.count > grown) {
        grown *= 2;
      }
      GSHSCENE_Clip_Member * more = gvec_arena_alloc(&ctx->arena, grown * sizeof *more, sizeof(double));
      if (!more) {
        return GVEC_ERR_OOM;
      }
      memcpy(more, members, count * sizeof *more);
      members = more;
      capacity = grown;
    }
    for (size_t k = 0; k < terms.count; k++) {
      const clip_term * t = &terms.terms[k];
      members[count].resource = t->clips[0].resource;
      members[count].map = t->clips[0].map;
      members[count].rule = t->clips[0].rule;
      members[count].inner_count = t->count - 1;
      members[count].inner = t->count > 1 ? t->clips + 1 : NULL;
      count++;
    }
  }
  if (count == 0) {
    *state = GVEC_LW_CLIP_ALL;
    return GVEC_OK;
  }
  const GSHSCENE_Resource * region = NULL;
  GSHSCENE_Result g = gshscene_store_intern_clip_union(ctx->store, members, count, &region);
  if (g == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (g != GSHSCENE_OK) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip has more shapes than the waist's union holds");
  }
  GVEC_Result held = gvec_lw_hold(ctx, region);
  if (held != GVEC_OK) {
    return held;
  }
  return push_node(c, region, base, GSHGEOM_FILL_NONZERO, state);
}

/* The box of a path under `map`, merged into `acc`. An axis-aligned map moves the path's box; any other map moves each segment, and a
 * arc is mapped as an arc and bounded by the curves it is made of. False when a piece cannot be bounded. */
static bool merge_box(const GSHGEOM_Path * path, const GSHGEOM_Map * map, GSHGEOM_Bounds * acc) {
  GSHGEOM_Bounds b;
  if (map->m[0][1] == 0 && map->m[1][0] == 0) {
    GSHGEOM_Point lo, hi;
    if (gshgeom_path_bounds(path, &b) != GSHGEOM_OK) {
      return false;
    }
    if (b.empty) {
      return true;
    }
    if (gshgeom_map_apply_point(map, b.min, &lo) != GSHGEOM_OK || gshgeom_map_apply_point(map, b.max, &hi) != GSHGEOM_OK) {
      return false;
    }
    GSHGEOM_Bounds m = {0, {lo.x < hi.x ? lo.x : hi.x, lo.y < hi.y ? lo.y : hi.y, 0}, {lo.x < hi.x ? hi.x : lo.x, lo.y < hi.y ? hi.y : lo.y, 0}};
    b = m;
    goto merge;
  }
  b.empty = 1;
  for (size_t i = 0, n = gshgeom_path_element_count(path); i < n; i++) {
    GSHGEOM_Element el;
    GSHGEOM_Curve pieces[GSHGEOM_ARC_MAX_PIECES];
    int np = 0;
    GSHGEOM_Result g = gshgeom_path_element(path, i, &el);
    if (g == GSHGEOM_ERR_UNSUPPORTED) {
      /* An arc cannot be read back as a segment: map it, then take the pieces it is made of. */
      GSHGEOM_Arc arc, moved_arc;
      if (gshgeom_path_arc(path, i, &arc) != GSHGEOM_OK || gshgeom_arc_map(map, &arc, &moved_arc) != GSHGEOM_OK ||
          gshgeom_arc_pieces(&moved_arc, pieces, &np) != GSHGEOM_OK) {
        return false;
      }
    }
    else if (g != GSHGEOM_OK) {
      return false;
    }
    else if (el.verb == GSHGEOM_VERB_MOVE) {
      continue;
    }
    else {
      if (gshgeom_map_apply_curve(map, &el.curve, &pieces[0]) != GSHGEOM_OK) {
        return false;
      }
      np = 1;
    }
    for (int k = 0; k < np; k++) {
      GSHGEOM_Bounds cb;
      if (gshgeom_curve_bounds(&pieces[k], &cb) != GSHGEOM_OK) {
        return false;
      }
      if (b.empty) {
        b = cb;
        b.empty = 0;
      }
      else {
        b.min.x = cb.min.x < b.min.x ? cb.min.x : b.min.x;
        b.min.y = cb.min.y < b.min.y ? cb.min.y : b.min.y;
        b.max.x = cb.max.x > b.max.x ? cb.max.x : b.max.x;
        b.max.y = cb.max.y > b.max.y ? cb.max.y : b.max.y;
      }
    }
  }
  if (b.empty) {
    return true;
  }
merge:
  if (acc->empty) {
    *acc = b;
    acc->empty = 0;
  }
  else {
    acc->min.x = b.min.x < acc->min.x ? b.min.x : acc->min.x;
    acc->min.y = b.min.y < acc->min.y ? b.min.y : acc->min.y;
    acc->max.x = b.max.x > acc->max.x ? b.max.x : acc->max.x;
    acc->max.y = b.max.y > acc->max.y ? b.max.y : acc->max.y;
  }
  return true;
}

/* `b` merged into `acc`. */
static void merge_bounds(GSHGEOM_Bounds * acc, const GSHGEOM_Bounds * b) {
  if (b->empty) {
    return;
  }
  if (acc->empty) {
    *acc = *b;
    acc->empty = 0;
    return;
  }
  acc->min.x = b->min.x < acc->min.x ? b->min.x : acc->min.x;
  acc->min.y = b->min.y < acc->min.y ? b->min.y : acc->min.y;
  acc->max.x = b->max.x > acc->max.x ? b->max.x : acc->max.x;
  acc->max.y = b->max.y > acc->max.y ? b->max.y : acc->max.y;
}

/* The box of the stroke of a shape, in the space `map` leads to: the bounds of the outline the stroker makes of the shape's centerline (joins, caps,
 * miters and dashes included), grown into `*box` (which holds the fill box in that space). The outline is made at 16 device units to the unit, so
 * the box is exact to 1/1024 of a unit. *ok is false when the stroke cannot be boxed (it is refused, or the stroker's limits are passed). */
GVEC_Result gvec_lw_stroke_bounds(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHSCENE_Resource * centerline, const GSHGEOM_Map * map,
    GSHGEOM_Bounds * box, bool * ok) {
  enum { SCALE = 16 };
  *ok = true;
  gvec_lw_stroke stroke;
  GVEC_Result r = gvec_lw_stroke_of(ctx, inst, &stroke);
  if (r != GVEC_OK) {
    return r;
  }
  if (stroke.decision == GVEC_LW_PAINT_REFUSED) {
    *ok = false;
    return GVEC_OK;
  }
  if (stroke.decision != GVEC_LW_PAINT_DRAW) {
    return GVEC_OK; /* no stroke: the box is the fill box */
  }
  GSHGEOM_Stroke geometry;
  GSHGEOM_Map scaled = *map;
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < 2; j++) {
      scaled.m[i][j] *= SCALE;
    }
    scaled.t[i] *= SCALE;
  }
  GSHGEOM_Stroke_Outline * outline = NULL;
  if (gshscene_resource_stroke_geometry(stroke.style, &geometry) != GSHSCENE_OK ||
      gshgeom_stroke_expand(gshscene_resource_path(centerline), &geometry, &scaled, NULL, NULL, &outline) != GSHGEOM_OK) {
    *ok = false;
    return GVEC_OK;
  }
  if (gshgeom_stroke_outline_status(outline) == GSHGEOM_STROKE_DRAWN) {
    const GSHGEOM_Flat_Path * parts[2] = {gshgeom_stroke_outline_body(outline), gshgeom_stroke_outline_aligned(outline)};
    GSHGEOM_Bounds grown = *box;
    for (int k = 0; k < 2; k++) {
      const GSHGEOM_Device_Point * points = gshgeom_flat_path_points(parts[k]);
      for (size_t i = 0, n = gshgeom_flat_path_point_count(parts[k]); points && i < n; i++) {
        GSHGEOM_Point p = {(double) points[i].x / (GSHGEOM_DEVICE_ONE * SCALE), (double) points[i].y / (GSHGEOM_DEVICE_ONE * SCALE), 0};
        GSHGEOM_Bounds one = {0, p, p};
        merge_bounds(&grown, &one);
      }
    }
    *box = grown;
  }
  gshgeom_stroke_outline_destroy(outline);
  return GVEC_OK;
}

/* The bounding box of a group in its own space (SVG 2, 8.10): the union of the fill boxes of the shapes in it, each in the group's space.
 * What `display` removes is left out; `visibility` and `opacity` do not remove a shape from the box. *known is false when the group holds
 * what this does not box (a `use` of a viewport, a nested viewport, a transform that needs a box of its own). */
/* The box of a `text` element's glyph cells in the space `map` leads to, merged into `box`. `*ok` is false when the text cannot be laid out. */
static GVEC_Result text_leaf_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * map, GSHGEOM_Bounds * box, bool * ok) {
  gvec_lw_text text;
  bool usable = true;
  *ok = false;
  GVEC_Result r = gvec_lw_text_layout(ctx, inst, &text, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  *ok = true;
  if (text.box.empty) {
    return GVEC_OK;
  }
  GSHGEOM_Bounds mapped = {1, {0, 0, 0}, {0, 0, 0}};
  for (int c = 0; c < 4; c++) {
    GSHGEOM_Point in = {c & 1 ? text.box.max.x : text.box.min.x, c & 2 ? text.box.max.y : text.box.min.y, 0}, out;
    if (gshgeom_map_apply_point(map, in, &out) != GSHGEOM_OK) {
      *ok = false;
      return GVEC_OK;
    }
    GSHGEOM_Bounds one = {0, out, out};
    merge_bounds(&mapped, &one);
  }
  merge_bounds(box, &mapped);
  return GVEC_OK;
}

static GVEC_Result element_box_in(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * outer, GSHGEOM_Bounds * box,
    bool * known);
static GVEC_Result group_box(gvec_lw_ctx * ctx, const GVEC_Svg_Element * refused, const GVEC_Svg_Element * group, const GVEC_Svg_Instance * ginst,
    const GSHGEOM_Map * map, GSHGEOM_Bounds * box, bool * known, int depth, const GVEC_Svg_Element * only);

/* The box of an `svg` or a `symbol` viewport (SVG 2, 8.10: the clip of its viewport does not shorten it): the box of its content, put in the space
 * `placed` leads to through the viewport's own map. A viewport with no size draws nothing and adds nothing. */
static GVEC_Result viewport_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * placed, GSHGEOM_Bounds * box, bool * known) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  double x, y, w, h;
  GSHGEOM_Map inner, moved;
  int state;
  const char * why;
  *known = false;
  GVEC_Result r = gvec_lw_viewport_inner(ctx, inst, &x, &y, &w, &h, &inner, &state, &why);
  if (r != GVEC_OK) {
    return r;
  }
  if (state == 1) {
    *known = true;
    return GVEC_OK;
  }
  if (state != 0 || !gvec_lw_compose(placed, &inner, &moved)) {
    return GVEC_OK;
  }
  *known = true;
  return group_box(ctx, e, e, inst, &moved, box, known, 0, NULL);
}

/* The box of what a `use` draws, in the space `placed` leads to (the use's own transform already in it): the box of the instance of its
 * referent, moved by the use's `x` and `y`.  A `use` of nothing, or of an element `display` removes, draws nothing and adds nothing. */
static GVEC_Result use_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * placed, GSHGEOM_Bounds * box, bool * known) {
  *known = false;
  if (ctx->use_depth > 32) {
    return GVEC_OK;
  }
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  if (!gvec_svg_element_href_target(e)) {
    *known = true; /* a reference to nothing, a cycle, or another document: nothing is drawn */
    return GVEC_OK;
  }
  double xy[2] = {0, 0};
  for (int i = 0; i < 2; i++) {
    GVEC_Svg_Style_Value v;
    bool usable;
    GVEC_Result r = gvec_lw_read_property(ctx, inst, i == 0 ? GVEC_SVG_PROP_X : GVEC_SVG_PROP_Y, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    xy[i] = v.kind == GVEC_SVG_STYLE_LENGTH && !v.length_auto ? v.number : 0;
  }
  GSHGEOM_Map shift, moved;
  if (gshgeom_map_translate(2, xy[0], xy[1], 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(placed, &shift, &moved)) {
    return GVEC_OK;
  }
  const GVEC_Svg_Instance * clone = NULL;
  GVEC_Result r = gvec_svg_instance_use(ctx->style, inst, &clone);
  if (r == GVEC_ERR_LIMIT) {
    return GVEC_OK;
  }
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Style_Value display;
  bool usable;
  r = gvec_lw_read_property(ctx, clone, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &display, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (gvec_lw_keyword_is(&display, "none") || gvec_svg_element_conditional_false(gvec_svg_instance_element(clone))) {
    *known = true;
    return GVEC_OK;
  }
  ctx->use_depth++;
  r = element_box_in(ctx, clone, &moved, box, known);
  ctx->use_depth--;
  return r;
}

static GVEC_Result group_box(gvec_lw_ctx * ctx, const GVEC_Svg_Element * refused, const GVEC_Svg_Element * group, const GVEC_Svg_Instance * ginst,
    const GSHGEOM_Map * map, GSHGEOM_Bounds * box, bool * known, int depth, const GVEC_Svg_Element * only) {
  if (depth > 32) {
    *known = false;
    return GVEC_OK;
  }
  for (const GVEC_Svg_Element * child = gvec_svg_element_first_child(group); child; child = gvec_svg_element_next_sibling(child)) {
    if (only && child != only) {
      continue;
    }
    GVEC_Svg_Kind ck = gvec_svg_element_kind(child);
    bool shape = is_shape(ck), container = ck == GVEC_SVG_KIND_G || ck == GVEC_SVG_KIND_A, text_leaf = ck == GVEC_SVG_KIND_TEXT;
    const bool image_leaf = ck == GVEC_SVG_KIND_IMAGE;
    const bool use_leaf = ck == GVEC_SVG_KIND_USE;
    const bool viewport_leaf = ck == GVEC_SVG_KIND_SVG;
    const bool switch_leaf = ck == GVEC_SVG_KIND_SWITCH;
    if (!shape && !container && !text_leaf && !image_leaf && !use_leaf && !viewport_leaf && !switch_leaf) {
      switch (ck) {
      case GVEC_SVG_KIND_DEFS: case GVEC_SVG_KIND_DESC: case GVEC_SVG_KIND_TITLE: case GVEC_SVG_KIND_METADATA:
      case GVEC_SVG_KIND_LINEAR_GRADIENT: case GVEC_SVG_KIND_RADIAL_GRADIENT: case GVEC_SVG_KIND_CLIP_PATH: case GVEC_SVG_KIND_MASK:
      case GVEC_SVG_KIND_MARKER: case GVEC_SVG_KIND_FILTER: case GVEC_SVG_KIND_PATTERN: case GVEC_SVG_KIND_STYLE: case GVEC_SVG_KIND_SYMBOL:
        continue;
      default:
        *known = false;
        return GVEC_OK;
      }
    }
    const GVEC_Svg_Instance * ci;
    GVEC_Result r = gvec_svg_instance_child(ctx->style, ginst, child, &ci);
    if (r != GVEC_OK) {
      return r;
    }
    GVEC_Svg_Style_Value v;
    bool usable;
    r = gvec_lw_read_property(ctx, ci, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      *known = false;
      return r;
    }
    if (gvec_lw_keyword_is(&v, "none") || gvec_svg_element_conditional_false(child)) {
      continue;
    }
    GSHGEOM_Map t, inner;
    r = own_transform(ctx, ci, refused, &t, &usable);
    if (r != GVEC_OK || !usable) {
      *known = false;
      return r;
    }
    if (!gvec_lw_compose(map, &t, &inner)) {
      *known = false;
      return GVEC_OK;
    }
    if (container || switch_leaf) {
      /* A switch draws the first of its children whose conditions hold, and no other: only that one is boxed. */
      const GVEC_Svg_Element * chosen = switch_leaf ? gvec_svg_element_switch_selected(child) : NULL;
      if (switch_leaf && !chosen) {
        continue;
      }
      r = group_box(ctx, refused, child, ci, &inner, box, known, depth + 1, chosen);
      if (r != GVEC_OK || !*known) {
        return r;
      }
      continue;
    }
    if (viewport_leaf) {
      bool ok;
      r = viewport_box(ctx, ci, &inner, box, &ok);
      if (r != GVEC_OK || !ok) {
        *known = false;
        return r;
      }
      continue;
    }
    if (use_leaf) {
      bool ok;
      r = use_box(ctx, ci, &inner, box, &ok);
      if (r != GVEC_OK || !ok) {
        *known = false;
        return r;
      }
      continue;
    }
    if (text_leaf || image_leaf) {
      bool ok;
      r = text_leaf ? text_leaf_box(ctx, ci, &inner, box, &ok) : gvec_lw_image_leaf_box(ctx, ci, &inner, box, &ok);
      if (r != GVEC_OK || !ok) {
        *known = false;
        return r;
      }
      continue;
    }
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * path = NULL;
    r = gvec_lw_centerline_of(ctx, ci, &geometry, &path, &usable);
    if (r != GVEC_OK || !usable || geometry == GVEC_LW_GEOMETRY_LIMIT) {
      *known = false;
      return r;
    }
    if (geometry == GVEC_LW_GEOMETRY_PATH) {
      GSHGEOM_Bounds one = {1, {0, 0, 0}, {0, 0, 0}};
      if (!merge_box(gshscene_resource_path(path), &inner, &one)) {
        *known = false;
        return GVEC_OK;
      }
      if (ctx->box_stroke && !one.empty) {
        bool stroked;
        r = gvec_lw_stroke_bounds(ctx, ci, path, &inner, &one, &stroked);
        if (r != GVEC_OK) {
          return r;
        }
        if (!stroked) {
          *known = false;
          return GVEC_OK;
        }
      }
      merge_bounds(box, &one);
    }
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_box_transform(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Transform * transform, GSHGEOM_Map * out,
    bool * known) {
  GSHGEOM_Bounds box;
  memset(&box, 0, sizeof box);
  box.empty = 1;
  const bool stroke = transform->box == GVEC_SVG_BOX_STROKE || transform->box == GVEC_SVG_BOX_BORDER;
  const bool saved = ctx->box_stroke;
  ctx->box_stroke = stroke;
  GVEC_Result r = gvec_lw_inner_box(ctx, inst, &box, known);
  ctx->box_stroke = saved;
  if (r != GVEC_OK || !*known) {
    return r;
  }
  /* An element that draws nothing has a box of no size at the origin: the percentages are of nothing. */
  const double x = box.empty ? 0 : box.min.x, y = box.empty ? 0 : box.min.y;
  const double w = box.empty ? 0 : box.max.x - box.min.x, h = box.empty ? 0 : box.max.y - box.min.y;
  if (gvec_svg_transform_resolve(transform, x, y, w, h, out) != GVEC_OK) {
    *known = false;
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_element_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GSHGEOM_Bounds * box, bool * known) {
  return element_box_in(ctx, inst, NULL, box, known);
}

/* The box of `inst` in the space of its parent, moved on through `outer` when there is one. */
static GVEC_Result element_box_in(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * outer, GSHGEOM_Bounds * box,
    bool * known) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  GVEC_Svg_Kind kind = gvec_svg_element_kind(e);
  GSHGEOM_Map own;
  bool usable;
  *known = false;
  box->empty = 1;
  GVEC_Result r = own_transform(ctx, inst, e, &own, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (outer) {
    GSHGEOM_Map composed;
    if (!gvec_lw_compose(outer, &own, &composed)) {
      return GVEC_OK;
    }
    own = composed;
  }
  *known = true;
  if (kind == GVEC_SVG_KIND_USE) {
    return use_box(ctx, inst, &own, box, known);
  }
  if (kind == GVEC_SVG_KIND_G || kind == GVEC_SVG_KIND_A) {
    return group_box(ctx, e, e, inst, &own, box, known, 0, NULL);
  }
  if (kind == GVEC_SVG_KIND_SVG || kind == GVEC_SVG_KIND_SYMBOL) {
    return viewport_box(ctx, inst, &own, box, known);
  }
  if (kind == GVEC_SVG_KIND_SWITCH) {
    const GVEC_Svg_Element * chosen = gvec_svg_element_switch_selected(e);
    return chosen ? group_box(ctx, e, e, inst, &own, box, known, 0, chosen) : GVEC_OK;
  }
  if (kind == GVEC_SVG_KIND_TEXT) {
    bool ok;
    r = text_leaf_box(ctx, inst, &own, box, &ok);
    *known = ok;
    return r;
  }
  if (kind == GVEC_SVG_KIND_IMAGE) {
    bool ok;
    r = gvec_lw_image_leaf_box(ctx, inst, &own, box, &ok);
    *known = ok;
    return r;
  }
  if (!is_shape(kind)) {
    *known = false;
    return GVEC_OK;
  }
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * path = NULL;
  r = gvec_lw_centerline_of(ctx, inst, &geometry, &path, &usable);
  if (r != GVEC_OK || !usable || geometry == GVEC_LW_GEOMETRY_LIMIT) {
    *known = false;
    return r;
  }
  if (geometry == GVEC_LW_GEOMETRY_PATH && !merge_box(gshscene_resource_path(path), &own, box)) {
    *known = false;
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_inner_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GSHGEOM_Bounds * box, bool * known) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *known = true;
  box->empty = 1;
  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_IMAGE) {
    GSHGEOM_Map identity;
    bool ok;
    if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
      return GVEC_ERR_INTERNAL;
    }
    GVEC_Result image_result = gvec_lw_image_leaf_box(ctx, inst, &identity, box, &ok);
    *known = ok;
    return image_result;
  }
  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_TEXT) {
    /* The box of the text is the cells of its glyphs, the font's ascent and descent high (the references measure it so): a filter, a mask or a
       clip in the units of the box of a `text` places itself in it, where it used to find no box and draw nothing. */
    GSHGEOM_Map identity;
    bool ok;
    if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
      return GVEC_ERR_INTERNAL;
    }
    GVEC_Result text_result = text_leaf_box(ctx, inst, &identity, box, &ok);
    /* A text whose layout cannot be made (no face to set it in) is refused where it is drawn; its box stays the empty one it always was. */
    *known = true;
    if (!ok) {
      box->empty = 1;
    }
    return text_result;
  }
  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_TSPAN || gvec_svg_element_kind(e) == GVEC_SVG_KIND_TEXT_PATH) {
    /* The box of a span is the cells of its own glyphs, in the space of its text: what a filter on the span is placed in. */
    bool ok;
    GVEC_Result span_result = gvec_lw_text_span_box(ctx, inst, box, &ok);
    *known = true;
    if (!ok) {
      box->empty = 1;
    }
    return span_result;
  }
  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_USE) {
    GSHGEOM_Map identity;
    if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
      return GVEC_ERR_INTERNAL;
    }
    return use_box(ctx, inst, &identity, box, known); /* in the use's own space: its `x` and `y` are inside it */
  }
  if (!is_shape(gvec_svg_element_kind(e))) {
    GSHGEOM_Map identity;
    if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
      return GVEC_ERR_INTERNAL;
    }
    return group_box(ctx, e, e, inst, &identity, box, known, 0, gvec_svg_element_kind(e) == GVEC_SVG_KIND_SWITCH ? gvec_svg_element_switch_selected(e) : NULL);
  }
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * path = NULL;
  bool usable;
  GVEC_Result r = gvec_lw_centerline_of(ctx, inst, &geometry, &path, &usable);
  if (r != GVEC_OK || !usable || geometry == GVEC_LW_GEOMETRY_LIMIT) {
    *known = false;
    return r;
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH || gshgeom_path_bounds(gshscene_resource_path(path), box) != GSHGEOM_OK) {
    box->empty = 1;
  }
  if (ctx->box_stroke && !box->empty) {
    /* The stroke box: the fill box grown by what the stroke reaches past the path. */
    GSHGEOM_Map identity;
    bool stroked;
    if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
      return GVEC_ERR_INTERNAL;
    }
    r = gvec_lw_stroke_bounds(ctx, inst, path, &identity, box, &stroked);
    if (r != GVEC_OK) {
      return r;
    }
    if (!stroked) {
      *known = false;
    }
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_content_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * root, GSHGEOM_Bounds * box, bool * known) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(root);
  GSHGEOM_Map identity;
  box->empty = 1;
  *known = true;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    *known = false;
    return GVEC_OK;
  }
  ctx->box_stroke = true;
  GVEC_Result r = group_box(ctx, e, e, root, &identity, box, known, 0, NULL);
  ctx->box_stroke = false;
  return r;
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
    /* A basic shape on the clipPath: an intersection, measured against the element the clip is applied to. */
    clip_dnf shape_terms = {NULL, 0, 0};
    bool shape_refused = false;
    r = basic_terms(c, clip_inst, c->inst, space, &shape_terms, &shape_refused);
    if (r != GVEC_OK || shape_refused) {
      return r;
    }
    if (shape_terms.count == 0) {
      *state = GVEC_LW_CLIP_ALL;
      return GVEC_OK;
    }
    if (shape_terms.terms[0].count == 1) {
      r = push_node(c, shape_terms.terms[0].clips[0].resource, &shape_terms.terms[0].clips[0].map, shape_terms.terms[0].clips[0].rule, state);
      if (r != GVEC_OK) {
        return r;
      }
    }
  }
  else if (nested == 1 && gvec_svg_element_kind(nested_target) == GVEC_SVG_KIND_CLIP_PATH && !seen_before(c, nested_target)) {
    gvec_lw_clip_state inner;
    r = add_clip(c, nested_target, space, depth + 1, &inner);
    if (r != GVEC_OK || inner != GVEC_LW_CLIP_NODE) {
      *state = inner;
      return r;
    }
  }
  /* Its shapes: the children that are shapes (or `use`s of one) and are drawn; text is refused, and anything else is not part of a clip. */
  clip_child * list;
  size_t shapes;
  bool refused;
  r = gather_children(c, clip_path, &list, &shapes, &refused);
  if (r != GVEC_OK || refused) {
    return r;
  }
  const GVEC_Svg_Element * shape = shapes ? list[shapes - 1].shape : NULL;
  const GVEC_Svg_Instance * shape_inst = shapes ? list[shapes - 1].inst : NULL;
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
  if (boxed_units(clip_path)) {
    GSHGEOM_Bounds box;
    bool all, refused_box;
    r = subject_box(c, c->inst, &box, &all, &refused_box);
    if (r != GVEC_OK || refused_box) {
      return r;
    }
    if (all) {
      *state = GVEC_LW_CLIP_ALL;
      return GVEC_OK;
    }
    if (!place_on_box(&map, &box, &map)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the clip's map is past the range the waist can carry");
    }
  }
  const GSHGEOM_Map union_base = map;
  if (shapes > 1 || list[0].preset || list[0].use_inst) {
    return add_union(c, list, &map, shapes, state);
  }
  r = own_transform(ctx, shape_inst, e, &t, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!gvec_lw_compose(&map, &list[shapes - 1].pre, &map) || !gvec_lw_compose(&map, &t, &map)) {
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
    return add_union(c, list, &union_base, shapes, state); /* a basic shape is a term of the region, as the union makes it */
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
  GSHGEOM_Fill_Rule rule;
  r = region_of(c, shape, shape_inst, &geometry, &path, &rule);
  if (r != GVEC_OK) {
    return r;
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH) {
    *state = GVEC_LW_CLIP_ALL; /* A shape with no area clips everything away. */
    return GVEC_OK;
  }
  return push_node(c, path, &map, rule, state);
}

/* The text of a `clip-path` that is a basic shape with its box keyword, or NULL when it holds a `url()` (another document, or a mix). */
static GVEC_Result shape_text(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const char ** text) {
  GVEC_Svg_Style_Value v;
  bool usable;
  *text = NULL;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_CLIP_PATH, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  size_t length = 1;
  for (size_t i = 0; i < v.reference_count; i++) {
    if (v.references[i].is_url || !v.references[i].text) {
      return GVEC_OK;
    }
    length += strlen(v.references[i].text) + 1;
  }
  char * joined = gvec_arena_alloc(&ctx->arena, length, 1);
  if (!joined) {
    return GVEC_ERR_OOM;
  }
  joined[0] = 0;
  for (size_t i = 0; i < v.reference_count; i++) {
    strcat(joined, v.references[i].text);
    strcat(joined, " ");
  }
  *text = joined;
  return GVEC_OK;
}

/* `clip-path` that is a basic shape: one clip region in the element's space. */
static GVEC_Result basic_shape_begin(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_clip_state * state,
    gvec_lw_node ** out, size_t * count) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  const char * text;
  GVEC_Result r = shape_text(ctx, inst, &text);
  if (r != GVEC_OK) {
    *state = GVEC_LW_CLIP_REFUSED;
    return r;
  }
  if (!text) {
    *state = GVEC_LW_CLIP_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "a clip-path that names an element the resolver does not supply is not lowered");
  }
  *state = GVEC_LW_CLIP_REFUSED;
  gvec_lw_shape shape;
  r = gvec_lw_basic_shape(ctx, inst, text, &shape);
  if (r != GVEC_OK) {
    return r;
  }
  switch (shape.kind) {
  case GVEC_LW_SHAPE_REFUSED:
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, shape.why);
  case GVEC_LW_SHAPE_ERROR:
    return GVEC_OK; /* The element was refused where the reason arose. */
  case GVEC_LW_SHAPE_INVALID:
    *state = GVEC_LW_CLIP_NONE;
    return gvec_diag_add(ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_STYLE_INVALID, 0, e->node.line, e->node.column, e->node.offset,
        e->name, "a clip-path that is not a shape or a reference is as if it were not specified");
  case GVEC_LW_SHAPE_NONE:
    *state = GVEC_LW_CLIP_NONE;
    return GVEC_OK;
  case GVEC_LW_SHAPE_ALL:
    *state = GVEC_LW_CLIP_ALL;
    return GVEC_OK;
  case GVEC_LW_SHAPE_PATH:
    break;
  }
  GSHGEOM_Map t, local, moved, placed;
  bool usable;
  r = own_transform(ctx, inst, e, &t, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!gvec_lw_compose(frame, &t, &local) || gshgeom_map_translate(2, shape.ox, shape.oy, 0, &moved) != GSHGEOM_OK ||
      !gvec_lw_compose(&local, &moved, &placed)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's transform leaves the range the waist can carry");
  }
  chain c;
  memset(&c, 0, sizeof c);
  c.ctx = ctx;
  c.element = e;
  c.inst = inst;
  c.nodes = out;
  r = push_node(&c, shape.path, &placed, shape.rule, state);
  *count = c.count;
  return r;
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
    return basic_shape_begin(ctx, inst, frame, state, out, count);
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
