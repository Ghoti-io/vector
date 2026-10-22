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
 * @file svg_grammar.h
 *
 * The attribute grammars of the typed document: each is a total function over a
 * byte span that returns a typed value or an error with the offset of the first
 * bad byte. None reads outside its span, none recurses on its input, and every
 * loop is bounded by the span's length or by a limit.
 */

#ifndef GHOTI_IO_GVEC_SVG_GRAMMAR_H
#define GHOTI_IO_GVEC_SVG_GRAMMAR_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/vector/allocator.h>
#include <ghoti.io/vector/svg_value.h>

#include "svg_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How a parse ended. */
typedef enum gvec_svg_parse_status {
  GVEC_SVG_PARSE_OK = 0,   ///< The whole span is a value.
  GVEC_SVG_PARSE_PARTIAL,  ///< A valid prefix is a value (path data, points); the rest is the error.
  GVEC_SVG_PARSE_INVALID,  ///< Not a value. SVG 2's error rule: the attribute is unspecified.
  GVEC_SVG_PARSE_OOM,      ///< An allocation failed.
  GVEC_SVG_PARSE_LIMIT     ///< A list or path crossed its bound; the prefix up to the bound is the value.
} gvec_svg_parse_status;

/** Where and why a parse stopped. `reason` is static. */
typedef struct gvec_svg_parse_error {
  size_t offset;
  const char * reason;
} gvec_svg_parse_error;

/** What a grammar may use. */
typedef struct gvec_svg_parse_context {
  gvec_arena * arena;             ///< Where results live.
  const GVEC_Allocator * allocator; ///< Scratch space.
  size_t max_list_items;          ///< Numbers or lengths in a list.
  size_t max_path_segments;       ///< Commands in path data.
} gvec_svg_parse_context;

/** A list of numbers. */
typedef struct gvec_svg_numbers {
  const double * items;
  size_t count;
} gvec_svg_numbers;

/** A list of lengths. */
typedef struct gvec_svg_lengths {
  const GVEC_Svg_Length * items;
  size_t count;
} gvec_svg_lengths;

/** Path data in both forms. */
typedef struct gvec_svg_path {
  const GVEC_Svg_Path_Command * commands;
  size_t command_count;
  const GVEC_Svg_Path_Segment * segments;
  size_t segment_count;
} gvec_svg_path;

/** The white space of the attribute grammars: space, tab, line feed, form feed, carriage return. */
GVEC_INTERNAL_API bool gvec_svg_is_wsp(unsigned char c);

/** Read the longest number at `s[*pos]` (SVG 2's <number>); false with `err` when there is none or it overflows. */
GVEC_INTERNAL_API bool gvec_svg_scan_number(
    const char * s, size_t n, size_t * pos, double * out, gvec_svg_parse_error * err);

/** `wsp* number wsp*`. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_number(
    const char * s, size_t n, double * out, gvec_svg_parse_error * err);

/** `wsp* number unit? wsp*`, the unit one of the length units (`%` included). */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_length(
    const char * s, size_t n, GVEC_Svg_Length * out, gvec_svg_parse_error * err);

/** As gvec_svg_parse_length(), with the angle units (a unitless angle is degrees). */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_angle(
    const char * s, size_t n, GVEC_Svg_Length * out, gvec_svg_parse_error * err);

/** An angle in degrees: `deg`, `grad`, `rad`, `turn`, or none. */
GVEC_INTERNAL_API double gvec_svg_angle_degrees(const GVEC_Svg_Length * angle);

/** Numbers separated by comma-wsp. PARTIAL never; LIMIT keeps the first `max_list_items`. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_number_list(const char * s, size_t n,
    const gvec_svg_parse_context * ctx, gvec_svg_numbers * out, gvec_svg_parse_error * err);

/** Lengths separated by comma-wsp. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_length_list(const char * s, size_t n,
    const gvec_svg_parse_context * ctx, gvec_svg_lengths * out, gvec_svg_parse_error * err);

/** `points`: pairs of numbers; an odd count is PARTIAL with the complete pairs (as x, y, x, y, ...). */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_points(const char * s, size_t n,
    const gvec_svg_parse_context * ctx, gvec_svg_numbers * out, gvec_svg_parse_error * err);

/** Path data (SVG 2 section 9.3). PARTIAL keeps the segments before the first error; INVALID when none is complete. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_path(const char * s, size_t n,
    const gvec_svg_parse_context * ctx, gvec_svg_path * out, gvec_svg_parse_error * err);

/** `viewBox`: four numbers, the width and height not negative. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_view_box(
    const char * s, size_t n, GVEC_Svg_View_Box * out, gvec_svg_parse_error * err);

/** `preserveAspectRatio`: `defer`? align (`meet`|`slice`)?. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_par(
    const char * s, size_t n, GVEC_Svg_Preserve_Aspect_Ratio * out, gvec_svg_parse_error * err);

/** A marker's `orient`. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_orient(
    const char * s, size_t n, GVEC_Svg_Orient * out, gvec_svg_parse_error * err);

/** A stop's `offset`: a number or a percentage, as a number (a percentage divided by 100); not clamped. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_offset(
    const char * s, size_t n, double * out, gvec_svg_parse_error * err);

/** An integer (`tabindex`). */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_integer(
    const char * s, size_t n, long * out, gvec_svg_parse_error * err);

/** A keyword from a space-free list (ASCII case-sensitive, as SVG's enumerations are); the index, or INVALID. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_keyword(const char * s, size_t n,
    const char * const * keywords, size_t count, size_t * out, gvec_svg_parse_error * err);

/** `refX`/`refY`: a length or `left`/`center`/`right`/`top`/`bottom` (stored as 0%, 50%, 100% of the viewBox). */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_ref_coord(
    const char * s, size_t n, bool horizontal, GVEC_Svg_Length * out, gvec_svg_parse_error * err);

/** An `href`: classify and, for `data:`, parse. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_reference(const char * s, size_t n,
    const gvec_svg_parse_context * ctx, GVEC_Svg_Reference * out, gvec_svg_parse_error * err);

/** `systemLanguage`: a comma-separated list of language tags, trimmed. */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_language_list(const char * s, size_t n,
    const gvec_svg_parse_context * ctx, const char *** out_tags, size_t * out_count, gvec_svg_parse_error * err);

/** `requiredExtensions`: a space-separated list of strings; the count (an empty list is count 0). */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_parse_extension_list(const char * s, size_t n, size_t * out_count);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_GRAMMAR_H
