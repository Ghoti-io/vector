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
 * The motion path of `animateMotion` (anim_motion.c).
 */

#ifndef GHOTI_IO_GVEC_SVG_ANIM_MOTION_H
#define GHOTI_IO_GVEC_SVG_ANIM_MOTION_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/shape-geometry/along.h>
#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/svg_value.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A measured motion path. */
typedef struct gvec_anim_motion {
  const GVEC_Allocator * allocator;
  GSHGEOM_Measured * measured;
  double length;
  double * vertices;     ///< The distance of the start and of the end of each segment that is not a move.
  size_t vertex_count;
} gvec_anim_motion;

/** Measure path data (absolute segments). GVEC_ERR_LIMIT for a number the geometry cannot hold. */
GVEC_INTERNAL_API GVEC_Result gvec_anim_motion_path(const GVEC_Svg_Path_Segment * segments, size_t count, const GVEC_Allocator * allocator,
    gvec_anim_motion * out);
/** The polyline through `points` pairs of `xy` (`values`, `from`, `to`, `by`). */
GVEC_INTERNAL_API GVEC_Result gvec_anim_motion_points(const double * xy, size_t points, const GVEC_Allocator * allocator, gvec_anim_motion * out);
/** The point at a distance (clamped into the path) and the direction of travel there, in degrees. */
GVEC_INTERNAL_API bool gvec_anim_motion_at(const gvec_anim_motion * m, double distance, double * x, double * y, double * angle);
GVEC_INTERNAL_API void gvec_anim_motion_free(gvec_anim_motion * m);

#ifdef __cplusplus
}
#endif

#endif
