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
 * Numbers, lengths, angles and lists (svg_grammar.h).
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/decimal.h>

#include "svg_grammar.h"

bool gvec_svg_is_wsp(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static void fail(gvec_svg_parse_error * err, size_t offset, const char * reason) {
  if (err) {
    err->offset = offset;
    err->reason = reason;
  }
}

static size_t skip_wsp(const char * s, size_t n, size_t pos) {
  while (pos < n && gvec_svg_is_wsp((unsigned char) s[pos])) {
    pos++;
  }
  return pos;
}

bool gvec_svg_scan_number(const char * s, size_t n, size_t * pos, double * out, gvec_svg_parse_error * err) {
  double value = 0;
  size_t consumed = 0;
  unsigned flags = 0;
  if (*pos >= n || gmath_decimal_parse(s + *pos, n - *pos, &value, &consumed, &flags) != GMATH_OK || consumed == 0) {
    fail(err, *pos, "expected a number");
    return false;
  }
  if (flags & GMATH_DECIMAL_OVERFLOW) {
    fail(err, *pos, "the number is too large");
    return false;
  }
  *out = value;
  *pos += consumed;
  return true;
}

gvec_svg_parse_status gvec_svg_parse_number(const char * s, size_t n, double * out, gvec_svg_parse_error * err) {
  size_t pos = skip_wsp(s, n, 0);
  double v;
  if (!gvec_svg_scan_number(s, n, &pos, &v, err)) {
    return GVEC_SVG_PARSE_INVALID;
  }
  pos = skip_wsp(s, n, pos);
  if (pos != n) {
    fail(err, pos, "unexpected text after the number");
    return GVEC_SVG_PARSE_INVALID;
  }
  *out = v;
  return GVEC_SVG_PARSE_OK;
}

static const struct {
  const char * name;
  GVEC_Svg_Unit unit;
} k_units[] = {
    {"px", GVEC_SVG_UNIT_PX}, {"em", GVEC_SVG_UNIT_EM}, {"ex", GVEC_SVG_UNIT_EX}, {"ch", GVEC_SVG_UNIT_CH},
    {"rem", GVEC_SVG_UNIT_REM}, {"vw", GVEC_SVG_UNIT_VW}, {"vh", GVEC_SVG_UNIT_VH}, {"vmin", GVEC_SVG_UNIT_VMIN},
    {"vmax", GVEC_SVG_UNIT_VMAX}, {"cm", GVEC_SVG_UNIT_CM}, {"mm", GVEC_SVG_UNIT_MM}, {"q", GVEC_SVG_UNIT_Q},
    {"in", GVEC_SVG_UNIT_IN}, {"pt", GVEC_SVG_UNIT_PT}, {"pc", GVEC_SVG_UNIT_PC}, {"%", GVEC_SVG_UNIT_PERCENT},
    {"deg", GVEC_SVG_UNIT_DEG}, {"grad", GVEC_SVG_UNIT_GRAD}, {"rad", GVEC_SVG_UNIT_RAD}, {"turn", GVEC_SVG_UNIT_TURN},
};

static unsigned char lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char) (c + 32) : c;
}

/** The unit at `s[*pos]`, restricted to [first, last) of k_units; advances past it. */
static bool scan_unit(const char * s, size_t n, size_t * pos, size_t first, size_t last, GVEC_Svg_Unit * unit) {
  size_t start = *pos, end = start;
  if (end < n && s[end] == '%') {
    end++;
  } else {
    while (end < n && ((s[end] >= 'a' && s[end] <= 'z') || (s[end] >= 'A' && s[end] <= 'Z'))) {
      end++;
    }
  }
  size_t len = end - start;
  if (len == 0) {
    *unit = GVEC_SVG_UNIT_NONE;
    return true;
  }
  for (size_t i = first; i < last; i++) {
    if (strlen(k_units[i].name) != len) {
      continue;
    }
    size_t j = 0;
    while (j < len && lower((unsigned char) s[start + j]) == (unsigned char) k_units[i].name[j]) {
      j++;
    }
    if (j == len) {
      *unit = k_units[i].unit;
      *pos = end;
      return true;
    }
  }
  return false;
}

#define LENGTH_UNITS 0, 16
#define ANGLE_UNITS 16, 20

static bool scan_dimension(const char * s, size_t n, size_t * pos, size_t first, size_t last, GVEC_Svg_Length * out,
    gvec_svg_parse_error * err) {
  double v;
  if (!gvec_svg_scan_number(s, n, pos, &v, err)) {
    return false;
  }
  GVEC_Svg_Unit unit;
  size_t at = *pos;
  if (!scan_unit(s, n, pos, first, last, &unit)) {
    fail(err, at, "unknown unit");
    return false;
  }
  out->value = v;
  out->unit = unit;
  return true;
}

static gvec_svg_parse_status parse_dimension(const char * s, size_t n, size_t first, size_t last, GVEC_Svg_Length * out,
    gvec_svg_parse_error * err) {
  size_t pos = skip_wsp(s, n, 0);
  GVEC_Svg_Length v;
  if (!scan_dimension(s, n, &pos, first, last, &v, err)) {
    return GVEC_SVG_PARSE_INVALID;
  }
  pos = skip_wsp(s, n, pos);
  if (pos != n) {
    fail(err, pos, "unexpected text after the value");
    return GVEC_SVG_PARSE_INVALID;
  }
  *out = v;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_length(const char * s, size_t n, GVEC_Svg_Length * out, gvec_svg_parse_error * err) {
  return parse_dimension(s, n, LENGTH_UNITS, out, err);
}

gvec_svg_parse_status gvec_svg_parse_angle(const char * s, size_t n, GVEC_Svg_Length * out, gvec_svg_parse_error * err) {
  return parse_dimension(s, n, ANGLE_UNITS, out, err);
}

double gvec_svg_angle_degrees(const GVEC_Svg_Length * angle) {
  switch (angle->unit) {
  case GVEC_SVG_UNIT_GRAD:
    return angle->value * 0.9;
  case GVEC_SVG_UNIT_RAD:
    return angle->value * 57.29577951308232;
  case GVEC_SVG_UNIT_TURN:
    return angle->value * 360.0;
  default:
    return angle->value;
  }
}

/* After a list item: comma-wsp ((wsp+ comma? wsp*) | (comma wsp*)) before another item, or the end. A comma
 * with nothing after it is an error. Returns false with `err` set; advances `pos` past the separator. */
static bool after_item(const char * s, size_t n, size_t * pos, bool adjacent_negative, gvec_svg_parse_error * err) {
  size_t before = *pos, p = before;
  while (p < n && gvec_svg_is_wsp((unsigned char) s[p])) {
    p++;
  }
  size_t comma_at = p;
  bool comma = false;
  if (p < n && s[p] == ',') {
    comma = true;
    p++;
    while (p < n && gvec_svg_is_wsp((unsigned char) s[p])) {
      p++;
    }
  }
  if (p >= n) {
    if (comma) {
      fail(err, comma_at, "a list may not end in a comma");
      return false;
    }
    *pos = p;
    return true;
  }
  if (p == before) {
    if (adjacent_negative && s[p] == '-') {
      return true; /* SVG 2's coordinate-pair: coordinate negative-coordinate */
    }
    fail(err, before, "expected a separator");
    return false;
  }
  *pos = p;
  return true;
}

/* Count or fill a list of numbers (`out` NULL counts). Returns the status; *count is the items read. */
static gvec_svg_parse_status scan_numbers(const char * s, size_t n, size_t max_items, bool adjacent_negative, double * out,
    size_t * count, gvec_svg_parse_error * err) {
  size_t pos = skip_wsp(s, n, 0), items = 0;
  while (pos < n) {
    if (items == max_items) {
      fail(err, pos, "the list has more items than the limit");
      *count = items;
      return GVEC_SVG_PARSE_LIMIT;
    }
    double v;
    if (!gvec_svg_scan_number(s, n, &pos, &v, err)) {
      *count = items;
      return GVEC_SVG_PARSE_INVALID;
    }
    if (out) {
      out[items] = v;
    }
    items++;
    if (!after_item(s, n, &pos, adjacent_negative, err)) {
      *count = items;
      return GVEC_SVG_PARSE_INVALID;
    }
  }
  *count = items;
  return GVEC_SVG_PARSE_OK;
}

static gvec_svg_parse_status number_list(const char * s, size_t n, const gvec_svg_parse_context * ctx, size_t multiple,
    gvec_svg_numbers * out, gvec_svg_parse_error * err) {
  size_t count = 0;
  gvec_svg_parse_status status = scan_numbers(s, n, ctx->max_list_items, multiple == 2, NULL, &count, err);
  if (status == GVEC_SVG_PARSE_INVALID) {
    return status;
  }
  size_t keep = count - (count % multiple);
  gvec_svg_parse_error again;
  double * items = gvec_arena_array(ctx->arena, count ? count : 1, sizeof(double), 8);
  if (!items) {
    return GVEC_SVG_PARSE_OOM;
  }
  /* The second pass cannot fail differently from the first; it stops at the same item. */
  size_t filled = 0;
  scan_numbers(s, n, ctx->max_list_items, multiple == 2, items, &filled, &again);
  out->items = items;
  out->count = keep;
  if (status == GVEC_SVG_PARSE_LIMIT) {
    return GVEC_SVG_PARSE_LIMIT;
  }
  if (keep != count) {
    fail(err, n, "an odd number of coordinates; the last one has no partner");
    return GVEC_SVG_PARSE_PARTIAL;
  }
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_number_list(const char * s, size_t n, const gvec_svg_parse_context * ctx,
    gvec_svg_numbers * out, gvec_svg_parse_error * err) {
  return number_list(s, n, ctx, 1, out, err);
}

gvec_svg_parse_status gvec_svg_parse_points(const char * s, size_t n, const gvec_svg_parse_context * ctx,
    gvec_svg_numbers * out, gvec_svg_parse_error * err) {
  return number_list(s, n, ctx, 2, out, err);
}

static gvec_svg_parse_status scan_lengths(const char * s, size_t n, size_t max_items, GVEC_Svg_Length * out,
    size_t * count, gvec_svg_parse_error * err) {
  size_t pos = skip_wsp(s, n, 0), items = 0;
  while (pos < n) {
    if (items == max_items) {
      fail(err, pos, "the list has more items than the limit");
      *count = items;
      return GVEC_SVG_PARSE_LIMIT;
    }
    GVEC_Svg_Length v;
    if (!scan_dimension(s, n, &pos, LENGTH_UNITS, &v, err)) {
      *count = items;
      return GVEC_SVG_PARSE_INVALID;
    }
    if (out) {
      out[items] = v;
    }
    items++;
    if (!after_item(s, n, &pos, false, err)) {
      *count = items;
      return GVEC_SVG_PARSE_INVALID;
    }
  }
  *count = items;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_length_list(const char * s, size_t n, const gvec_svg_parse_context * ctx,
    gvec_svg_lengths * out, gvec_svg_parse_error * err) {
  size_t count = 0;
  gvec_svg_parse_status status = scan_lengths(s, n, ctx->max_list_items, NULL, &count, err);
  if (status == GVEC_SVG_PARSE_INVALID) {
    return status;
  }
  GVEC_Svg_Length * items = gvec_arena_array(ctx->arena, count ? count : 1, sizeof(GVEC_Svg_Length), 8);
  if (!items) {
    return GVEC_SVG_PARSE_OOM;
  }
  size_t filled = 0;
  gvec_svg_parse_error again;
  scan_lengths(s, n, ctx->max_list_items, items, &filled, &again);
  out->items = items;
  out->count = count;
  return status;
}
