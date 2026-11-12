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
 * @file lower_internal.h
 *
 * The lowering of an SVG document (P1 to P10): one walk that builds a drawing tree, and two emitters that record the tree
 * as a list or as an edit.
 *
 * The walk owns every decision: which element is drawn, which is refused and why, the maps, the paints and the paths. An
 * emitter owns only how a node of the tree reaches its form, so a decision cannot be made twice, once per form.
 */

#ifndef GHOTI_IO_GVEC_SVG_LOWER_INTERNAL_H
#define GHOTI_IO_GVEC_SVG_LOWER_INTERNAL_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/edit.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/resource.h>

#include <ghoti.io/vector/lower.h>
#include <ghoti.io/vector/style.h>
#include <ghoti.io/vector/svg.h>

#include "../../../core/core_internal.h"
#include <ghoti.io/font/face.h>

#include "../svg_arena.h"
#include "../svg_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The domain of every key this library gives a node: the ASCII of "GVECSVG1". */
#define GVEC_LOWER_KEY_DOMAIN UINT64_C(0x4756454353564731)

/** What a node of the drawing tree is. */
typedef enum gvec_lw_kind {
  GVEC_LW_FILL = 0, ///< A path filled under a map with a paint.
  GVEC_LW_STROKE,   ///< A centerline stroked under a map with a style and a paint.
  GVEC_LW_GROUP,    ///< An opacity group.
  GVEC_LW_CLIP,     ///< The content, clipped to a path.
  GVEC_LW_USE       ///< An instance of a picture: its children are the picture's content.
} gvec_lw_kind;

/** The roles a node has for an element; part of the node's key. */
typedef enum gvec_lw_role {
  GVEC_LW_ROLE_SHAPE = 0,
  GVEC_LW_ROLE_OPACITY,
  GVEC_LW_ROLE_CLIP,
  GVEC_LW_ROLE_CONTAINER, ///< The hidden group that holds a picture's content in an edit.
  GVEC_LW_ROLE_INSTANCE,
  GVEC_LW_ROLE_DRAWING,   ///< The group all the drawing is under.
  GVEC_LW_ROLE_STROKE,    ///< The stroke of a shape (its fill is the SHAPE role).
  GVEC_LW_ROLE_CLIP_PATH, ///< The clip a `clip-path` makes around an element's content.
  GVEC_LW_ROLE_MASK,      ///< The group a `mask` makes around an element's content.
  GVEC_LW_ROLE_MASK_REGION, ///< The clip to a mask's region, in the mask's picture.
  GVEC_LW_ROLE_MASK_CONTAINER ///< The hidden group that holds a mask's picture.
} gvec_lw_role;

typedef struct gvec_lw_node gvec_lw_node;

/** One node of the drawing tree. */
struct gvec_lw_node {
  gvec_lw_kind kind;
  gvec_lw_node * first;
  gvec_lw_node * last;
  gvec_lw_node * next;
  uint64_t key;                      ///< The value of the node's GSHSCENE_Key (the domain is GVEC_LOWER_KEY_DOMAIN).
  uint64_t container_key;            ///< USE: the key of the hidden container the instance's target is.
  uint32_t site;                     ///< The site (element and chain) of the node, for the trace.
  uint32_t container_site;           ///< USE: the site of the container.
  GSHGEOM_Map map;                   ///< FILL, STROKE: the item's map. CLIP: the clip's map. USE: the instance's map. GROUP: the identity.
  const GSHSCENE_Resource * resource; ///< FILL, STROKE, CLIP: the path (held by the context).
  const GSHSCENE_Resource * style;   ///< STROKE: the stroke style (held by the context).
  GSHGEOM_Fill_Rule rule;
  GSHSCENE_Paint paint;
  bool crisp;                        ///< FILL, STROKE: the item asks for no antialiasing (`shape-rendering` crispEdges or optimizeSpeed).
  double opacity;                    ///< GROUP.
  const GSHSCENE_Resource * mask;    ///< GROUP: the soft mask (held by the context), or NULL.
  uint32_t composite;                ///< GROUP: a GSHSCENE_Composite (`mix-blend-mode`); 0 is source-over.
  bool isolate;                      ///< GROUP: `isolation: isolate`; the group names the space it blends in, since the waist drops a group that is all defaults.
  bool content;                      ///< Something is drawn by this node or below it.
  size_t items;                      ///< The items this node and its subtree are in the list the node is in.
  size_t inner_items;                ///< USE: the items of the picture's list.
  size_t weight;                     ///< The items once every instance is replaced by its picture's items (saturating).
  size_t occurrences;                ///< The nodes of a retained scene counted once for each time they are drawn (saturating).
  size_t nodes;                      ///< The nodes of a retained scene that this node and its subtree are.
};

/** The provenance of a node: for the trace. */
typedef struct gvec_lw_site {
  const GVEC_Svg_Element * element;
  uint64_t key;
  uint32_t use_depth;
} gvec_lw_site;

/** The items of one list as a trace records them: the site of each, and the table of the picture an instance item draws. */
typedef struct gvec_lw_table {
  uint32_t * site;
  uint32_t * child; ///< For an instance item the index of its picture's table plus one; else 0.
  size_t count;
  size_t capacity;
} gvec_lw_table;

#define GVEC_LW_NO_SITE UINT32_MAX

/** Masks whose pictures may be inside one another (a mask's children masked, a mask that is masked). */
#define GVEC_LW_MASK_NESTING 8

/** Patterns whose tiles may be inside one another. */
#define GVEC_LW_PATTERN_NESTING 8

/** The structural limits of the scene the drawing is checked against, so that both emitters are given a tree that fits. */
typedef struct gvec_lw_caps {
  size_t node_depth;    ///< Levels of nodes in a version, an instance's target counting as its child.
  size_t open;          ///< Clips and groups open at once in one list.
  size_t picture_depth; ///< Pictures nested in pictures.
  size_t list_items;    ///< Items in one list.
  size_t expansion;     ///< Items of a list with every instance replaced by its picture's, and occurrences of the nodes of a version.
  size_t nodes;         ///< Nodes in a version.
  size_t marker_instances; ///< Marker instances in one lowering.
} gvec_lw_caps;

/** The state of one lowering. */
typedef struct gvec_lw_ctx {
  const GVEC_Doc * doc;
  const GVEC_Svg_Document * document;
  const GVEC_Allocator * allocator;
  GVEC_Diagnostics * diagnostics;
  const GVEC_Lower_Options * options;
  GVEC_Svg_Style_Context * style;
  GSHSCENE_Store * store;
  const GSHSCENE_Allocator * scene_allocator;
  const GSHSCENE_Limits * scene_limits;
  gvec_arena arena;                  ///< The nodes.
  gvec_lw_site * sites;
  size_t site_count, site_capacity;
  const GSHSCENE_Resource ** held;   ///< Resources the context holds a reference to, released when it ends.
  size_t held_count, held_capacity;
  uint32_t * refused;                ///< For each element, a bit for each reason it has been refused for.
  size_t depth;
  size_t max_depth;
  uint64_t chain;                    ///< Hash of the `use` elements the walk is inside.
  uint32_t use_depth;
  gvec_lw_caps caps;
  size_t levels;                     ///< The level of the node the walk is adding children to (the root is 1).
  size_t open;                       ///< Clips and groups open in the list being built.
  size_t picture_depth;              ///< Pictures the walk is inside.
  const GVEC_Svg_Element ** use_stack;
  size_t use_stack_count, use_stack_capacity;
  size_t marker_instances;           ///< Marker instances made so far, nested ones included.
  bool context_set;                  ///< The walk is inside a marker: context-fill and context-stroke name the paints below.
  GVEC_Svg_Paint context_fill, context_stroke;
  /* A context paint that is a gradient is placed in the box of the element the paints came from (`context_inst`, NULL when that is not
   * known) and its space is that element's user space: `context_back` takes a point of the picture the walk is in to that space,
   * inverted, and `item_local` is the map of the shape being drawn within the picture. */
  const GVEC_Svg_Instance * context_inst;
  bool box_stroke;                   ///< A group's box takes in the strokes of its shapes (the size of a document that gives none).
  bool paint_box_set;                ///< While the paint of text is resolved: `paint_box` is the box a paint server is placed in (the text's, whatever span is painted).
  GSHGEOM_Bounds paint_box;
  bool context_box_set;              ///< `context_box` is the box (a `use` has no geometry of its own); otherwise it is that of the shape `context_inst`.
  GSHGEOM_Bounds context_box;
  GSHGEOM_Map context_back;
  GSHGEOM_Map item_local;
  bool item_crisp;                   ///< The shape being drawn has `shape-rendering` crispEdges or optimizeSpeed.
  bool tracing;
  gvec_lw_table * tables;
  size_t table_count, table_capacity;
  struct gradient_cache * gradients; ///< What of each paint server does not depend on the element that uses it (lower_gradient.c).
  const GVEC_Svg_Element * mask_stack[GVEC_LW_MASK_NESTING];   ///< The masks whose picture the walk is inside (lower_mask.c).
  size_t mask_depth;
  const GVEC_Svg_Instance * blend_moved; ///< The instance whose `mix-blend-mode` its mask group carries (the blend is the outermost of the stack: lower_walk.c).
  const GVEC_Svg_Element * pattern_stack[GVEC_LW_PATTERN_NESTING]; ///< The patterns whose tile the walk is inside (lower_pattern.c).
  size_t pattern_depth;
  gvec_lw_node root;                 ///< The root of the tree: only its children are drawn.
} gvec_lw_ctx;

/** The key of a node (AD-9): the element's key, the chain of `use` elements above it and the node's role. */
GVEC_INTERNAL_API uint64_t gvec_lw_key(uint64_t element_key, uint64_t chain, gvec_lw_role role);

/** Start a lowering of `doc`: the context, with the style context made. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_begin(gvec_lw_ctx * ctx, const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_Store * store, GVEC_Diagnostics * diagnostics);

/** Free the context: the tree, the sites, the held resources and the style context. */
GVEC_INTERNAL_API void gvec_lw_end(gvec_lw_ctx * ctx);

/** The outermost `svg`'s used width and height. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_root_size(gvec_lw_ctx * ctx, double * width, double * height);

/** Walk the document, building the tree under `ctx->root`. Refusals are diagnostics; the result is an error only for OOM. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_walk(gvec_lw_ctx * ctx);

/** Report that an element is not drawn (once for each element and reason). */
GVEC_INTERNAL_API GVEC_Result gvec_lw_refuse(
    gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, GVEC_Lower_Reason reason, const char * message);

/** A new node in the context's arena (zeroed, the identity map, opacity 1); NULL when out of memory. */
GVEC_INTERNAL_API gvec_lw_node * gvec_lw_new_node(gvec_lw_ctx * ctx, gvec_lw_kind kind);

/** Make a site for an element at the context's chain; GVEC_LW_NO_SITE when out of memory. */
GVEC_INTERNAL_API uint32_t gvec_lw_new_site(gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, uint64_t key);

/** Take over a reference to a resource; it is released when the context ends. On failure the reference is released. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_hold(gvec_lw_ctx * ctx, const GSHSCENE_Resource * resource);

/**
 * Link `child` last under `parent`, unless the tree would no longer fit the scene's limits, in which case the element of `child`
 * is refused (GVEC_LOWER_LIMIT) and `child` is not linked.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_append(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * child);

/** Link `first` then `second` last under `parent`, both or (when they would not fit the scene's limits) neither, refusing the element. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_append_pair(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * first, gvec_lw_node * second);

/** Link all of `nodes` last under `parent`, or (when they would not fit the scene's limits) none of them, refusing the element of the first. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_append_all(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * const * nodes, size_t count);

/** `outer . inner`, normalised so that no entry is negative zero (see lower_emit.c); false when an entry leaves the waist's range. */
GVEC_INTERNAL_API bool gvec_lw_compose(const GSHGEOM_Map * outer, const GSHGEOM_Map * inner, GSHGEOM_Map * out);

/** The normal form of a map (composed with the identity). */
GVEC_INTERNAL_API bool gvec_lw_normal(const GSHGEOM_Map * map, GSHGEOM_Map * out);

//-----------------------------------------------------------------------------
// Reading values and paint (lower_walk.c, lower_paint.c, lower_stroke.c)
//-----------------------------------------------------------------------------

/** Whether a keyword value is `word` (ASCII case-insensitively; `word` is lower case). */
GVEC_INTERNAL_API bool gvec_lw_keyword_is(const GVEC_Svg_Style_Value * v, const char * word);

/**
 * Read a property. A value the style layer could not use refuses the element (`unusable` is its reason); any other failure
 * (except running out of memory) is a limit.
 *
 * @param usable Receives whether the value can be used.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_read_property(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property,
    GVEC_Lower_Reason unusable, GVEC_Svg_Style_Value * v, bool * usable);

/** What a paint of a fill or a stroke amounts to. */
typedef enum gvec_lw_paint_decision {
  GVEC_LW_PAINT_NONE = 0, ///< Nothing is drawn (`none`, or no alpha).
  GVEC_LW_PAINT_DRAW,     ///< `*out` is the paint.
  GVEC_LW_PAINT_REFUSED   ///< The element is refused (already reported).
} gvec_lw_paint_decision;

/** The paint of `paint` for the element of `inst` with its alpha multiplied by `opacity` (AD-23); a pattern or a context paint of a pattern refuses the element. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_paint_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint,
    double opacity, GSHSCENE_Paint * out, gvec_lw_paint_decision * decision);

/**
 * A `pattern` paint for the element of `inst` (lower_pattern.c): the pattern resource, its tile a picture of the pattern's content
 * made while the element is walked. A pattern that is not rendered (no size, a transform with no inverse, a box of no area, a pattern
 * that paints its own content with itself) is `NONE` with a notice; one with no content is `NONE`.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_pattern_paint(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint,
    double opacity, GSHSCENE_Paint * out, gvec_lw_paint_decision * decision);

/** As gvec_lw_pattern_paint(), for the box of `inst` (or `given`) but a pattern space that `into` takes to the space the paint is used in. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_pattern_paint_in(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint, double opacity,
    const GSHGEOM_Map * into, const GSHGEOM_Bounds * given, GSHSCENE_Paint * out, gvec_lw_paint_decision * decision);

/**
 * The children of `holder` as a picture of the store (a list of its own, closed and interned): the tile of a pattern. `*out` is a
 * reference the caller releases, or NULL when a limit of the scene was reached (the element of the node that failed is refused).
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_emit_picture(gvec_lw_ctx * ctx, const gvec_lw_node * holder, const GSHSCENE_Resource ** out);

/**
 * A `linearGradient` or `radialGradient` paint for the element of `inst` (lower_gradient.c): the gradient resource, with the
 * bounding box of the element's fill geometry when the units are `objectBoundingBox`. A gradient that is not rendered (a box of
 * no area, a transform with no inverse, a negative radius) is `NONE` with a notice; one of no stops is `NONE`, of one stop or of
 * no extent a solid paint.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_gradient_paint(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint,
    double opacity, GSHSCENE_Paint * out, gvec_lw_paint_decision * decision);

/** As gvec_lw_gradient_paint(), for the box of `inst` but a gradient space that `into` takes to the space the paint is used in. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_gradient_paint_in(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Paint * paint,
    double opacity, const GSHGEOM_Map * into, const GSHGEOM_Bounds * box, GSHSCENE_Paint * out, gvec_lw_paint_decision * decision);

/**
 * The bounding box (SVG 2, 8.10) of what the element of `inst` draws, in the space inside its own `transform` and the space it
 * is in: the fill box of a shape, or the union of those of the shapes in a `g` or `a`, each moved by its transform. What `display`
 * removes is left out; `visibility` and `opacity` do not remove a shape. `*known` is false when it holds what this does not box.
 * `box->empty` is set when nothing in it has area.
 */
/** The box of the drawing under the outermost `svg`, strokes included, in its user space; *known false when it holds what is not boxed. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_content_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * root, GSHGEOM_Bounds * box, bool * known);
/** The bounding box of the element of `inst` in its own user space (inside its `transform`), as a clip or a mask in the units of the box takes it. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_inner_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GSHGEOM_Bounds * box, bool * known);
GVEC_INTERNAL_API GVEC_Result gvec_lw_element_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GSHGEOM_Bounds * box, bool * known);

/** The inverse of a 2D map; false when it has none (or the result is not finite). */
GVEC_INTERNAL_API bool gvec_lw_invert(const GSHGEOM_Map * map, GSHGEOM_Map * out);

/** What the stroke of an element amounts to. */
typedef struct gvec_lw_stroke {
  gvec_lw_paint_decision decision;
  const GSHSCENE_Resource * style; ///< DRAW: the stroke style (held by the context).
  GSHSCENE_Paint paint;            ///< DRAW.
} gvec_lw_stroke;

/** Read the stroke properties of an instance and make the style and the paint; refuses the element whole when it cannot. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_stroke_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_stroke * out);

/** The instance of an element of the document, made through its ancestors so that a property inherits as it does in the tree. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_instance_of(gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, const GVEC_Svg_Instance ** out);

/** What `clip-path` makes of an element. */
typedef enum gvec_lw_clip_state {
  GVEC_LW_CLIP_NONE = 0, ///< The element is not clipped: draw it as it is.
  GVEC_LW_CLIP_NODE,     ///< `out` holds the clip nodes, outermost first: draw the element under the last.
  GVEC_LW_CLIP_ALL,      ///< The clip holds nothing, so nothing of the element is seen.
  GVEC_LW_CLIP_REFUSED   ///< The element is refused (already reported).
} gvec_lw_clip_state;

/**
 * A face of the fonts for `codepoint`, in the style given (`weight` 1 to 1000, `italic` 0 normal, 1 italic, 2 oblique, `width` in percent):
 * the first family of the list that has a face that maps it, else any face of the set that does. `*found` is false when none does.
 * The face is owned by the fonts and is open for their life.
 */
GVEC_INTERNAL_API GVEC_Result gvec_fonts_face(const GVEC_Fonts * fonts, const char * const * families, size_t family_count, double weight,
    int italic, double width, uint32_t codepoint, const GFNT_Face ** out, bool * found);

/** The face resource of `face` in `store`, with a reference the caller releases (the scene font is made on first use and kept). */
GVEC_INTERNAL_API GVEC_Result gvec_fonts_scene_face(
    const GVEC_Fonts * fonts, const GFNT_Face * face, GSHSCENE_Store * store, const GSHSCENE_Resource ** out);

/** The glyph runs of a `text` element, each in the element's own user space (the glyphs carry their absolute positions, y down). */
typedef struct gvec_lw_text_piece {
  const GSHSCENE_Resource * run;      ///< A glyph run (held by the context): the glyphs of one span in one face.
  const GVEC_Svg_Instance * inst;     ///< The span it is of: its fill is the run's paint.
} gvec_lw_text_piece;

typedef struct gvec_lw_text {
  gvec_lw_text_piece * pieces; ///< The runs, in the order they are drawn; empty when nothing is drawn.
  size_t count;
  GSHGEOM_Bounds box;          ///< The glyphs' cells, in the element's space: what a gradient or a pattern in the units of the box is placed in.
} gvec_lw_text;

/**
 * Shape the character data of the `text` element of `inst` with the fonts of the options (lower_text.c). `*usable` is false when the
 * element was refused (and reported). Nothing is drawn for a text with no characters: `count` is 0.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_text_layout(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_text * out, bool * usable);

/** What `mask` makes of an element. */
typedef enum gvec_lw_mask_state {
  GVEC_LW_MASK_NONE = 0, ///< The element is not masked: draw it as it is.
  GVEC_LW_MASK_NODE,     ///< `out` is a group with the soft mask: draw the element under it.
  GVEC_LW_MASK_ALL,      ///< The mask cannot be placed (a box of no area, a region of no size): the element is not rendered.
  GVEC_LW_MASK_REFUSED   ///< The element is refused (already reported).
} gvec_lw_mask_state;

/**
 * The soft mask of an element's `mask` (lower_mask.c): a group of the waist that names the mask, to draw the element under. The mask's
 * picture is the content of the `mask` element in the element's own space (`frame`, the element's transform included) clipped to the
 * mask's region, read as luminance or as alpha through `mask-type` and `color-interpolation`; a `mask` on the mask element, or on its
 * children, is a mask of its own.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_mask_begin(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame,
    gvec_lw_mask_state * state, gvec_lw_node ** out);

/** The most clip nodes one element's `clip-path` makes: a clip that is clipped is a nested clip, to this depth. */
#define GVEC_LW_CLIP_MAX 8

/**
 * The clips of an element's `clip-path` (lower_clip.c): a `clipPath` of one shape becomes one clip node whose path is the shape's
 * and whose map takes the clipPath's and the shape's transforms and `clipPathUnits` into account, in the element's own space; a
 * clip that is itself clipped, on the clipPath or on its shape, is a nested clip, whose region is the intersection. The walk draws
 * the element under the last node and attaches the nodes inwards when anything was drawn. Several shapes, a basic shape and a
 * `use` child are refused until they are lowered; a reference to nothing is no clip.
 *
 * @param out Room for GVEC_LW_CLIP_MAX nodes.
 * @param count The nodes made, when the state is GVEC_LW_CLIP_NODE.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_clip_begin(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame,
    gvec_lw_clip_state * state, gvec_lw_node ** out, size_t * count);

/** What an element draws, for `paint-order`. */
typedef enum gvec_lw_part { GVEC_LW_PART_FILL = 0, GVEC_LW_PART_STROKE, GVEC_LW_PART_MARKERS } gvec_lw_part;

/** The order an element draws its fill, its stroke and its markers in (`paint-order`, SVG 2, 13.6). */
GVEC_INTERNAL_API GVEC_Result gvec_lw_paint_order(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_part order[3], bool * usable);

//-----------------------------------------------------------------------------
// Walking (lower_walk.c), for the markers
//-----------------------------------------------------------------------------

/** Enter a clip or an opacity group: one level deeper and one more open, unless the scene's limits would not hold what is inside (then refused, `*entered` false). */
GVEC_INTERNAL_API GVEC_Result gvec_lw_wrap_enter(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, bool * entered);

/** Leave what gvec_lw_wrap_enter() entered. */
GVEC_INTERNAL_API void gvec_lw_wrap_leave(gvec_lw_ctx * ctx);

/** Link `node` under `parent` if it draws anything. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_attach(gvec_lw_ctx * ctx, gvec_lw_node * parent, gvec_lw_node * node);

/** Walk the children of `inst` in `frame`, recording what they draw under `parent`. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_children(
    gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_node * parent);

/** A length in user units when its unit is absolute (none, px, in, cm, mm, Q, pt, pc); false for a unit that needs the cascade or a basis. */
GVEC_INTERNAL_API bool gvec_lw_absolute_length(const GVEC_Svg_Length * length, double * out);

//-----------------------------------------------------------------------------
// Viewports (lower_viewport.c)
//-----------------------------------------------------------------------------

/**
 * The map from a viewBox to a viewport of `width` by `height` at the origin (SVG 2, 8.2): `preserveAspectRatio` decides how the
 * box is scaled and aligned. Every entry is correctly rounded from the exact value except where the exact value lies within
 * 2^-100 (relative) of a rounding boundary.
 *
 * @return false when an input is not finite, a size is not positive, or a result leaves the waist's range.
 */
GVEC_INTERNAL_API bool gvec_lw_viewbox_map(const GVEC_Svg_View_Box * box, const GVEC_Svg_Preserve_Aspect_Ratio * par, double width,
    double height, GSHGEOM_Map * out);

//-----------------------------------------------------------------------------
// Paths (lower_path.c)
//-----------------------------------------------------------------------------

/** What converting an element's geometry gave. */
typedef enum gvec_lw_geometry {
  GVEC_LW_GEOMETRY_EMPTY = 0, ///< Nothing to fill.
  GVEC_LW_GEOMETRY_PATH,      ///< `*out_resource` is a path (the context holds it).
  GVEC_LW_GEOMETRY_LIMIT      ///< A coordinate or a count the waist cannot carry: refuse the element.
} gvec_lw_geometry;

/**
 * The path of a `path`, `polygon`, `polyline` or `line` element, interned in the store.
 *
 * @param for_stroke The path is a centerline: a `polyline` stays open, a `line` is a path, and a subpath of no length is kept
 *   (a cap draws it). Else the path is what a fill covers, and a path with no extent is empty.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_path_of(gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, bool for_stroke,
    gvec_lw_geometry * out_kind, const GSHSCENE_Resource ** out_resource);

/**
 * The named primitive of a `rect`, `circle` or `ellipse` instance, interned in the store with its tag. `*usable` is false when a
 * property had no usable value (the element was refused); a shape that is not rendered (a zero size) is `EMPTY`.
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_primitive_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_geometry * out_kind,
    const GSHSCENE_Resource ** out_resource, bool * usable);

/** The centerline from (`x1`, `y1`) to (`x2`, `y2`), interned in the store. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_line_path(gvec_lw_ctx * ctx, double x1, double y1, double x2, double y2,
    gvec_lw_geometry * out_kind, const GSHSCENE_Resource ** out_resource);

/** The path of absolute path segments (a `path()` shape of a clip), interned in the store. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_segments_path(gvec_lw_ctx * ctx, const GVEC_Svg_Path_Segment * segments, size_t count,
    gvec_lw_geometry * out_kind, const GSHSCENE_Resource ** out_resource);

/** The closed polygon of `count` numbers (x, y pairs), interned in the store. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_points_path(gvec_lw_ctx * ctx, const double * numbers, size_t count, gvec_lw_geometry * out_kind,
    const GSHSCENE_Resource ** out_resource);

/** What `clip-path` written as a basic shape comes to. */
typedef enum gvec_lw_shape_kind {
  GVEC_LW_SHAPE_INVALID, ///< Not a shape this reads: the property is as if not specified.
  GVEC_LW_SHAPE_NONE,    ///< A shape for an element that draws nothing: no clip is needed.
  GVEC_LW_SHAPE_ALL,     ///< A shape with no area: nothing gets through.
  GVEC_LW_SHAPE_PATH,    ///< The region `path`, filled by `rule`, in the element's space moved by (`ox`, `oy`).
  GVEC_LW_SHAPE_REFUSED, ///< Valid, and not lowered (`why`).
  GVEC_LW_SHAPE_ERROR    ///< The call's own result says why (out of memory, or an element already refused).
} gvec_lw_shape_kind;

typedef struct gvec_lw_basic_shape {
  gvec_lw_shape_kind kind;
  const GSHSCENE_Resource * path;
  GSHGEOM_Fill_Rule rule;
  double ox, oy;
  const char * why;
} gvec_lw_shape;

/** Read the text of a `clip-path` that is a basic shape, with its reference box, against the element `inst`. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_basic_shape(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const char * text, gvec_lw_shape * out);

/** The path of the rectangle `x`, `y`, `width`, `height` as four lines (a viewport's clip); interned in the store. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_rect_path(
    gvec_lw_ctx * ctx, double x, double y, double width, double height, gvec_lw_geometry * out_kind, const GSHSCENE_Resource ** out_resource);

/** The centerline of a `path`, `polygon`, `polyline` or `line` instance (a stroke's and a marker's); `*usable` is false when the element was refused. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_centerline_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_geometry * geometry,
    const GSHSCENE_Resource ** path, bool * usable);

//-----------------------------------------------------------------------------
// Markers (lower_marker.c)
//-----------------------------------------------------------------------------

/** The marker instances of a shape: one node for each, in vertex order. */
typedef struct gvec_lw_marker_set {
  gvec_lw_node ** nodes;
  size_t count;
  bool refused; ///< The element was refused (reported): draw none of it.
} gvec_lw_marker_set;

/**
 * The markers of a `path`, `line`, `polyline` or `polygon`: a start marker at the first vertex of the path, an end marker at the
 * last and a mid marker at each other, each a picture of the marker's content under the map of the vertex, its direction, its scale
 * and its reference point. The nodes are not linked anywhere.
 *
 * @param local The element's map (the frame composed with its `transform`).
 */
GVEC_INTERNAL_API GVEC_Result gvec_lw_markers(
    gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * local, gvec_lw_marker_set * out);

/** The paint of `property` (fill or stroke) of `inst` as a context paint names it: its own, or, inside a marker or a use, the one it takes from there. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_context_paint(
    gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, GVEC_Svg_Paint * out, bool * usable);

//-----------------------------------------------------------------------------
// Emitters (lower_emit_list.c, lower_emit_edit.c) and the trace (lower_trace.c)
//-----------------------------------------------------------------------------

/** Record the tree in a list. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_emit_list(gvec_lw_ctx * ctx, GSHSCENE_List * list);

/** Record the tree in an edit, under the drawing's group. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_emit_edit(gvec_lw_ctx * ctx, GSHSCENE_Edit * edit);

/** Add a row to a trace table; `table` is an index into `ctx->tables`. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_table_add(gvec_lw_ctx * ctx, size_t table, uint32_t site, uint32_t child);

/** A new trace table; its index. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_table_new(gvec_lw_ctx * ctx, size_t * out_index);

/** Hand the sites and tables to the trace of the options, if there is one. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_trace_fill(gvec_lw_ctx * ctx);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_LOWER_INTERNAL_H
