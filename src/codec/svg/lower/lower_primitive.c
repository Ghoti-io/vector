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
 * @file lower_primitive.c
 *
 * `rect`, `circle` and `ellipse` as the shape family's named primitives (SVG 2, 10.2 to 10.4). The style layer supplies each
 * geometry property as a used length in user units (a percentage already resolved against its basis); this file applies the
 * rules the format owns and hands the waist the resolved numbers:
 *
 * - a `rect` of no width or no height, a `circle` of no radius and an `ellipse` with a zero radius are not rendered;
 * - `rx` and `ry` take each other's value when one is `auto` (or negative, which is an error that falls back to `auto`), both
 *   missing is square, each is clamped to half its side, and a zero radius is a plain rectangle;
 * - the corners are the same ellipse in all four, so the waist's per-corner radii are equal.
 *
 * Nothing here builds a curve: the waist's constructor makes the path and tags it, and the tag is what a sink that reads it needs.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/shape-scene/primitive.h>
#include <ghoti.io/shape-scene/resource.h>

#include "lower_internal.h"

static bool finite(double v) {
  return v - v == 0;
}

/* A geometry property as a used length; `auto` and a negative value are reported as `automatic`. */
static GVEC_Result length_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, double * out,
    bool * automatic, bool * usable) {
  GVEC_Svg_Style_Value v;
  *out = 0;
  *automatic = false;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, property, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  if (v.kind != GVEC_SVG_STYLE_LENGTH || v.length_auto || v.number < 0) {
    *automatic = true;
    return GVEC_OK;
  }
  *out = v.number;
  return GVEC_OK;
}

/* Intern the primitive, hold it, and say what came of it. */
static GVEC_Result intern(gvec_lw_ctx * ctx, const GSHSCENE_Primitive * primitive, gvec_lw_geometry * kind,
    const GSHSCENE_Resource ** out) {
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result r = gshscene_store_intern_primitive(ctx->store, primitive, &resource);
  if (r == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (r != GSHSCENE_OK) {
    *kind = GVEC_LW_GEOMETRY_LIMIT; /* A number past the waist's range, or a store limit: the element is refused. */
    return GVEC_OK;
  }
  GVEC_Result held = gvec_lw_hold(ctx, resource);
  if (held != GVEC_OK) {
    return held;
  }
  *out = resource;
  *kind = GVEC_LW_GEOMETRY_PATH;
  return GVEC_OK;
}

static GVEC_Result rect_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_geometry * kind, const GSHSCENE_Resource ** out,
    bool * usable) {
  double x, y, w, h, rx, ry;
  bool automatic, rx_auto, ry_auto;
  GVEC_Result r = length_of(ctx, inst, GVEC_SVG_PROP_X, &x, &automatic, usable);
  if (r == GVEC_OK && *usable) {
    r = length_of(ctx, inst, GVEC_SVG_PROP_Y, &y, &automatic, usable);
  }
  if (r == GVEC_OK && *usable) {
    r = length_of(ctx, inst, GVEC_SVG_PROP_WIDTH, &w, &automatic, usable);
  }
  if (r == GVEC_OK && *usable) {
    r = length_of(ctx, inst, GVEC_SVG_PROP_HEIGHT, &h, &automatic, usable);
  }
  if (r == GVEC_OK && *usable) {
    r = length_of(ctx, inst, GVEC_SVG_PROP_RX, &rx, &rx_auto, usable);
  }
  if (r == GVEC_OK && *usable) {
    r = length_of(ctx, inst, GVEC_SVG_PROP_RY, &ry, &ry_auto, usable);
  }
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  if (!finite(x) || !finite(y) || !finite(w) || !finite(h) || !finite(rx) || !finite(ry)) {
    *kind = GVEC_LW_GEOMETRY_LIMIT;
    return GVEC_OK;
  }
  if (!(w > 0) || !(h > 0)) {
    return GVEC_OK; /* A width or height of zero (or `auto`) disables rendering. */
  }
  if (rx_auto) {
    rx = ry; /* Both `auto` leaves both at 0: square. */
  }
  if (ry_auto) {
    ry = rx;
  }
  if (rx > w / 2) {
    rx = w / 2;
  }
  if (ry > h / 2) {
    ry = h / 2;
  }
  GSHSCENE_Primitive p;
  memset(&p, 0, sizeof p);
  p.x = x;
  p.y = y;
  p.w = w;
  p.h = h;
  if (rx > 0 && ry > 0) {
    p.kind = GSHSCENE_PRIMITIVE_ROUNDED_RECT;
    for (int i = 0; i < 4; i++) {
      p.r[i].rx = rx;
      p.r[i].ry = ry;
    }
  } else {
    p.kind = GSHSCENE_PRIMITIVE_RECT;
  }
  return intern(ctx, &p, kind, out);
}

static GVEC_Result ellipse_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, bool circle, gvec_lw_geometry * kind,
    const GSHSCENE_Resource ** out, bool * usable) {
  double cx, cy, rx, ry;
  bool automatic, rx_auto = false, ry_auto = false;
  GVEC_Result r = length_of(ctx, inst, GVEC_SVG_PROP_CX, &cx, &automatic, usable);
  if (r == GVEC_OK && *usable) {
    r = length_of(ctx, inst, GVEC_SVG_PROP_CY, &cy, &automatic, usable);
  }
  if (r == GVEC_OK && *usable) {
    if (circle) {
      r = length_of(ctx, inst, GVEC_SVG_PROP_R, &rx, &rx_auto, usable);
      ry = rx;
      ry_auto = rx_auto;
    } else {
      r = length_of(ctx, inst, GVEC_SVG_PROP_RX, &rx, &rx_auto, usable);
      if (r == GVEC_OK && *usable) {
        r = length_of(ctx, inst, GVEC_SVG_PROP_RY, &ry, &ry_auto, usable);
      }
    }
  }
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  if (rx_auto) {
    rx = ry; /* Both `auto` leaves both at 0 (ry is 0 then): not rendered. */
  }
  if (ry_auto) {
    ry = rx;
  }
  if (!finite(cx) || !finite(cy) || !finite(rx) || !finite(ry)) {
    *kind = GVEC_LW_GEOMETRY_LIMIT;
    return GVEC_OK;
  }
  if (!(rx > 0) || !(ry > 0)) {
    return GVEC_OK;
  }
  GSHSCENE_Primitive p;
  memset(&p, 0, sizeof p);
  p.kind = GSHSCENE_PRIMITIVE_ELLIPSE;
  p.cx = cx;
  p.cy = cy;
  p.rx = rx;
  p.ry = ry;
  return intern(ctx, &p, kind, out);
}

GVEC_Result gvec_lw_primitive_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_geometry * out_kind,
    const GSHSCENE_Resource ** out_resource, bool * usable) {
  *out_kind = GVEC_LW_GEOMETRY_EMPTY;
  *out_resource = NULL;
  *usable = true;
  switch (gvec_svg_element_kind(gvec_svg_instance_element(inst))) {
  case GVEC_SVG_KIND_RECT:
    return rect_of(ctx, inst, out_kind, out_resource, usable);
  case GVEC_SVG_KIND_CIRCLE:
    return ellipse_of(ctx, inst, true, out_kind, out_resource, usable);
  case GVEC_SVG_KIND_ELLIPSE:
    return ellipse_of(ctx, inst, false, out_kind, out_resource, usable);
  default:
    return GVEC_OK;
  }
}
