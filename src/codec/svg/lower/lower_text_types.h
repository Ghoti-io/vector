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
 * @file lower_text_types.h
 *
 * What the lowering of `text` is made of: the spans' styles, the characters with the positions they were given, the lists and lengths and decorations of the
 * elements, and the glyphs a layout puts down. `lower_text.c` reads the content and lays it out; `lower_text_wrap.c` breaks it into the lines of a wrapping
 * area (SVG 2, 11.7) before that layout runs.
 */

#ifndef GHOTI_IO_GVEC_SVG_LOWER_TEXT_TYPES_H
#define GHOTI_IO_GVEC_SVG_LOWER_TEXT_TYPES_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/font/shape.h>
#include <ghoti.io/font/variation.h>
#include <ghoti.io/shape-geometry/along.h>
#include <ghoti.io/shape-geometry/map.h>

#include "../style/svg_style.h"
#include "lower_internal.h"

#define FEATURE_MAX 40u
#define VARIATION_MAX 16u ///< How many axes of `font-variation-settings` a span keeps.
#define AXES_MAX 32u      ///< How many axes of one face a location holds.

/** What a span says about the face and the line: read once for each element of the text. */
typedef struct tstyle {
  const GVEC_Svg_Instance * inst;
  const char ** families;
  size_t family_count;
  double size, weight, width, anchor;
  double letter_spacing, word_spacing;
  double line_height;       ///< `line-height`, in user units, unless it is `normal`.
  bool line_normal;         ///< `line-height: normal` (or a value this does not read): the face's own ascent, descent and gap make the line.
  int dominant, alignment; ///< The baseline the text is set on, and the one a span is aligned by (-1: its parent's): see baseline_kind().
  double shift_length;     ///< The baseline-shifts of the span and its ancestors that are lengths, up positive, in user units.
  int shift_sub, shift_super; ///< How many of them are `sub` and `super`: the face says how far each is.
  int italic;
  GFNT_Tag language;  ///< The OpenType language system of the text's `lang`, or 0.
  int tailoring;      ///< The case tailoring of that language (GUNI_LANG_*).
  int orientation;    ///< `text-orientation` in vertical text: 0 mixed, 1 upright, 2 sideways.
  double oblique;     ///< The angle of `font-style: oblique`, in degrees (14 when the style gives none).
  bool optical_none;  ///< `font-optical-sizing: none`: an `opsz` axis stays where the face has it.
  GFNT_Tag var_tags[VARIATION_MAX]; ///< `font-variation-settings`: the axes it sets, and to what.
  double var_values[VARIATION_MAX];
  size_t var_count;
  bool visible;
  GFNT_ShapeFeature features[FEATURE_MAX]; ///< The OpenType features the span asks for, as the shaper takes them: kerning, ligatures, caps, numerals, settings.
  size_t feature_count;
  double size_adjust; ///< `font-size-adjust`: the x-height to size ratio the text keeps, or 0 for none.
  int transform;      ///< `text-transform`: 0 none, 1 uppercase, 2 lowercase, 3 capitalize.
  bool full_width;    ///< `text-transform: full-width`.
  bool rtl, bidi_override; ///< `direction: rtl`; `unicode-bidi: bidi-override` (or isolate-override): the order is the direction's, not the algorithm's.
} tstyle;

static inline bool same_features(const tstyle * a, const tstyle * b) {
  if (a->feature_count != b->feature_count) {
    return false;
  }
  for (size_t i = 0; i < a->feature_count; i++) {
    if (a->features[i].tag != b->features[i].tag || a->features[i].value != b->features[i].value) {
      return false;
    }
  }
  return true;
}

static inline bool same_variations(const tstyle * a, const tstyle * b) {
  if (a->var_count != b->var_count) {
    return false;
  }
  for (size_t i = 0; i < a->var_count; i++) {
    if (a->var_tags[i] != b->var_tags[i] || a->var_values[i] != b->var_values[i]) {
      return false;
    }
  }
  return true;
}

/* Whether two spans ask for one font: their characters are shaped together, so kerning and joining run across the boundary. */
static inline bool same_font(const tstyle * a, const tstyle * b) {
  if (a->size != b->size || a->weight != b->weight || a->width != b->width || a->italic != b->italic || a->orientation != b->orientation || a->language != b->language || a->oblique != b->oblique || a->optical_none != b->optical_none || !same_variations(a, b) || a->family_count != b->family_count || !same_features(a, b) || a->size_adjust != b->size_adjust || a->rtl != b->rtl || a->bidi_override != b->bidi_override || a->dominant != b->dominant || a->alignment != b->alignment ||
      a->shift_length != b->shift_length || a->shift_sub != b->shift_sub || a->shift_super != b->shift_super) {
    return false;
  }
  for (size_t i = 0; i < a->family_count; i++) {
    if (strcmp(a->families[i], b->families[i]) != 0) {
      return false;
    }
  }
  return true;
}

typedef struct tchar {
  uint32_t cp;
  uint32_t style;
  bool has_x, has_y, has_dy, has_rotate;
  bool chunk_start;   ///< A textPath begins or ends before the character: a new chunk.
  bool newline;       ///< A line feed the white space keeps as a line break (a space, where the text is not wrapped).
  uint32_t tpath;     ///< One more than the textPath the character is in, or 0.
  double x, y, dx, dy;
  double rotate; ///< Degrees, about the glyph's origin.
} tchar;

/** A list of a span, applied to the characters of its content once the content is known (the tree's order, outer first, so that an inner list wins). */
typedef struct tlist {
  size_t start, end;   ///< The characters of the element's content (end is set when the content is known).
  const GVEC_Svg_Length * items;
  const double * numbers; ///< Kind 4: the angles.
  size_t count;
  int kind; ///< 0 x, 1 y, 2 dx, 3 dy, 4 rotate.
  const GVEC_Svg_Instance * inst;
} tlist;

/** A `textLength` of an element: the content from `start` to `end` is to take `length`. */
typedef struct tlength {
  size_t start, end;
  double length;
  bool glyphs; ///< `lengthAdjust` is `spacingAndGlyphs`: the glyphs are stretched too.
  const GVEC_Svg_Instance * inst;
  GSHGEOM_Map map; ///< The stretch, once laid out (spacingAndGlyphs), in the element's space: the layout frame's, turned back when vertical.
  double factor;   ///< The glyphs' stretch along the line.
  bool multi;      ///< The content spans several text chunks: its length is shared out over all of them (a first pass measures it).
  double natural;  ///< The natural width of the whole content, across its chunks.
  size_t total, seen; ///< The glyphs of the whole content, and how many the chunks so far have had.
} tlength;

/** A `text-decoration` of an element (or of an ancestor of the text): the lines are drawn across its content, with its own paint and font. */
typedef struct tdecor {
  const GVEC_Svg_Instance * inst;
  uint32_t style;
  size_t start, end;
  unsigned flags; ///< 1 underline, 2 overline, 4 line-through.
  bool has_color;       ///< `text-decoration-color` is a color (not `currentcolor`, which is the fill and stroke of the element that declares the decoration).
  GVEC_Svg_Color color;
  int line_style;       ///< `text-decoration-style`: 0 solid, 1 double, 2 dotted, 3 dashed, 4 wavy.
  int thickness_kind;   ///< `text-decoration-thickness`: 0 auto or from-font (the face's), 1 a length, 2 a percentage of the font size.
  double thickness;     ///< The length in user units, or the percentage.
  bool has_offset;      ///< `text-underline-offset` is a length or a percentage.
  bool offset_percent;
  double offset;
} tdecor;

/** A `textPath`: the path it follows, where the text starts on it, and the path measured (by the layout). */
#define TPATH_MAX 16
typedef struct ttpath {
  const GVEC_Svg_Element * path;
  const GVEC_Svg_Path_Segment * segments; ///< The `path` attribute's own data (SVG 2), which is preferred to `href`; else NULL.
  size_t segment_count;
  GSHGEOM_Map map;   ///< The referenced element's own `transform`, which the path is under.
  bool mapped;
  bool right;        ///< `side="right"`: the text is on the other side of the path, which it runs along backwards.
  double offset;
  bool percent;
  GSHGEOM_Measured * measured;
} ttpath;

typedef struct tctx {
  gvec_lw_ctx * lw;
  tstyle * styles;
  size_t style_count, style_capacity;
  tchar * chars;
  size_t count, capacity;
  tlist * lists;
  size_t list_count, list_capacity;
  tlength * lengths;
  size_t length_count, length_capacity;
  tdecor * decors;
  size_t decor_count, decor_capacity;
  bool last_space;
  bool refused;
  ttpath tpaths[TPATH_MAX];
  size_t tpath_count;
  uint32_t current_tpath;
  bool pending_break;
  bool vertical_lr; ///< ...and the lines go to the right (`vertical-lr`), not to the left.
  bool vertical; ///< `writing-mode` of the text is vertical: the layout is made in a frame turned a quarter, and the glyphs are put back.
} tctx;

#define TEXT_DEPTH_MAX 64

typedef struct tglyph {
  const GFNT_Face * face;
  const GFNT_F2Dot14 * coords; ///< The place of the face in its design space this glyph is drawn at (normalised), or NULL for the face's default.
  size_t coord_count;
  uint32_t id;
  double x, y;
  double cell_x0, cell_x1, cell_y0, cell_y1; ///< The glyph's cell: its advance wide, the face's ascent above its baseline and descent below.
  uint32_t style;
  double frame[5];  ///< On a path: the point (x, y) and the tangent (x, y) the glyph is put at, and the distance of its middle along the line.
  bool hidden;      ///< On a path: the glyph's middle is off the path's end, and it is not drawn.
  double advance;   ///< The advance of the glyph alone, without the spacing after it.
  double trail;     ///< The spacing after the glyph's cluster (letter-spacing, word-spacing), which its cell holds and the text's box does not (CSS Text: no space at the end).
  bool sideways;    ///< Vertical text: a glyph of a script that lies on its side.
  size_t ch;        ///< The character the glyph is of (its cluster's first).
  double base;      ///< The baseline the glyph sits on.
  double rotate;    ///< Degrees about the origin of the glyph at the start of the glyph's cluster.
  size_t pivot;     ///< That glyph's index in the text's glyphs.
  uint32_t group;   ///< One more than the textLength entry that stretches the glyphs, or 0.
  double size;      ///< The em size the glyph is drawn at (a small capital is smaller).
  bool on_path;     ///< The glyph is put on a path: its position is in the element's space already, not in the frame vertical text is laid out in.
  double path_angle; ///< On a path: the degrees the path turns the glyph by, in addition to its own rotation.
  bool placed;      ///< On a path: `placed_box` holds the box of the glyph's cell where the path puts it (the cell itself is along the line, unplaced).
  double placed_box[4]; ///< min x, min y, max x, max y of the turned cell, in the element's space.
} tglyph;

static inline bool same_coords(const tglyph * a, const tglyph * b) {
  return a->coord_count == b->coord_count && (a->coord_count == 0 || a->coords == b->coords || memcmp(a->coords, b->coords, a->coord_count * sizeof a->coords[0]) == 0);
}

/** A length of a text's style (a spacing, a line height) in user units: `normal` is 0; a percentage is of the font size when `percent_of_font`, else of the viewport's width. False when it is in a unit this does not read. */
GVEC_INTERNAL_API bool gvec_lw_text_spacing_of(const GVEC_Svg_Instance * inst, const GVEC_Svg_Style_Value * v, double font_size, bool percent_of_font, double * out);

/** Shape the characters `from` to `to` (one span's font) from the pen, appending glyphs. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_text_shape_run(tctx * t, const GVEC_Fonts * fonts, size_t from, size_t to, double * pen_x, double * pen_y, tglyph * glyphs, size_t * glyph_count);

/** Lay the text of the element out in its wrapping area (SVG 2, 11.7): the lines are broken and each puts its first character at the place it begins, the
 * other positions dropped, and the characters the lines do not keep (the spaces and line feeds at their ends) removed. `*wrapped` is false (and nothing is
 * changed) when the text has no `inline-size`, or is on a path. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_text_wrap(tctx * t, const GVEC_Svg_Instance * inst, tglyph * glyphs, size_t glyph_capacity, bool * wrapped);

/** A stretch of a line of the layout's frame. */
typedef struct tspan {
  double from, to;
} tspan;

/** The shapes a text flows into (`shape-inside`, without its subtracted shapes, padding and margin), flattened in the layout's frame. */
typedef struct tarea tarea;

/** The area of the element's `shape-inside` (and what it subtracts); `*out` is NULL when the element has none. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_text_area_make(tctx * t, const GVEC_Svg_Instance * inst, tarea ** out);

/** The heights (in the layout's frame) between which the shape the text flows into lies. */
GVEC_INTERNAL_API void gvec_lw_text_area_extent(const tarea * area, double * top, double * bottom);

/** The stretches of a line box from height `top` to `bottom` the text can take (at most `cap`), from the left: inside the shape at every height of the box, clear of
 * every shape subtracted. */
GVEC_INTERNAL_API GVEC_Result gvec_lw_text_area_band(tctx * t, tarea * area, double top, double bottom, tspan * out, size_t cap, size_t * count);

#endif
