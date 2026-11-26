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
 * The typed values an animation of an attribute works on (anim_value.c), and a growing text buffer.
 */

#ifndef GHOTI_IO_GVEC_SVG_ANIM_VALUE_H
#define GHOTI_IO_GVEC_SVG_ANIM_VALUE_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/svg.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Text that grows; `failed` once an allocation failed (every later call does nothing). */
typedef struct gvec_anim_buf {
  const GVEC_Allocator * allocator;
  char * data;
  size_t length, capacity;
  bool failed;
} gvec_anim_buf;

GVEC_INTERNAL_API bool gvec_anim_buf_put(gvec_anim_buf * b, const char * s, size_t n);
/** A double in the shortest spelling that reads back to it, in the number syntax of SVG and CSS (`15e-1`). */
GVEC_INTERNAL_API bool gvec_anim_buf_number(gvec_anim_buf * b, double x);
GVEC_INTERNAL_API void gvec_anim_buf_free(gvec_anim_buf * b);

/** What resolving a unit needs, for two lengths in units that differ (design A5). */
typedef struct gvec_anim_context {
  const GVEC_Allocator * allocator;
  bool bounding_box;      ///< The attribute is in object bounding box units.
  double percent_basis;   ///< What 100% is, in user units.
  double font_size;
  double viewport_width, viewport_height;
} gvec_anim_context;

/** A typed value as numbers with a shape. */
typedef struct gvec_anim_value {
  uint8_t kind;           ///< A GVEC_Svg_Value_Kind.
  double * numbers;
  uint8_t * units;        ///< A GVEC_Svg_Unit per number (NONE once in user units).
  size_t count;
  char * letters;         ///< Path data: the command letters, as written.
  size_t letter_count;
  bool oom;
} gvec_anim_value;

/** The value of a typed attribute, when its kind is one that interpolates (numbers, lengths, lists, `viewBox`, path data). */
GVEC_INTERNAL_API bool gvec_anim_value_of(const GVEC_Svg_Attribute * attribute, const gvec_anim_context * context, gvec_anim_value * out);
GVEC_INTERNAL_API void gvec_anim_value_free(gvec_anim_value * value, const GVEC_Allocator * allocator);
GVEC_INTERNAL_API bool gvec_anim_value_copy(const gvec_anim_value * from, gvec_anim_value * to, const GVEC_Allocator * allocator);
/** The same kind, count and commands (SVG 2: path data interpolates when it has the same commands in the same order). */
GVEC_INTERNAL_API bool gvec_anim_value_same_shape(const gvec_anim_value * a, const gvec_anim_value * b);
/** Put the numbers of two values of one shape whose units differ into one unit. */
GVEC_INTERNAL_API void gvec_anim_value_unify(gvec_anim_value * a, gvec_anim_value * b, const gvec_anim_context * context);
/** `into` = a + (b - a) p, number by number (all three of one shape). */
GVEC_INTERNAL_API void gvec_anim_value_mix(gvec_anim_value * into, const gvec_anim_value * a, const gvec_anim_value * b, double p);
/** `into` += b * times. */
GVEC_INTERNAL_API void gvec_anim_value_add(gvec_anim_value * into, const gvec_anim_value * b, double times);
/** The Euclidean distance of the numbers (SMIL's distance for `paced`). */
GVEC_INTERNAL_API double gvec_anim_value_distance(const gvec_anim_value * a, const gvec_anim_value * b);
/** Write the value in its attribute's syntax. */
GVEC_INTERNAL_API bool gvec_anim_value_write(const gvec_anim_value * value, gvec_anim_buf * b);

#ifdef __cplusplus
}
#endif

#endif
