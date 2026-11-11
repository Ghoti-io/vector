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
 * @file svg_used.h
 *
 * The used values (C6 to C9): what only an SVG renderer can resolve from the engine's computed values.
 */

#ifndef GHOTI_IO_GVEC_SVG_USED_H
#define GHOTI_IO_GVEC_SVG_USED_H

#include <ghoti.io/vector/macros.h>

#include "svg_style.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A copy of `length` bytes of `text` that lives as long as the context; NULL when out of memory. */
GVEC_INTERNAL_API const char * gvec_svg_pool_string(GVEC_Svg_Style_Context * context, const char * text, size_t length);

/** The computed value of property `property` of `instance` (the registry property it reads); NULL where the style has none. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_computed(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    GVEC_Svg_Property property, const GTEXT_CSS_Computed_Style ** out_style, const GTEXT_CSS_Value ** out_value);

/** The basis a percentage of a property is of, in user units, for an instance (C6). */
/* A number read from a declaration, kept inside the range of a single-precision float as Chromium does: "2e999" is the largest
 * magnitude there is, not an infinity, so no computed value is ever not finite. */
static inline double gvec_svg_clamp_number(double v) {
  if (v != v) {
    return 0;
  }
  if (v > 3.4028234663852886e38) {
    return 3.4028234663852886e38;
  }
  if (v < -3.4028234663852886e38) {
    return -3.4028234663852886e38;
  }
  return v;
}

GVEC_INTERNAL_API double gvec_svg_basis(const GVEC_Svg_Instance * instance, gvec_svg_prop_basis basis);

/**
 * A used length (C6): the value of a length-typed property in user units, a percentage resolved against the property's basis.
 *
 * @param out_auto Receives whether the value is `auto` (then `*out` is 0).
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_used_length(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    GVEC_Svg_Property property, double * out, bool * out_auto);

/** A length value (a dimension, a percentage, a number, a calculation) in user units against `basis`; false if it is none of these. */
GVEC_INTERNAL_API bool gvec_svg_length_of_value(const GTEXT_CSS_Value * value, double basis, double * out, bool * out_auto);

/** A colour value: the used colour of `color`, `stop-color` and the other colour properties, and the colour in a paint (C7, C8). */
GVEC_INTERNAL_API GVEC_Result gvec_svg_used_color(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    const GTEXT_CSS_Value * value, const GVEC_Svg_Element * where, GVEC_Svg_Color * out);

/**
 * The element a `url()` names: a fragment, `#id`, with its percent-escapes decoded. Anything else names nothing here.
 *
 * @param out_id Receives the decoded id, in the context's memory, when the url is a fragment (even if no element has that id).
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_resolve_url(GVEC_Svg_Style_Context * context, const GTEXT_CSS_String * url, bool * out_local,
    const char ** out_id, const GVEC_Svg_Element ** out_target);

/** Whether a reference of a property to `target` is cut because both are on one cycle through references (gvec_svg_style_resolve_references). */
GVEC_INTERNAL_API bool gvec_svg_reference_cut(const GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * from, const GVEC_Svg_Element * to);

/** A paint (`fill`, `stroke`). */
GVEC_INTERNAL_API GVEC_Result gvec_svg_used_paint(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    const GTEXT_CSS_Value * value, GVEC_Svg_Paint * out);

/** `stroke-dasharray`: entries in user units; negative values and arrays summing to zero are `none` (SVG 2, 13.5.2). */
GVEC_INTERNAL_API GVEC_Result gvec_svg_used_dasharray(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    const GTEXT_CSS_Value * value, GVEC_Svg_Style_Value * out);

/** `transform`, `transform-origin` and `transform-box`. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_used_transform(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    GVEC_Svg_Transform * out);

/** `clip-path`, `mask`, `filter` and the markers. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_used_references(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance,
    const GTEXT_CSS_Value * value, GVEC_Svg_Style_Value * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_USED_H
