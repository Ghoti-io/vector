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
#include <strings.h>

#include <ghoti.io/cutil/safemath.h>

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/glyph.h>

#include "../../../codec/codec_internal.h"
#include "../style/svg_style.h"
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

bool gvec_lw_invert(const GSHGEOM_Map * map, GSHGEOM_Map * out) {
  double a = map->m[0][0], b = map->m[0][1], c = map->m[1][0], d = map->m[1][1];
  double det = a * d - b * c;
  if (!(det != 0) || det - det != 0) {
    return false;
  }
  const double linear[4] = {d / det, -b / det, -c / det, a / det};
  const double shift[2] = {-(linear[0] * map->t[0] + linear[1] * map->t[1]), -(linear[2] * map->t[0] + linear[3] * map->t[1])};
  GSHGEOM_Map inverse;
  return gshgeom_map_from_matrix(2, linear, shift, &inverse) == GSHGEOM_OK && gvec_lw_normal(&inverse, out);
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
  case GVEC_LW_STROKE:
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

/* Whether `parent` can take `count` more children of the given totals without the tree leaving the scene's limits. */
static bool room(const gvec_lw_ctx * ctx, const gvec_lw_node * parent, size_t weight, size_t occurrences, size_t nodes, size_t items) {
  bool fits = saturating_add(parent->weight, weight) <= ctx->caps.expansion &&
              saturating_add(parent->occurrences, occurrences) <= ctx->caps.expansion &&
              saturating_add(parent->nodes, nodes) <= ctx->caps.nodes;
  /* The items of a list are known where the list begins: at the root, and in a picture. */
  if (parent == &ctx->root) {
    fits = fits && saturating_add(parent->items, items) <= ctx->caps.list_items;
  }
  else if (parent->kind == GVEC_LW_USE) {
    fits = fits && saturating_add(parent->inner_items, items) <= ctx->caps.list_items;
  }
  return fits;
}

static void link_child(gvec_lw_node * parent, gvec_lw_node * child) {
  if (parent->last) {
    parent->last->next = child;
  }
  else {
    parent->first = child;
  }
  parent->last = child;
  parent->weight = saturating_add(parent->weight, child->weight);
  parent->occurrences = saturating_add(parent->occurrences, child->occurrences);
  parent->nodes = saturating_add(parent->nodes, child->nodes);
  if (parent->kind == GVEC_LW_USE) {
    parent->inner_items = saturating_add(parent->inner_items, child->items);
  }
  else {
    parent->items = saturating_add(parent->items, child->items);
  }
  parent->content = parent->content || child->content;
}

GVEC_Result gvec_lw_append(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * child) {
  if (!room(ctx, parent, child->weight, child->occurrences, child->nodes, child->items)) {
    const GVEC_Svg_Element * element = child->site != GVEC_LW_NO_SITE ? ctx->sites[child->site].element : NULL;
    return element ? gvec_lw_refuse(ctx, element, GVEC_LOWER_LIMIT, "the drawing is larger than the scene's limits allow") : GVEC_OK;
  }
  link_child(parent, child);
  return GVEC_OK;
}

GVEC_Result gvec_lw_append_pair(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * first, gvec_lw_node * second) {
  if (!room(ctx, parent, saturating_add(first->weight, second->weight), saturating_add(first->occurrences, second->occurrences),
          saturating_add(first->nodes, second->nodes), saturating_add(first->items, second->items))) {
    const GVEC_Svg_Element * element = first->site != GVEC_LW_NO_SITE ? ctx->sites[first->site].element : NULL;
    return element ? gvec_lw_refuse(ctx, element, GVEC_LOWER_LIMIT, "the drawing is larger than the scene's limits allow") : GVEC_OK;
  }
  link_child(parent, first);
  link_child(parent, second);
  return GVEC_OK;
}

GVEC_Result gvec_lw_append_all(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * const * nodes, size_t count) {
  size_t weight = 0, occurrences = 0, node_count = 0, items = 0;
  for (size_t i = 0; i < count; i++) {
    weight = saturating_add(weight, nodes[i]->weight);
    occurrences = saturating_add(occurrences, nodes[i]->occurrences);
    node_count = saturating_add(node_count, nodes[i]->nodes);
    items = saturating_add(items, nodes[i]->items);
  }
  if (!room(ctx, parent, weight, occurrences, node_count, items)) {
    const GVEC_Svg_Element * element = count && nodes[0]->site != GVEC_LW_NO_SITE ? ctx->sites[nodes[0]->site].element : NULL;
    return element ? gvec_lw_refuse(ctx, element, GVEC_LOWER_LIMIT, "the drawing is larger than the scene's limits allow") : GVEC_OK;
  }
  for (size_t i = 0; i < count; i++) {
    link_child(parent, nodes[i]);
  }
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
    ctx->caps.marker_instances = ctx->caps.nodes / 2;
    if (options && options->max_marker_instances && options->max_marker_instances < ctx->caps.marker_instances) {
      ctx->caps.marker_instances = options->max_marker_instances;
    }
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

bool gvec_lw_keyword_is(const GVEC_Svg_Style_Value * v, const char * word) {
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
GVEC_Result gvec_lw_read_property(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property,
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
  uint32_t composite;  ///< `mix-blend-mode` as a GSHSCENE_Composite; 0 (source-over) for `normal`.
  bool isolate;        ///< `isolation: isolate`.
} common;

/* A property of references: whether it names anything this version does not lower. */
static GVEC_Result references_refuse(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property,
    bool dangling_counts, bool * refuse, bool * usable) {
  GVEC_Svg_Style_Value v;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, property, GVEC_LOWER_STYLE, &v, usable);
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

/* `mix-blend-mode` as the waist's operator: the sixteen values of Compositing and Blending 1 (`normal` is source-over), 0 for anything else. */
static uint32_t blend_composite(const GVEC_Svg_Style_Value * v) {
  static const struct {
    const char * word;
    uint32_t op;
  } modes[] = {
      {"multiply", GSHSCENE_COMPOSITE_MULTIPLY}, {"screen", GSHSCENE_COMPOSITE_SCREEN}, {"overlay", GSHSCENE_COMPOSITE_OVERLAY},
      {"darken", GSHSCENE_COMPOSITE_DARKEN}, {"lighten", GSHSCENE_COMPOSITE_LIGHTEN}, {"color-dodge", GSHSCENE_COMPOSITE_COLOR_DODGE},
      {"color-burn", GSHSCENE_COMPOSITE_COLOR_BURN}, {"hard-light", GSHSCENE_COMPOSITE_HARD_LIGHT},
      {"soft-light", GSHSCENE_COMPOSITE_SOFT_LIGHT}, {"difference", GSHSCENE_COMPOSITE_DIFFERENCE},
      {"exclusion", GSHSCENE_COMPOSITE_EXCLUSION}, {"hue", GSHSCENE_COMPOSITE_HUE}, {"saturation", GSHSCENE_COMPOSITE_SATURATION},
      {"color", GSHSCENE_COMPOSITE_COLOR}, {"luminosity", GSHSCENE_COMPOSITE_LUMINOSITY},
  };
  for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
    if (gvec_lw_keyword_is(v, modes[i].word)) {
      return modes[i].op;
    }
  }
  return 0;
}

/*
 * The part of an element's preparation every kind shares. Sets `draw` false, with a refusal where one is due, when the element
 * is not drawn.
 *
 * @param leaf Shapes and the elements that are only refused: nothing is checked if they cannot be seen.
 */
static GVEC_Result prepare(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, bool leaf, common * c) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  GVEC_Svg_Style_Value v;
  bool usable;
  GVEC_Result r;
  c->draw = false;
  c->visible = true;
  c->opacity = 1;
  c->composite = 0;
  c->isolate = false;
  if (gvec_svg_element_conditional_false(e)) {
    return GVEC_OK;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable || gvec_lw_keyword_is(&v, "none")) {
    return r;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  c->visible = !(gvec_lw_keyword_is(&v, "hidden") || gvec_lw_keyword_is(&v, "collapse"));
  if (leaf && !c->visible) {
    return GVEC_OK;
  }
  /* The features of the later stories refuse the element whole, so that nothing is half drawn. */
  bool refuse = false; /* `clip-path` and `mask` are lowered by the wrappers around the element (lower_clip.c, lower_mask.c). */
  r = references_refuse(ctx, inst, GVEC_SVG_PROP_FILTER, true, &refuse, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (refuse) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_FILTER, "filter is not lowered yet");
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_MIX_BLEND_MODE, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  c->composite = ctx->blend_moved == inst ? 0 : blend_composite(&v);
  if (ctx->blend_moved == inst) {
    ctx->blend_moved = NULL;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_ISOLATION, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  c->isolate = gvec_lw_keyword_is(&v, "isolate");
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (v.transform.depends_on_box) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TRANSFORM_BOX, "a transform with percentages of a bounding box is not lowered yet");
  }
  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_SYMBOL) {
    c->local = *frame; /* A symbol has no transform in SVG 1.1, and the reference corpus follows that. */
  }
  else if (!gvec_lw_compose(frame, &v.transform.map, &c->local)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's transform leaves the range the waist can carry");
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_OPACITY, GVEC_LOWER_STYLE, &v, &usable);
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
GVEC_Result gvec_lw_wrap_enter(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, bool * entered) {
  *entered = false;
  if (ctx->levels + 2 > ctx->caps.node_depth || ctx->open + 1 > ctx->caps.open) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the drawing nests deeper than the scene's limits allow");
  }
  ctx->levels++;
  ctx->open++;
  *entered = true;
  return GVEC_OK;
}

void gvec_lw_wrap_leave(gvec_lw_ctx * ctx) {
  ctx->levels--;
  ctx->open--;
}

/*
 * A group node for an element's opacity, entered, or NULL when it has none to carry. `skip` is set when the element was refused
 * for the limits (then nothing is entered). A caller that gets a group leaves it with wrap_leave().
 */
static GVEC_Result opacity_group(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const common * c, gvec_lw_node ** out, bool * skip) {
  double opacity = c->opacity;
  *out = NULL;
  *skip = false;
  if (!(opacity < 1) && c->composite == 0 && !c->isolate) {
    return GVEC_OK; /* Nothing makes this element a stacking context. */
  }
  bool entered;
  GVEC_Result r = gvec_lw_wrap_enter(ctx, e, &entered);
  if (r != GVEC_OK || !entered) {
    *skip = true;
    return r;
  }
  gvec_lw_node * g = gvec_lw_new_node(ctx, GVEC_LW_GROUP);
  if (!g) {
    return GVEC_ERR_OOM;
  }
  g->opacity = opacity;
  g->composite = c->composite;
  g->isolate = c->isolate;
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

GVEC_Result gvec_lw_attach(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * node) {
  return node->content ? gvec_lw_append(ctx, parent, node) : GVEC_OK;
}

//-----------------------------------------------------------------------------
// The walk
//-----------------------------------------------------------------------------

static GVEC_Result lw_element(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent);

GVEC_Result gvec_lw_children(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
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
  GVEC_Result r = prepare(ctx, inst, frame, false, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  gvec_lw_node * group;
  bool skip;
  r = opacity_group(ctx, gvec_svg_instance_element(inst), &c, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  r = gvec_lw_children(ctx, inst, &c.local, TARGET(group, parent));
  if (group) {
    gvec_lw_wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, parent, group);
    }
  }
  return r;
}

/* `switch`: the child it selected, in the frame of its own transform. */
static GVEC_Result lw_switch(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, false, &c);
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
  r = opacity_group(ctx, e, &c, &group, &skip);
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
    gvec_lw_wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, parent, group);
    }
  }
  return r;
}

/* The paint of a fill and its rule, or the reason there is none. */
static GVEC_Result fill_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GSHSCENE_Paint * paint, GSHGEOM_Fill_Rule * rule,
    gvec_lw_paint_decision * decision) {
  GVEC_Svg_Style_Value v, n;
  bool usable;
  *decision = GVEC_LW_PAINT_REFUSED;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_FILL, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  GVEC_Svg_Paint fill = v.paint;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_FILL_OPACITY, GVEC_LOWER_STYLE, &n, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  double fill_opacity = n.kind == GVEC_SVG_STYLE_NUMBER ? n.number : 1;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_FILL_RULE, GVEC_LOWER_STYLE, &n, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  *rule = gvec_lw_keyword_is(&n, "evenodd") ? GSHGEOM_FILL_EVEN_ODD : GSHGEOM_FILL_NONZERO;
  return gvec_lw_paint_of(ctx, inst, &fill, fill_opacity, paint, decision);
}

bool gvec_lw_absolute_length(const GVEC_Svg_Length * length, double * out) {
  switch (length->unit) {
  case GVEC_SVG_UNIT_NONE:
  case GVEC_SVG_UNIT_PX:
    *out = length->value;
    return true;
  case GVEC_SVG_UNIT_IN:
    *out = length->value * 96.0;
    return true;
  case GVEC_SVG_UNIT_CM:
    *out = length->value * 96.0 / 2.54;
    return true;
  case GVEC_SVG_UNIT_MM:
    *out = length->value * 96.0 / 25.4;
    return true;
  case GVEC_SVG_UNIT_Q:
    *out = length->value * 96.0 / 101.6;
    return true;
  case GVEC_SVG_UNIT_PT:
    *out = length->value * 96.0 / 72.0;
    return true;
  case GVEC_SVG_UNIT_PC:
    *out = length->value * 16.0;
    return true;
  default:
    return false;
  }
}

/* The length of an attribute of a `line` in user units; `usable` is false when the unit needs the cascade (the element is refused). */
static GVEC_Result line_coordinate(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const char * name, bool horizontal, double * out,
    bool * usable) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(e, name);
  GVEC_Svg_Length length;
  *out = 0;
  *usable = true;
  if (!attribute || !gvec_svg_attribute_length(attribute, &length)) {
    return GVEC_OK; /* Absent, or invalid (reported when the document was loaded): 0. */
  }
  if (length.unit == GVEC_SVG_UNIT_PERCENT) {
    double w, h;
    gvec_svg_instance_viewport(inst, &w, &h);
    *out = length.value * (horizontal ? w : h) / 100.0;
    return GVEC_OK;
  }
  if (gvec_lw_absolute_length(&length, out)) {
    return GVEC_OK;
  }
  *usable = false;
  return gvec_lw_refuse(ctx, e, GVEC_LOWER_STYLE, "a length of the line needs the font or the window, which the lowering does not read");
}

GVEC_Result gvec_lw_centerline_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_geometry * geometry,
    const GSHSCENE_Resource ** path, bool * usable) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *path = NULL;
  *usable = true;
  GVEC_Svg_Kind kind = gvec_svg_element_kind(e);
  if (kind == GVEC_SVG_KIND_RECT || kind == GVEC_SVG_KIND_CIRCLE || kind == GVEC_SVG_KIND_ELLIPSE) {
    return gvec_lw_primitive_of(ctx, inst, geometry, path, usable);
  }
  if (kind != GVEC_SVG_KIND_LINE) {
    return gvec_lw_path_of(ctx, e, true, geometry, path);
  }
  double x1, y1, x2, y2;
  GVEC_Result r = line_coordinate(ctx, inst, "x1", true, &x1, usable);
  if (r == GVEC_OK && *usable) {
    r = line_coordinate(ctx, inst, "y1", false, &y1, usable);
  }
  if (r == GVEC_OK && *usable) {
    r = line_coordinate(ctx, inst, "x2", true, &x2, usable);
  }
  if (r == GVEC_OK && *usable) {
    r = line_coordinate(ctx, inst, "y2", false, &y2, usable);
  }
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  return gvec_lw_line_path(ctx, x1, y1, x2, y2, geometry, path);
}

/* The node of a path painted under the element's map: a fill, or a stroke when `style` is given. */
static GVEC_Result shape_node(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const GSHGEOM_Map * map, const GSHSCENE_Resource * path,
    const GSHSCENE_Paint * paint, GSHGEOM_Fill_Rule rule, const GSHSCENE_Resource * style, gvec_lw_node ** out) {
  gvec_lw_node * node = gvec_lw_new_node(ctx, style ? GVEC_LW_STROKE : GVEC_LW_FILL);
  *out = node;
  if (!node) {
    return GVEC_ERR_OOM;
  }
  node->map = *map;
  node->resource = path;
  node->style = style;
  node->rule = rule;
  node->paint = *paint;
  node->crisp = ctx->item_crisp;
  node->content = true;
  node->key = gvec_lw_key(e->key, ctx->chain, style ? GVEC_LW_ROLE_STROKE : GVEC_LW_ROLE_SHAPE);
  node->site = gvec_lw_new_site(ctx, e, node->key);
  return node->site == GVEC_LW_NO_SITE ? GVEC_ERR_OOM : GVEC_OK;
}

static GVEC_Result lw_shape(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  GVEC_Svg_Kind kind = gvec_svg_element_kind(e);
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, true, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  ctx->item_local = c.local;
  {
    GVEC_Svg_Style_Value sr;
    bool sr_usable;
    r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_SHAPE_RENDERING, GVEC_LOWER_STYLE, &sr, &sr_usable);
    if (r != GVEC_OK || !sr_usable) {
      return r;
    }
    ctx->item_crisp = gvec_lw_keyword_is(&sr, "crispedges") || gvec_lw_keyword_is(&sr, "optimizespeed");
  }
  GSHSCENE_Paint paint;
  GSHGEOM_Fill_Rule rule = GSHGEOM_FILL_NONZERO;
  gvec_lw_paint_decision fill_decision;
  r = fill_of(ctx, inst, &paint, &rule, &fill_decision);
  if (r != GVEC_OK || fill_decision == GVEC_LW_PAINT_REFUSED) {
    return r;
  }
  gvec_lw_stroke stroke;
  r = gvec_lw_stroke_of(ctx, inst, &stroke);
  if (r != GVEC_OK || stroke.decision == GVEC_LW_PAINT_REFUSED) {
    return r;
  }
  bool filled = fill_decision == GVEC_LW_PAINT_DRAW; /* A line has no area: its fill path is empty. */
  bool stroked = stroke.decision == GVEC_LW_PAINT_DRAW;
  bool marked = true; /* Every shape has markers (SVG 2, 13.7). */
  if (!filled && !stroked && !marked) {
    return GVEC_OK;
  }
  gvec_lw_node * fill_node = NULL;
  gvec_lw_node * stroke_node = NULL;
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * path = NULL;
  if (filled) {
    if (kind == GVEC_SVG_KIND_RECT || kind == GVEC_SVG_KIND_CIRCLE || kind == GVEC_SVG_KIND_ELLIPSE) {
      bool usable;
      r = gvec_lw_primitive_of(ctx, inst, &geometry, &path, &usable);
      if (r != GVEC_OK || !usable) {
        return r;
      }
    } else {
      r = gvec_lw_path_of(ctx, e, false, &geometry, &path);
      if (r != GVEC_OK) {
        return r;
      }
    }
    if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
    }
    if (geometry == GVEC_LW_GEOMETRY_PATH) {
      r = shape_node(ctx, e, &c.local, path, &paint, rule, NULL, &fill_node);
      if (r != GVEC_OK) {
        return r;
      }
    }
  }
  if (stroked) {
    bool usable;
    r = gvec_lw_centerline_of(ctx, inst, &geometry, &path, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's geometry is past the range the waist can carry");
    }
    if (geometry == GVEC_LW_GEOMETRY_PATH) {
      r = shape_node(ctx, e, &c.local, path, &stroke.paint, GSHGEOM_FILL_NONZERO, stroke.style, &stroke_node);
      if (r != GVEC_OK) {
        return r;
      }
    }
  }
  gvec_lw_marker_set markers = {NULL, 0, false};
  if (marked) {
    r = gvec_lw_markers(ctx, inst, &c.local, &markers);
    if (r != GVEC_OK || markers.refused) {
      return r;
    }
  }
  size_t parts = (fill_node != NULL) + (stroke_node != NULL) + (markers.count > 0);
  if (!parts) {
    return GVEC_OK;
  }
  gvec_lw_part order[3] = {GVEC_LW_PART_FILL, GVEC_LW_PART_STROKE, GVEC_LW_PART_MARKERS};
  if (parts > 1) {
    bool usable;
    r = gvec_lw_paint_order(ctx, inst, order, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
  }
  size_t total = (fill_node != NULL) + (stroke_node != NULL) + markers.count;
  gvec_lw_node ** sequence = gvec_arena_alloc(&ctx->arena, total * sizeof *sequence, sizeof(void *));
  if (!sequence) {
    return GVEC_ERR_OOM;
  }
  size_t n = 0;
  bool used[3] = {false, false, false}; /* Each part once, whatever the order says: a node linked twice would close a loop. */
  for (int i = 0; i < 3; i++) {
    if (order[i] >= 3 || used[order[i]]) {
      continue;
    }
    used[order[i]] = true;
    if (order[i] == GVEC_LW_PART_FILL && fill_node) {
      sequence[n++] = fill_node;
    }
    else if (order[i] == GVEC_LW_PART_STROKE && stroke_node) {
      sequence[n++] = stroke_node;
    }
    else if (order[i] == GVEC_LW_PART_MARKERS) {
      for (size_t k = 0; k < markers.count; k++) {
        sequence[n++] = markers.nodes[k];
      }
    }
  }
  gvec_lw_node * group;
  bool skip;
  r = opacity_group(ctx, e, &c, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  if (ctx->levels + 1 > ctx->caps.node_depth) {
    r = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the drawing nests deeper than the scene's limits allow");
  }
  else {
    r = gvec_lw_append_all(ctx, TARGET(group, parent), sequence, n);
  }
  if (group) {
    gvec_lw_wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, parent, group);
    }
  }
  return r;
}

//-----------------------------------------------------------------------------
// `text`
//-----------------------------------------------------------------------------

/* `text`: its content as glyph runs, each filled and stroked with the paint of the span it is of, in the span's paint order, under the
 * span's opacity; a gradient or a pattern is placed in the box of the whole text. */
static GVEC_Result lw_text(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, true, &c);
  if (r != GVEC_OK || !c.draw) {
    return r;
  }
  ctx->item_local = c.local;
  ctx->item_crisp = false;
  gvec_lw_text text;
  bool usable;
  r = gvec_lw_text_layout(ctx, inst, &text, &usable);
  if (r != GVEC_OK || !usable || text.count == 0) {
    return r;
  }
  ctx->paint_box = text.box;
  gvec_lw_node ** sequence = gvec_arena_alloc(&ctx->arena, 2 * text.count * sizeof *sequence, sizeof(void *));
  if (!sequence) {
    return GVEC_ERR_OOM;
  }
  size_t n = 0;
  for (size_t i = 0; i < text.count; i++) {
    const GVEC_Svg_Instance * span = text.pieces[i].inst;
    GSHGEOM_Map piece_map;
    if (!gvec_lw_compose(&c.local, &text.pieces[i].map, &piece_map)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text's stretch is past the range the waist can carry");
    }
    const GVEC_Svg_Element * se = gvec_svg_instance_element(span);
    GVEC_Svg_Style_Value v;
    /* `text-rendering` of the text element (not of a span) says whether the glyphs are antialiased; `shape-rendering` does not affect text. */
    r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TEXT_RENDERING, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    ctx->item_crisp = gvec_lw_keyword_is(&v, "optimizespeed");
    ctx->paint_box_set = true;
    GSHSCENE_Paint paint;
    GSHGEOM_Fill_Rule rule = GSHGEOM_FILL_NONZERO;
    gvec_lw_paint_decision fill_decision;
    r = fill_of(ctx, span, &paint, &rule, &fill_decision);
    gvec_lw_stroke stroke;
    memset(&stroke, 0, sizeof stroke);
    if (r == GVEC_OK && fill_decision != GVEC_LW_PAINT_REFUSED) {
      r = gvec_lw_stroke_of(ctx, span, &stroke);
    }
    ctx->paint_box_set = false;
    if (r != GVEC_OK || fill_decision == GVEC_LW_PAINT_REFUSED || stroke.decision == GVEC_LW_PAINT_REFUSED) {
      return r;
    }
    gvec_lw_node * fill_node = NULL;
    gvec_lw_node * stroke_node = NULL;
    if (fill_decision == GVEC_LW_PAINT_DRAW) {
      r = shape_node(ctx, e, &piece_map, text.pieces[i].run, &paint, rule, NULL, &fill_node);
      if (r != GVEC_OK) {
        return r;
      }
    }
    if (stroke.decision == GVEC_LW_PAINT_DRAW) {
      r = shape_node(ctx, e, &piece_map, text.pieces[i].run, &stroke.paint, GSHGEOM_FILL_NONZERO, stroke.style, &stroke_node);
      if (r != GVEC_OK) {
        return r;
      }
    }
    if (!fill_node && !stroke_node) {
      continue;
    }
    gvec_lw_node * pair[2];
    size_t parts = 0;
    gvec_lw_part order[3] = {GVEC_LW_PART_FILL, GVEC_LW_PART_STROKE, GVEC_LW_PART_MARKERS};
    if (fill_node && stroke_node) {
      r = gvec_lw_paint_order(ctx, span, order, &usable);
      if (r != GVEC_OK || !usable) {
        return r;
      }
    }
    bool used[3] = {false, false, false};
    for (int k = 0; k < 3; k++) {
      if (order[k] >= 3 || used[order[k]]) {
        continue;
      }
      used[order[k]] = true;
      if (order[k] == GVEC_LW_PART_FILL && fill_node) {
        pair[parts++] = fill_node;
      }
      else if (order[k] == GVEC_LW_PART_STROKE && stroke_node) {
        pair[parts++] = stroke_node;
      }
    }
    /* Each run has its own nodes, told apart by its place among them. */
    uint64_t salt = (uint64_t) (i + 1) * UINT64_C(0x9E3779B97F4A7C15);
    for (size_t k = 0; k < parts; k++) {
      pair[k]->key = gvec_lw_key(e->key, ctx->chain ^ salt ^ (pair[k] == stroke_node ? UINT64_C(0xA5A5A5A5) : 0), pair[k] == stroke_node ? GVEC_LW_ROLE_STROKE : GVEC_LW_ROLE_SHAPE);
      pair[k]->site = gvec_lw_new_site(ctx, e, pair[k]->key);
      if (pair[k]->site == GVEC_LW_NO_SITE) {
        return GVEC_ERR_OOM;
      }
    }
    /* The opacity of a span other than the text itself is a group of its own around what the span paints. */
    double span_opacity = 1;
    if (se != e) {
      r = gvec_lw_read_property(ctx, span, GVEC_SVG_PROP_OPACITY, GVEC_LOWER_STYLE, &v, &usable);
      if (r != GVEC_OK || !usable) {
        return r;
      }
      span_opacity = v.kind == GVEC_SVG_STYLE_NUMBER ? v.number : 1;
    }
    if (span_opacity < 1) {
      if (!(span_opacity > 0)) {
        continue;
      }
      common cc = c;
      cc.opacity = span_opacity;
      cc.composite = 0;
      cc.isolate = false;
      gvec_lw_node * group;
      bool skip;
      r = opacity_group(ctx, se, &cc, &group, &skip);
      if (r != GVEC_OK || skip) {
        return r;
      }
      r = gvec_lw_append_all(ctx, group, pair, parts);
      gvec_lw_wrap_leave(ctx);
      if (r != GVEC_OK) {
        return r;
      }
      sequence[n++] = group;
      continue;
    }
    for (size_t k = 0; k < parts; k++) {
      sequence[n++] = pair[k];
    }
  }
  if (n == 0) {
    return GVEC_OK;
  }
  gvec_lw_node * group;
  bool skip;
  r = opacity_group(ctx, e, &c, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  if (ctx->levels + 1 > ctx->caps.node_depth) {
    r = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the drawing nests deeper than the scene's limits allow");
  }
  else {
    r = gvec_lw_append_all(ctx, TARGET(group, parent), sequence, n);
  }
  if (group) {
    gvec_lw_wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, parent, group);
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
  GVEC_Result r = gvec_lw_read_property(ctx, inst, property, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  *out = v.kind == GVEC_SVG_STYLE_LENGTH && !v.length_auto ? v.number : fallback;
  return GVEC_OK;
}

static GVEC_Result lw_viewport(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  common c;
  GVEC_Result r = prepare(ctx, inst, frame, false, &c);
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
  GSHGEOM_Map view;
  bool has_view = false;
  if (has_box) {
    GVEC_Svg_Preserve_Aspect_Ratio par = {GVEC_SVG_ALIGN_X_MID_Y_MID, 0, 0};
    const GVEC_Svg_Attribute * p = gvec_svg_element_find_attribute(e, "preserveAspectRatio");
    if (p && gvec_svg_attribute_class(p) == GVEC_SVG_ATTR_TYPED) {
      gvec_svg_attribute_preserve_aspect_ratio(p, &par);
    }
    if (!gvec_lw_viewbox_map(&box, &par, w, h, &view)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewBox is past the range the waist can carry");
    }
    has_view = true;
  }
  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_SYMBOL) {
    /* A symbol's reference point is placed at its x, y instead of the corner of its viewport; one that is not given moves
     * nothing in its direction (SVG 2, 5.5.1). The point is in the content's coordinates, after the viewBox. */
    for (int axis = 0; axis < 2; axis++) {
      const GVEC_Svg_Attribute * attribute = gvec_svg_element_find_attribute(e, axis ? "refY" : "refX");
      GVEC_Svg_Length length;
      if (!attribute || gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_length(attribute, &length)) {
        continue;
      }
      double v;
      if (length.unit == GVEC_SVG_UNIT_PERCENT) { /* the keywords: 0%, 50% and 100% of the viewBox, or of the viewport without one */
        double origin = has_box ? (axis ? box.y : box.x) : 0;
        double extent = has_box ? (axis ? box.height : box.width) : (axis ? h : w);
        v = origin + extent * length.value / 100.0;
      }
      else if (!gvec_lw_absolute_length(&length, &v)) {
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_STYLE, "a symbol reference point in a unit that needs the cascade is not lowered");
      }
      GSHGEOM_Point at, ref;
      memset(&ref, 0, sizeof ref);
      ref.x = axis ? 0 : v;
      ref.y = axis ? v : 0;
      if (has_view) {
        if (gshgeom_map_apply_point(&view, ref, &at) != GSHGEOM_OK) {
          return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the reference point is past the range the waist can carry");
        }
      }
      else {
        at = ref;
      }
      if (axis) {
        y -= at.y;
      }
      else {
        x -= at.x;
      }
    }
  }
  GSHGEOM_Map inner;
  if (gshgeom_map_translate(2, x, y, 0, &inner) != GSHGEOM_OK) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewport is past the range the waist can carry");
  }
  if (has_view && !gvec_lw_compose(&inner, &view, &inner)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewBox is past the range the waist can carry");
  }
  GSHGEOM_Map children_frame;
  if (!gvec_lw_compose(&c.local, &inner, &children_frame)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the viewport is past the range the waist can carry");
  }
  GVEC_Svg_Style_Value overflow;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_OVERFLOW, GVEC_LOWER_STYLE, &overflow, &usable);
  if (r != GVEC_OK || !usable) {
    return r;
  }
  bool clips = !(gvec_lw_keyword_is(&overflow, "visible") || gvec_lw_keyword_is(&overflow, "auto"));
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
  r = opacity_group(ctx, e, &c, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  gvec_lw_node * target = TARGET(group, parent);
  bool clip_entered = false;
  if (clip) {
    r = gvec_lw_wrap_enter(ctx, e, &clip_entered);
  }
  if (r == GVEC_OK && (!clip || clip_entered)) {
    r = gvec_lw_children(ctx, inst, &children_frame, clip ? clip : target);
    if (clip) {
      gvec_lw_wrap_leave(ctx);
      if (r == GVEC_OK) {
        r = gvec_lw_attach(ctx, target, clip);
      }
    }
  }
  if (group) {
    gvec_lw_wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, parent, group);
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
  GVEC_Result r = prepare(ctx, inst, frame, false, &c);
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
  r = opacity_group(ctx, e, &c, &group, &skip);
  if (r != GVEC_OK || skip) {
    return r;
  }
  /* The instance is a level and its target another, and the picture is a list of its own. */
  if (ctx->levels + 3 > ctx->caps.node_depth || ctx->picture_depth + 1 > ctx->caps.picture_depth) {
    if (group) {
      gvec_lw_wrap_leave(ctx);
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
  /* `context-fill` and `context-stroke` in the referent are the paints of the `use`. */
  bool saved_set = ctx->context_set;
  GVEC_Svg_Paint saved_fill = ctx->context_fill, saved_stroke = ctx->context_stroke;
  const GVEC_Svg_Instance * saved_context_inst = ctx->context_inst;
  GSHGEOM_Map saved_context_back = ctx->context_back;
  bool saved_context_box_set = ctx->context_box_set;
  GSHGEOM_Bounds saved_context_box = ctx->context_box;
  ctx->context_inst = NULL;
  ctx->context_box_set = false;
  GVEC_Svg_Paint use_fill, use_stroke;
  r = gvec_lw_context_paint(ctx, inst, GVEC_SVG_PROP_FILL, &use_fill, &usable);
  if (r == GVEC_OK && usable) {
    r = gvec_lw_context_paint(ctx, inst, GVEC_SVG_PROP_STROKE, &use_stroke, &usable);
  }
  if (r == GVEC_OK && usable) {
    ctx->context_set = true;
    ctx->context_fill = use_fill;
    ctx->context_stroke = use_stroke;
    /* A gradient among them is placed in the box of what the use draws, in the use's space: the picture's space is that moved by `x` and `y`. */
    GVEC_Svg_Style_Value own;
    bool direct = true;
    for (int i = 0; r == GVEC_OK && i < 2; i++) {
      r = gvec_lw_read_property(ctx, inst, i == 0 ? GVEC_SVG_PROP_FILL : GVEC_SVG_PROP_STROKE, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &own, &usable);
      if (r == GVEC_OK && usable && (own.paint.kind == GVEC_SVG_PAINT_CONTEXT_FILL || own.paint.kind == GVEC_SVG_PAINT_CONTEXT_STROKE)) {
        direct = false;
      }
    }
    GSHGEOM_Map back;
    bool known = false;
    if (r == GVEC_OK && direct && (use_fill.kind == GVEC_SVG_PAINT_SERVER || use_stroke.kind == GVEC_SVG_PAINT_SERVER)) {
      GSHGEOM_Map unshift;
      r = gvec_lw_element_box(ctx, clone, &ctx->context_box, &known);
      if (r == GVEC_OK && known && gshgeom_map_translate(2, -x, -y, 0, &unshift) == GSHGEOM_OK && gvec_lw_normal(&unshift, &back)) {
        /* The box is of the target as the picture has it; in the use's space it is moved by `x` and `y`. */
        ctx->context_box.min.x += x;
        ctx->context_box.max.x += x;
        ctx->context_box.min.y += y;
        ctx->context_box.max.y += y;
        ctx->context_inst = inst;
        ctx->context_back = back;
        ctx->context_box_set = true;
      }
    }
    if (r == GVEC_OK) {
      r = push_use(ctx, e);
    }
    if (r == GVEC_OK) {
      GSHGEOM_Map identity;
      r = gshgeom_map_identity(2, &identity) == GSHGEOM_OK ? lw_element(ctx, clone, &identity, use) : GVEC_ERR_INTERNAL;
      ctx->use_stack_count--;
    }
  }
  ctx->context_set = saved_set;
  ctx->context_fill = saved_fill;
  ctx->context_stroke = saved_stroke;
  ctx->context_inst = saved_context_inst;
  ctx->context_back = saved_context_back;
  ctx->context_box_set = saved_context_box_set;
  ctx->context_box = saved_context_box;
  ctx->use_depth--;
  ctx->chain = saved_chain;
  ctx->levels -= 2;
  ctx->picture_depth--;
  ctx->open = saved_open;
  if (r == GVEC_OK) {
    r = gvec_lw_attach(ctx, TARGET(group, parent), use);
  }
  if (group) {
    gvec_lw_wrap_leave(ctx);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, parent, group);
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
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_DISPLAY, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable || gvec_lw_keyword_is(&v, "none")) {
    return r;
  }
  if (leaf) {
    r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable || gvec_lw_keyword_is(&v, "hidden") || gvec_lw_keyword_is(&v, "collapse")) {
      return r;
    }
  }
  return gvec_lw_refuse(ctx, e, reason, message);
}

static GVEC_Result lw_content(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
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
    r = lw_text(ctx, inst, frame, parent);
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

/* An element: its content, under the clips its `clip-path` makes when it has one (SVG 2, 14.3; the clips are outside the element's own
 * opacity group, which composes the same way, a clip and an opacity commuting). */
static GVEC_Result lw_element(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent) {
  gvec_lw_clip_state state;
  gvec_lw_node * clips[GVEC_LW_CLIP_MAX + 1];
  size_t count = 0;
  GVEC_Result r = gvec_lw_clip_begin(ctx, inst, frame, &state, clips, &count);
  if (r != GVEC_OK || state == GVEC_LW_CLIP_REFUSED || state == GVEC_LW_CLIP_ALL) {
    gvec_svg_instance_release(ctx->style, inst);
    return r;
  }
  /* The mask is a group inside the clips: the clipped element is what is masked. */
  gvec_lw_mask_state masked;
  gvec_lw_node * mask_group = NULL;
  r = gvec_lw_mask_begin(ctx, inst, frame, &masked, &mask_group);
  if (r != GVEC_OK || masked == GVEC_LW_MASK_REFUSED || masked == GVEC_LW_MASK_ALL) {
    gvec_svg_instance_release(ctx->style, inst);
    return r;
  }
  if (masked == GVEC_LW_MASK_NODE) {
    /* A mask is an isolated group, so what the element blends with is the group's result: the blend belongs to the mask group. */
    GVEC_Svg_Style_Value blend;
    bool usable;
    r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_MIX_BLEND_MODE, GVEC_LOWER_STYLE, &blend, &usable);
    if (r != GVEC_OK) {
      return r;
    }
    if (usable && blend_composite(&blend) != 0) {
      mask_group->composite = blend_composite(&blend);
      ctx->blend_moved = inst;
    }
    clips[count++] = mask_group;
  }
  if (count == 0) {
    return lw_content(ctx, inst, frame, parent);
  }
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  size_t entered = 0;
  for (; entered < count; entered++) {
    bool ok;
    r = gvec_lw_wrap_enter(ctx, e, &ok);
    if (r != GVEC_OK || !ok) {
      break;
    }
  }
  if (entered < count) {
    while (entered--) {
      gvec_lw_wrap_leave(ctx);
    }
    gvec_svg_instance_release(ctx->style, inst);
    return r;
  }
  r = lw_content(ctx, inst, frame, clips[count - 1]);
  for (size_t i = 0; i < count; i++) {
    gvec_lw_wrap_leave(ctx);
  }
  for (size_t i = count; r == GVEC_OK && i-- > 1;) {
    r = gvec_lw_attach(ctx, clips[i - 1], clips[i]);
  }
  return r == GVEC_OK ? gvec_lw_attach(ctx, parent, clips[0]) : r;
}

GVEC_Result gvec_lw_root_size(gvec_lw_ctx * ctx, double * width, double * height) {
  const GVEC_Svg_Instance * root = gvec_svg_instance_root(ctx->style);
  if (!root) {
    return GVEC_ERR_OOM;
  }
  double vw, vh;
  gvec_svg_instance_viewport(root, &vw, &vh);
  bool usable;
  GVEC_Result r = read_length(ctx, root, GVEC_SVG_PROP_WIDTH, vw, width, &usable);
  if (r == GVEC_OK && usable) {
    r = read_length(ctx, root, GVEC_SVG_PROP_HEIGHT, vh, height, &usable);
  }
  if (r == GVEC_OK && !usable) {
    *width = vw;
    *height = vh;
  }
  if (r != GVEC_OK) {
    return r;
  }
  /* A document that gives no size and no viewBox is as large as its drawing reaches right and down, strokes included. */
  const GVEC_Svg_Element * e = gvec_svg_instance_element(root);
  if (!gvec_svg_element_find_attribute(e, "viewBox")) {
    bool no_w = !gvec_svg_element_find_attribute(e, "width"), no_h = !gvec_svg_element_find_attribute(e, "height");
    if (no_w || no_h) {
      GSHGEOM_Bounds box;
      bool known;
      r = gvec_lw_content_box(ctx, root, &box, &known);
      if (r == GVEC_OK && known && !box.empty && box.max.x > 0 && box.max.y > 0) {
        *width = no_w ? box.max.x : *width;
        *height = no_h ? box.max.y : *height;
      }
    }
  }
  return r;
}

/* The sheets of the document's `style` elements are applied by the style layer. One it cannot apply (an `@import`, which is never fetched, or a
 * conditional rule) refuses its element: drawing without it would be drawing the wrong picture. */
static GVEC_Result refuse_stylesheets(gvec_lw_ctx * ctx) {
  GVEC_Result r = gvec_svg_style_sheets(ctx->style);
  if (r != GVEC_OK) {
    return r;
  }
  const char * why = NULL;
  const GVEC_Svg_Element * e = gvec_svg_style_sheet_refused(ctx->style, &why);
  return e ? gvec_lw_refuse(ctx, e, GVEC_LOWER_STYLESHEET, why) : GVEC_OK;
}

GVEC_Result gvec_lw_walk(gvec_lw_ctx * ctx) {
  GVEC_Result sheets = refuse_stylesheets(ctx);
  if (sheets != GVEC_OK) {
    return sheets;
  }
  const GVEC_Svg_Instance * root = gvec_svg_instance_root(ctx->style);
  if (!root) {
    return GVEC_ERR_OOM;
  }
  GSHGEOM_Map identity;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  /* A negative width or height on the root is an error that the browsers and `resvg` answer by drawing nothing. */
  const GVEC_Svg_Element * root_element = gvec_svg_instance_element(root);
  static const char * const sizes[] = {"width", "height"};
  for (size_t i = 0; i < 2; i++) {
    const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(root_element, sizes[i]);
    size_t n = 0;
    const char * t = a ? gvec_svg_attribute_raw(a, &n) : NULL;
    while (t && n > 0 && (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')) {
      t++;
      n--;
    }
    if (t && n > 1 && t[0] == '-' && ((t[1] >= '0' && t[1] <= '9') || t[1] == '.')) {
      return gvec_diag_add(ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_RENDERING_DISABLED, 0, root_element->node.line,
          root_element->node.column, root_element->node.offset, root_element->name,
          "a negative width or height of the root disables the rendering of the document");
    }
  }
  return lw_element(ctx, root, &identity, &ctx->root);
}
