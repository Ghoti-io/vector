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
 * @file
 *
 * The motion path of `animateMotion` (design.md A7): path data measured by the shape family's arc length, the distance of each
 * vertex (for the calcModes that give each segment of the path its own share of the time), and the point and direction at a distance.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/math/elementary.h>
#include <ghoti.io/shape-geometry/along.h>
#include <ghoti.io/shape-geometry/arc.h>
#include <ghoti.io/shape-geometry/path.h>

#include "../../../core/core_internal.h"
#include "anim_motion.h"

static GSHGEOM_Point pt(double x, double y) {
  GSHGEOM_Point p = {x, y, 0};
  return p;
}

static bool finite_(double x) {
  return x - x == 0;
}

/* Append one segment; a move starts a subpath. */
static GSHGEOM_Result append(GSHGEOM_Path * path, const GVEC_Svg_Path_Segment * s, bool * open) {
  switch (s->kind) {
  case GVEC_SVG_SEGMENT_MOVE:
    *open = true;
    return gshgeom_path_move_to(path, pt(s->x, s->y));
  case GVEC_SVG_SEGMENT_LINE:
    return gshgeom_path_line_to(path, pt(s->x, s->y));
  case GVEC_SVG_SEGMENT_QUAD:
    return gshgeom_path_quadratic_to(path, pt(s->x1, s->y1), pt(s->x, s->y));
  case GVEC_SVG_SEGMENT_CUBIC:
    return gshgeom_path_cubic_to(path, pt(s->x1, s->y1), pt(s->x2, s->y2), pt(s->x, s->y));
  case GVEC_SVG_SEGMENT_ARC:
    return gshgeom_path_arc_endpoint_to(path, pt(s->x, s->y), s->rx, s->ry, s->rotation, GSHGEOM_ANGLE_DEGREES, s->large_arc, s->sweep, NULL);
  case GVEC_SVG_SEGMENT_CLOSE:
    *open = false;
    return gshgeom_path_close(path);
  default:
    return GSHGEOM_ERR_INVALID;
  }
}

static GVEC_Result measure(const GVEC_Svg_Path_Segment * segments, size_t count, GSHGEOM_Measured ** out) {
  GSHGEOM_Path * path = NULL;
  if (gshgeom_path_create(NULL, NULL, 2, &path) != GSHGEOM_OK) {
    return GVEC_ERR_OOM;
  }
  bool open = false;
  for (size_t i = 0; i < count; i++) {
    if (i == 0 && segments[0].kind != GVEC_SVG_SEGMENT_MOVE) {
      gshgeom_path_move_to(path, pt(segments[0].x0, segments[0].y0));
    }
    GSHGEOM_Result g = append(path, &segments[i], &open);
    if (g != GSHGEOM_OK) {
      gshgeom_path_destroy(path);
      return g == GSHGEOM_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_LIMIT;
    }
  }
  GSHGEOM_Result g = gshgeom_measured_create(NULL, NULL, path, 0, out);
  gshgeom_path_destroy(path);
  return g == GSHGEOM_OK ? GVEC_OK : g == GSHGEOM_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_LIMIT;
}

GVEC_Result gvec_anim_motion_path(const GVEC_Svg_Path_Segment * segments, size_t count, const GVEC_Allocator * allocator, gvec_anim_motion * out) {
  memset(out, 0, sizeof *out);
  out->allocator = allocator;
  for (size_t i = 0; i < count; i++) {
    const GVEC_Svg_Path_Segment * s = &segments[i];
    if (!finite_(s->x) || !finite_(s->y) || !finite_(s->x1) || !finite_(s->y1) || !finite_(s->x2) || !finite_(s->y2) || !finite_(s->rx) ||
        !finite_(s->ry) || !finite_(s->rotation)) {
      return GVEC_ERR_LIMIT;
    }
  }
  if (count == 0) {
    return GVEC_ERR_INVALID;
  }
  GVEC_Result r = measure(segments, count, &out->measured);
  if (r != GVEC_OK) {
    return r;
  }
  out->length = gshgeom_measured_length(out->measured);
  /* The vertices: the start of the path and the end of each segment that is not a move (a move jumps; it takes no time of its own). */
  out->vertices = gvec_alloc(allocator, (count + 1) * sizeof *out->vertices);
  if (!out->vertices) {
    gvec_anim_motion_free(out);
    return GVEC_ERR_OOM;
  }
  out->vertices[out->vertex_count++] = 0;
  double at = 0;
  for (size_t i = 0; i < count; i++) {
    const GVEC_Svg_Path_Segment * s = &segments[i];
    if (s->kind == GVEC_SVG_SEGMENT_MOVE) {
      continue;
    }
    GVEC_Svg_Path_Segment one[2];
    memset(one, 0, sizeof one);
    one[0].kind = GVEC_SVG_SEGMENT_MOVE;
    one[0].x = s->x0;
    one[0].y = s->y0;
    one[1] = *s;
    if (s->kind == GVEC_SVG_SEGMENT_CLOSE) {
      one[1].kind = GVEC_SVG_SEGMENT_LINE;
    }
    GSHGEOM_Measured * m = NULL;
    r = measure(one, 2, &m);
    if (r != GVEC_OK) {
      gvec_anim_motion_free(out);
      return r;
    }
    at += gshgeom_measured_length(m);
    gshgeom_measured_destroy(m);
    if (at > out->length) {
      at = out->length;
    }
    out->vertices[out->vertex_count++] = at;
  }
  return GVEC_OK;
}

GVEC_Result gvec_anim_motion_points(const double * xy, size_t points, const GVEC_Allocator * allocator, gvec_anim_motion * out) {
  if (points == 0) {
    return GVEC_ERR_INVALID;
  }
  GVEC_Svg_Path_Segment * segs = gvec_alloc(allocator, points * sizeof *segs);
  if (!segs) {
    return GVEC_ERR_OOM;
  }
  memset(segs, 0, points * sizeof *segs);
  for (size_t i = 0; i < points; i++) {
    segs[i].kind = i == 0 ? GVEC_SVG_SEGMENT_MOVE : GVEC_SVG_SEGMENT_LINE;
    segs[i].x0 = i ? xy[2 * (i - 1)] : xy[0];
    segs[i].y0 = i ? xy[2 * (i - 1) + 1] : xy[1];
    segs[i].x = xy[2 * i];
    segs[i].y = xy[2 * i + 1];
  }
  GVEC_Result r = gvec_anim_motion_path(segs, points, allocator, out);
  gvec_free(allocator, segs);
  return r;
}

bool gvec_anim_motion_at(const gvec_anim_motion * m, double distance, double * x, double * y, double * angle) {
  GSHGEOM_Measured_Frame f;
  if (!m->measured || gshgeom_measured_at_clamped(m->measured, distance, &f) != GSHGEOM_OK) {
    return false;
  }
  *x = f.point.x;
  *y = f.point.y;
  *angle = f.has_tangent ? gmath_atan2(f.tangent.y, f.tangent.x) * (180 / 3.14159265358979323846) : 0;
  return true;
}

void gvec_anim_motion_free(gvec_anim_motion * m) {
  if (m->measured) {
    gshgeom_measured_destroy(m->measured);
  }
  gvec_free(m->allocator, m->vertices);
  m->measured = NULL;
  m->vertices = NULL;
  m->vertex_count = 0;
}
