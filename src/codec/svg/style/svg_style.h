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
 * @file svg_style.h
 *
 * The style layer's parts: the registry of properties, the context and its instances, and the used values.
 */

#ifndef GHOTI_IO_GVEC_SVG_STYLE_H
#define GHOTI_IO_GVEC_SVG_STYLE_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/text/css.h>

#include <ghoti.io/vector/style.h>

#include "../../../core/core_internal.h"
#include "../svg_arena.h"
#include "../svg_doc.h"
#include "svg_prop_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The registry of `text` with this library's registrations and overrides, and the property ids this layer reads. */
typedef struct gvec_svg_registry {
  GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Property_Id id[GVEC_SVG_PROP_COUNT];    ///< The registry's property for each SVG property, aliases followed.
  GTEXT_CSS_Property_Id reads[GVEC_SVG_PROP_COUNT]; ///< The property whose computed value is read.
  GTEXT_CSS_Property_Id color_scheme;               ///< `color-scheme`, which decides the used scheme of the system colors.
  GTEXT_CSS_Presentation_Row * rows;                ///< The presentation attributes: SVG 2's table of `text` and this library's additions.
  GTEXT_CSS_Presentation_Table table;
  size_t row_count;
  const GVEC_Allocator * allocator;
} gvec_svg_registry;

/**
 * Build the registry: the generated layer, the properties the CR has and the pin lacks, and the overrides; then check that
 * every generated row exists and every registered one does not (the pin changed under this library: GVEC_ERR_INTERNAL).
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_registry_new(const GVEC_Allocator * allocator, gvec_svg_registry * out);

GVEC_INTERNAL_API void gvec_svg_registry_free(gvec_svg_registry * registry);

/** The things reported once for an element, whichever instance of it is drawn. */
typedef enum gvec_svg_once {
  GVEC_ONCE_STYLE = 0,   ///< The declarations dropped from its style.
  GVEC_ONCE_COLOR,       ///< A colour it uses that the waist cannot carry.
  GVEC_ONCE_REFERENCE,   ///< A reference of a property it uses that names nothing.
  GVEC_ONCE_CYCLE,       ///< It is on a cycle through the references of properties.
  GVEC_ONCE_COUNT
} gvec_svg_once;

/**
 * What a style context needs of `text` that depends on nothing in the document but the environment: the registry of properties, the
 * compute context made over it (the initial value of every property is computed once, here) and the element interface. Making it costs
 * milliseconds, so engines made with the default allocator are kept, by environment, and shared between documents and threads (they are
 * immutable once made); an engine made with another allocator belongs to its context.
 */
typedef struct gvec_svg_engine {
  gvec_svg_registry reg;
  GTEXT_CSS_Compute_Context * compute;
  GTEXT_CSS_Element_Interface table;
  GTEXT_CSS_Environment env;
  /* What names the environment: the key of the cache. */
  double viewport_width, viewport_height;
  GVEC_Color_Scheme color_scheme;
  size_t max_custom_expansion;
  const GVEC_Allocator * allocator;
  bool shared;                     ///< In the cache (and owned by it while it is there).
  size_t references;               ///< Contexts using it, plus one while it is in the cache. Guarded by the cache's lock when `shared`.
  struct gvec_svg_engine * next;   ///< The cache's list.
} gvec_svg_engine;

/** The engine for an environment: from the cache when `allocator` is the default and one was made, else made now. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_engine_acquire(const GVEC_Allocator * allocator, double viewport_width, double viewport_height,
    GVEC_Color_Scheme scheme, size_t max_custom_expansion, gvec_svg_engine ** out);

/** Give up a reference; the last one frees an engine that is not in the cache. */
GVEC_INTERNAL_API void gvec_svg_engine_release(gvec_svg_engine * engine);

/** An instance (see style.h). */
struct GVEC_Svg_Instance {
  gvec_svg_handle handle;                 ///< What the element interface is handed for this instance (its `element` is `element`).
  const GVEC_Svg_Element * element;
  const GVEC_Svg_Instance * parent;       ///< The inheritance parent; NULL for the root of inheritance.
  const GVEC_Svg_Instance * use;          ///< The nearest enclosing `use` instance: the clone context; NULL in the document's own.
  double viewport_width, viewport_height; ///< The basis of percentages for this element's own properties.
  double override_width, override_height; ///< The `width` and `height` a `use` gives the `svg` or `symbol` it instantiates, where `has_override_*`.
  bool has_override_width, has_override_height;
  GTEXT_CSS_Computed_Style * style;       ///< Memoised: owned by the context.
  size_t style_slot;                      ///< One more than the index of `style` in the context's list, while it has one.
  uint32_t depth;                         ///< Inheritance ancestors above this instance.
  bool style_failed;                      ///< A computation that failed is not repeated.
  GVEC_Result style_result;
};

/** A style context (see style.h). */
struct GVEC_Svg_Style_Context {
  const GVEC_Svg_Document * document;
  const GVEC_Allocator * allocator;
  GVEC_Diagnostics * diagnostics;
  GVEC_Limits limits;
  gvec_arena arena;                 ///< Instances, strings and the arrays the values point into.
  gvec_svg_engine * engine;
  GTEXT_CSS_Style_Options style_options;
  double viewport_width, viewport_height;
  GVEC_Color_Scheme color_scheme;
  size_t instances;                 ///< Made so far, for the bound.
  uint8_t * once[GVEC_ONCE_COUNT];  ///< One bit for each element of the document and each kind of thing reported once for it.
  const GVEC_Svg_Instance ** document_instances; ///< Made by gvec_svg_document_instances().
  uint32_t * component;             ///< After gvec_svg_style_resolve_references(): for each element, the number of the reference cycle it is on (from 1), else 0.
  bool references_resolved;
  GTEXT_CSS_Rule_Index * rules;     ///< The rules of the document's `style` elements, frozen; NULL when it has none or before gvec_svg_style_sheets().
  GTEXT_CSS_Selector_Match_Context * match; ///< The match context of `rules` over the document's tree.
  bool sheets_ready;                ///< gvec_svg_style_sheets() ran.
  GVEC_Result sheets_result;        ///< What it returned.
  const GVEC_Svg_Element * sheet_refused; ///< The first `style` element whose sheet could not be applied (an `@import`, a conditional rule), else NULL.
  const char * sheet_refused_why;   ///< Static: why.
  GTEXT_CSS_Computed_Style ** styles; ///< Every style made, to free.
  size_t style_count, style_capacity;
};

/** The element of an instance handle given back by the engine's callbacks. */
GVEC_INTERNAL_API void gvec_svg_element_interface(GTEXT_CSS_Element_Interface * table, void * user);

/** The environment of the engine for a context (viewport, color scheme, the font metrics of a document with no fonts). */
GVEC_INTERNAL_API void gvec_svg_environment(GTEXT_CSS_Environment * env, double width, double height, GVEC_Color_Scheme scheme, void * user);

/**
 * An instance of every element of the document in its own context, in document order: made once, in memory the context owns.
 * (The instance of an element is made from its parent's, which came earlier.)
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_document_instances(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * const ** out);

/** Whether this is the first time `kind` is reported for `element` (and remember it). */
GVEC_INTERNAL_API bool gvec_svg_once_first(GVEC_Svg_Style_Context * context, gvec_svg_once kind, const GVEC_Svg_Element * element);

/** A diagnostic of the style layer, located at the attribute when there is one, else at the element. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_style_diag(GVEC_Svg_Style_Context * context, GVEC_Diag_Severity severity, GVEC_Diag_Code code,
    uint32_t detail, const GVEC_Svg_Element * element, const GVEC_Svg_Attribute * attribute, const char * message);

/**
 * Read the document's `style` elements (those of type `text/css` or of none, in document order, a sheet being the character data of one)
 * into one rule index and make the match context over the document's tree, the first time; every computation of a style uses them.
 * A rule or declaration a sheet drops is reported as a warning at its `style` element. A sheet that cannot be applied at all (an
 * `@import`, which is never fetched, and `@media` and the other conditional rules, which need an evaluator this layer has none of) is
 * not applied and is named by gvec_svg_style_sheet_refused(): drawing without it would be drawing the wrong picture.
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_style_sheets(GVEC_Svg_Style_Context * context);

/** After gvec_svg_style_sheets(): the first `style` element whose sheet was not applied, or NULL; `why` receives a static reason. */
GVEC_INTERNAL_API const GVEC_Svg_Element * gvec_svg_style_sheet_refused(const GVEC_Svg_Style_Context * context, const char ** why);

/** The most declarations of the user agent style sheet that match one element. */
#define GVEC_SVG_UA_MAX 6

/**
 * The declarations of the user agent style sheet of SVG 2 that match the instance's element (svg_ua.c), in the sheet's order, each a
 * declaration list of one declaration; static strings. Returns how many.
 */
GVEC_INTERNAL_API size_t gvec_svg_ua_declarations(const GVEC_Svg_Instance * instance, const char * out[GVEC_SVG_UA_MAX]);

/** The computed style of an instance, computing it and its inheritance ancestors' (iteratively) the first time. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_instance_style(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    const GTEXT_CSS_Computed_Style ** out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_STYLE_H
