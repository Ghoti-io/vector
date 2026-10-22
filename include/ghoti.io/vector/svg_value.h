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
 * @file svg_value.h
 *
 * The typed values of the SVG document's attributes: the shapes the grammars of
 * the attribute chapter produce. A value that failed its grammar is not one of
 * these; the attribute says so (svg.h).
 */

#ifndef GHOTI_IO_GVEC_SVG_VALUE_H
#define GHOTI_IO_GVEC_SVG_VALUE_H

#include <ghoti.io/vector/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The unit of a length or an angle, as the CSS unit identifier names it (ASCII case-insensitively). */
typedef enum GVEC_Svg_Unit {
  GVEC_SVG_UNIT_NONE = 0, ///< No unit: user units for a length, degrees for an angle.
  GVEC_SVG_UNIT_PX,
  GVEC_SVG_UNIT_EM,
  GVEC_SVG_UNIT_EX,
  GVEC_SVG_UNIT_CH,
  GVEC_SVG_UNIT_REM,
  GVEC_SVG_UNIT_VW,
  GVEC_SVG_UNIT_VH,
  GVEC_SVG_UNIT_VMIN,
  GVEC_SVG_UNIT_VMAX,
  GVEC_SVG_UNIT_CM,
  GVEC_SVG_UNIT_MM,
  GVEC_SVG_UNIT_Q,
  GVEC_SVG_UNIT_IN,
  GVEC_SVG_UNIT_PT,
  GVEC_SVG_UNIT_PC,
  GVEC_SVG_UNIT_PERCENT,
  GVEC_SVG_UNIT_DEG,
  GVEC_SVG_UNIT_GRAD,
  GVEC_SVG_UNIT_RAD,
  GVEC_SVG_UNIT_TURN,
  GVEC_SVG_UNIT_COUNT
} GVEC_Svg_Unit;

/** A number with its unit. The unit is kept as written and never converted here: `em` and `ex` need the cascade. */
typedef struct GVEC_Svg_Length {
  double value;
  GVEC_Svg_Unit unit;
} GVEC_Svg_Length;

/** The commands of path data, as written. */
typedef struct GVEC_Svg_Path_Command {
  char letter;        ///< `MmZzLlHhVvCcSsQqTtAa`, as written (the case is the relative flag).
  uint8_t implicit;   ///< 1 when the letter was not written: an implicit repeat of the previous command.
  uint8_t arg_count;  ///< 0 to 7.
  uint8_t _pad;
  size_t offset;      ///< Byte offset of the command (or of its first argument when implicit) in the value.
  double args[7];     ///< The arguments in source order; an arc's two flags are 0 or 1.
} GVEC_Svg_Path_Command;

/** The kinds of segment of the absolute form. */
typedef enum GVEC_Svg_Segment_Kind {
  GVEC_SVG_SEGMENT_MOVE = 0, ///< `x`, `y`.
  GVEC_SVG_SEGMENT_LINE,     ///< To `x`, `y`.
  GVEC_SVG_SEGMENT_QUAD,     ///< Control `x1`, `y1`; to `x`, `y`.
  GVEC_SVG_SEGMENT_CUBIC,    ///< Controls `x1`, `y1`, `x2`, `y2`; to `x`, `y`.
  GVEC_SVG_SEGMENT_ARC,      ///< Radii `rx`, `ry`, x-axis rotation (degrees), the two flags; to `x`, `y`: endpoint form.
  GVEC_SVG_SEGMENT_CLOSE,    ///< Back to the start of the subpath, which is `x`, `y`.
  GVEC_SVG_SEGMENT_COUNT
} GVEC_Svg_Segment_Kind;

/**
 * One segment of the absolute form: every point is absolute, `H`/`V` are lines, and the reflected control point of
 * `S`/`T` is written out. An arc stays an arc in endpoint form (the exact conversion is the shape family's).
 */
typedef struct GVEC_Svg_Path_Segment {
  GVEC_Svg_Segment_Kind kind;
  uint32_t command;   ///< Index of the command (GVEC_Svg_Path_Command) it came from.
  double x0, y0;      ///< The start point (the current point before the segment).
  double x1, y1;      ///< Quad and cubic: the first control point.
  double x2, y2;      ///< Cubic: the second control point.
  double rx, ry;      ///< Arc: the radii as written (not yet corrected).
  double rotation;    ///< Arc: degrees.
  uint8_t large_arc;  ///< Arc: the large-arc flag.
  uint8_t sweep;      ///< Arc: the sweep flag.
  double x, y;        ///< The end point.
} GVEC_Svg_Path_Segment;

/** The viewBox attribute. */
typedef struct GVEC_Svg_View_Box {
  double x, y, width, height;
} GVEC_Svg_View_Box;

/** preserveAspectRatio's alignment. The numbers are stable. */
typedef enum GVEC_Svg_Align {
  GVEC_SVG_ALIGN_NONE = 0,
  GVEC_SVG_ALIGN_X_MIN_Y_MIN,
  GVEC_SVG_ALIGN_X_MID_Y_MIN,
  GVEC_SVG_ALIGN_X_MAX_Y_MIN,
  GVEC_SVG_ALIGN_X_MIN_Y_MID,
  GVEC_SVG_ALIGN_X_MID_Y_MID,
  GVEC_SVG_ALIGN_X_MAX_Y_MID,
  GVEC_SVG_ALIGN_X_MIN_Y_MAX,
  GVEC_SVG_ALIGN_X_MID_Y_MAX,
  GVEC_SVG_ALIGN_X_MAX_Y_MAX,
  GVEC_SVG_ALIGN_COUNT
} GVEC_Svg_Align;

/** The preserveAspectRatio attribute. */
typedef struct GVEC_Svg_Preserve_Aspect_Ratio {
  GVEC_Svg_Align align;
  uint8_t slice;  ///< 0 for `meet` (the default), 1 for `slice`.
  uint8_t defer;  ///< 1 when the SVG 1.1 `defer` keyword was written (accepted and ignored).
} GVEC_Svg_Preserve_Aspect_Ratio;

/** The orient attribute of a marker. */
typedef enum GVEC_Svg_Orient_Mode {
  GVEC_SVG_ORIENT_ANGLE = 0,    ///< An angle, in degrees, in `angle`.
  GVEC_SVG_ORIENT_AUTO,
  GVEC_SVG_ORIENT_AUTO_START_REVERSE
} GVEC_Svg_Orient_Mode;

typedef struct GVEC_Svg_Orient {
  GVEC_Svg_Orient_Mode mode;
  double angle;  ///< Degrees; 0 unless mode is ANGLE.
} GVEC_Svg_Orient;

/** What a reference (`href`) points at. */
typedef enum GVEC_Svg_Ref_Kind {
  GVEC_SVG_REF_EMPTY = 0,   ///< No reference (an empty value).
  GVEC_SVG_REF_INTERNAL,    ///< `#id`: an element of this document.
  GVEC_SVG_REF_DATA,        ///< A `data:` URI.
  GVEC_SVG_REF_EXTERNAL     ///< Anything else: kept, never fetched.
} GVEC_Svg_Ref_Kind;

/** A parsed `data:` URI (RFC 2397). The pointers are into the document's arena. */
typedef struct GVEC_Svg_Data_Uri {
  const char * media_type;   ///< As written, lower-cased; empty when omitted (the default is text/plain;charset=US-ASCII).
  const char * parameters;   ///< The parameters between the media type and the comma, without `base64`, as written.
  uint8_t base64;            ///< 1 when `;base64` ended the header.
  const char * payload;      ///< The data after the comma, as written (percent-encoded or base64).
  size_t payload_length;
} GVEC_Svg_Data_Uri;

/** A reference attribute. */
typedef struct GVEC_Svg_Reference {
  GVEC_Svg_Ref_Kind kind;
  const char * fragment;       ///< INTERNAL: the id, without `#`.
  const char * url;            ///< EXTERNAL: the whole value, trimmed.
  GVEC_Svg_Data_Uri data;      ///< DATA.
} GVEC_Svg_Reference;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_VALUE_H
