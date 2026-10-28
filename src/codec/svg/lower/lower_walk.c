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
 * @file lower_walk.c
 *
 * The walk (P1, P5 to P8): it visits every element in the context it is drawn in, decides what is drawn or refused, and builds
 * the drawing tree the emitters record. It reads the typed document and the used values of the style layer, and nothing else
 * (D-B): no XML, no CSS text.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include "../../../codec/codec_internal.h"
#include "lower_internal.h"

//-----------------------------------------------------------------------------
// The context
//-----------------------------------------------------------------------------

uint64_t gvec_lw_key(uint64_t element_key, uint64_t chain, gvec_lw_role role) {
  uint8_t buf[24];
  uint64_t parts[3] = {element_key, chain, (uint64_t) role};
  for (int p = 0; p < 3; p++) {
    for (int i = 0; i < 8; i++) {
      buf[p * 8 + i] = (uint8_t) (parts[p] >> (8 * i));
    }
  }
  return gvec_svg_hash64(buf, sizeof buf);
}

bool gvec_lw_compose(const GSHGEOM_Map * outer, const GSHGEOM_Map * inner, GSHGEOM_Map * out) {
  GSHGEOM_Map product;
  if (gshgeom_map_compose(outer, inner, &product) != GSHGEOM_OK) {
    return false;
  }
  return gvec_lw_normal(&product, out);
}

bool gvec_lw_normal(const GSHGEOM_Map * map, GSHGEOM_Map * out) {
  GSHGEOM_Map identity;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    return false;
  }
  /* The version of a retained scene composes every item with the maps of the groups above it, which are identities here, and a
   * product has no negative zero. Doing the same here, once, is what makes the list that is recorded and the list that is
   * emitted equal in every bit. */
  return gshgeom_map_compose(&identity, map, out) == GSHGEOM_OK;
}

gvec_lw_node * gvec_lw_new_node(gvec_lw_ctx * ctx, gvec_lw_kind kind) {
  gvec_lw_node * node = gvec_arena_alloc(&ctx->arena, sizeof *node, 8);
  if (!node) {
    return NULL;
  }
  node->kind = kind;
  node->opacity = 1;
  node->site = GVEC_LW_NO_SITE;
  switch (kind) {
  case GVEC_LW_FILL:
    node->items = node->weight = node->nodes = node->occurrences = 1;
    break;
  case GVEC_LW_GROUP:
  case GVEC_LW_CLIP:
    node->items = node->weight = 2; /* a push and a pop */
    node->nodes = node->occurrences = 1;
    break;
  case GVEC_LW_USE:
    node->items = 1;
    node->weight = 1;
    node->nodes = 2; /* the hidden container and the instance */
    node->occurrences = 2; /* the instance, and its target drawn through it */
    break;
  }
  node->container_site = GVEC_LW_NO_SITE;
  if (gshgeom_map_identity(2, &node->map) != GSHGEOM_OK) {
    return NULL;
  }
  return node;
}

static size_t saturating_add(size_t a, size_t b) {
  return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

GVEC_Result gvec_lw_append(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * child) {
  size_t weight = saturating_add(parent->weight, child->weight);
  size_t occurrences = saturating_add(parent->occurrences, child->occurrences);
  size_t nodes = saturating_add(parent->nodes, child->nodes);
  bool fits = weight <= ctx->caps.expansion && occurrences <= ctx->caps.expansion && nodes <= ctx->caps.nodes;
  /* The items of a list are known where the list begins: at the root, and in a picture. */
  if (parent == &ctx->root) {
    fits = fits && saturating_add(parent->items, child->items) <= ctx->caps.list_items;
  }
  else if (parent->kind == GVEC_LW_USE) {
    fits = fits && saturating_add(parent->inner_items, child->items) <= ctx->caps.list_items;
  }
  if (!fits) {
    const GVEC_Svg_Element * element = child->site != GVEC_LW_NO_SITE ? ctx->sites[child->site].element : NULL;
    return element ? gvec_lw_refuse(ctx, element, GVEC_LOWER_LIMIT, "the drawing is larger than the scene's limits allow") : GVEC_OK;
  }
  if (parent->last) {
    parent->last->next = child;
  }
  else {
    parent->first = child;
  }
  parent->last = child;
  parent->weight = weight;
  parent->occurrences = occurrences;
  parent->nodes = nodes;
  if (parent->kind == GVEC_LW_USE) {
    parent->inner_items = saturating_add(parent->inner_items, child->items);
  }
  else {
    parent->items = saturating_add(parent->items, child->items);
  }
  parent->content = parent->content || child->content;
  return GVEC_OK;
}

uint32_t gvec_lw_new_site(gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, uint64_t key) {
  if (ctx->site_count == ctx->site_capacity) {
    size_t capacity = ctx->site_capacity ? ctx->site_capacity * 2 : 64, bytes;
    if (capacity > UINT32_MAX - 1 || !gcu_safe_mul_size(capacity, sizeof *ctx->sites, &bytes)) {
      return GVEC_LW_NO_SITE;
    }
    void * fresh = gvec_realloc(ctx->allocator, ctx->sites, bytes);
    if (!fresh) {
      return GVEC_LW_NO_SITE;
    }
    ctx->sites = fresh;
    ctx->site_capacity = capacity;
  }
  gvec_lw_site * site = &ctx->sites[ctx->site_count];
  site->element = element;
  site->key = key;
  site->use_depth = ctx->use_depth;
  return (uint32_t) ctx->site_count++;
}

GVEC_Result gvec_lw_hold(gvec_lw_ctx * ctx, const GSHSCENE_Resource * resource) {
  if (ctx->held_count == ctx->held_capacity) {
    size_t capacity = ctx->held_capacity ? ctx->held_capacity * 2 : 32, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof *ctx->held, &bytes)) {
      gshscene_resource_release(resource);
      return GVEC_ERR_OOM;
    }
    void * fresh = gvec_realloc(ctx->allocator, (void *) ctx->held, bytes);
    if (!fresh) {
      gshscene_resource_release(resource);
      return GVEC_ERR_OOM;
    }
    ctx->held = fresh;
    ctx->held_capacity = capacity;
  }
  ctx->held[ctx->held_count++] = resource;
  return GVEC_OK;
}

GVEC_Result gvec_lw_begin(gvec_lw_ctx * ctx, const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_Store * store,
    GVEC_Diagnostics * diagnostics) {
  memset(ctx, 0, sizeof *ctx);
  ctx->doc = doc;
  ctx->document = gvec_svg_document(doc);
  if (!ctx->document || !store) {
    return GVEC_ERR_INVALID;
  }
  ctx->allocator = doc->allocator;
  ctx->diagnostics = diagnostics;
  ctx->options = options;
  ctx->store = store;
  if (options) {
    ctx->scene_allocator = options->scene_allocator;
    ctx->scene_limits = options->scene_limits;
    ctx->tracing = options->trace != NULL;
  }
  gvec_arena_init(&ctx->arena, ctx->allocator);
  ctx->max_depth = ctx->document->limits.max_depth;
  {
    GSHSCENE_Limits defaults;
    gshscene_limits_default(&defaults);
    const GSHSCENE_Limits * given = ctx->scene_limits;
#define GVEC_LW_CAP(field) (given && given->field ? given->field : defaults.field)
    ctx->caps.node_depth = GVEC_LW_CAP(max_node_depth);
    ctx->caps.open = GVEC_LW_CAP(max_depth);
    ctx->caps.picture_depth = GVEC_LW_CAP(max_picture_depth);
    ctx->caps.list_items = GVEC_LW_CAP(max_list_items);
    ctx->caps.expansion = GVEC_LW_CAP(max_expansion);
    ctx->caps.nodes = GVEC_LW_CAP(max_nodes);
#undef GVEC_LW_CAP
  }
  size_t count = gvec_svg_element_count(ctx->document);
  ctx->refused = gvec_alloc(ctx->allocator, count * sizeof *ctx->refused);
  if (count && !ctx->refused) {
    gvec_arena_destroy(&ctx->arena);
    return GVEC_ERR_OOM;
  }
  if (ctx->refused) {
    memset(ctx->refused, 0, count * sizeof *ctx->refused);
  }
  GVEC_Result r = gvec_svg_style_context_new(ctx->document, diagnostics, ctx->allocator, &ctx->style);
  if (r == GVEC_OK) {
    r = gvec_svg_style_resolve_references(ctx->style);
  }
  if (r != GVEC_OK) {
    gvec_lw_end(ctx);
    return r == GVEC_ERR_LIMIT ? GVEC_ERR_LIMIT : r;
  }
  if (gshgeom_map_identity(2, &ctx->root.map) != GSHGEOM_OK) {
    gvec_lw_end(ctx);
    return GVEC_ERR_INTERNAL;
  }
  ctx->root.opacity = 1;
  ctx->root.weight = 2; /* the scene's root group and the drawing's */
  ctx->root.occurrences = 2;
  ctx->root.nodes = 2;
  ctx->levels = 2;
  return GVEC_OK;
}

void gvec_lw_end(gvec_lw_ctx * ctx) {
  for (size_t i = 0; i < ctx->held_count; i++) {
    gshscene_resource_release(ctx->held[i]);
  }
  gvec_free(ctx->allocator, (void *) ctx->held);
  gvec_free(ctx->allocator, ctx->sites);
  gvec_free(ctx->allocator, ctx->refused);
  gvec_free(ctx->allocator, ctx->use_stack);
  for (size_t i = 0; i < ctx->table_count; i++) {
    gvec_free(ctx->allocator, ctx->tables[i].site);
    gvec_free(ctx->allocator, ctx->tables[i].child);
  }
  gvec_free(ctx->allocator, ctx->tables);
  gvec_svg_style_context_free(ctx->style);
  gvec_arena_destroy(&ctx->arena);
  memset(ctx, 0, sizeof *ctx);
}

GVEC_Result gvec_lw_refuse(gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, GVEC_Lower_Reason reason, const char * message) {
  uint32_t bit = UINT32_C(1) << (unsigned) reason;
  if (ctx->refused[element->order] & bit) {
    return GVEC_OK;
  }
  ctx->refused[element->order] |= bit;
  return gvec_diag_add(ctx->diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LOWER_REFUSED, (uint32_t) reason, element->node.line,
      element->node.column, element->node.offset, element->name, message);
}

//-----------------------------------------------------------------------------
// Reading values
//-----------------------------------------------------------------------------

static bool keyword_is(const GVEC_Svg_Style_Value * v, const char * word) {
  if (v->kind != GVEC_SVG_STYLE_KEYWORD || !v->keyword) {
    return false;
  }
  const char * a = v->keyword;
  const char * b = word;
  for (; *a && *b; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') {
      x = (char) (x - 'A' + 'a');
    }
    if (x != y) {
      return false;
    }
  }
  return *a == 0 && *b == 0;
}

/*
 * Read a property. A value the style layer could not use refuses the element (`unusable` is its reason); any other failure
 * (except running out of memory) is a limit.
 *
 * @param usable Receives whether the value can be used.
 */
static GVEC_Result read_property(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property,
    GVEC_Lower_Reason unusable, GVEC_Svg_Style_Value * v, bool * usable) {
  GVEC_Result r = gvec_svg_get(ctx->style, inst, property, v);
  *usable = r == GVEC_OK;
  if (r == GVEC_OK || r == GVEC_ERR_OOM) {
    return r;
  }
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  if (r == GVEC_ERR_UNSUPPORTED) {
    return gvec_lw_refuse(ctx, e, unusable, "a property of the element has no value that can be drawn");
  }
  if (r == GVEC_ERR_LIMIT) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a limit was reached reading the element's style");
  }
  return r;
}

//-----------------------------------------------------------------------------
// What every drawn element has: display, transform, opacity, and the features that are not lowered yet
//-----------------------------------------------------------------------------

typedef struct common {
  bool draw;           ///< The element is drawn (not removed by `display`, a condition, or a refusal).
  bool visible;
  GSHGEOM_Map local;   ///< The frame composed with the element's `transform`.
  double opacity;
} common;

/* A property of references: whether it names anything this version does not lower. */
static GVEC_Result references_refuse(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property,
    bool dangling_counts, bool * refuse, bool * usable) {
  GVEC_Svg_Style_Value v;
  GVEC_Result r = read_property(ctx, inst, property, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  for (size_t i = 0; i < v.reference_count; i++) {
    const GVEC_Svg_Style_Reference * ref = &v.references[i];
    if (!ref->is_url || !ref->local) {
      *refuse = true; /* A function, a basic shape, or a reference outside the document. */
    }
    else if (ref->target || (dangling_counts && !ref->cut)) {
      *refuse = true;
    }
  }
  return GVEC_OK;
}

/*
 * The part of an element's preparation every kind shares. Sets `draw` false, with a refusal where one is due, when the element
 * is not drawn.
 *
 * @param leaf Shapes and the elements that are only refused: nothing is checked if they cannot be seen.
 */
static GVEC_Result prepare(
    gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, bool leaf, bool markers, common * c) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  GVEC_Svg_Style_Value v;
  bool usable;
  GVEC_Result r;
  c->draw = false;
  c->visible = true;
  c->opacity = 1;
  if (gvec_svg_element_conditional_false(e)) {
    return GVEC_OK;
  }
  r = read_property(ctx, inst, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable || keyword_is(&v, "none")) {
    return r;
  }
  r = read_property(ctx, inst, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  c->visible = !(keyword_is(&v, "hidden") || keyword_is(&v, "collapse"));
  if (leaf && !c->visible) {
    return GVEC_OK;
  }
  /* The features of the later stories refuse the element whole, so that nothing is half drawn. */
  bool refuse = false;
  r = references_refuse(ctx, inst, GVEC_SVG_PROP_CLIP_PATH, false, &refuse, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (refuse) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, "clip-path is not lowered yet");
  }
  r = references_refuse(ctx, inst, GVEC_SVG_PROP_MASK, true, &refuse, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (refuse) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_MASK, "mask is not lowered yet");
  }
  r = references_refuse(ctx, inst, GVEC_SVG_PROP_FILTER, true, &refuse, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (refuse) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_FILTER, "filter is not lowered yet");
  }
  r = read_property(ctx, inst, GVEC_SVG_PROP_MIX_BLEND_MODE, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!keyword_is(&v, "normal")) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_BLEND, "mix-blend-mode is not lowered yet");
  }
  if (markers) {
    static const GVEC_Svg_Property marker_properties[3] = {
        GVEC_SVG_PROP_MARKER_START, GVEC_SVG_PROP_MARKER_MID, GVEC_SVG_PROP_MARKER_END};
    for (int i = 0; i < 3; i++) {
      r = references_refuse(ctx, inst, marker_properties[i], false, &refuse, &usable);
      if (r != GVEC_OK || !usable) {
        return r;
      }
    }
    if (refuse) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_MARKER, "markers are not lowered yet");
    }
  }
  r = read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (v.transform.depends_on_box) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TRANSFORM_BOX, "a transform with percentages of a bounding box is not lowered yet");
  }
  if (!gvec_lw_compose(frame, &v.transform.map, &c->local)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's transform leaves the range the waist can carry");
  }
  r = read_property(ctx, inst, GVEC_SVG_PROP_OPACITY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  c->opacity = v.kind == GVEC_SVG_STYLE_NUMBER ? v.number : 1;
  c->draw = c->opacity > 0; /* Opacity zero draws nothing, whatever is inside. */
  return GVEC_OK;
}

/*
 * Enter a wrapper (an opacity group or a clip): the walk is one level deeper and one clip or group more open until it leaves.
 * Refuses the element when the scene's limits would not hold what is inside.
 */
static GVEC_Result wrap_enter(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, bool * entered) {
  *entered = false;
  if (ctx->levels + 2 > ctx->caps.node_depth || ctx->open + 1 > ctx->caps.open) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the drawing nests deeper than the scene's limits allow");
  }
  ctx->levels++;
  ctx->open++;
  *entered = true;
  return GVEC_OK;
}

static void wrap_leave(gvec_lw_ctx * ctx) {
  ctx->levels--;
  ctx->open--;
}

/*
 * A group node for an element's opacity, entered, or NULL when it has none to carry. `skip` is set when the element was refused
 * for the limits (then nothing is entered). A caller that gets a group leaves it with wrap_leave().
 */
static GVEC_Result opacity_group(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, double opacity, gvec_lw_node ** out, bool * skip) {
  *out = NULL;
  *skip = false;
  if (!(opacity < 1)) {
    return GVEC_OK;
  }
  bool entered;
  GVEC_Result r = wrap_enter(ctx, e, &entered);
  if (r != GVEC_OK || !entered) {
    *skip = true;
    return r;
  }
  gvec_lw_node * g = gvec_lw_new_node(ctx, GVEC_LW_GROUP);
  if (!g) {
    return GVEC_ERR_OOM;
  }
  g->opacity = opacity;
  g->key = gvec_lw_key(e->key, ctx->chain, GVEC_LW_ROLE_OPACITY);
  g->site = gvec_lw_new_site(ctx, e, g->key);
  if (g->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  *out = g;
  return GVEC_OK;
}

/* The tree an element's content is recorded under: its opacity group when it has one. */
#define TARGET(group, parent) ((group) ? (group) : (parent))

static GVEC_Result attach(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * node) {
  return node->content ? gvec_lw_append(ctx, parent, node) : GVEC_OK;
}

//-----------------------------------------------------------------------------
// The walk
//-----------------------------------------------------------------------------

static GVEC_Result lw_element(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent);

static GVEC_Result lw_children(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  for (const GVEC_Svg_Element * child = gvec_svg_element_first_child(e); child; child = gvec_svg_element_next_sibling(child)) {
    const GVEC_Svg_Instance * ci = NULL;
    GVEC_Result r = gvec_svg_instance_child(ctx->style, inst, child, &ci);
    if (r == GVEC_ERR_LIMIT) {
      /* More instances than the document and its references allow: reported by the style layer; stop here. */
      return gvec_lw_refuse(ctx, child, GVEC_LOWER_LIMIT, "more elements are drawn than the limits allow");
    }
    if (r != GVEC_OK) {
      return r;
    }
    r = lw_element(ctx, ci, frame, parent);
    if (r != GVEC_OK) {
      return r;
    }
  }
  return GVEC_OK;
}

/* `g` and `a`: the transform is the frame of the children, and nothing else is drawn. */
static GVEC_Result lw_group(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, false, false, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  gvec_lw_node * group;
  bool skip;
  r = opacity_group(ctx, gvec_svg_instance_element(inst), c.opacity, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  r = lw_children(ctx, inst, &c.local, TARGET(group, parent));
  if (group) {
    wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = attach(ctx, parent, group);
    }
  }
  return r;
}

/* `switch`: the child it selected, in the frame of its own transform. */
static GVEC_Result lw_switch(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, false, false, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  const GVEC_Svg_Element * selected = gvec_svg_element_switch_selected(e);
  if (!selected) {
    return GVEC_OK;
  }
  gvec_lw_node * group;
  bool skip;
  r = opacity_group(ctx, e, c.opacity, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  const GVEC_Svg_Instance * ci = NULL;
  r = gvec_svg_instance_child(ctx->style, inst, selected, &ci);
  if (r == GVEC_ERR_LIMIT) {
    r = gvec_lw_refuse(ctx, selected, GVEC_LOWER_LIMIT, "more elements are drawn than the limits allow");
  }
  else if (r == GVEC_OK) {
    r = lw_element(ctx, ci, &c.local, TARGET(group, parent));
  }
  if (group) {
    wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = attach(ctx, parent, group);
    }
  }
  return r;
}

/* The solid paint of a fill, or the reason it is not drawn. */
typedef enum fill_decision {
  FILL_NONE = 0, ///< Nothing is painted.
  FILL_PAINT,    ///< `paint` is what to paint.
  FILL_REFUSED   ///< The element was refused.
} fill_decision;

static GVEC_Result fill_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GSHSCENE_Paint * paint, GSHGEOM_Fill_Rule * rule,
    fill_decision * decision, bool * stroked) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  GVEC_Svg_Style_Value v;
  bool usable;
  GVEC_Result r;
  *decision = FILL_REFUSED;
  *stroked = false;
  r = read_property(ctx, inst, GVEC_SVG_PROP_STROKE, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (v.paint.kind != GVEC_SVG_PAINT_NONE) {
    double width = 1, opacity = 1;
    GVEC_Svg_Style_Value w;
    r = read_property(ctx, inst, GVEC_SVG_PROP_STROKE_WIDTH, GVEC_LOWER_STYLE, &w, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    width = w.number;
    r = read_property(ctx, inst, GVEC_SVG_PROP_STROKE_OPACITY, GVEC_LOWER_STYLE, &w, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    opacity = w.number;
    /* A stroke that is invisible (no width, no opacity) draws nothing, exactly. */
    *stroked = width > 0 && opacity > 0;
  }
  r = read_property(ctx, inst, GVEC_SVG_PROP_FILL, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  GVEC_Svg_Paint fill = v.paint;
  GVEC_Svg_Style_Value n;
  r = read_property(ctx, inst, GVEC_SVG_PROP_FILL_OPACITY, GVEC_LOWER_STYLE, &n, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  double fill_opacity = n.kind == GVEC_SVG_STYLE_NUMBER ? n.number : 1;
  r = read_property(ctx, inst, GVEC_SVG_PROP_FILL_RULE, GVEC_LOWER_STYLE, &n, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  *rule = keyword_is(&n, "evenodd") ? GSHGEOM_FILL_EVEN_ODD : GSHGEOM_FILL_NONZERO;
  *decision = FILL_NONE;
  switch (fill.kind) {
  case GVEC_SVG_PAINT_NONE:
    return GVEC_OK;
  case GVEC_SVG_PAINT_COLOR: {
    memset(paint, 0, sizeof *paint);
    double alpha = fill.color.color.rgba[3] * fill_opacity;
    if (fill.color.kind == GVEC_SVG_COLOR_FOREGROUND) {
      paint->kind = GSHSCENE_PAINT_FOREGROUND;
      paint->rgba[3] = alpha;
    }
    else {
      paint->kind = GSHSCENE_PAINT_SOLID;
      paint->rgba[0] = fill.color.color.rgba[0];
      paint->rgba[1] = fill.color.color.rgba[1];
      paint->rgba[2] = fill.color.color.rgba[2];
      paint->rgba[3] = alpha;
      paint->space = fill.color.color.space;
    }
    *decision = alpha > 0 ? FILL_PAINT : FILL_NONE;
    return GVEC_OK;
  }
  case GVEC_SVG_PAINT_SERVER: {
    GVEC_Lower_Reason reason = gvec_svg_element_kind(fill.server) == GVEC_SVG_KIND_PATTERN ? GVEC_LOWER_PATTERN : GVEC_LOWER_GRADIENT;
    *decision = FILL_REFUSED;
    return gvec_lw_refuse(ctx, e, reason, reason == GVEC_LOWER_PATTERN ? "a pattern paint is not lowered yet" : "a gradient paint is not lowered yet");
  }
  case GVEC_SVG_PAINT_CONTEXT_FILL:
  case GVEC_SVG_PAINT_CONTEXT_STROKE:
  default:
    *decision = FILL_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_CONTEXT_PAINT, "context-fill and context-stroke are not lowered yet");
  }
}

static GVEC_Result lw_shape(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  GVEC_Svg_Kind kind = gvec_svg_element_kind(e);
  bool marked = kind == GVEC_SVG_KIND_PATH || kind == GVEC_SVG_KIND_LINE || kind == GVEC_SVG_KIND_POLYLINE || kind == GVEC_SVG_KIND_POLYGON;
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, true, marked, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  GSHSCENE_Paint paint;
  GSHGEOM_Fill_Rule rule = GSHGEOM_FILL_NONZERO;
  fill_decision decision;
  bool stroked;
  r = fill_of(ctx, inst, &paint, &rule, &decision, &stroked);
  if (r != GVEC_OK || decision == FILL_REFUSED) {
    return r;
  }
  if (stroked) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_STROKE, "strokes are not lowered yet");
  }
  if (decision == FILL_NONE) {
    return GVEC_OK;
  }
  switch (kind) {
  case GVEC_SVG_KIND_RECT:
  case GVEC_SVG_KIND_CIRCLE:
  case GVEC_SVG_KIND_ELLIPSE:
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_RECT, "rect, circle and ellipse wait for the shape family's exact primitives");
  case GVEC_SVG_KIND_LINE:
    return GVEC_OK; /* A line has no area. */
  default:
    break;
  }
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * path = NULL;
  r = gvec_lw_path_of(ctx, e, &geometry, &path);
  if (r != GVEC_OK) {
    return r;
  }
  if (geometry == GVEC_LW_GEOMETRY_ARC) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_ARC, "an elliptical arc waits for the shape family's exact primitive");
  }
  if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
  }
  if (geometry == GVEC_LW_GEOMETRY_EMPTY) {
    return GVEC_OK;
  }
  gvec_lw_node * fill = gvec_lw_new_node(ctx, GVEC_LW_FILL);
  if (!fill) {
    return GVEC_ERR_OOM;
  }
  fill->map = c.local;
  fill->resource = path;
  fill->rule = rule;
  fill->paint = paint;
  fill->content = true;
  fill->key = gvec_lw_key(e->key, ctx->chain, GVEC_LW_ROLE_SHAPE);
  fill->site = gvec_lw_new_site(ctx, e, fill->key);
  if (fill->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  gvec_lw_node * group;
  bool skip;
  r = opacity_group(ctx, e, c.opacity, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  if (ctx->levels + 1 > ctx->caps.node_depth) {
    r = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the drawing nests deeper than the scene's limits allow");
  }
  else {
    r = gvec_lw_append(ctx, TARGET(group, parent), fill);
  }
  if (group) {
    wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = attach(ctx, parent, group);
    }
  }
  return r;
}

//-----------------------------------------------------------------------------
// Viewports: `svg`, and `symbol` drawn by a `use`
//-----------------------------------------------------------------------------

static GVEC_Result read_length(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, double fallback,
    double * out, bool * usable) {
  GVEC_Svg_Style_Value v;
  GVEC_Result r = read_property(ctx, inst, property, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  *out = v.kind == GVEC_SVG_STYLE_LENGTH && !v.length_auto ? v.number : fallback;
  return GVEC_OK;
}

static GVEC_Result lw_viewport(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, false, false, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  bool outermost = gvec_svg_instance_parent(inst) == NULL;
  double vw, vh;
  gvec_svg_instance_viewport(inst, &vw, &vh);
  double x = 0, y = 0, w = vw, h = vh;
  bool usable;
  if (!outermost) {
    r = read_length(ctx, inst, GVEC_SVG_PROP_X, 0, &x, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    r = read_length(ctx, inst, GVEC_SVG_PROP_Y, 0, &y, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
  }
  r = read_length(ctx, inst, GVEC_SVG_PROP_WIDTH, vw, &w, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  r = read_length(ctx, inst, GVEC_SVG_PROP_HEIGHT, vh, &h, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  bool has_w, has_h;
  double ow = 0, oh = 0;
  gvec_svg_instance_override(inst, &has_w, &ow, &has_h, &oh);
  if (has_w) {
    w = ow;
  }
  if (has_h) {
    h = oh;
  }
  GVEC_Svg_View_Box box;
  bool has_box = false;
  const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(e, "viewBox");
  if (attribute && gvec_svg_attribute_class(attribute) == GVEC_SVG_ATTR_TYPED) {
    has_box = gvec_svg_attribute_view_box(attribute, &box);
  }
  if (!(w > 0) || !(h > 0) || (has_box && (!(box.width > 0) || !(box.height > 0)))) {
    return gvec_diag_add(ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_RENDERING_DISABLED, 0, e->node.line, e->node.column,
        e->node.offset, e->name, "a viewport or viewBox of no size disables the rendering of the element");
  }
  GSHGEOM_Map inner;
  if (gshgeom_map_translate(2, x, y, 0, &inner) != GSHGEOM_OK) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewport is past the range the waist can carry");
  }
  if (has_box) {
    GVEC_Svg_Preserve_Aspect_Ratio par = {GVEC_SVG_ALIGN_X_MID_Y_MID, 0, 0};
    const GVEC_Svg_Attribute * p = gvec_svg_element_find_attribute(e, "preserveAspectRatio");
    if (p && gvec_svg_attribute_class(p) == GVEC_SVG_ATTR_TYPED) {
      gvec_svg_attribute_preserve_aspect_ratio(p, &par);
    }
    GSHGEOM_Map view;
    if (!gvec_lw_viewbox_map(&box, &par, w, h, &view) || !gvec_lw_compose(&inner, &view, &inner)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewBox is past the range the waist can carry");
    }
  }
  GSHGEOM_Map children_frame;
  if (!gvec_lw_compose(&c.local, &inner, &children_frame)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewport is past the range the waist can carry");
  }
  GVEC_Svg_Style_Value overflow;
  r = read_property(ctx, inst, GVEC_SVG_PROP_OVERFLOW, GVEC_LOWER_STYLE, &overflow, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  bool clips = !(keyword_is(&overflow, "visible") || keyword_is(&overflow, "auto"));
  gvec_lw_node * clip = NULL;
  if (clips) {
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * rect = NULL;
    r = gvec_lw_rect_path(ctx, x, y, w, h, &geometry, &rect);
    if (r != GVEC_OK) {
      return r;
    }
    if (geometry != GVEC_LW_GEOMETRY_PATH) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewport is past the range the waist can carry");
    }
    clip = gvec_lw_new_node(ctx, GVEC_LW_CLIP);
    if (!clip) {
      return GVEC_ERR_OOM;
    }
    clip->map = c.local;
    clip->resource = rect;
    clip->rule = GSHGEOM_FILL_NONZERO;
    clip->key = gvec_lw_key(e->key, ctx->chain, GVEC_LW_ROLE_CLIP);
    clip->site = gvec_lw_new_site(ctx, e, clip->key);
    if (clip->site == GVEC_LW_NO_SITE) {
      return GVEC_ERR_OOM;
    }
  }
  gvec_lw_node * group;
  bool skip;
  r = opacity_group(ctx, e, c.opacity, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  gvec_lw_node * target = TARGET(group, parent);
  bool clip_entered = false;
  if (clip) {
    r = wrap_enter(ctx, e, &clip_entered);
  }
  if (r == GVEC_OK && (!clip || clip_entered)) {
    r = lw_children(ctx, inst, &children_frame, clip ? clip : target);
    if (clip) {
      wrap_leave(ctx);
      if (r == GVEC_OK) {
        r = attach(ctx, target, clip);
      }
    }
  }
  if (group) {
    wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = attach(ctx, parent, group);
    }
  }
  return r;
}

//-----------------------------------------------------------------------------
// `use`
//-----------------------------------------------------------------------------

static GVEC_Result push_use(gvec_lw_ctx * ctx, const GVEC_Svg_Element * use) {
  if (ctx->use_stack_count == ctx->use_stack_capacity) {
    size_t capacity = ctx->use_stack_capacity ? ctx->use_stack_capacity * 2 : 16, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof *ctx->use_stack, &bytes)) {
      return GVEC_ERR_OOM;
    }
    void * fresh = gvec_realloc(ctx->allocator, (void *) ctx->use_stack, bytes);
    if (!fresh) {
      return GVEC_ERR_OOM;
    }
    ctx->use_stack = fresh;
    ctx->use_stack_capacity = capacity;
  }
  ctx->use_stack[ctx->use_stack_count++] = use;
  return GVEC_OK;
}

static GVEC_Result lw_use(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, false, false, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  const GVEC_Svg_Element * target = gvec_svg_element_href_target(e);
  if (!target) {
    /* Why there is no referent: a cycle or an expansion past the limit (both diagnosed when the document was loaded), a
     * reference to another document (never fetched), or a reference to nothing (diagnosed; the use draws nothing). */
    if (gvec_svg_element_on_cycle(e)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_REFERENCE_CYCLE, "the use is on a reference cycle");
    }
    if (gvec_svg_element_expansion_limited(e)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_EXPANSION_LIMIT, "the use instantiates more elements than the limit allows");
    }
    const GVEC_Svg_Attribute * href = gvec_svg_element_find_attribute(e, "href");
    if (!href) {
      href = gvec_svg_element_find_attribute(e, "xlink:href");
    }
    GVEC_Svg_Reference ref;
    if (href && gvec_svg_attribute_class(href) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_reference(href, &ref) &&
        (ref.kind == GVEC_SVG_REF_EXTERNAL || ref.kind == GVEC_SVG_REF_DATA)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_EXTERNAL_REFERENCE, "the use refers to something outside the document, which is never fetched");
    }
    return GVEC_OK;
  }
  for (size_t i = 0; i < ctx->use_stack_count; i++) {
    if (ctx->use_stack[i] == e) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_REFERENCE_CYCLE, "the use is on a reference cycle");
    }
  }
  double x, y;
  bool usable;
  r = read_length(ctx, inst, GVEC_SVG_PROP_X, 0, &x, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  r = read_length(ctx, inst, GVEC_SVG_PROP_Y, 0, &y, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  GSHGEOM_Map shift, placement;
  if (gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&c.local, &shift, &placement)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the use is placed past the range the waist can carry");
  }
  const GVEC_Svg_Instance * clone = NULL;
  r = gvec_svg_instance_use(ctx->style, inst, &clone);
  if (r == GVEC_ERR_LIMIT) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_EXPANSION_LIMIT, "the use instantiates more elements than the limit allows");
  }
  if (r != GVEC_OK) {
    return r;
  }
  gvec_lw_node * use = gvec_lw_new_node(ctx, GVEC_LW_USE);
  gvec_lw_node * group;
  if (!use) {
    return GVEC_ERR_OOM;
  }
  use->map = placement;
  use->key = gvec_lw_key(e->key, ctx->chain, GVEC_LW_ROLE_INSTANCE);
  use->container_key = gvec_lw_key(e->key, ctx->chain, GVEC_LW_ROLE_CONTAINER);
  use->site = gvec_lw_new_site(ctx, e, use->key);
  use->container_site = gvec_lw_new_site(ctx, e, use->container_key);
  if (use->site == GVEC_LW_NO_SITE || use->container_site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  bool skip;
  r = opacity_group(ctx, e, c.opacity, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  /* The instance is a level and its target another, and the picture is a list of its own. */
  if (ctx->levels + 3 > ctx->caps.node_depth || ctx->picture_depth + 1 > ctx->caps.picture_depth) {
    if (group) {
      wrap_leave(ctx);
    }
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the use nests pictures deeper than the scene's limits allow");
  }
  /* The referent is drawn inside the use: its keys are those of the chain through the use. */
  uint64_t saved_chain = ctx->chain;
  size_t saved_open = ctx->open;
  ctx->levels += 2;
  ctx->picture_depth++;
  ctx->open = 0;
  uint8_t buf[16];
  for (int i = 0; i < 8; i++) {
    buf[i] = (uint8_t) (saved_chain >> (8 * i));
    buf[8 + i] = (uint8_t) (e->key >> (8 * i));
  }
  ctx->chain = gvec_svg_hash64(buf, sizeof buf);
  ctx->use_depth++;
  r = push_use(ctx, e);
  if (r == GVEC_OK) {
    GSHGEOM_Map identity;
    r = gshgeom_map_identity(2, &identity) == GSHGEOM_OK ? lw_element(ctx, clone, &identity, use) : GVEC_ERR_INTERNAL;
    ctx->use_stack_count--;
  }
  ctx->use_depth--;
  ctx->chain = saved_chain;
  ctx->levels -= 2;
  ctx->picture_depth--;
  ctx->open = saved_open;
  if (r == GVEC_OK) {
    r = attach(ctx, TARGET(group, parent), use);
  }
  if (group) {
    wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = attach(ctx, parent, group);
    }
  }
  return r;
}

//-----------------------------------------------------------------------------
// Elements that are refused whole
//-----------------------------------------------------------------------------

/* An element of a kind that is only refused: unless it is removed by a condition or by `display` (or, for a leaf, cannot be seen),
 * it is refused whole with `reason`. */
static GVEC_Result lw_refused_kind(
    gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Lower_Reason reason, const char * message, bool leaf) {
  GVEC_Svg_Style_Value v;
  bool usable;
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  if (gvec_svg_element_conditional_false(e)) {
    return GVEC_OK;
  }
  GVEC_Result r = read_property(ctx, inst, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable || keyword_is(&v, "none")) {
    return r;
  }
  if (leaf) {
    r = read_property(ctx, inst, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable || keyword_is(&v, "hidden") || keyword_is(&v, "collapse")) {
      return r;
    }
  }
  return gvec_lw_refuse(ctx, e, reason, message);
}

static GVEC_Result lw_element(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  if (ctx->depth >= ctx->max_depth) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the drawing nests deeper than the limit allows once references are expanded");
  }
  GVEC_Result r;
  ctx->depth++;
  switch (gvec_svg_element_kind(e)) {
  case GVEC_SVG_KIND_G:
  case GVEC_SVG_KIND_A:
    r = lw_group(ctx, inst, frame, parent);
    break;
  case GVEC_SVG_KIND_SWITCH:
    r = lw_switch(ctx, inst, frame, parent);
    break;
  case GVEC_SVG_KIND_SVG:
    r = lw_viewport(ctx, inst, frame, parent);
    break;
  case GVEC_SVG_KIND_SYMBOL: {
    /* A symbol is drawn only as the referent of a use. */
    const GVEC_Svg_Instance * use = gvec_svg_instance_use_context(inst);
    r = use && gvec_svg_instance_parent(inst) == use ? lw_viewport(ctx, inst, frame, parent) : GVEC_OK;
    break;
  }
  case GVEC_SVG_KIND_USE:
    r = lw_use(ctx, inst, frame, parent);
    break;
  case GVEC_SVG_KIND_PATH:
  case GVEC_SVG_KIND_POLYGON:
  case GVEC_SVG_KIND_POLYLINE:
  case GVEC_SVG_KIND_LINE:
  case GVEC_SVG_KIND_RECT:
  case GVEC_SVG_KIND_CIRCLE:
  case GVEC_SVG_KIND_ELLIPSE:
    r = lw_shape(ctx, inst, frame, parent);
    break;
  case GVEC_SVG_KIND_TEXT:
    r = lw_refused_kind(ctx, inst, GVEC_LOWER_TEXT, "text is not lowered yet", false);
    break;
  case GVEC_SVG_KIND_IMAGE:
    r = lw_refused_kind(ctx, inst, GVEC_LOWER_IMAGE, "images are not lowered yet", true);
    break;
  case GVEC_SVG_KIND_FOREIGN_OBJECT:
    r = lw_refused_kind(ctx, inst, GVEC_LOWER_FOREIGN_OBJECT, "foreignObject content is never rendered by this library", false);
    break;
  case GVEC_SVG_KIND_AUDIO:
  case GVEC_SVG_KIND_VIDEO:
  case GVEC_SVG_KIND_CANVAS:
  case GVEC_SVG_KIND_IFRAME:
    r = lw_refused_kind(ctx, inst, GVEC_LOWER_EMBEDDED_CONTENT, "embedded content is never rendered by this library", true);
    break;
  default:
    /* Never rendered by definition: definitions, descriptions, paint servers, markers, clip paths, masks, filters, styles,
     * scripts, animation, and what SVG 2 does not define. They are not refusals. */
    r = GVEC_OK;
    break;
  }
  ctx->depth--;
  /* The element and everything under it are done: its style is not read again. */
  gvec_svg_instance_release(ctx->style, inst);
  return r;
}

GVEC_Result gvec_lw_walk(gvec_lw_ctx * ctx) {
  const GVEC_Svg_Instance * root = gvec_svg_instance_root(ctx->style);
  if (!root) {
    return GVEC_ERR_OOM;
  }
  GSHGEOM_Map identity;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  return lw_element(ctx, root, &identity, &ctx->root);
}
