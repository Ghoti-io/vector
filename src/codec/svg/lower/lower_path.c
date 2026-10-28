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
 * @file lower_path.c
 *
 * The geometry of `path`, `polygon` and `polyline` as a model path (P7), made once for both emitters and interned in the store.
 * Arcs are not converted: they are the shape family's exact primitive and the element is refused until it draws them.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/resource.h>

#include "lower_internal.h"

static GVEC_Result translate(GSHGEOM_Result r, gvec_lw_geometry * kind) {
  if (r == GSHGEOM_OK) {
    return GVEC_OK;
  }
  if (r == GSHGEOM_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  /* A coordinate past the waist's range or a count past its limit: the element is refused. */
  *kind = GVEC_LW_GEOMETRY_LIMIT;
  return GVEC_OK;
}

/* Whether a number is finite: a relative path whose sums pass the largest double has no point to put in a path. */
static bool finite(double v) {
  return v - v == 0;
}

static GSHGEOM_Point point(double x, double y) {
  GSHGEOM_Point p;
  p.x = x;
  p.y = y;
  p.z = 0;
  return p;
}

/* Intern a finished path and hold the resource. */
static GVEC_Result finish(gvec_lw_ctx * ctx, GSHGEOM_Path * path, gvec_lw_geometry * kind, const GSHSCENE_Resource ** out) {
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result r = gshscene_store_intern_path(ctx->store, path, &resource);
  gshgeom_path_destroy(path);
  if (r == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (r != GSHSCENE_OK) {
    *kind = GVEC_LW_GEOMETRY_LIMIT;
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

static GVEC_Result path_segments(gvec_lw_ctx * ctx, const GVEC_Svg_Path_Segment * segments, size_t count, gvec_lw_geometry * kind,
    const GSHSCENE_Resource ** out) {
  /* What is drawn: a segment with length. One move, or a move and a close, fills nothing (SVG 2, 9.3.4). */
  bool drawn = false;
  for (size_t i = 0; i < count; i++) {
    const GVEC_Svg_Path_Segment * s = &segments[i];
    if (s->kind == GVEC_SVG_SEGMENT_ARC) {
      *kind = GVEC_LW_GEOMETRY_ARC;
      return GVEC_OK;
    }
    if (!finite(s->x0) || !finite(s->y0) || !finite(s->x1) || !finite(s->y1) || !finite(s->x2) || !finite(s->y2) || !finite(s->x) ||
        !finite(s->y)) {
      *kind = GVEC_LW_GEOMETRY_LIMIT;
      return GVEC_OK;
    }
    switch (s->kind) {
    case GVEC_SVG_SEGMENT_LINE:
      drawn = drawn || s->x != s->x0 || s->y != s->y0;
      break;
    case GVEC_SVG_SEGMENT_QUAD:
      drawn = drawn || s->x != s->x0 || s->y != s->y0 || s->x1 != s->x0 || s->y1 != s->y0;
      break;
    case GVEC_SVG_SEGMENT_CUBIC:
      drawn = drawn || s->x != s->x0 || s->y != s->y0 || s->x1 != s->x0 || s->y1 != s->y0 || s->x2 != s->x0 || s->y2 != s->y0;
      break;
    default:
      break;
    }
  }
  if (!drawn) {
    *kind = GVEC_LW_GEOMETRY_EMPTY;
    return GVEC_OK;
  }
  GSHGEOM_Path * path = NULL;
  GVEC_Result r = translate(gshgeom_path_create(NULL, NULL, 2, &path), kind);
  if (r != GVEC_OK || *kind == GVEC_LW_GEOMETRY_LIMIT) {
    return r;
  }
  for (size_t i = 0; i < count; i++) {
    const GVEC_Svg_Path_Segment * s = &segments[i];
    GSHGEOM_Result g = GSHGEOM_OK;
    switch (s->kind) {
    case GVEC_SVG_SEGMENT_MOVE:
      g = gshgeom_path_move_to(path, point(s->x, s->y));
      break;
    case GVEC_SVG_SEGMENT_LINE:
      g = gshgeom_path_line_to(path, point(s->x, s->y));
      break;
    case GVEC_SVG_SEGMENT_QUAD:
      g = gshgeom_path_quadratic_to(path, point(s->x1, s->y1), point(s->x, s->y));
      break;
    case GVEC_SVG_SEGMENT_CUBIC:
      g = gshgeom_path_cubic_to(path, point(s->x1, s->y1), point(s->x2, s->y2), point(s->x, s->y));
      break;
    case GVEC_SVG_SEGMENT_CLOSE:
      g = gshgeom_path_close(path);
      break;
    default:
      g = GSHGEOM_ERR_INVALID;
      break;
    }
    if (g != GSHGEOM_OK) {
      gshgeom_path_destroy(path);
      if (g == GSHGEOM_ERR_INVALID) {
        return GVEC_ERR_INTERNAL;
      }
      return translate(g, kind);
    }
  }
  return finish(ctx, path, kind, out);
}

static GVEC_Result polygon_points(
    gvec_lw_ctx * ctx, const double * numbers, size_t count, gvec_lw_geometry * kind, const GSHSCENE_Resource ** out) {
  size_t points = count / 2; /* An odd trailing number is an error that ends the list (SVG 2, 9.8.1): the points before it are kept. */
  if (points < 2) {
    *kind = GVEC_LW_GEOMETRY_EMPTY;
    return GVEC_OK;
  }
  for (size_t i = 0; i < 2 * points; i++) {
    if (!finite(numbers[i])) {
      *kind = GVEC_LW_GEOMETRY_LIMIT;
      return GVEC_OK;
    }
  }
  bool distinct = false;
  for (size_t i = 1; i < points && !distinct; i++) {
    distinct = numbers[2 * i] != numbers[0] || numbers[2 * i + 1] != numbers[1];
  }
  if (!distinct) {
    *kind = GVEC_LW_GEOMETRY_EMPTY;
    return GVEC_OK;
  }
  GSHGEOM_Path * path = NULL;
  GVEC_Result r = translate(gshgeom_path_create(NULL, NULL, 2, &path), kind);
  if (r != GVEC_OK || *kind == GVEC_LW_GEOMETRY_LIMIT) {
    return r;
  }
  GSHGEOM_Result g = gshgeom_path_move_to(path, point(numbers[0], numbers[1]));
  for (size_t i = 1; i < points && g == GSHGEOM_OK; i++) {
    g = gshgeom_path_line_to(path, point(numbers[2 * i], numbers[2 * i + 1]));
  }
  if (g == GSHGEOM_OK) {
    g = gshgeom_path_close(path);
  }
  if (g != GSHGEOM_OK) {
    gshgeom_path_destroy(path);
    return g == GSHGEOM_ERR_INVALID ? GVEC_ERR_INTERNAL : translate(g, kind);
  }
  return finish(ctx, path, kind, out);
}

GVEC_Result gvec_lw_path_of(
    gvec_lw_ctx * ctx, const GVEC_Svg_Element * element, gvec_lw_geometry * out_kind, const GSHSCENE_Resource ** out_resource) {
  *out_kind = GVEC_LW_GEOMETRY_EMPTY;
  *out_resource = NULL;
  switch (gvec_svg_element_kind(element)) {
  case GVEC_SVG_KIND_PATH: {
    const GVEC_Svg_Attribute * d = gvec_svg_element_find_attribute(element, "d");
    const GVEC_Svg_Path_Command * commands;
    const GVEC_Svg_Path_Segment * segments;
    size_t command_count, segment_count;
    if (!d || !gvec_svg_attribute_path(d, &commands, &command_count, &segments, &segment_count)) {
      return GVEC_OK;
    }
    return path_segments(ctx, segments, segment_count, out_kind, out_resource);
  }
  case GVEC_SVG_KIND_POLYGON:
  case GVEC_SVG_KIND_POLYLINE: {
    const GVEC_Svg_Attribute * points = gvec_svg_element_find_attribute(element, "points");
    const double * numbers;
    size_t count;
    if (!points || !gvec_svg_attribute_numbers(points, &numbers, &count)) {
      return GVEC_OK;
    }
    return polygon_points(ctx, numbers, count, out_kind, out_resource);
  }
  default:
    return GVEC_OK;
  }
}

GVEC_Result gvec_lw_rect_path(gvec_lw_ctx * ctx, double x, double y, double width, double height, gvec_lw_geometry * out_kind,
    const GSHSCENE_Resource ** out_resource) {
  *out_kind = GVEC_LW_GEOMETRY_EMPTY;
  *out_resource = NULL;
  /* The corners are x + width and y + height: one rounding each, which is the nearest the waist can hold to the box. */
  double x1 = x + width, y1 = y + height;
  if (!finite(x) || !finite(y) || !finite(x1) || !finite(y1)) {
    *out_kind = GVEC_LW_GEOMETRY_LIMIT;
    return GVEC_OK;
  }
  GSHGEOM_Path * path = NULL;
  GVEC_Result r = translate(gshgeom_path_create(NULL, NULL, 2, &path), out_kind);
  if (r != GVEC_OK || *out_kind == GVEC_LW_GEOMETRY_LIMIT) {
    return r;
  }
  GSHGEOM_Result g = gshgeom_path_move_to(path, point(x, y));
  if (g == GSHGEOM_OK) {
    g = gshgeom_path_line_to(path, point(x1, y));
  }
  if (g == GSHGEOM_OK) {
    g = gshgeom_path_line_to(path, point(x1, y1));
  }
  if (g == GSHGEOM_OK) {
    g = gshgeom_path_line_to(path, point(x, y1));
  }
  if (g == GSHGEOM_OK) {
    g = gshgeom_path_close(path);
  }
  if (g != GSHGEOM_OK) {
    gshgeom_path_destroy(path);
    return g == GSHGEOM_ERR_INVALID ? GVEC_ERR_INTERNAL : translate(g, out_kind);
  }
  return finish(ctx, path, out_kind, out_resource);
}
