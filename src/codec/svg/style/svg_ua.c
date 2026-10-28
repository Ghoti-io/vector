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
 * @file svg_ua.c
 *
 * The user agent style sheet of SVG 2 (6.8), as declarations the cascade takes for an element of a given kind.
 *
 * The engine has no selectors yet, and the sheet's selectors are all of the kind of the element, its parent and its place in a clone, which
 * the instance knows: so each element is given the declarations of the rules that match it, at the user agent origin, in the order of the
 * sheet, one declaration to a list (so that the bound on the declarations of a `style` attribute, which the engine applies to every list,
 * never refuses the sheet's own).
 *
 *     svg:not(:root), image, marker, pattern, symbol          { overflow: hidden }
 *     *:not(svg), *:not(foreignObject) > svg                  { transform-origin: 0 0 }
 *     *[xml|space=preserve]                                   { white-space-collapse: preserve-spaces }
 *     defs, clipPath, mask, marker, desc, title, metadata,
 *     pattern, linearGradient, radialGradient, script, style,
 *     symbol                                                  { display: none !important }
 *     :host(use) > symbol                                     { display: inline !important }
 *     :link, :visited                                         { cursor: pointer }
 *
 * The root `svg` keeps the initial `transform-origin`, the center, as the sheet says. `:link` is an `a` with an `href`.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "svg_style.h"

size_t gvec_svg_ua_declarations(const GVEC_Svg_Instance * instance, const char * out[GVEC_SVG_UA_MAX]) {
  const GVEC_Svg_Element * e = instance->element;
  const GVEC_Svg_Element * parent = gvec_svg_element_parent(e);
  size_t n = 0;
  bool svg = e->kind == GVEC_SVG_KIND_SVG;
  if ((svg && parent) || e->kind == GVEC_SVG_KIND_IMAGE || e->kind == GVEC_SVG_KIND_MARKER || e->kind == GVEC_SVG_KIND_PATTERN ||
      e->kind == GVEC_SVG_KIND_SYMBOL) {
    out[n++] = "overflow:hidden";
  }
  if (!svg || (parent && parent->kind != GVEC_SVG_KIND_FOREIGN_OBJECT)) {
    out[n++] = "transform-origin:0 0";
  }
  const GVEC_Svg_Attribute * space = gvec_svg_element_find_attribute(e, "xml:space");
  if (space) {
    size_t length = 0;
    const char * raw = gvec_svg_attribute_raw(space, &length);
    if (length == 8 && memcmp(raw, "preserve", 8) == 0) {
      out[n++] = "white-space-collapse:preserve-spaces";
    }
  }
  switch (e->kind) {
  case GVEC_SVG_KIND_DEFS:
  case GVEC_SVG_KIND_CLIP_PATH:
  case GVEC_SVG_KIND_MASK:
  case GVEC_SVG_KIND_MARKER:
  case GVEC_SVG_KIND_DESC:
  case GVEC_SVG_KIND_TITLE:
  case GVEC_SVG_KIND_METADATA:
  case GVEC_SVG_KIND_PATTERN:
  case GVEC_SVG_KIND_LINEAR_GRADIENT:
  case GVEC_SVG_KIND_RADIAL_GRADIENT:
  case GVEC_SVG_KIND_SCRIPT:
  case GVEC_SVG_KIND_STYLE:
    out[n++] = "display:none!important";
    break;
  case GVEC_SVG_KIND_SYMBOL:
    /* The shadow tree of a use holds the symbol it instantiates, and that one is drawn. */
    out[n++] = instance->use && instance->parent == instance->use ? "display:inline!important" : "display:none!important";
    break;
  case GVEC_SVG_KIND_A:
    if (gvec_svg_element_find_attribute(e, "href") || gvec_svg_element_find_attribute(e, "xlink:href")) {
      out[n++] = "cursor:pointer";
    }
    break;
  default:
    break;
  }
  return n;
}
