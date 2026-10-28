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
 * @file style.h
 *
 * The style of an SVG document: the cascade, and the used values only an SVG
 * renderer can resolve.
 *
 * SVG 2 makes most of what a drawing means a CSS property, so nothing can be
 * drawn until each element has specified, computed and used values. The
 * engine is the `text` library's: the property registry, the cascade, `var()`
 * and the computed value of every type. This file is the SVG view of it.
 *
 * **Instances.** One element can be drawn many times, each time inheriting
 * from somewhere else: a `use` clone inherits from the `use`, not from where
 * it was written. A style is therefore a property of an element *as drawn*, so
 * the handle is an @ref GVEC_Svg_Instance, a pair of an element and the
 * instance it inherits from. Instances are made on demand by the caller (the
 * lowering) as it walks, belong to the style context, and are cheap; a style is
 * computed for an instance the first time it is read and kept. There is no
 * expansion of `use` into a cloned tree.
 *
 * **Reading.** gvec_svg_get() reads one property of an instance as a typed
 * @ref GVEC_Svg_Style_Value: keyword, number, length in user units, colour,
 * paint, dash array, transform or references. Lengths are *used* values:
 * percentages are resolved against the basis the property's definition names
 * (the width, the height or the normalised diagonal of the nearest viewport,
 * its `viewBox` size when it has one), and `em`, `ex` and `rem` are already
 * absolute. A colour is never 8 bits: straight `double` components and the
 * space of the colour. `currentColor` is the element's computed `color`; when
 * `color` is the initial `CanvasText` everywhere on the inheritance chain the
 * used value is the waist's foreground, so that a host can supply it.
 * gvec_svg_get_specified() reads the computed value as text, with where the
 * cascade got it from.
 *
 * **Diagnostics.** A declaration or attribute that is not valid is dropped, as
 * CSS says, and reported (GVEC_DIAG_STYLE_INVALID) once for the element, at the
 * attribute that holds it. A colour the waist cannot carry
 * (GVEC_DIAG_UNSUPPORTED_COLOR_SPACE), a reference to nothing and a reference
 * cycle through a property are reported where they are read.
 *
 * **Determinism.** Nothing is read from the environment, the locale or the
 * clock. A style is a function of the document, the load options and the
 * instance.
 *
 * A context is one thread at a time, as the document is.
 */

#ifndef GHOTI_IO_GVEC_STYLE_H
#define GHOTI_IO_GVEC_STYLE_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/paint.h>

#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/stream.h>
#include <ghoti.io/vector/svg.h>
#include <ghoti.io/vector/svg_prop.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The style of one document: the registry, the cascade's context and the instances made so far. */
typedef struct GVEC_Svg_Style_Context GVEC_Svg_Style_Context;

/** An element as drawn (see the file's description). Owned by the context; valid until it is freed. */
typedef struct GVEC_Svg_Instance GVEC_Svg_Instance;

/**
 * Make the style context of a document.
 *
 * The viewport, the colour scheme and the languages are the load options the
 * document was loaded with (GVEC_Load_Options).
 *
 * @param document The typed document; it must outlive the context.
 * @param diagnostics Receives what is dropped or refused as styles are read; caller-owned, it
 *   must outlive the context. May be NULL (nothing is reported).
 * @param allocator For the context; NULL for the default.
 * @param out_context Receives the context; free it with gvec_svg_style_context_free().
 * @return GVEC_ERR_INVALID for a NULL document or output; GVEC_ERR_OOM; GVEC_ERR_INTERNAL if the
 *   registry of properties could not be made (a defect in this library).
 */
GVEC_API GVEC_Result gvec_svg_style_context_new(const GVEC_Svg_Document * document, GVEC_Diagnostics * diagnostics,
    const GVEC_Allocator * allocator, GVEC_Svg_Style_Context ** out_context);

/** Free a context, its instances and every style. Ignored when NULL. */
GVEC_API void gvec_svg_style_context_free(GVEC_Svg_Style_Context * context);

/**
 * Release the engines of style that the library keeps for the next document.
 *
 * What a context needs of the CSS engine (the registry of properties and the initial value of each) depends only on the viewport, the
 * color scheme and the bound of `var()` expansion of the load, and costs milliseconds to make, so contexts made with the default
 * allocator share engines made once for each environment (at most eight are kept). A host that wants the memory back, and a test that
 * counts allocations, calls this; an engine a context still uses is freed with the context. Safe from any thread.
 */
GVEC_API void gvec_svg_style_cache_clear(void);

//-----------------------------------------------------------------------------
// The registry of properties
//-----------------------------------------------------------------------------

/** Where the registry's definition of a property comes from (properties.md). */
typedef enum GVEC_Svg_Property_Source {
  GVEC_SVG_PROPERTY_GENERATED = 0, ///< The pin of `text` defines it and the definition stands.
  GVEC_SVG_PROPERTY_REGISTERED,    ///< The pin lacks it; this library registers it.
  GVEC_SVG_PROPERTY_OVERRIDDEN     ///< The pin defines it differently from the CR; this library overrides it, and says so.
} GVEC_Svg_Property_Source;

/** Where a property's definition comes from; GVEC_SVG_PROPERTY_GENERATED for a value that is not a property. */
GVEC_API GVEC_Svg_Property_Source gvec_svg_property_source(GVEC_Svg_Property property);

/** The number of definitions this library overrides (never silent: each is one of these). */
GVEC_API size_t gvec_svg_style_context_override_count(const GVEC_Svg_Style_Context * context);

/**
 * The `index`th override, in the byte order of the property names.
 *
 * @param out_note Receives why, quoting the specification (static); may be NULL.
 * @return false past the end.
 */
GVEC_API bool gvec_svg_style_context_override_at(
    const GVEC_Svg_Style_Context * context, size_t index, GVEC_Svg_Property * out_property, const char ** out_note);

//-----------------------------------------------------------------------------
// Instances
//-----------------------------------------------------------------------------

/**
 * The root element drawn in the document itself: the outermost `svg`, inheriting from nothing, in the viewport
 * of the load options.
 */
GVEC_API const GVEC_Svg_Instance * gvec_svg_instance_root(GVEC_Svg_Style_Context * context);

/**
 * A child element drawn in the same context as its parent: it inherits from the parent.
 *
 * @param parent The parent's instance.
 * @param element A child element of `parent`'s element.
 * @param out_instance Receives the instance (made once for each call; the caller keeps what it needs).
 * @return GVEC_ERR_INVALID when `element` is not a child of the parent's element; GVEC_ERR_OOM; GVEC_ERR_LIMIT
 *   past the bound on the instances of a context: the elements of the document and the elements its references may instantiate
 *   (GVEC_Limits.max_elements plus max_reference_expansion), reported.
 */
GVEC_API GVEC_Result gvec_svg_instance_child(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * parent,
    const GVEC_Svg_Element * element, const GVEC_Svg_Instance ** out_instance);

/**
 * The root of a `use`'s referent, drawn in a clone context: its **inheritance parent** is the `use` instance, its
 * **tree parent** the referent's parent in the document. Every descendant made with gvec_svg_instance_child()
 * inherits through its own parent, so the clone's root is the inheritance ancestor of all of them.
 *
 * The referent's viewport is the `use`'s when it is no `svg` or `symbol`; an `svg` or `symbol` establishes
 * one from its own (or the `use`'s, which overrides it) `width` and `height`.
 *
 * @param use_instance The instance of a `use` element whose `href` resolved (gvec_svg_element_href_target()).
 * @return GVEC_ERR_INVALID when the instance is not a `use` or has no target; as gvec_svg_instance_child() otherwise.
 */
GVEC_API GVEC_Result gvec_svg_instance_use(
    GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * use_instance, const GVEC_Svg_Instance ** out_instance);

/**
 * Give back the memory of an instance's computed style, which is the larger part of what an instance costs (tens of kilobytes
 * for the properties of the engine). A style read again is computed again, from the style of the instance it inherits from
 * (which is computed again too if it was given back), so this is for a walk that is done with an element and its subtree: the
 * lowering calls it, so that what a document that instantiates a million elements holds at once is its depth and not its size.
 * Values already read stay valid (they are in the context's own memory). Ignored for NULL.
 */
GVEC_API void gvec_svg_instance_release(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance);

/** The element an instance is of. */
GVEC_API const GVEC_Svg_Element * gvec_svg_instance_element(const GVEC_Svg_Instance * instance);

/** The instance this one inherits from; NULL for the root of inheritance. */
GVEC_API const GVEC_Svg_Instance * gvec_svg_instance_parent(const GVEC_Svg_Instance * instance);

/** The nearest enclosing `use` instance, whose clone this instance is part of; NULL in the document's own context. */
GVEC_API const GVEC_Svg_Instance * gvec_svg_instance_use_context(const GVEC_Svg_Instance * instance);

/**
 * The `width` and `height` a `use` gave the `svg` or `symbol` this instance is the clone of (they override the element's own).
 *
 * @param out_width Receives the used width; written only when the result is true.
 * @param out_height Receives the used height; written only when the result is true.
 * @param has_width Receives whether the `use` gave a width (not `auto`); may be NULL.
 * @param has_height Receives whether the `use` gave a height; may be NULL.
 */
GVEC_API void gvec_svg_instance_override(
    const GVEC_Svg_Instance * instance, bool * has_width, double * out_width, bool * has_height, double * out_height);

/** The viewport this element's percentages are resolved against, in user units: the `viewBox` size when the nearest viewport has one. */
GVEC_API void gvec_svg_instance_viewport(const GVEC_Svg_Instance * instance, double * out_width, double * out_height);

//-----------------------------------------------------------------------------
// Values
//-----------------------------------------------------------------------------

/** What a read value is. The numbers are stable. */
typedef enum GVEC_Svg_Style_Value_Kind {
  GVEC_SVG_STYLE_NONE = 0,   ///< The property has no value of this kind (a shorthand, or the value could not be used; see the result).
  GVEC_SVG_STYLE_KEYWORD,    ///< The computed value as text.
  GVEC_SVG_STYLE_NUMBER,     ///< A number.
  GVEC_SVG_STYLE_LENGTH,     ///< A length in user units.
  GVEC_SVG_STYLE_COLOR,      ///< A colour.
  GVEC_SVG_STYLE_PAINT,      ///< A paint.
  GVEC_SVG_STYLE_NUMBER_LIST, ///< A list of numbers in user units (`stroke-dasharray`).
  GVEC_SVG_STYLE_TRANSFORM,  ///< A transform.
  GVEC_SVG_STYLE_REFERENCES, ///< `url()` references and other values the property takes, as written.
  GVEC_SVG_STYLE_KIND_COUNT
} GVEC_Svg_Style_Value_Kind;

/** What a colour is. */
typedef enum GVEC_Svg_Color_Kind {
  GVEC_SVG_COLOR_SOLID = 0,    ///< `color`: straight `double` components in their space.
  GVEC_SVG_COLOR_FOREGROUND,   ///< The waist's foreground; `color.rgba[3]` is the alpha multiplier.
  GVEC_SVG_COLOR_COUNT
} GVEC_Svg_Color_Kind;

/** A colour as the waist carries it: never 8 bits. */
typedef struct GVEC_Svg_Color {
  GVEC_Svg_Color_Kind kind;
  GSHSCENE_Color color; ///< SOLID: the colour. FOREGROUND: `rgba[3]` is the alpha, the rest zero, the space the target's.
} GVEC_Svg_Color;

/** What a paint is. */
typedef enum GVEC_Svg_Paint_Kind {
  GVEC_SVG_PAINT_NONE = 0,
  GVEC_SVG_PAINT_COLOR,          ///< `color`.
  GVEC_SVG_PAINT_SERVER,         ///< A paint server: `server`, with `fallback` when the document gave one.
  GVEC_SVG_PAINT_CONTEXT_FILL,   ///< `context-fill`.
  GVEC_SVG_PAINT_CONTEXT_STROKE, ///< `context-stroke`.
  GVEC_SVG_PAINT_COUNT
} GVEC_Svg_Paint_Kind;

/**
 * A paint. A `url()` whose target is missing or is no paint server is resolved as SVG 2 says: to the fallback when
 * the document gave one, else to `none` (and the reference is reported).
 */
typedef struct GVEC_Svg_Paint {
  GVEC_Svg_Paint_Kind kind;
  GVEC_Svg_Color color;                   ///< COLOR.
  const GVEC_Svg_Element * server;        ///< SERVER: the `linearGradient`, `radialGradient` or `pattern`.
  const char * server_id;                 ///< SERVER: the id as written; owned by the context.
  bool has_fallback;                      ///< SERVER: the document gave a paint after the url.
  GVEC_Svg_Paint_Kind fallback_kind;      ///< SERVER: NONE or COLOR.
  GVEC_Svg_Color fallback_color;          ///< SERVER with a COLOR fallback.
} GVEC_Svg_Paint;

/** The functions of a transform list that a 2D drawing takes. */
typedef enum GVEC_Svg_Transform_Op_Kind {
  GVEC_SVG_TRANSFORM_MATRIX = 0, ///< `matrix(a, b, c, d, e, f)`: `v[0..5]`.
  GVEC_SVG_TRANSFORM_TRANSLATE,  ///< `v[0]`, `v[1]`; a percentage is a fraction of the reference box (`percent`).
  GVEC_SVG_TRANSFORM_SCALE,      ///< `v[0]`, `v[1]`.
  GVEC_SVG_TRANSFORM_ROTATE,     ///< `v[0]` degrees, positive from +x toward +y, about the origin.
  GVEC_SVG_TRANSFORM_SKEW_X,     ///< `v[0]` degrees.
  GVEC_SVG_TRANSFORM_SKEW_Y,     ///< `v[0]` degrees.
  GVEC_SVG_TRANSFORM_OP_COUNT
} GVEC_Svg_Transform_Op_Kind;

/** One function of a transform list, in the list's order (the first is outermost). */
typedef struct GVEC_Svg_Transform_Op {
  GVEC_Svg_Transform_Op_Kind kind;
  double v[6];
  bool percent[2]; ///< TRANSLATE: `v[0]` or `v[1]` is a fraction (50% is 0.5) of the reference box's width or height, not a length.
} GVEC_Svg_Transform_Op;

/** What `transform-box` makes the reference box of percentages. The numbers are stable. */
typedef enum GVEC_Svg_Transform_Box {
  GVEC_SVG_BOX_VIEW = 0,   ///< The nearest viewport: resolved already.
  GVEC_SVG_BOX_FILL,       ///< The object bounding box.
  GVEC_SVG_BOX_STROKE,     ///< The stroke bounding box.
  GVEC_SVG_BOX_CONTENT,    ///< As the object bounding box, for SVG.
  GVEC_SVG_BOX_BORDER,     ///< As the stroke bounding box, for SVG.
  GVEC_SVG_BOX_COUNT
} GVEC_Svg_Transform_Box;

/**
 * A transform: the list of functions, the origin they turn about, and the box the percentages are of.
 *
 * With `transform-box: view-box` (the initial value) the reference box is the nearest viewport, which the instance knows, so
 * every percentage is resolved and `map` is the final map. With another box a percentage depends on a bounding box that only the
 * lowering has: `depends_on_box` is set, and gvec_svg_transform_resolve() makes the map from the box.
 */
typedef struct GVEC_Svg_Transform {
  const GVEC_Svg_Transform_Op * ops; ///< The list (empty for `none`); owned by the context.
  size_t op_count;
  double origin[2];            ///< The origin: the absolute part of `transform-origin`, in user units.
  double origin_fraction[2];   ///< Its percentage part as a fraction (50% is 0.5) of the reference box; zero once resolved against the viewport.
  GVEC_Svg_Transform_Box box;  ///< `transform-box`.
  bool depends_on_box;         ///< Some percentage is of a bounding box: use gvec_svg_transform_resolve().
  GSHGEOM_Map map;             ///< `translate(origin) * ops * translate(-origin)` when `depends_on_box` is false; the identity otherwise.
} GVEC_Svg_Transform;

/**
 * The map of a transform about its origin: `translate(origin) * ops * translate(-origin)`, a percentage being a fraction of the
 * box: the origin is `box_x + origin + fraction * box_width` and likewise in y.
 *
 * @param box_x The reference box (the bounding box, in the element's user space); ignored when the transform does not depend on it.
 * @param out_map Receives the map.
 * @return GVEC_ERR_INVALID for a NULL argument; GVEC_ERR_UNSUPPORTED when an entry of the result is not finite or past the waist's
 *   coordinate limit (a skew of 90 degrees).
 */
GVEC_API GVEC_Result gvec_svg_transform_resolve(const GVEC_Svg_Transform * transform, double box_x, double box_y, double box_width,
    double box_height, GSHGEOM_Map * out_map);

/** One `url()` of a reference list, or one value written another way. */
typedef struct GVEC_Svg_Style_Reference {
  const char * text;                 ///< The item as CSS writes it (`url("#a")`, `blur(2px)`); owned by the context.
  bool is_url;                       ///< The item is a `url()`.
  bool local;                        ///< A `url()` that names an element of this document (`#id`).
  const char * id;                   ///< Local: the id (without the `#`); owned by the context.
  const GVEC_Svg_Element * target;   ///< Local: the element, or NULL when the document has none of that id or the reference is cut.
  bool cut;                          ///< The reference closes a cycle through the references of properties
                                     ///< (gvec_svg_style_resolve_references()): it is read as no reference.
} GVEC_Svg_Style_Reference;

/** A read value. Which member is meaningful is `kind`. Strings and arrays are owned by the context. */
typedef struct GVEC_Svg_Style_Value {
  GVEC_Svg_Style_Value_Kind kind;
  const char * keyword;          ///< KEYWORD: the computed value as text, as CSS serialises it.
  double number;                 ///< NUMBER: the value; LENGTH: user units.
  bool length_auto;              ///< LENGTH: the value is `auto` (`number` is 0).
  GVEC_Svg_Color color;          ///< COLOR.
  GVEC_Svg_Paint paint;          ///< PAINT.
  const double * numbers;        ///< NUMBER_LIST: the entries in user units; empty for `none`.
  size_t number_count;
  GVEC_Svg_Transform transform;  ///< TRANSFORM.
  const GVEC_Svg_Style_Reference * references; ///< REFERENCES: empty for `none`.
  size_t reference_count;
} GVEC_Svg_Style_Value;

/**
 * Read the used value of a property of an instance.
 *
 * @param out_value Receives the value (`kind` NONE for a shorthand, which has no value of its own).
 * @return GVEC_OK; GVEC_ERR_INVALID (a NULL argument or a value that is no property); GVEC_ERR_OOM; GVEC_ERR_UNSUPPORTED
 *   when the value cannot be used: a colour in a space the waist cannot carry (reported GVEC_DIAG_UNSUPPORTED_COLOR_SPACE),
 *   or a property the engine cannot compute for this element (reported GVEC_DIAG_STYLE_INVALID). The caller refuses
 *   the element.
 */
GVEC_API GVEC_Result gvec_svg_get(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    GVEC_Svg_Property property, GVEC_Svg_Style_Value * out_value);

/** Where the cascade got a value. */
typedef enum GVEC_Svg_Style_Source {
  GVEC_SVG_STYLE_FROM_INHERITANCE = 0, ///< No declaration: the inherited or the initial value.
  GVEC_SVG_STYLE_FROM_PRESENTATION,    ///< A presentation attribute.
  GVEC_SVG_STYLE_FROM_USER_AGENT,      ///< The user agent style sheet of SVG 2 (`display: none` of a `defs`, `overflow: hidden` of a nested `svg`).
  GVEC_SVG_STYLE_FROM_STYLE_ATTRIBUTE, ///< The `style` attribute.
  GVEC_SVG_STYLE_FROM_COUNT
} GVEC_Svg_Style_Source;

/** The computed value of a property and where it came from. */
typedef struct GVEC_Svg_Style_Specified {
  GVEC_Svg_Style_Source source;
  bool important;        ///< A `style` declaration with `!important`.
  const char * computed; ///< The computed value, as the engine serialises it (`em` already absolute, percentages and colours as written); NULL when the property has none.
} GVEC_Svg_Style_Specified;

/** Read the computed value of a property, unresolved, with its provenance. Errors as gvec_svg_get(). */
GVEC_API GVEC_Result gvec_svg_get_specified(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    GVEC_Svg_Property property, GVEC_Svg_Style_Specified * out_specified);

//-----------------------------------------------------------------------------
// References
//-----------------------------------------------------------------------------

/**
 * Add the references of properties to the document's reference graph and find its cycles.
 *
 * `fill` and `stroke` refer to a paint server, `clip-path` to a `clipPath`, `mask` to a `mask`, `marker-start`, `marker-mid` and
 * `marker-end` to a `marker`, and `filter` to a `filter`; with the `use` and `href` edges the loader found and the containment of
 * elements, the strongly connected components of the graph are the cycles. A clip path that clips itself, a pattern painted with
 * itself and a marker whose content uses it are each one. Every element of a cycle through a property is reported once
 * (GVEC_DIAG_REFERENCE_CYCLE), and the reference of a property that closes the cycle is **cut**: the paint reads as its fallback or
 * `none`, and a reference as a cut one (GVEC_Svg_Style_Reference::cut).
 *
 * This styles every element of the document in its own context, so a caller that walks the whole document calls it once, before
 * reading references. Cycles that exist only inside a `use` clone (an inherited paint that refers to an ancestor of the `use`) are not
 * found here: the lowering, which holds the stack of the elements being instantiated, cuts them.
 *
 * @return GVEC_OK (also when called again: the work is done once); GVEC_ERR_OOM; GVEC_ERR_LIMIT.
 */
GVEC_API GVEC_Result gvec_svg_style_resolve_references(GVEC_Svg_Style_Context * context);

/** Whether the element is on a cycle through the references of properties found by gvec_svg_style_resolve_references(). */
GVEC_API bool gvec_svg_style_on_reference_cycle(const GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * element);

//-----------------------------------------------------------------------------
// The dump and the hash
//-----------------------------------------------------------------------------

/**
 * Write the computed style of one instance in the canonical, versioned text of the engine
 * (a first line `css-computed 1`, then one line for each property in the byte order of the names).
 *
 * @return GVEC_ERR_IO if a write fails; GVEC_ERR_OOM; GVEC_ERR_INVALID.
 */
GVEC_API GVEC_Result gvec_svg_style_dump(
    GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Stream * out);

/**
 * The hash of the style of every element of the document drawn in the document's own context, in document order: the
 * low 64 bits of MurmurHash3 x64-128 over the element keys and their computed-style dumps. Identical on every target.
 */
GVEC_API GVEC_Result gvec_svg_style_hash(GVEC_Svg_Style_Context * context, uint64_t * out_hash);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_STYLE_H
