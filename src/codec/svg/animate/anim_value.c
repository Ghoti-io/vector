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
 * The values an animation of an attribute (or of `d`) works on itself (design.md A5): a value is a list of numbers with a shape
 * (the units of a length, the letters of path data) that two values must share to be interpolated or added. Writing one back is
 * the attribute's own syntax, numbers in the shortest spelling that reads back to the same double.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/math/decimal.h>
#include <ghoti.io/math/elementary.h>

#include "../../../core/core_internal.h"
#include "../svg_doc.h"
#include "../svg_grammar.h"
#include "anim_value.h"

/* ---- text --------------------------------------------------------------------------------------------------------- */

bool gvec_anim_buf_put(gvec_anim_buf * b, const char * s, size_t n) {
  if (b->failed) {
    return false;
  }
  if (b->length + n + 1 > b->capacity) {
    size_t c = b->capacity ? b->capacity * 2 : 64;
    while (c < b->length + n + 1) {
      c *= 2;
    }
    char * fresh = gvec_realloc(b->allocator, b->data, c);
    if (!fresh) {
      b->failed = true;
      return false;
    }
    b->data = fresh;
    b->capacity = c;
  }
  memcpy(b->data + b->length, s, n);
  b->length += n;
  b->data[b->length] = '\0';
  return true;
}

bool gvec_anim_buf_number(gvec_anim_buf * b, double x) {
  if (x == 0) {
    return gvec_anim_buf_put(b, "0", 1);
  }
  GMATH_Decimal d;
  if (gmath_decimal_shortest(x, &d) != GMATH_OK) {
    return gvec_anim_buf_put(b, "0", 1);
  }
  if (d.negative) {
    gvec_anim_buf_put(b, "-", 1);
  }
  gvec_anim_buf_put(b, d.digits, strlen(d.digits));
  if (d.exponent != 0) {
    char e[16];
    int n = d.exponent;
    size_t k = 0;
    e[k++] = 'e';
    if (n < 0) {
      e[k++] = '-';
      n = -n;
    }
    char rev[8];
    size_t r = 0;
    while (n > 0 && r < sizeof rev) {
      rev[r++] = (char) ('0' + n % 10);
      n /= 10;
    }
    while (r > 0) {
      e[k++] = rev[--r];
    }
    gvec_anim_buf_put(b, e, k);
  }
  return !b->failed;
}

void gvec_anim_buf_free(gvec_anim_buf * b) {
  gvec_free(b->allocator, b->data);
  b->data = NULL;
  b->length = b->capacity = 0;
}

/* ---- values ------------------------------------------------------------------------------------------------------- */

static const char * const k_unit[] = {"", "px", "em", "ex", "ch", "rem", "vw", "vh", "vmin", "vmax", "cm", "mm", "Q", "in",
    "pt", "pc", "%", "deg", "grad", "rad", "turn"};

/* Absolute units in user units (CSS Values 4: 96 per inch); 0 for a unit that needs a context. */
static double absolute_scale(GVEC_Svg_Unit u) {
  switch (u) {
  case GVEC_SVG_UNIT_NONE:
  case GVEC_SVG_UNIT_PX:
    return 1;
  case GVEC_SVG_UNIT_IN:
    return 96;
  case GVEC_SVG_UNIT_CM:
    return 96 / 2.54;
  case GVEC_SVG_UNIT_MM:
    return 96 / 25.4;
  case GVEC_SVG_UNIT_Q:
    return 96 / 101.6;
  case GVEC_SVG_UNIT_PT:
    return 96.0 / 72.0;
  case GVEC_SVG_UNIT_PC:
    return 16;
  default:
    return 0;
  }
}

/* A list of numbers separated by comma-wsp, as raw text; false for anything else. */
static bool raw_numbers(const char * s, size_t n, const gvec_anim_context * c, gvec_anim_value * out) {
  size_t count = 0, i = 0;
  double * v = NULL;
  size_t cap = 0;
  while (i < n) {
    while (i < n && (gvec_svg_is_wsp((unsigned char) s[i]) || s[i] == ',')) {
      i++;
    }
    if (i == n) {
      break;
    }
    size_t start = i;
    while (i < n && !gvec_svg_is_wsp((unsigned char) s[i]) && s[i] != ',') {
      i++;
    }
    double x;
    gvec_svg_parse_error err;
    if (gvec_svg_parse_number(s + start, i - start, &x, &err) != GVEC_SVG_PARSE_OK) {
      gvec_free(c->allocator, v);
      return false;
    }
    if (count == cap) {
      cap = cap ? cap * 2 : 4;
      double * fresh = gvec_realloc(c->allocator, v, cap * sizeof *v);
      if (!fresh) {
        gvec_free(c->allocator, v);
        out->oom = true;
        return false;
      }
      v = fresh;
    }
    v[count++] = x;
  }
  if (count == 0) {
    gvec_free(c->allocator, v);
    return false;
  }
  out->kind = GVEC_SVG_VALUE_NUMBER_LIST;
  out->numbers = v;
  out->count = count;
  out->units = gvec_alloc(c->allocator, count);
  if (!out->units) {
    gvec_anim_value_free(out, c->allocator);
    out->oom = true;
    return false;
  }
  memset(out->units, 0, count);
  return true;
}

bool gvec_anim_value_of(const GVEC_Svg_Attribute * a, const gvec_anim_context * c, gvec_anim_value * out) {
  memset(out, 0, sizeof *out);
  out->kind = a->value_kind;
  if (a->cls == GVEC_SVG_ATTR_RAW && a->grammar == GVEC_SVG_GRAMMAR_RAW_FILTER) {
    /* The filter primitives' attributes are read where they are used, not typed by the build; the numeric ones (`stdDeviation`, `dx`,
     * `values`, `kernelMatrix`, ...) are lists of numbers. */
    return raw_numbers(a->raw, a->raw_length, c, out);
  }
  if (a->cls != GVEC_SVG_ATTR_TYPED || a->partial) {
    return false;
  }
  const gvec_svg_value * v = &a->value;
  size_t n = 0;
  switch ((GVEC_Svg_Value_Kind) a->value_kind) {
  case GVEC_SVG_VALUE_NUMBER:
  case GVEC_SVG_VALUE_OFFSET:
  case GVEC_SVG_VALUE_INTEGER:
    n = 1;
    break;
  case GVEC_SVG_VALUE_LENGTH:
    n = 1;
    break;
  case GVEC_SVG_VALUE_NUMBER_LIST:
  case GVEC_SVG_VALUE_POINTS:
    n = v->numbers.count;
    break;
  case GVEC_SVG_VALUE_LENGTH_LIST:
    n = v->lengths.count;
    break;
  case GVEC_SVG_VALUE_VIEW_BOX:
    n = 4;
    break;
  case GVEC_SVG_VALUE_PATH:
    for (size_t i = 0; i < v->path.command_count; i++) {
      n += v->path.commands[i].arg_count;
    }
    break;
  default:
    return false;
  }
  out->count = n;
  out->numbers = n ? gvec_alloc(c->allocator, n * sizeof *out->numbers) : NULL;
  out->units = n ? gvec_alloc(c->allocator, n * sizeof *out->units) : NULL;
  if (n && (!out->numbers || !out->units)) {
    gvec_anim_value_free(out, c->allocator);
    out->oom = true;
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    out->units[i] = GVEC_SVG_UNIT_NONE;
  }
  switch ((GVEC_Svg_Value_Kind) a->value_kind) {
  case GVEC_SVG_VALUE_NUMBER:
  case GVEC_SVG_VALUE_OFFSET:
    out->numbers[0] = v->number;
    break;
  case GVEC_SVG_VALUE_INTEGER:
    out->numbers[0] = (double) v->integer;
    break;
  case GVEC_SVG_VALUE_LENGTH:
    out->numbers[0] = v->length.value;
    out->units[0] = (uint8_t) v->length.unit;
    break;
  case GVEC_SVG_VALUE_NUMBER_LIST:
  case GVEC_SVG_VALUE_POINTS:
    memcpy(out->numbers, v->numbers.items, n * sizeof *out->numbers);
    break;
  case GVEC_SVG_VALUE_LENGTH_LIST:
    for (size_t i = 0; i < n; i++) {
      out->numbers[i] = v->lengths.items[i].value;
      out->units[i] = (uint8_t) v->lengths.items[i].unit;
    }
    break;
  case GVEC_SVG_VALUE_VIEW_BOX:
    out->numbers[0] = v->view_box.x;
    out->numbers[1] = v->view_box.y;
    out->numbers[2] = v->view_box.width;
    out->numbers[3] = v->view_box.height;
    break;
  case GVEC_SVG_VALUE_PATH: {
    out->letters = v->path.command_count ? gvec_alloc(c->allocator, v->path.command_count + 1) : NULL;
    if (v->path.command_count && !out->letters) {
      gvec_anim_value_free(out, c->allocator);
      out->oom = true;
      return false;
    }
    size_t k = 0;
    for (size_t i = 0; i < v->path.command_count; i++) {
      out->letters[i] = v->path.commands[i].letter;
      for (size_t j = 0; j < v->path.commands[i].arg_count; j++) {
        out->numbers[k++] = v->path.commands[i].args[j];
      }
    }
    if (out->letters) {
      out->letters[v->path.command_count] = '\0';
    }
    out->letter_count = v->path.command_count;
    break;
  }
  default:
    break;
  }
  /* Lengths in absolute units are put in user units, so that two of them in different units have one shape (A5). A unit that needs a
   * context is kept, and is resolved only when it meets another unit (gvec_anim_value_unify). */
  for (size_t i = 0; i < n; i++) {
    double s = absolute_scale((GVEC_Svg_Unit) out->units[i]);
    if (s != 0) {
      out->numbers[i] *= s;
      out->units[i] = GVEC_SVG_UNIT_NONE;
    }
  }
  return true;
}

void gvec_anim_value_free(gvec_anim_value * v, const GVEC_Allocator * allocator) {
  gvec_free(allocator, v->numbers);
  gvec_free(allocator, v->units);
  gvec_free(allocator, v->letters);
  v->numbers = NULL;
  v->units = NULL;
  v->letters = NULL;
}

/* A length with a unit that needs a context, in user units: a percentage of the axis the context gives, `em` and the other font units of
 * its font size, the viewport units of its viewport; in object bounding box units a number is a fraction and a percentage a hundredth. */
static double resolve(double x, GVEC_Svg_Unit u, const gvec_anim_context * c) {
  if (c->bounding_box) {
    return u == GVEC_SVG_UNIT_PERCENT ? x / 100 : x;
  }
  switch (u) {
  case GVEC_SVG_UNIT_PERCENT:
    return x / 100 * c->percent_basis;
  case GVEC_SVG_UNIT_EM:
  case GVEC_SVG_UNIT_REM:
    return x * c->font_size;
  case GVEC_SVG_UNIT_EX:
  case GVEC_SVG_UNIT_CH:
    return x * c->font_size / 2;
  case GVEC_SVG_UNIT_VW:
    return x / 100 * c->viewport_width;
  case GVEC_SVG_UNIT_VH:
    return x / 100 * c->viewport_height;
  case GVEC_SVG_UNIT_VMIN:
    return x / 100 * (c->viewport_width < c->viewport_height ? c->viewport_width : c->viewport_height);
  case GVEC_SVG_UNIT_VMAX:
    return x / 100 * (c->viewport_width > c->viewport_height ? c->viewport_width : c->viewport_height);
  default:
    return x;
  }
}

bool gvec_anim_value_same_shape(const gvec_anim_value * a, const gvec_anim_value * b) {
  if (a->kind != b->kind || a->count != b->count || a->letter_count != b->letter_count) {
    return false;
  }
  if (a->letter_count && memcmp(a->letters, b->letters, a->letter_count) != 0) {
    return false;
  }
  return true;
}

void gvec_anim_value_unify(gvec_anim_value * a, gvec_anim_value * b, const gvec_anim_context * c) {
  for (size_t i = 0; i < a->count && i < b->count; i++) {
    if (a->units[i] != b->units[i]) {
      a->numbers[i] = resolve(a->numbers[i], (GVEC_Svg_Unit) a->units[i], c);
      b->numbers[i] = resolve(b->numbers[i], (GVEC_Svg_Unit) b->units[i], c);
      if (c->bounding_box && (a->kind == GVEC_SVG_VALUE_LENGTH || a->kind == GVEC_SVG_VALUE_LENGTH_LIST)) {
        /* Fractions are written as percentages, which every such attribute takes. */
        a->numbers[i] *= 100;
        b->numbers[i] *= 100;
        a->units[i] = b->units[i] = GVEC_SVG_UNIT_PERCENT;
      } else {
        a->units[i] = b->units[i] = GVEC_SVG_UNIT_NONE;
      }
    }
  }
}

bool gvec_anim_value_copy(const gvec_anim_value * from, gvec_anim_value * to, const GVEC_Allocator * allocator) {
  memset(to, 0, sizeof *to);
  to->kind = from->kind;
  to->count = from->count;
  to->letter_count = from->letter_count;
  if (from->count) {
    to->numbers = gvec_alloc(allocator, from->count * sizeof *to->numbers);
    to->units = gvec_alloc(allocator, from->count * sizeof *to->units);
    if (!to->numbers || !to->units) {
      gvec_anim_value_free(to, allocator);
      return false;
    }
    memcpy(to->numbers, from->numbers, from->count * sizeof *to->numbers);
    memcpy(to->units, from->units, from->count * sizeof *to->units);
  }
  if (from->letter_count) {
    to->letters = gvec_alloc(allocator, from->letter_count + 1);
    if (!to->letters) {
      gvec_anim_value_free(to, allocator);
      return false;
    }
    memcpy(to->letters, from->letters, from->letter_count + 1);
  }
  return true;
}

void gvec_anim_value_mix(gvec_anim_value * into, const gvec_anim_value * a, const gvec_anim_value * b, double p) {
  for (size_t i = 0; i < into->count; i++) {
    into->numbers[i] = a->numbers[i] + (b->numbers[i] - a->numbers[i]) * p;
  }
}

void gvec_anim_value_add(gvec_anim_value * into, const gvec_anim_value * b, double times) {
  for (size_t i = 0; i < into->count; i++) {
    into->numbers[i] += b->numbers[i] * times;
  }
}

double gvec_anim_value_distance(const gvec_anim_value * a, const gvec_anim_value * b) {
  double s = 0;
  for (size_t i = 0; i < a->count; i++) {
    double d = b->numbers[i] - a->numbers[i];
    s += d * d;
  }
  return gmath_sqrt(s);
}

/* The arguments of path data that are flags: an arc's fourth and fifth. */
static bool is_flag(char letter, size_t arg) {
  return (letter == 'A' || letter == 'a') && (arg % 7 == 3 || arg % 7 == 4);
}

static size_t letter_args(char letter) {
  switch (letter) {
  case 'M': case 'm': case 'L': case 'l': case 'T': case 't':
    return 2;
  case 'H': case 'h': case 'V': case 'v':
    return 1;
  case 'C': case 'c':
    return 6;
  case 'S': case 's': case 'Q': case 'q':
    return 4;
  case 'A': case 'a':
    return 7;
  default:
    return 0;
  }
}

bool gvec_anim_value_write(const gvec_anim_value * v, gvec_anim_buf * b) {
  switch ((GVEC_Svg_Value_Kind) v->kind) {
  case GVEC_SVG_VALUE_INTEGER:
    gvec_anim_buf_number(b, gmath_floor(v->numbers[0] + 0.5));
    break;
  case GVEC_SVG_VALUE_PATH: {
    size_t k = 0;
    for (size_t i = 0; i < v->letter_count; i++) {
      if (i) {
        gvec_anim_buf_put(b, " ", 1);
      }
      char letter = v->letters[i];
      gvec_anim_buf_put(b, &letter, 1);
      size_t n = letter_args(letter);
      for (size_t j = 0; j < n && k < v->count; j++, k++) {
        gvec_anim_buf_put(b, " ", 1);
        double x = v->numbers[k];
        if (is_flag(letter, j)) {
          /* SVG 2, paths.html: a flag is interpolated as a fraction, and any value that is not zero is true. */
          x = x != 0 ? 1 : 0;
        }
        gvec_anim_buf_number(b, x);
      }
    }
    break;
  }
  default:
    for (size_t i = 0; i < v->count; i++) {
      if (i) {
        gvec_anim_buf_put(b, v->kind == GVEC_SVG_VALUE_POINTS && (i % 2) == 1 ? "," : " ", 1);
      }
      gvec_anim_buf_number(b, v->numbers[i]);
      if (v->units[i] != GVEC_SVG_UNIT_NONE && v->units[i] < GVEC_SVG_UNIT_COUNT) {
        const char * u = k_unit[v->units[i]];
        gvec_anim_buf_put(b, u, strlen(u));
      }
    }
    break;
  }
  return !b->failed;
}
