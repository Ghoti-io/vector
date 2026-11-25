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
 * The layers of `mask` (CSS Masking 1): a list of mask images, each an image, a gradient or the `mask` element of an earlier time, positioned, sized and
 * repeated in a box of the element, clipped to another, and composited with the layers below it.
 *
 * Each layer is made into a picture of what it covers; the layers are put in one picture bottom up, each in a group that composites it with what is
 * under it (`add` is source-over, `subtract` source-out, `intersect` source-in and `exclude` xor: Porter and Duff, as the waist's groups do), and the
 * mask is that picture's alpha. A layer whose mode is luminance is first made into the alpha that its luminance is, by a mask of its own.
 */

#include "../../../../include/ghoti.io/vector/macros.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/math/elementary.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/mask.h>
#include <ghoti.io/shape-scene/paint.h>
#include <ghoti.io/shape-scene/resource.h>

#include "../svg_grammar.h"
#include "../style/svg_style.h"
#include "../style/svg_used.h"
#include "lower_internal.h"

#define LAYERS_MAX 16
#define TILES_MAX 4096
#define STOPS_MAX 64
#define TOKENS_MAX 24

//-----------------------------------------------------------------------------
// Text
//-----------------------------------------------------------------------------

typedef struct span {
  const char * p;
  size_t n;
} span;

static bool is_blank(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static span trim(span s) {
  while (s.n && is_blank(s.p[0])) {
    s.p++;
    s.n--;
  }
  while (s.n && is_blank(s.p[s.n - 1])) {
    s.n--;
  }
  return s;
}

/* The pieces of `text` between its top-level separators (a comma, or a blank when `blank`): not inside parentheses or quotes. */
static size_t split(span text, bool blank, span * out, size_t capacity) {
  size_t count = 0, depth = 0, start = 0;
  char quote = 0;
  for (size_t i = 0; i <= text.n; i++) {
    const char c = i < text.n ? text.p[i] : 0;
    if (quote) {
      quote = c == quote ? 0 : quote;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    }
    else if (c == '(') {
      depth++;
    }
    else if (c == ')' && depth) {
      depth--;
    }
    if (i == text.n || (depth == 0 && (c == ',' || (blank && is_blank(c))))) {
      span piece = trim((span) {text.p + start, i - start});
      if (piece.n || (!blank && count < capacity)) {
        if (count < capacity) {
          out[count] = piece;
        }
        count++;
      }
      start = i + 1;
    }
  }
  return count;
}

static bool is(span s, const char * word) {
  const size_t n = strlen(word);
  return s.n == n && strncmp(s.p, word, n) == 0;
}

static bool starts(span s, const char * prefix) {
  const size_t n = strlen(prefix);
  return s.n >= n && strncmp(s.p, prefix, n) == 0;
}

/* A number with an optional unit: its value, and the unit's span. False when it is not one. */
static bool number_unit(span s, double * value, span * unit) {
  size_t pos = 0;
  if (!s.n || !gvec_svg_scan_number(s.p, s.n, &pos, value, NULL)) {
    return false;
  }
  *unit = (span) {s.p + pos, s.n - pos};
  return true;
}

/* A length (`px`, or a bare zero) or a percentage: `*percent` says which. */
static bool length_or_percent(span s, double * value, bool * percent) {
  span unit;
  if (!number_unit(s, value, &unit)) {
    return false;
  }
  if (is(unit, "%")) {
    *percent = true;
    return true;
  }
  *percent = false;
  return is(unit, "px") || (unit.n == 0 && *value == 0);
}

static bool angle_of(span s, double * degrees) {
  double v;
  span unit;
  if (!number_unit(s, &v, &unit)) {
    return false;
  }
  if (is(unit, "deg") || (unit.n == 0 && v == 0)) {
    *degrees = v;
  }
  else if (is(unit, "grad")) {
    *degrees = v * 0.9;
  }
  else if (is(unit, "rad")) {
    *degrees = v * 180.0 / 3.14159265358979323846;
  }
  else if (is(unit, "turn")) {
    *degrees = v * 360.0;
  }
  else {
    return false;
  }
  return true;
}

//-----------------------------------------------------------------------------
// The layers
//-----------------------------------------------------------------------------

typedef enum layer_kind { LAYER_NONE = 0, LAYER_ELEMENT, LAYER_IMAGE, LAYER_GRADIENT, LAYER_BORDER } layer_kind;

typedef struct amount {
  int kind; ///< 0 auto, 1 length, 2 percent, 3 a percentage and a length (`right 10px` is 100% less 10px)
  double v;
  double len; ///< Kind 3: the length.
} amount;

typedef struct layer {
  layer_kind kind;
  span image;
  int mode;             ///< 0 luminance, 1 alpha.
  int repeat[2];        ///< 0 repeat, 1 no-repeat, 2 space, 3 round, for x and y.
  amount position[2];   ///< Kind 1 or 2 (a position has no auto).
  int size_keyword;     ///< 0 the two amounts, 1 contain, 2 cover.
  amount size[2];
  int origin, clip;     ///< 0 fill-box, 1 stroke-box, 2 view-box, 3 no-clip.
  int composite;        ///< 0 add, 1 subtract, 2 intersect, 3 exclude.
  int named_mode;       ///< The `mask-mode` the layer names: 0 match-source, 1 alpha, 2 luminance.
  double resolution;    ///< The pixels of the image for each CSS pixel (an `image-set()` candidate's `2x`); 0 for 1.
  /* LAYER_BORDER: the mask border (CSS Masking 1, 6.2), `image` being its source. */
  double slice[4];      ///< `mask-border-slice`, top right bottom left: a number of pixels of the image, or a percentage of it (`slice_percent`).
  bool slice_percent[4];
  bool slice_fill;
  amount width[4];      ///< `mask-border-width`: kind 0 auto, 1 length, 2 percentage of the area, 4 number (of the border width, which an SVG element does not have).
  double outset[4];     ///< `mask-border-outset`, in pixels (a number is of the border width, which is none).
  int border_repeat[2]; ///< `mask-border-repeat`: 0 stretch, 1 repeat, 2 round, 3 space, for the horizontal and the vertical edges.
} layer;

static int box_keyword(span s) {
  if (is(s, "fill-box") || is(s, "content-box") || is(s, "padding-box")) {
    return 0;
  }
  if (is(s, "stroke-box") || is(s, "border-box") || is(s, "margin-box")) {
    return 1;
  }
  if (is(s, "view-box")) {
    return 2;
  }
  if (is(s, "no-clip")) {
    return 3;
  }
  return -1;
}

static GVEC_Result text_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, span * out) {
  GVEC_Svg_Style_Specified sp;
  GVEC_Result r = gvec_svg_get_specified(ctx->style, inst, property, &sp);
  *out = (span) {sp.computed, sp.computed ? strlen(sp.computed) : 0};
  return r;
}

/* The `i`th entry of a list property, cycled when the list is shorter than the layers (CSS Backgrounds 3, 2.1: a list is repeated to the number of layers). */
static span entry(const span * list, size_t count, size_t i) {
  return count ? list[i % count] : (span) {NULL, 0};
}

static GVEC_Result refuse_layer(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const char * message) {
  return gvec_lw_refuse(ctx, e, GVEC_LOWER_MASK, message);
}

/* A `url()` that names an element (`#id`, or another document's `file#id`), not an image: a fragment, and not a `data:` URI (whose payload may hold a `#`). */
static bool local_url(span one) {
  if (!starts(one, "url(") || one.n < 7 || one.p[one.n - 1] != ')') {
    return false;
  }
  span inner = trim((span) {one.p + 4, one.n - 5});
  if (inner.n >= 2 && (inner.p[0] == '"' || inner.p[0] == '\'') && inner.p[inner.n - 1] == inner.p[0]) {
    inner.p++;
    inner.n -= 2;
  }
  return !starts(inner, "data:") && memchr(inner.p, '#', inner.n) != NULL;
}

/* Whether the text of a `mask` is one `url()` of an element (a `mask` element of this document or another, not a mask image): the old path of the lowering. */
bool gvec_lw_mask_is_element_reference(const char * text) {
  span all = trim((span) {text, text ? strlen(text) : 0});
  span layers[2];
  return split(all, false, layers, 2) == 1 && local_url(layers[0]);
}

//-----------------------------------------------------------------------------
// Boxes
//-----------------------------------------------------------------------------

/* The box a keyword names, in the element's user space. `*known` is false when the element has no box that can be known (it holds more than shapes). */
static GVEC_Result box_of(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * masked, int which, GSHGEOM_Bounds * out, bool * known) {
  *known = true;
  out->empty = 0;
  if (which == 2) {
    double vw, vh;
    gvec_svg_instance_viewport(masked, &vw, &vh);
    out->min.x = 0;
    out->min.y = 0;
    out->max.x = vw;
    out->max.y = vh;
    return GVEC_OK;
  }
  GSHGEOM_Bounds box;
  box.empty = 1;
  GVEC_Result r = gvec_lw_inner_box(ctx, masked, &box, known);
  if (r != GVEC_OK || !*known) {
    return r;
  }
  if (box.empty) {
    *out = box;
    return GVEC_OK;
  }
  *out = box;
  if (which == 1 || which == 3) {
    /* The stroke's half width on every side (the stroke-box with the joins and caps of a plain stroke). */
    GVEC_Svg_Style_Value v;
    bool usable;
    r = gvec_lw_read_property(ctx, masked, GVEC_SVG_PROP_STROKE, GVEC_LOWER_STYLE, &v, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
    if (v.kind == GVEC_SVG_STYLE_PAINT && v.paint.kind != GVEC_SVG_PAINT_NONE) {
      GVEC_Svg_Style_Value w;
      r = gvec_lw_read_property(ctx, masked, GVEC_SVG_PROP_STROKE_WIDTH, GVEC_LOWER_STYLE, &w, &usable);
      if (r != GVEC_OK || !usable) {
        return r;
      }
      const double half = w.kind == GVEC_SVG_STYLE_LENGTH || w.kind == GVEC_SVG_STYLE_NUMBER ? w.number / 2 : 0;
      if (half > 0) {
        out->min.x -= half;
        out->min.y -= half;
        out->max.x += half;
        out->max.y += half;
      }
    }
  }
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// CSS gradients
//-----------------------------------------------------------------------------

typedef struct gradient_stops {
  GSHSCENE_Gradient_Stop stops[STOPS_MAX];
  double position[STOPS_MAX]; ///< As written: a fraction of the gradient line, or NAN when it is not given.
  bool given[STOPS_MAX];
  span color[STOPS_MAX];      ///< The text of each stop's color, to mix between two of them.
  double hint[STOPS_MAX];     ///< The position of an interpolation hint before the stop (a fraction as `position`), or NAN.
  size_t count;
  GSHSCENE_Gradient_Stop * expanded; ///< The stops as the waist is given them: more than `count` where a hint or a color space needed some between.
  size_t expanded_count;
} gradient_stops;

/** The most stops a gradient is given once its hints and its color space have added the ones between. */
#define EXPANDED_STOPS_MAX 2048

/* The color stops of a gradient's arguments `args[from..count)`, positions as fractions of `length` (for a length) and fixed up (CSS Images 3, 3.4.3). */
static GVEC_Result read_stops(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Element * e, const span * args, size_t from, size_t count,
    double length, bool angular, gradient_stops * out, bool * ok) {
  *ok = false;
  out->count = 0;
  double pending_hint = NAN;
  for (size_t i = from; i < count; i++) {
    span tokens[TOKENS_MAX];
    size_t n = split(args[i], true, tokens, TOKENS_MAX);
    /* A color and at most one position: the computed value of a stop with two positions is already two stops. */
    if (n == 0 || n > 2 || n > TOKENS_MAX) {
      return GVEC_OK;
    }
    GVEC_Svg_Color c;
    bool valid;
    GVEC_Result r = gvec_svg_style_color_of_text(ctx->style, inst, e, tokens[0].p, tokens[0].n, &c, &valid);
    if (r != GVEC_OK) {
      return r;
    }
    if (!valid) {
      /* An interpolation hint (a position and no color), between two stops; or a color this does not read. */
      double hv;
      bool hp;
      double hint_fraction;
      if (n != 1 || out->count == 0 || i + 1 >= count || !isnan(pending_hint)) {
        return GVEC_OK;
      }
      if (angular) {
        double degrees;
        if (angle_of(tokens[0], &degrees)) {
          hint_fraction = degrees / 360.0;
        }
        else if (length_or_percent(tokens[0], &hv, &hp) && hp) {
          hint_fraction = hv / 100.0;
        }
        else {
          return GVEC_OK;
        }
      }
      else {
        if (!length_or_percent(tokens[0], &hv, &hp) || (!hp && !(length > 0))) {
          return GVEC_OK;
        }
        hint_fraction = hp ? hv / 100.0 : hv / length;
      }
      pending_hint = hint_fraction;
      continue;
    }
    GVEC_Svg_Paint given;
    memset(&given, 0, sizeof given);
    given.kind = GVEC_SVG_PAINT_COLOR;
    given.color = c;
    gvec_lw_paint_decision decision;
    GSHSCENE_Paint paint;
    r = gvec_lw_paint_of(ctx, inst, &given, 1.0, &paint, &decision);
    if (r != GVEC_OK) {
      return r;
    }
    if (decision == GVEC_LW_PAINT_NONE) {
      memset(&paint, 0, sizeof paint);
      paint.kind = GSHSCENE_PAINT_SOLID;
    }
    else if (decision != GVEC_LW_PAINT_DRAW) {
      return GVEC_OK;
    }
    double fraction_given = 0;
    size_t positions = 0;
    for (size_t k = 1; k < n; k++) {
      double v;
      bool percent;
      double fraction;
      if (angular) {
        double degrees;
        if (angle_of(tokens[k], &degrees)) {
          fraction = degrees / 360.0;
        }
        else if (length_or_percent(tokens[k], &v, &percent) && percent) {
          fraction = v / 100.0;
        }
        else {
          return GVEC_OK;
        }
      }
      else {
        if (!length_or_percent(tokens[k], &v, &percent) || (!percent && !(length > 0))) {
          return GVEC_OK;
        }
        fraction = percent ? v / 100.0 : v / length;
      }
      fraction_given = fraction;
      positions++;
    }
    if (out->count + 1 > STOPS_MAX) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a gradient of a mask has more stops than the lowering keeps");
    }
    out->stops[out->count].color = paint;
    out->position[out->count] = positions ? fraction_given : 0;
    out->given[out->count] = positions != 0;
    out->color[out->count] = tokens[0];
    out->hint[out->count] = pending_hint;
    pending_hint = NAN;
    out->count++;
  }
  if (out->count < 2 || !isnan(pending_hint)) {
    return GVEC_OK;
  }
  /* The positions: the first is 0 and the last 1 when not given, none is before an earlier one, and the ones not given are spread evenly between those that are. */
  if (!out->given[0]) {
    out->position[0] = 0;
    out->given[0] = true;
  }
  if (!out->given[out->count - 1]) {
    out->position[out->count - 1] = 1;
    out->given[out->count - 1] = true;
  }
  double high = out->position[0];
  for (size_t i = 0; i < out->count; i++) {
    if (out->given[i]) {
      if (out->position[i] < high) {
        out->position[i] = high;
      }
      high = out->position[i];
    }
  }
  for (size_t i = 0; i < out->count;) {
    if (out->given[i]) {
      i++;
      continue;
    }
    size_t j = i;
    while (!out->given[j]) {
      j++;
    }
    for (size_t k = i; k < j; k++) {
      out->position[k] = out->position[i - 1] + (out->position[j] - out->position[i - 1]) * (double) (k - i + 1) / (double) (j - i + 1);
    }
    i = j;
  }
  for (size_t i = 0; i < out->count; i++) {
    out->stops[i].offset = out->position[i];
    /* A hint is between the two stops it lies between: past either, it is at that one. */
    if (i > 0 && !isnan(out->hint[i])) {
      const double lo = out->position[i - 1], hi = out->position[i];
      out->hint[i] = out->hint[i] < lo ? lo : out->hint[i] > hi ? hi : out->hint[i];
    }
  }
  *ok = true;
  return GVEC_OK;
}

/* A `<length-percentage>`: a length, a percentage, or `calc()` of a percentage and a length (what the cascade writes for an offset from a far edge). */
static bool amount_of(span t, amount * out) {
  double v;
  bool percent;
  if (length_or_percent(t, &v, &percent)) {
    *out = (amount) {percent ? 2 : 1, v, 0};
    return true;
  }
  if (!starts(t, "calc(") || t.n < 7 || t.p[t.n - 1] != ')') {
    return false;
  }
  span inner = trim((span) {t.p + 5, t.n - 6});
  span parts[4];
  size_t n = split(inner, true, parts, 4);
  if (n == 1) {
    return amount_of(parts[0], out);
  }
  if (n != 3 || (!is(parts[1], "+") && !is(parts[1], "-"))) {
    return false;
  }
  amount a, b;
  if (!amount_of(parts[0], &a) || !amount_of(parts[2], &b) || a.kind == 3 || b.kind == 3) {
    return false;
  }
  const double sign = is(parts[1], "-") ? -1.0 : 1.0;
  if (a.kind == b.kind) {
    *out = (amount) {a.kind, a.v + sign * b.v, 0};
  }
  else if (a.kind == 2) {
    *out = (amount) {3, a.v, sign * b.v};
  }
  else {
    *out = (amount) {3, sign * b.v, a.v};
  }
  return true;
}

/* The place of a `<position>` along each axis (CSS Values 4, 9.3): one to four values, the keywords `left`, `right`, `top`, `bottom` and `center`, and lengths and
 * percentages, with the offset of a keyword from its edge (`right 10px bottom 5px`, which is 100% less 10px and 100% less 5px). */
static bool position_value(const span * t, size_t n, amount out[2]) {
  if (n == 0 || n > 4) {
    return false;
  }
  enum { LEFT, RIGHT, TOP, BOTTOM, CENTER, VALUE, NONE };
  int kind[4];
  amount value[4];
  for (size_t i = 0; i < n; i++) {
    value[i] = (amount) {0, 0, 0};
    kind[i] = is(t[i], "left") ? LEFT : is(t[i], "right") ? RIGHT : is(t[i], "top") ? TOP : is(t[i], "bottom") ? BOTTOM : is(t[i], "center") ? CENTER : VALUE;
    if (kind[i] == VALUE && !amount_of(t[i], &value[i])) {
      return false;
    }
  }
  amount place[2] = {{2, 50, 0}, {2, 50, 0}};
  if (n <= 2) {
    /* [x] [y], or [keyword] [keyword] in either order */
    if (n == 1) {
      const int k = kind[0];
      const bool vertical = k == TOP || k == BOTTOM;
      const int axis = vertical ? 1 : 0;
      if (k == VALUE) {
        place[0] = value[0];
      }
      else if (k != CENTER) {
        place[axis] = (amount) {2, (k == RIGHT || k == BOTTOM) ? 100.0 : 0.0, 0};
      }
      out[0] = place[0];
      out[1] = place[1];
      return true;
    }
    const bool first_vertical = kind[0] == TOP || kind[0] == BOTTOM, second_horizontal = kind[1] == LEFT || kind[1] == RIGHT;
    const bool second_vertical = kind[1] == TOP || kind[1] == BOTTOM, first_horizontal = kind[0] == LEFT || kind[0] == RIGHT;
    int xi = 0, yi = 1;
    if (first_vertical || second_horizontal) {
      if ((first_vertical && (kind[1] == VALUE || second_vertical)) || (second_horizontal && kind[0] == VALUE) || (first_horizontal && second_horizontal) ||
          (first_vertical && second_vertical)) {
        return false; /* a value after a vertical keyword, or before a horizontal one, is not a position */
      }
      xi = 1;
      yi = 0;
    }
    else if (second_vertical || first_horizontal) {
      xi = 0;
      yi = 1;
    }
    else if (kind[0] == VALUE && kind[1] == CENTER) {
      xi = 0;
      yi = 1;
    }
    for (int a = 0; a < 2; a++) {
      const int idx = a == 0 ? xi : yi;
      const int k = kind[idx];
      if ((a == 0 && (k == TOP || k == BOTTOM)) || (a == 1 && (k == LEFT || k == RIGHT))) {
        return false;
      }
      place[a] = k == VALUE ? value[idx] : (amount) {2, k == CENTER ? 50.0 : (k == RIGHT || k == BOTTOM) ? 100.0 : 0.0, 0};
    }
    out[0] = place[0];
    out[1] = place[1];
    return true;
  }
  /* three or four values: the cascade writes an offset from a far edge as a `calc()`, so a keyword with an offset does not arrive, and browsers refuse the three-value forms */
  return false;
}

/* A position of a gradient's center, in a box of `w` by `h` (a percentage is of the box). */
static bool position_of(const span * t, size_t n, double w, double h, double * x, double * y) {
  amount a[2];
  if (!position_value(t, n, a)) {
    return false;
  }
  *x = a[0].kind == 2 ? w * a[0].v / 100.0 : a[0].kind == 3 ? w * a[0].v / 100.0 + a[0].len : a[0].v;
  *y = a[1].kind == 2 ? h * a[1].v / 100.0 : a[1].kind == 3 ? h * a[1].v / 100.0 + a[1].len : a[1].v;
  return true;
}

//-----------------------------------------------------------------------------
// Interpolation hints and color interpolation methods
//-----------------------------------------------------------------------------

/*
 * The waist interpolates a gradient in one RGB gamut (its `space`), straight or premultiplied, along straight lines between stops. A CSS gradient
 * can interpolate in other spaces (`in oklab`, `in hsl longer hue`) and can bend the interpolation between two stops with a hint. Neither is
 * a line in an RGB space, so each is given to the waist as stops that sample it: between two stops the colors are made by `color-mix()` (CSS
 * Color 5: the same space, hue method and premultiplication the gradient names, by the engine that already computes a mix), and a stop is
 * added wherever the straight line between two samples is further from the true color than a 512th of a channel at the middle of the stretch.
 */

typedef struct sampler {
  gvec_lw_ctx * ctx;
  const GVEC_Svg_Instance * inst;
  const GVEC_Svg_Element * e;
  span from, to;      ///< The text of the two colors.
  const char * method; ///< `in oklab`, `in hsl longer hue`: what follows the comma of `color-mix(`.
  double exponent;    ///< The weight is `t` to this power (1: no hint).
  GSHSCENE_Gradient_Stop * out;
  size_t count;
  double offset0, offset1; ///< Where the stretch begins and ends.
  bool failed;
} sampler;

#define SAMPLE_DEPTH 12
#define SAMPLE_TOLERANCE (1.0 / 512.0)

/* The color at weight `w` of the second (0 and 1 are the stops themselves). */
static GVEC_Result sample_color(sampler * m, double w, GSHSCENE_Paint * paint, bool * ok) {
  char text[640];
  *ok = false;
  if (m->from.n > 200 || m->to.n > 200) {
    return GVEC_OK;
  }
  int n = snprintf(text, sizeof text, "color-mix(%s, %.*s %.10g%%, %.*s %.10g%%)", m->method, (int) m->from.n, m->from.p, (1.0 - w) * 100.0, (int) m->to.n, m->to.p, w * 100.0);
  if (n <= 0 || (size_t) n >= sizeof text) {
    return GVEC_OK;
  }
  GVEC_Svg_Color c;
  bool valid;
  GVEC_Result r = gvec_svg_style_color_of_text(m->ctx->style, m->inst, m->e, text, (size_t) n, &c, &valid);
  if (r != GVEC_OK || !valid) {
    return r;
  }
  GVEC_Svg_Paint given;
  memset(&given, 0, sizeof given);
  given.kind = GVEC_SVG_PAINT_COLOR;
  given.color = c;
  gvec_lw_paint_decision decision;
  r = gvec_lw_paint_of(m->ctx, m->inst, &given, 1.0, paint, &decision);
  if (r != GVEC_OK) {
    return r;
  }
  if (decision == GVEC_LW_PAINT_NONE) {
    memset(paint, 0, sizeof *paint);
    paint->kind = GSHSCENE_PAINT_SOLID;
  }
  else if (decision != GVEC_LW_PAINT_DRAW) {
    return GVEC_OK;
  }
  *ok = paint->kind == GSHSCENE_PAINT_SOLID;
  return GVEC_OK;
}

static double weight_at(const sampler * m, double t) {
  return m->exponent == 1.0 ? t : (double) gmath_pow(t, m->exponent);
}

static GVEC_Result sample_between(sampler * m, double t0, const GSHSCENE_Paint * c0, double t1, const GSHSCENE_Paint * c1, int depth) {
  if (depth >= SAMPLE_DEPTH || m->count + 2 >= EXPANDED_STOPS_MAX) {
    return GVEC_OK;
  }
  const double tm = (t0 + t1) / 2;
  GSHSCENE_Paint mid;
  bool ok;
  GVEC_Result r = sample_color(m, weight_at(m, tm), &mid, &ok);
  if (r != GVEC_OK || !ok) {
    m->failed = m->failed || (r == GVEC_OK && !ok);
    return r;
  }
  /* The straight line between the ends, premultiplied (what the waist draws between two samples), against the true color at the middle. */
  double worst = 0;
  const double a0 = c0->rgba[3], a1 = c1->rgba[3], am = mid.rgba[3];
  const double line[4] = {(c0->rgba[0] * a0 + c1->rgba[0] * a1) / 2, (c0->rgba[1] * a0 + c1->rgba[1] * a1) / 2, (c0->rgba[2] * a0 + c1->rgba[2] * a1) / 2, (a0 + a1) / 2};
  const double true_color[4] = {mid.rgba[0] * am, mid.rgba[1] * am, mid.rgba[2] * am, am};
  for (int k = 0; k < 4; k++) {
    const double d = line[k] > true_color[k] ? line[k] - true_color[k] : true_color[k] - line[k];
    worst = d > worst ? d : worst;
  }
  if (worst <= SAMPLE_TOLERANCE && depth >= 2) {
    return GVEC_OK;
  }
  r = sample_between(m, t0, c0, tm, &mid, depth + 1);
  if (r != GVEC_OK) {
    return r;
  }
  m->out[m->count].offset = m->offset0 + (m->offset1 - m->offset0) * tm;
  m->out[m->count].color = mid;
  m->count++;
  return sample_between(m, tm, &mid, t1, c1, depth + 1);
}

/*
 * `stops` as the waist is given them: each stop, and between two the samples that a hint or a color interpolation method needs. `offset_start` and
 * `offset_end` map the fractions of the line to the waist's offsets (a repeating gradient's first and last stop are 0 and 1). With no hint and no
 * method the stops are given as they are. *ok is false when a color could not be mixed.
 */
static GVEC_Result expand_stops(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Element * e, gradient_stops * stops, const char * method,
    double offset_start, double offset_end, bool * ok) {
  *ok = true;
  stops->expanded = stops->stops;
  stops->expanded_count = stops->count;
  bool any_hint = false;
  for (size_t i = 0; i < stops->count; i++) {
    any_hint = any_hint || !isnan(stops->hint[i]);
  }
  if (!any_hint && !method) {
    return GVEC_OK;
  }
  GSHSCENE_Gradient_Stop * out = gvec_arena_alloc(&ctx->arena, EXPANDED_STOPS_MAX * sizeof *out, sizeof(double));
  if (!out) {
    return GVEC_ERR_OOM;
  }
  sampler m;
  memset(&m, 0, sizeof m);
  m.ctx = ctx;
  m.inst = inst;
  m.e = e;
  m.out = out;
  m.method = method ? method : "in srgb";
  const double span_offset = offset_end - offset_start;
  for (size_t i = 0; i < stops->count; i++) {
    if (m.count + 1 >= EXPANDED_STOPS_MAX) {
      *ok = false;
      return GVEC_OK;
    }
    out[m.count++] = stops->stops[i];
    if (i + 1 == stops->count) {
      break;
    }
    const double o0 = stops->stops[i].offset, o1 = stops->stops[i + 1].offset;
    const double hint = stops->hint[i + 1];
    if ((isnan(hint) && !method) || !(o1 > o0)) {
      continue;
    }
    m.from = stops->color[i];
    m.to = stops->color[i + 1];
    m.offset0 = o0;
    m.offset1 = o1;
    m.exponent = 1.0;
    if (!isnan(hint)) {
      const double h = span_offset != 0 ? (hint - offset_start) / span_offset : 0;
      const double hn = (h - o0) / (o1 - o0); /* where the hint is, as a fraction of the stretch: the middle color is there */
      if (hn <= 0.0 || hn >= 1.0) {
        /* A hint at an end: the whole stretch is the color of the other end, with a jump at the end it is on. */
        if (m.count + 2 >= EXPANDED_STOPS_MAX) {
          *ok = false;
          return GVEC_OK;
        }
        const GSHSCENE_Gradient_Stop jump = hn <= 0.0 ? stops->stops[i + 1] : stops->stops[i];
        out[m.count] = jump;
        out[m.count++].offset = hn <= 0.0 ? o0 : o1;
        continue;
      }
      m.exponent = (double) gmath_log(0.5) / (double) gmath_log(hn);
    }
    /* the samples inside the stretch, in order. The error is measured against the ends as the mix gives them (in the mix's own space, which the
     * ends' own colors, given in the space they were written in, are not): the ends are still given to the waist as they were. */
    GSHSCENE_Paint first, last;
    bool ok_first, ok_last;
    GVEC_Result r = sample_color(&m, 0.0, &first, &ok_first);
    if (r != GVEC_OK) {
      return r;
    }
    r = sample_color(&m, 1.0, &last, &ok_last);
    if (r != GVEC_OK) {
      return r;
    }
    if (!ok_first || !ok_last) {
      *ok = false;
      return GVEC_OK;
    }
    r = sample_between(&m, 0.0, &first, 1.0, &last, 0);
    if (r != GVEC_OK) {
      return r;
    }
    if (m.failed) {
      *ok = false;
      return GVEC_OK;
    }
  }
  stops->expanded = out;
  stops->expanded_count = m.count;
  return GVEC_OK;
}

/* The color interpolation method in the head of a gradient (`in <space> [<hue method> hue]`, in any place among the other words of the head): it is
 * taken out of `tokens`, and written to `method` as `color-mix()` takes it. A method that is not one is left, and the head is not read. */
static bool take_method(span * tokens, size_t * count, char * method, size_t capacity) {
  method[0] = '\0';
  for (size_t i = 0; i < *count; i++) {
    if (!is(tokens[i], "in")) {
      continue;
    }
    size_t used = 2;
    if (i + 1 >= *count) {
      return false;
    }
    if (i + 3 < *count + 0 && is(tokens[i + 3], "hue")) {
      used = 4;
    }
    if (i + used > *count) {
      return false;
    }
    int n = used == 4 ? snprintf(method, capacity, "in %.*s %.*s hue", (int) tokens[i + 1].n, tokens[i + 1].p, (int) tokens[i + 2].n, tokens[i + 2].p)
                      : snprintf(method, capacity, "in %.*s", (int) tokens[i + 1].n, tokens[i + 1].p);
    if (n <= 0 || (size_t) n >= capacity) {
      return false;
    }
    for (size_t j = i; j + used < *count; j++) {
      tokens[j] = tokens[j + used];
    }
    *count -= used;
    return true;
  }
  return true;
}

/*
 * The gradient of `image` (`linear-gradient(...)`, `radial-gradient(...)`, and the repeating forms) over a tile of `w` by `h` at the origin, as the
 * waist's gradient with its stops (the arrays are the caller's). `*ok` is false when this does not read it.
 */
static GVEC_Result make_gradient(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GVEC_Svg_Element * e, span image, double w, double h,
    GSHSCENE_Gradient * g, gradient_stops * stops, bool * ok) {
  *ok = false;
  const bool repeating = starts(image, "repeating-");
  span body = image;
  if (repeating) {
    body.p += 10;
    body.n -= 10;
  }
  const bool linear = starts(body, "linear-gradient(");
  const bool radial = starts(body, "radial-gradient(");
  const bool conic = starts(body, "conic-gradient(");
  if ((!linear && !radial && !conic) || body.n < 2 || body.p[body.n - 1] != ')') {
    return GVEC_OK;
  }
  const size_t open = conic ? 15 : 16;
  span inner = trim((span) {body.p + open, body.n - open - 1});
  span args[STOPS_MAX + 2];
  size_t count = split(inner, false, args, STOPS_MAX + 2);
  if (count > STOPS_MAX + 1) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a gradient of a mask has more stops than the lowering keeps");
  }
  if (count < 2) {
    return GVEC_OK;
  }
  memset(g, 0, sizeof *g);
  GSHGEOM_Map identity;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  g->map = identity;
  g->premultiplied = 1;
  g->extend = repeating ? GSHSCENE_EXTEND_REPEAT : GSHSCENE_EXTEND_PAD;
  GVEC_Result r;
  char method[96];
  method[0] = '\0';
  double offset_start = 0, offset_end = 1;
  if (linear) {
    size_t first_stop = 0;
    double degrees = 180; /* to bottom */
    span tokens[TOKENS_MAX];
    size_t nt = split(args[0], true, tokens, TOKENS_MAX);
    if (nt > TOKENS_MAX || !take_method(tokens, &nt, method, sizeof method)) {
      return GVEC_OK;
    }
    if (nt == 0 && method[0]) {
      first_stop = 1; /* a head that is only the method */
    }
    else if (nt && nt <= TOKENS_MAX && (starts(tokens[0], "to") && tokens[0].n == 2)) {
      double sx = 0, sy = 0; /* the direction toward the named sides: x positive to the right, y positive up */
      for (size_t i = 1; i < nt; i++) {
        if (is(tokens[i], "left")) {
          sx = -1;
        }
        else if (is(tokens[i], "right")) {
          sx = 1;
        }
        else if (is(tokens[i], "top")) {
          sy = 1;
        }
        else if (is(tokens[i], "bottom")) {
          sy = -1;
        }
        else {
          return GVEC_OK;
        }
      }
      if (sx == 0 && sy == 0) {
        return GVEC_OK;
      }
      if (sx == 0 || sy == 0) {
        degrees = sy > 0 ? 0 : sy < 0 ? 180 : sx > 0 ? 90 : 270;
      }
      else {
        /* A corner: the 50% line runs through the two corners that are not named, so the direction is perpendicular to that diagonal. */
        const double dx = sx * h, dy = sy * w; /* x to the right, y up */
        degrees = gmath_atan2(dx, dy) * 180.0 / 3.14159265358979323846;
      }
      first_stop = 1;
    }
    else if (nt == 1 && angle_of(tokens[0], &degrees)) {
      first_stop = 1;
    }
    const double rad = degrees * 3.14159265358979323846 / 180.0;
    const double dx = gmath_sin(rad), dy = -gmath_cos(rad);
    const double length = (dx < 0 ? -dx : dx) * w + (dy < 0 ? -dy : dy) * h;
    if (!(length > 0)) {
      return GVEC_OK;
    }
    bool read;
    r = read_stops(ctx, inst, e, args, first_stop, count, length, false, stops, &read);
    if (r != GVEC_OK || !read) {
      return r;
    }
    double start = 0, end = 1;
    if (repeating) {
      start = stops->position[0];
      end = stops->position[stops->count - 1];
      if (!(end > start)) {
        return GVEC_OK;
      }
      for (size_t i = 0; i < stops->count; i++) {
        stops->stops[i].offset = (stops->position[i] - start) / (end - start);
      }
    }
    offset_start = start;
    offset_end = end;
    const double cx = w / 2, cy = h / 2;
    g->kind = GSHSCENE_GRADIENT_LINEAR;
    g->p0[0] = cx + dx * length * (start - 0.5);
    g->p0[1] = cy + dy * length * (start - 0.5);
    g->p1[0] = cx + dx * length * (end - 0.5);
    g->p1[1] = cy + dy * length * (end - 0.5);
  }
  else if (radial) {
    /* [circle | ellipse] [<size>] [at <position>], then the stops. */
    size_t first_stop = 0;
    bool circle = false, has_size_keyword = false;
    int keyword = 3; /* closest-side 0, closest-corner 1, farthest-side 2, farthest-corner 3 */
    double rx = 0, ry = 0, cx = w / 2, cy = h / 2;
    bool explicit_size = false;
    span tokens[TOKENS_MAX];
    size_t nt = split(args[0], true, tokens, TOKENS_MAX);
    if (nt > TOKENS_MAX || !take_method(tokens, &nt, method, sizeof method)) {
      return GVEC_OK;
    }
    bool is_head = method[0] != '\0';
    for (size_t i = 0; i < nt && i < TOKENS_MAX; i++) {
      is_head = is_head || is(tokens[i], "circle") || is(tokens[i], "ellipse") || is(tokens[i], "at") || starts(tokens[i], "closest-") || starts(tokens[i], "farthest-");
    }
    if (nt && nt <= TOKENS_MAX && !is_head) {
      /* a lone length or percent head is a size: `radial-gradient(10px, ...)` is a circle's radius */
      double v;
      bool percent;
      is_head = nt <= 2 && length_or_percent(tokens[0], &v, &percent) && (nt == 1 || length_or_percent(tokens[1], &v, &percent));
    }
    if (is_head) {
      size_t at = nt;
      for (size_t i = 0; i < nt; i++) {
        if (is(tokens[i], "at")) {
          at = i;
          break;
        }
      }
      size_t lengths = 0;
      double sizes[2] = {0, 0};
      bool size_percent[2] = {false, false};
      for (size_t i = 0; i < at; i++) {
        if (is(tokens[i], "circle")) {
          circle = true;
        }
        else if (is(tokens[i], "ellipse")) {
          circle = false;
        }
        else if (is(tokens[i], "closest-side")) {
          keyword = 0, has_size_keyword = true;
        }
        else if (is(tokens[i], "closest-corner")) {
          keyword = 1, has_size_keyword = true;
        }
        else if (is(tokens[i], "farthest-side")) {
          keyword = 2, has_size_keyword = true;
        }
        else if (is(tokens[i], "farthest-corner")) {
          keyword = 3, has_size_keyword = true;
        }
        else if (lengths < 2 && length_or_percent(tokens[i], &sizes[lengths], &size_percent[lengths])) {
          lengths++;
        }
        else {
          return GVEC_OK;
        }
      }
      if (lengths) {
        if (has_size_keyword) {
          return GVEC_OK;
        }
        explicit_size = true;
        if (lengths == 1) {
          if (size_percent[0]) {
            return GVEC_OK; /* A circle's radius is not a percentage. */
          }
          circle = true;
          rx = ry = sizes[0];
        }
        else {
          if (circle) {
            return GVEC_OK;
          }
          rx = size_percent[0] ? sizes[0] / 100.0 * w : sizes[0];
          ry = size_percent[1] ? sizes[1] / 100.0 * h : sizes[1];
        }
      }
      if (at < nt && !position_of(tokens + at + 1, nt - at - 1, w, h, &cx, &cy)) {
        return GVEC_OK;
      }
      first_stop = 1;
    }
    if (!explicit_size) {
      const double left = cx, right = w - cx, top = cy, bottom = h - cy;
      const double ax = left < right ? left : right, bx = left < right ? right : left;
      const double ay = top < bottom ? top : bottom, by = top < bottom ? bottom : top;
      if (circle) {
        const double near_side = ax < ay ? ax : ay, far_side = bx > by ? bx : by;
        const double near_corner = gmath_sqrt(ax * ax + ay * ay), far_corner = gmath_sqrt(bx * bx + by * by);
        rx = ry = keyword == 0 ? near_side : keyword == 1 ? near_corner : keyword == 2 ? far_side : far_corner;
      }
      else {
        const double k = keyword == 1 || keyword == 3 ? 1.4142135623730951 : 1.0;
        rx = (keyword <= 1 ? ax : bx) * k;
        ry = (keyword <= 1 ? ay : by) * k;
      }
    }
    if (!(rx > 0) || !(ry > 0)) {
      return GVEC_OK;
    }
    bool read;
    r = read_stops(ctx, inst, e, args, first_stop, count, rx, false, stops, &read);
    if (r != GVEC_OK || !read) {
      return r;
    }
    double start = 0, end = 1;
    if (repeating) {
      start = stops->position[0];
      end = stops->position[stops->count - 1];
      if (!(end > start)) {
        return GVEC_OK;
      }
      for (size_t i = 0; i < stops->count; i++) {
        stops->stops[i].offset = (stops->position[i] - start) / (end - start);
      }
    }
    offset_start = start;
    offset_end = end;
    GSHGEOM_Map shift, scale;
    if (gshgeom_map_translate(2, cx, cy, 0, &shift) != GSHGEOM_OK || gshgeom_map_scale(2, rx, ry, 1, &scale) != GSHGEOM_OK || !gvec_lw_compose(&shift, &scale, &g->map)) {
      return GVEC_OK;
    }
    g->kind = GSHSCENE_GRADIENT_RADIAL;
    g->r0 = start;
    g->r1 = end;
  }
  else {
    /* [from <angle>] [at <position>], then the stops, at angles or percentages of a turn: the waist's sweep, from `+x` toward `+y`, where CSS starts
     * at the top and runs clockwise, so that CSS's angle is the sweep's plus 90 degrees. */
    size_t first_stop = 0;
    double from_degrees = 0, cx = w / 2, cy = h / 2;
    span tokens[TOKENS_MAX];
    size_t nt = split(args[0], true, tokens, TOKENS_MAX);
    if (nt > TOKENS_MAX || !take_method(tokens, &nt, method, sizeof method)) {
      return GVEC_OK;
    }
    if (method[0] || (nt && (is(tokens[0], "from") || is(tokens[0], "at")))) {
      size_t at = 0;
      if (at < nt && is(tokens[at], "from")) {
        if (at + 1 >= nt || !angle_of(tokens[at + 1], &from_degrees)) {
          return GVEC_OK;
        }
        at += 2;
      }
      if (at < nt) {
        if (!is(tokens[at], "at") || !position_of(tokens + at + 1, nt - at - 1, w, h, &cx, &cy)) {
          return GVEC_OK;
        }
      }
      first_stop = 1;
    }
    bool read;
    r = read_stops(ctx, inst, e, args, first_stop, count, 360.0, true, stops, &read);
    if (r != GVEC_OK || !read) {
      return r;
    }
    double start = 0, end = 1;
    if (repeating) {
      start = stops->position[0];
      end = stops->position[stops->count - 1];
      if (!(end > start)) {
        return GVEC_OK;
      }
      for (size_t i = 0; i < stops->count; i++) {
        stops->stops[i].offset = (stops->position[i] - start) / (end - start);
      }
    }
    else {
      /* The turn is covered from 0 to 1: before the first stop the first color, after the last the last. The sweep repeats, so the ends are stops. */
      if (stops->position[0] > 0 && stops->count < STOPS_MAX) {
        for (size_t i = stops->count; i > 0; i--) {
          stops->stops[i] = stops->stops[i - 1];
          stops->position[i] = stops->position[i - 1];
          stops->given[i] = stops->given[i - 1];
          stops->color[i] = stops->color[i - 1];
          stops->hint[i] = stops->hint[i - 1];
        }
        stops->hint[1] = NAN;
        stops->position[0] = 0;
        stops->stops[0].offset = 0;
        stops->count++;
      }
      if (stops->position[stops->count - 1] < 1 && stops->count < STOPS_MAX) {
        const size_t last = stops->count;
        stops->stops[last] = stops->stops[last - 1];
        stops->position[last] = 1;
        stops->stops[last].offset = 1;
        stops->given[last] = true;
        stops->color[last] = stops->color[last - 1];
        stops->hint[last] = NAN;
        stops->count++;
      }
    }
    offset_start = start;
    offset_end = end;
    const double a0 = from_degrees - 90.0 + 360.0 * start;
    g->kind = GSHSCENE_GRADIENT_SWEEP;
    g->center[0] = cx;
    g->center[1] = cy;
    g->angle_unit = GSHGEOM_ANGLE_DEGREES;
    g->a0 = a0;
    g->a1 = a0 + 360.0 * (end - start);
    g->extend = GSHSCENE_EXTEND_REPEAT;
  }
  bool expanded;
  r = expand_stops(ctx, inst, e, stops, method[0] ? method : NULL, offset_start, offset_end, &expanded);
  if (r != GVEC_OK || !expanded) {
    return r;
  }
  g->stop_count = stops->expanded_count;
  g->stops = stops->expanded;
  *ok = true;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// A layer's picture
//-----------------------------------------------------------------------------

static uint64_t mix(uint64_t a, uint64_t b) {
  a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2);
  return a ? a : 1;
}

static GVEC_Result new_node(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, gvec_lw_kind kind, gvec_lw_role role, uint64_t salt, gvec_lw_node ** out) {
  gvec_lw_node * node = gvec_lw_new_node(ctx, kind);
  *out = node;
  if (!node) {
    return GVEC_ERR_OOM;
  }
  node->key = gvec_lw_key(e->key, mix(ctx->chain, salt), role);
  node->site = gvec_lw_new_site(ctx, e, node->key);
  return node->site == GVEC_LW_NO_SITE ? GVEC_ERR_OOM : GVEC_OK;
}

static GVEC_Result rect_node(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const GSHGEOM_Map * local, double x, double y, double w, double h, const GSHSCENE_Paint * paint,
    uint64_t salt, gvec_lw_node ** out, bool * ok) {
  *ok = false;
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * path = NULL;
  GVEC_Result r = gvec_lw_rect_path(ctx, x, y, w, h, &geometry, &path);
  if (r != GVEC_OK) {
    return r;
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a tile of a mask is past the range the waist can carry");
  }
  r = new_node(ctx, e, GVEC_LW_FILL, GVEC_LW_ROLE_MASK_TILE, salt, out);
  if (r != GVEC_OK) {
    return r;
  }
  (*out)->map = *local;
  (*out)->resource = path;
  (*out)->rule = GSHGEOM_FILL_NONZERO;
  (*out)->paint = *paint;
  (*out)->content = true;
  *ok = true;
  return GVEC_OK;
}

/* The size of one tile of a layer in an area of `aw` by `ah`, from `mask-size` and the image's intrinsic size and ratio (CSS Backgrounds 3, 3.9). */
static void tile_size(const layer * l, double aw, double ah, double iw, double ih, double * w, double * h) {
  const bool ratio = iw > 0 && ih > 0;
  if (l->size_keyword && ratio) {
    const double sx = aw / iw, sy = ah / ih;
    const double s = l->size_keyword == 1 ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
    *w = iw * s;
    *h = ih * s;
    return;
  }
  if (l->size_keyword) {
    *w = aw;
    *h = ah;
    return;
  }
  const amount a = l->size[0], b = l->size[1];
  double tw = a.kind == 1 ? a.v : a.kind == 2 ? a.v / 100.0 * aw : -1;
  double th = b.kind == 1 ? b.v : b.kind == 2 ? b.v / 100.0 * ah : -1;
  if (tw < 0 && th < 0) {
    tw = iw > 0 ? iw : aw;
    th = ih > 0 ? ih : ah;
  }
  else if (tw < 0) {
    tw = ratio ? th * iw / ih : aw;
  }
  else if (th < 0) {
    th = ratio ? tw * ih / iw : ah;
  }
  *w = tw;
  *h = th;
}

/* The offsets, along one axis, of the tiles of a layer: the first tile's place and the step, and how many cover [lo, hi]. */
typedef struct axis_tiles {
  double first, step;
  long count;
} axis_tiles;

static bool axis_tiles_of(int repeat, double area_lo, double area_size, double size, const amount * position, double lo, double hi, axis_tiles * out) {
  if (!(size > 0)) {
    return false;
  }
  if (repeat == 3) {
    double n = gmath_floor(area_size / size + 0.5);
    n = n < 1 ? 1 : n;
    size = area_size / n;
  }
  double offset = position->kind == 2 ? (area_size - size) * position->v / 100.0
                  : position->kind == 3 ? (area_size - size) * position->v / 100.0 + position->len : position->v;
  double step = size;
  if (repeat == 2) {
    const double n = gmath_floor(area_size / size);
    if (n >= 2) {
      step = size + (area_size - n * size) / (n - 1);
      offset = 0;
    }
    else {
      repeat = 1;
    }
  }
  const double origin = area_lo + offset;
  if (repeat == 1) {
    out->first = origin;
    out->step = step;
    out->count = origin + size > lo && origin < hi ? 1 : 0;
    return true;
  }
  const double from = gmath_floor((lo - origin - size) / step) + 1; /* the first tile whose far edge is past lo */
  const double to = gmath_ceil((hi - origin) / step);                /* one past the last whose near edge is before hi */
  out->first = origin + from * step;
  out->step = step;
  const double span = to - from;
  out->count = span < 0 ? 0 : span > (double) TILES_MAX + 1 ? TILES_MAX + 1 : (long) span;
  return true;
}

/* The image a `url()` of a mask names, loaded: `*state` is 0 for ready (its size in `*iw` and `*ih`), 1 when it was refused (reported), 2 when it cannot be used (it covers nothing). */
static GVEC_Result load_source(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, span source, gvec_lw_image * image, double * iw, double * ih, int * state) {
  span url = trim(source);
  *state = 1;
  /* url("...") with the quotes the engine's serialisation gives it */
  if (!starts(url, "url(") || url.n < 7 || url.p[url.n - 1] != ')') {
    return refuse_layer(ctx, e, "a mask image that is not a url, a gradient or none is not lowered");
  }
  span inner = trim((span) {url.p + 4, url.n - 5});
  if (inner.n >= 2 && (inner.p[0] == '"' || inner.p[0] == '\'') && inner.p[inner.n - 1] == inner.p[0]) {
    inner.p++;
    inner.n -= 2;
  }
  char * text = gvec_arena_alloc(&ctx->arena, inner.n + 1, 1);
  if (!text) {
    return GVEC_ERR_OOM;
  }
  memcpy(text, inner.p, inner.n);
  text[inner.n] = 0;
  gvec_lw_image_state loaded;
  GVEC_Result r = gvec_lw_image_load_url(ctx, e, text, &loaded, image);
  if (r != GVEC_OK) {
    return r;
  }
  if (loaded == GVEC_LW_IMAGE_REFUSED) {
    return GVEC_OK; /* reported: the element is refused */
  }
  if (loaded != GVEC_LW_IMAGE_READY) {
    *state = 2;
    return GVEC_OK;
  }
  *iw = image->width;
  *ih = image->height;
  *state = 0;
  return GVEC_OK;
}

/*
 * The picture of a layer's art: a clip to its painting area around the tiles of its image. `*top` is NULL when the layer covers nothing. `*ok` is false
 * when the layer is not lowered (reported).
 */
static GVEC_Result layer_art(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * masked, const GSHGEOM_Map * local, const layer * l, size_t index, const GSHGEOM_Bounds * origin_box,
    const GSHGEOM_Bounds * clip_box, gvec_lw_node ** top, bool * ok) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(masked);
  *top = NULL;
  *ok = false;
  const double aw = origin_box->max.x - origin_box->min.x, ah = origin_box->max.y - origin_box->min.y;
  if (!(aw > 0) || !(ah > 0) || !(clip_box->max.x > clip_box->min.x) || !(clip_box->max.y > clip_box->min.y)) {
    *ok = true;
    return GVEC_OK;
  }
  gvec_lw_image image;
  memset(&image, 0, sizeof image);
  double iw = 0, ih = 0, pixel_w = 0, pixel_h = 0;
  if (l->kind == LAYER_IMAGE) {
    int state;
    GVEC_Result r = load_source(ctx, e, l->image, &image, &iw, &ih, &state);
    if (r != GVEC_OK || state == 1) {
      return r;
    }
    if (state == 2) {
      *ok = true; /* an image that cannot be used covers nothing */
      return GVEC_OK;
    }
    pixel_w = iw;
    pixel_h = ih;
    if (l->resolution > 0) {
      iw /= l->resolution; /* the size in CSS pixels */
      ih /= l->resolution;
    }
  }
  double tw, th;
  tile_size(l, aw, ah, iw, ih, &tw, &th);
  if (!(tw > 0) || !(th > 0) || tw - tw != 0 || th - th != 0) {
    *ok = true;
    return GVEC_OK;
  }
  axis_tiles ax, ay;
  if (!axis_tiles_of(l->repeat[0], origin_box->min.x, aw, tw, &l->position[0], clip_box->min.x, clip_box->max.x, &ax) ||
      !axis_tiles_of(l->repeat[1], origin_box->min.y, ah, th, &l->position[1], clip_box->min.y, clip_box->max.y, &ay)) {
    *ok = true;
    return GVEC_OK;
  }
  if (l->repeat[0] == 3) {
    tw = ax.step;
  }
  if (l->repeat[1] == 3) {
    th = ay.step;
  }
  if (ax.count == 0 || ay.count == 0) {
    *ok = true;
    return GVEC_OK;
  }
  if (ax.count > TILES_MAX || ay.count > TILES_MAX || ax.count * ay.count > TILES_MAX) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a mask image is repeated more times than the lowering draws");
  }
  /* The clip to the painting area, around the tiles. */
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * clip_path = NULL;
  GVEC_Result r = gvec_lw_rect_path(ctx, clip_box->min.x, clip_box->min.y, clip_box->max.x - clip_box->min.x, clip_box->max.y - clip_box->min.y, &geometry, &clip_path);
  if (r != GVEC_OK) {
    return r;
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the box of a mask is past the range the waist can carry");
  }
  gvec_lw_node * clip;
  r = new_node(ctx, e, GVEC_LW_CLIP, GVEC_LW_ROLE_MASK_LAYER, (uint64_t) index * 2 + 1, &clip);
  if (r != GVEC_OK) {
    return r;
  }
  clip->map = *local;
  clip->resource = clip_path;
  clip->rule = GSHGEOM_FILL_NONZERO;

  gradient_stops stops;
  GSHSCENE_Gradient gradient;
  if (l->kind == LAYER_GRADIENT) {
    bool read;
    r = make_gradient(ctx, masked, e, l->image, tw, th, &gradient, &stops, &read);
    if (r != GVEC_OK) {
      return r;
    }
    if (!read) {
      return refuse_layer(ctx, e, "a gradient of a mask that this does not read (a color it does not know, or a gradient this grammar does not say) is not lowered");
    }
  }
  uint32_t magnify = 0, minify = 0;
  if (l->kind == LAYER_IMAGE && image.resource) {
    bool usable;
    r = gvec_lw_image_filters(ctx, masked, &magnify, &minify, &usable);
    if (r != GVEC_OK || !usable) {
      return r;
    }
  }
  uint64_t tile = 0;
  for (long j = 0; j < ay.count; j++) {
    for (long i = 0; i < ax.count; i++, tile++) {
      const double x = ax.first + (double) i * ax.step, y = ay.first + (double) j * ay.step;
      gvec_lw_node * node = NULL;
      bool made = true;
      if (l->kind == LAYER_GRADIENT) {
        GSHSCENE_Gradient at = gradient;
        GSHGEOM_Map shift;
        if (gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&shift, &gradient.map, &at.map)) {
          return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a gradient of a mask is past the range the waist can carry");
        }
        if (gradient.kind == GSHSCENE_GRADIENT_LINEAR || gradient.kind == GSHSCENE_GRADIENT_SWEEP) {
          at.map = shift; /* the line, or the center, is in the tile's own coordinates */
        }
        const GSHSCENE_Resource * resource = NULL;
        GSHSCENE_Result sr = gshscene_store_intern_gradient(ctx->store, &at, &resource);
        if (sr == GSHSCENE_ERR_OOM) {
          return GVEC_ERR_OOM;
        }
        if (sr != GSHSCENE_OK) {
          return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a gradient of a mask is past what the waist can carry");
        }
        r = gvec_lw_hold(ctx, resource);
        if (r != GVEC_OK) {
          return r;
        }
        GSHSCENE_Paint paint;
        memset(&paint, 0, sizeof paint);
        paint.kind = GSHSCENE_PAINT_GRADIENT;
        paint.resource = resource;
        paint.rgba[3] = 1.0;
        r = rect_node(ctx, e, local, x, y, tw, th, &paint, (uint64_t) index * 100003 + tile, &node, &made);
      }
      else if (image.resource) {
        GSHGEOM_Map place, shift, scale, map;
        if (gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK || gshgeom_map_scale(2, tw / pixel_w, th / pixel_h, 1, &scale) != GSHGEOM_OK || !gvec_lw_compose(&shift, &scale, &place)) {
          return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a mask image's map is past the range the waist can carry");
        }
        map = place;
        GSHSCENE_Paint paint;
        const GSHSCENE_Resource * path = NULL;
        bool image_ok;
        r = gvec_lw_image_paint(ctx, e, &image, &map, magnify, minify, &paint, &path, &image_ok);
        if (r != GVEC_OK || !image_ok) {
          return r;
        }
        r = new_node(ctx, e, GVEC_LW_FILL, GVEC_LW_ROLE_MASK_TILE, (uint64_t) index * 100003 + tile, &node);
        if (r != GVEC_OK) {
          return r;
        }
        node->map = *local;
        node->resource = path;
        node->rule = GSHGEOM_FILL_NONZERO;
        node->paint = paint;
        node->content = true;
      }
      else if (image.svg) {
        GSHGEOM_Map shift, scale, placed;
        if (gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK || gshgeom_map_scale(2, tw / pixel_w, th / pixel_h, 1, &scale) != GSHGEOM_OK || !gvec_lw_compose(&shift, &scale, &placed)) {
          return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a mask image's map is past the range the waist can carry");
        }
        r = gvec_lw_svg_image_node(ctx, e, local, &image, &placed, &node);
      }
      if (r != GVEC_OK || !made) {
        return r;
      }
      if (node) {
        r = gvec_lw_attach(ctx, clip, node);
        if (r != GVEC_OK) {
          return r;
        }
      }
    }
  }
  *top = clip->content ? clip : NULL;
  *ok = true;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// The mask
//-----------------------------------------------------------------------------

/* The mask resource of a picture whose content is `art` (under a hidden holder). */
static GVEC_Result mask_of_art(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, gvec_lw_node * art, uint32_t mode, uint64_t salt, const GSHSCENE_Resource ** out) {
  gvec_lw_node * holder;
  GVEC_Result r = new_node(ctx, e, GVEC_LW_USE, GVEC_LW_ROLE_MASK_CONTAINER, salt, &holder);
  if (r != GVEC_OK) {
    return r;
  }
  r = gvec_lw_attach(ctx, holder, art);
  if (r != GVEC_OK) {
    return r;
  }
  const GSHSCENE_Resource * picture = NULL;
  r = gvec_lw_emit_picture(ctx, holder, &picture);
  if (r != GVEC_OK) {
    return r;
  }
  if (!picture) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask's picture is past what the waist can carry");
  }
  GSHSCENE_Mask m;
  memset(&m, 0, sizeof m);
  m.picture = picture;
  m.mode = mode;
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result sr = gshscene_store_intern_mask(ctx->store, &m, &resource);
  gshscene_resource_release(picture);
  if (sr == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (sr != GSHSCENE_OK) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask is past what the waist can carry");
  }
  r = gvec_lw_hold(ctx, resource);
  if (r != GVEC_OK) {
    return r;
  }
  *out = resource;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// The mask border (CSS Masking 1, 6.2; CSS Backgrounds 3, 6.2 for what a border image is)
//-----------------------------------------------------------------------------

static GVEC_Result image_function(gvec_lw_ctx * ctx, span text, span * out, double * resolution, bool * ok);

/* One to four values, as the four sides: top right bottom left (the missing ones are those the one before says, as for `margin`). */
static void four_sides(size_t n, size_t index[4]) {
  index[0] = 0;
  index[1] = n > 1 ? 1 : 0;
  index[2] = n > 2 ? 2 : 0;
  index[3] = n > 3 ? 3 : n > 1 ? 1 : 0;
}

/* `mask-border-source`, `-slice`, `-width`, `-outset`, `-repeat` and `-mode` of the element, as a layer of kind LAYER_BORDER. *ok is false when something is not read. */
static GVEC_Result parse_border(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * masked, span source, layer * l, bool * ok) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(masked);
  *ok = false;
  memset(l, 0, sizeof *l);
  l->kind = LAYER_BORDER;
  l->image = source;
  l->origin = l->clip = 1; /* the border image area is of the stroke box, the box an SVG element has in place of a border box */
  l->composite = 0;
  span slice, width, outset, repeat, mode;
  GVEC_Result r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_BORDER_SLICE, &slice);
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_BORDER_WIDTH, &width);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_BORDER_OUTSET, &outset);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_BORDER_REPEAT, &repeat);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_BORDER_MODE, &mode);
  }
  if (r != GVEC_OK) {
    return r;
  }
  if (starts(source, "image-set(") || starts(source, "-webkit-image-set(") || starts(source, "image(")) {
    bool picked;
    r = image_function(ctx, source, &l->image, &l->resolution, &picked);
    if (r != GVEC_OK) {
      return r;
    }
    if (!picked) {
      return refuse_layer(ctx, e, "an image-set() or image() of a mask that this does not read is not lowered");
    }
    source = l->image;
  }
  if (!(starts(source, "url(") || starts(source, "linear-gradient(") || starts(source, "radial-gradient(") || starts(source, "conic-gradient(") ||
        starts(source, "repeating-linear-gradient(") || starts(source, "repeating-radial-gradient(") || starts(source, "repeating-conic-gradient("))) {
    return refuse_layer(ctx, e, "a mask-border-source that is not a url or a gradient is not lowered");
  }
  l->mode = is(trim(mode), "luminance") ? 0 : 1;
  /* the slice: one to four numbers or percentages, and `fill` */
  span t[6];
  size_t n = split(trim(slice), true, t, 6);
  size_t values = 0;
  double sv[4] = {0, 0, 0, 0};
  bool sp[4] = {false, false, false, false};
  for (size_t i = 0; i < n; i++) {
    if (is(t[i], "fill")) {
      l->slice_fill = true;
      continue;
    }
    double v;
    span unit;
    if (values >= 4 || !number_unit(t[i], &v, &unit) || !(unit.n == 0 || is(unit, "%")) || v < 0) {
      return refuse_layer(ctx, e, "a mask-border-slice that this does not read is not lowered");
    }
    sv[values] = v;
    sp[values] = unit.n != 0;
    values++;
  }
  if (values == 0) {
    values = 1; /* the initial value, 0 */
  }
  size_t side[4];
  four_sides(values, side);
  for (size_t k = 0; k < 4; k++) {
    l->slice[k] = sv[side[k]];
    l->slice_percent[k] = sp[side[k]];
  }
  /* the width: a length, a percentage, a number or `auto`, one to four */
  n = split(trim(width), true, t, 6);
  amount wv[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  values = 0;
  for (size_t i = 0; i < n; i++) {
    double v;
    bool percent;
    span unit;
    if (values >= 4) {
      return refuse_layer(ctx, e, "a mask-border-width that this does not read is not lowered");
    }
    if (is(t[i], "auto")) {
      wv[values] = (amount) {0, 0, 0};
    }
    else if (length_or_percent(t[i], &v, &percent) && v >= 0) {
      wv[values] = (amount) {percent ? 2 : 1, v, 0};
    }
    else if (number_unit(t[i], &v, &unit) && unit.n == 0 && v >= 0) {
      wv[values] = (amount) {4, v, 0};
    }
    else {
      return refuse_layer(ctx, e, "a mask-border-width that this does not read is not lowered");
    }
    values++;
  }
  if (values == 0) {
    values = 1;
  }
  four_sides(values, side);
  for (size_t k = 0; k < 4; k++) {
    l->width[k] = wv[side[k]];
  }
  /* the outset: a length or a number, one to four; a number is of the border width, and an SVG element has none */
  n = split(trim(outset), true, t, 6);
  double ov[4] = {0, 0, 0, 0};
  values = 0;
  for (size_t i = 0; i < n; i++) {
    double v;
    bool percent;
    span unit;
    if (values >= 4) {
      return refuse_layer(ctx, e, "a mask-border-outset that this does not read is not lowered");
    }
    if (length_or_percent(t[i], &v, &percent) && !percent && v >= 0) {
      ov[values] = v;
    }
    else if (number_unit(t[i], &v, &unit) && unit.n == 0 && v >= 0) {
      ov[values] = 0;
    }
    else {
      return refuse_layer(ctx, e, "a mask-border-outset that this does not read is not lowered");
    }
    values++;
  }
  if (values == 0) {
    values = 1;
  }
  four_sides(values, side);
  for (size_t k = 0; k < 4; k++) {
    l->outset[k] = ov[side[k]];
  }
  /* the repeat: one or two of stretch, repeat, round and space */
  n = split(trim(repeat), true, t, 3);
  if (n == 0 || n > 2) {
    return refuse_layer(ctx, e, "a mask-border-repeat that this does not read is not lowered");
  }
  for (size_t a = 0; a < 2; a++) {
    const span w = a < n ? t[a] : t[0];
    l->border_repeat[a] = is(w, "stretch") ? 0 : is(w, "repeat") ? 1 : is(w, "round") ? 2 : is(w, "space") ? 3 : -1;
    if (l->border_repeat[a] < 0) {
      return refuse_layer(ctx, e, "a mask-border-repeat that this does not read is not lowered");
    }
  }
  *ok = true;
  return GVEC_OK;
}

/* One cell of a border: the region of the source image `from` (x, y, w, h) shown in `to`, drawn within `to_clip`. */
typedef struct border_cell {
  double sx, sy, sw, sh; ///< The slice of the image (pixels of the image).
  double dx, dy, dw, dh; ///< Where the whole slice goes: it is scaled to this.
  double cx0, cy0, cx1, cy1; ///< What of it is drawn (a repeated cell is cut off at the ends of its region).
} border_cell;

typedef struct border_paint {
  gvec_lw_ctx * ctx;
  const GVEC_Svg_Instance * masked;
  const GVEC_Svg_Element * e;
  const GSHGEOM_Map * local;
  size_t index;
  gvec_lw_node * holder;
  uint64_t cells;
  bool gradient;
  GSHSCENE_Gradient base;
  gradient_stops stops;
  gvec_lw_image image;
  uint32_t magnify, minify;
} border_paint;

static GVEC_Result border_cell_node(border_paint * b, const border_cell * c) {
  gvec_lw_ctx * ctx = b->ctx;
  if (!(c->cx1 > c->cx0) || !(c->cy1 > c->cy0) || !(c->sw > 0) || !(c->sh > 0)) {
    return GVEC_OK;
  }
  if (++b->cells > TILES_MAX) {
    return gvec_lw_refuse(ctx, b->e, GVEC_LOWER_LIMIT, "a mask border is drawn in more pieces than the lowering draws");
  }
  /* image space to the element's: the slice's corner goes to the cell's, scaled by what the slice is stretched by */
  const double fx = c->dw / c->sw, fy = c->dh / c->sh;
  GSHGEOM_Map shift, scale, map;
  if (gshgeom_map_translate(2, c->dx - c->sx * fx, c->dy - c->sy * fy, 0, &shift) != GSHGEOM_OK || gshgeom_map_scale(2, fx, fy, 1, &scale) != GSHGEOM_OK ||
      !gvec_lw_compose(&shift, &scale, &map)) {
    return gvec_lw_refuse(ctx, b->e, GVEC_LOWER_LIMIT, "a mask border is past the range the waist can carry");
  }
  const uint64_t salt = (uint64_t) b->index * 100003 + 77 + b->cells * 2;
  gvec_lw_node * node = NULL;
  GVEC_Result r;
  if (b->gradient) {
    GSHSCENE_Gradient at = b->base;
    if (!gvec_lw_compose(&map, &b->base.map, &at.map)) {
      return gvec_lw_refuse(ctx, b->e, GVEC_LOWER_LIMIT, "a gradient of a mask border is past the range the waist can carry");
    }
    const GSHSCENE_Resource * resource = NULL;
    GSHSCENE_Result sr = gshscene_store_intern_gradient(ctx->store, &at, &resource);
    if (sr == GSHSCENE_ERR_OOM) {
      return GVEC_ERR_OOM;
    }
    if (sr != GSHSCENE_OK) {
      return gvec_lw_refuse(ctx, b->e, GVEC_LOWER_LIMIT, "a gradient of a mask border is past what the waist can carry");
    }
    r = gvec_lw_hold(ctx, resource);
    if (r != GVEC_OK) {
      return r;
    }
    GSHSCENE_Paint paint;
    memset(&paint, 0, sizeof paint);
    paint.kind = GSHSCENE_PAINT_GRADIENT;
    paint.resource = resource;
    paint.rgba[3] = 1.0;
    bool made;
    r = rect_node(ctx, b->e, b->local, c->cx0, c->cy0, c->cx1 - c->cx0, c->cy1 - c->cy0, &paint, salt, &node, &made);
    if (r != GVEC_OK || !made) {
      return r;
    }
  }
  else {
    /* an image shows whole where it is drawn: it is cut to what is drawn by a clip */
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * clip_path = NULL;
    r = gvec_lw_rect_path(ctx, c->cx0, c->cy0, c->cx1 - c->cx0, c->cy1 - c->cy0, &geometry, &clip_path);
    if (r != GVEC_OK) {
      return r;
    }
    if (geometry != GVEC_LW_GEOMETRY_PATH) {
      return gvec_lw_refuse(ctx, b->e, GVEC_LOWER_LIMIT, "the box of a mask border is past the range the waist can carry");
    }
    gvec_lw_node * clip;
    r = new_node(ctx, b->e, GVEC_LW_CLIP, GVEC_LW_ROLE_MASK_LAYER, salt + 1, &clip);
    if (r != GVEC_OK) {
      return r;
    }
    clip->map = *b->local;
    clip->resource = clip_path;
    clip->rule = GSHGEOM_FILL_NONZERO;
    gvec_lw_node * art = NULL;
    if (b->image.resource) {
      GSHSCENE_Paint paint;
      const GSHSCENE_Resource * path = NULL;
      bool image_ok;
      r = gvec_lw_image_paint(ctx, b->e, &b->image, &map, b->magnify, b->minify, &paint, &path, &image_ok);
      if (r != GVEC_OK || !image_ok) {
        return r;
      }
      r = new_node(ctx, b->e, GVEC_LW_FILL, GVEC_LW_ROLE_MASK_TILE, salt, &art);
      if (r != GVEC_OK) {
        return r;
      }
      art->map = *b->local;
      art->resource = path;
      art->rule = GSHGEOM_FILL_NONZERO;
      art->paint = paint;
      art->content = true;
    }
    else if (b->image.svg) {
      r = gvec_lw_svg_image_node(ctx, b->e, b->local, &b->image, &map, &art);
      if (r != GVEC_OK) {
        return r;
      }
    }
    if (!art) {
      return GVEC_OK;
    }
    r = gvec_lw_attach(ctx, clip, art);
    if (r != GVEC_OK) {
      return r;
    }
    node = clip;
  }
  return gvec_lw_attach(ctx, b->holder, node);
}

/* The cells along one axis of a region `length` long, for a slice of `slice` pixels shown `scale` times as long: `[from, to)` (the whole cell, which a
 * repeated one is cut off from) in region coordinates. Returns how many. */
static size_t border_axis(int mode, double slice, double length, double scale, double * from, double * to, size_t capacity) {
  const double tile = slice * scale;
  size_t n = 0;
  if (!(length > 0) || !(slice > 0)) {
    return 0;
  }
  if (mode == 0 || !(tile > 0)) {
    from[0] = 0;
    to[0] = length;
    return 1;
  }
  if (mode == 2) {
    double count = gmath_floor(length / tile + 0.5);
    count = count < 1 ? 1 : count;
    if (count > (double) capacity) {
      return capacity + 1; /* more cells than the caller keeps: it refuses */
    }
    const double each = length / count;
    for (size_t k = 0; k < (size_t) count && n < capacity; k++, n++) {
      from[n] = (double) k * each;
      to[n] = (double) (k + 1) * each;
    }
    return n;
  }
  if (mode == 3) {
    const double count = gmath_floor(length / tile);
    if (!(count >= 1)) {
      return 0;
    }
    if (count > (double) capacity) {
      return capacity + 1;
    }
    const double gap = (length - count * tile) / (count + 1);
    for (size_t k = 0; k < (size_t) count && n < capacity; k++, n++) {
      from[n] = gap + (double) k * (tile + gap);
      to[n] = from[n] + tile;
    }
    return n;
  }
  /* repeat: the tiles are centered, and the ones at the ends are cut off */
  const double count = gmath_ceil(length / tile);
  if (count > (double) capacity) {
    return capacity + 1;
  }
  const double start = (length - count * tile) / 2;
  for (size_t k = 0; k < (size_t) count && n < capacity; k++, n++) {
    from[n] = start + (double) k * tile;
    to[n] = from[n] + tile;
  }
  return n;
}

/*
 * The art of a mask border: the image (a gradient is as large as the border image area, which is what an image with no size of its own is) cut into nine slices,
 * each shown in its place around the box, as CSS Backgrounds 3 (6.2) says a border image is. The widths of the borders are `mask-border-width`, whose `auto` is the
 * size of the slice (an image that has one) and whose numbers are of the width of a border, which an SVG element does not have.
 */
static GVEC_Result border_art(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * masked, const GSHGEOM_Map * local, const layer * l, size_t index, const GSHGEOM_Bounds * box,
    gvec_lw_node ** top, bool * ok) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(masked);
  *top = NULL;
  *ok = false;
  /* the border image area: the box, pushed out by the outset */
  const double x0 = box->min.x - l->outset[3], y0 = box->min.y - l->outset[0], x1 = box->max.x + l->outset[1], y1 = box->max.y + l->outset[2];
  const double area_w = x1 - x0, area_h = y1 - y0;
  if (!(area_w > 0) || !(area_h > 0)) {
    *ok = true;
    return GVEC_OK;
  }
  border_paint b;
  memset(&b, 0, sizeof b);
  b.ctx = ctx;
  b.masked = masked;
  b.e = e;
  b.local = local;
  b.index = index;
  b.gradient = !starts(trim(l->image), "url(");
  double iw = area_w, ih = area_h; /* no size of its own: the area's */
  bool intrinsic = false;
  GVEC_Result r;
  if (!b.gradient) {
    int state;
    r = load_source(ctx, e, l->image, &b.image, &iw, &ih, &state);
    if (r != GVEC_OK || state == 1) {
      return r;
    }
    if (state == 2) {
      *ok = true; /* an image that cannot be used covers nothing */
      return GVEC_OK;
    }
    intrinsic = iw > 0 && ih > 0; /* slices are in the pixels of the image, whatever its density */
    if (!intrinsic) {
      iw = area_w;
      ih = area_h;
    }
    bool usable;
    if (b.image.resource) {
      r = gvec_lw_image_filters(ctx, masked, &b.magnify, &b.minify, &usable);
      if (r != GVEC_OK || !usable) {
        return r;
      }
    }
  }
  else {
    bool read;
    r = make_gradient(ctx, masked, e, l->image, iw, ih, &b.base, &b.stops, &read);
    if (r != GVEC_OK) {
      return r;
    }
    if (!read) {
      return refuse_layer(ctx, e, "a gradient of a mask border that this does not read (a color it does not know, or a gradient this grammar does not say) is not lowered");
    }
  }
  /* the slices, in pixels of the image */
  double slice[4];
  for (int k = 0; k < 4; k++) {
    const double basis = (k == 0 || k == 2) ? ih : iw;
    double v = l->slice_percent[k] ? l->slice[k] * basis / 100.0 : l->slice[k];
    slice[k] = v > basis ? basis : v;
  }
  /* the widths of the borders, in the element's pixels: auto is the slice (when the image has a size), a length is itself, a percentage is of the area */
  double wide[4];
  for (int k = 0; k < 4; k++) {
    const amount a = l->width[k];
    const double basis = (k == 0 || k == 2) ? area_h : area_w;
    wide[k] = a.kind == 0 ? (intrinsic ? slice[k] / (l->resolution > 0 ? l->resolution : 1) : 0) : a.kind == 1 ? a.v : a.kind == 2 ? a.v * basis / 100.0 : 0;
  }
  /* sides that together are more than the area are shrunk, together, to fit it */
  double shrink = 1;
  if (wide[1] + wide[3] > area_w && wide[1] + wide[3] > 0) {
    shrink = area_w / (wide[1] + wide[3]);
  }
  if (wide[0] + wide[2] > area_h && wide[0] + wide[2] > 0 && area_h / (wide[0] + wide[2]) < shrink) {
    shrink = area_h / (wide[0] + wide[2]);
  }
  for (int k = 0; k < 4; k++) {
    wide[k] *= shrink;
  }
  /* the grid: image columns (left slice, middle, right slice) and rows, and where each goes */
  const double src_x[4] = {0, slice[3], iw - slice[1], iw}, src_y[4] = {0, slice[0], ih - slice[2], ih};
  const double dst_x[4] = {x0, x0 + wide[3], x1 - wide[1], x1}, dst_y[4] = {y0, y0 + wide[0], y1 - wide[2], y1};
  gvec_lw_node * holder;
  r = new_node(ctx, e, GVEC_LW_GROUP, GVEC_LW_ROLE_MASK_LAYER, (uint64_t) index * 2 + 5000, &holder);
  if (r != GVEC_OK) {
    return r;
  }
  holder->opacity = 1;
  holder->isolate = true;
  b.holder = holder;
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      if (row == 1 && col == 1 && !l->slice_fill) {
        continue;
      }
      const double sw = src_x[col + 1] - src_x[col], sh = src_y[row + 1] - src_y[row];
      const double dw = dst_x[col + 1] - dst_x[col], dh = dst_y[row + 1] - dst_y[row];
      if (!(sw > 0) || !(sh > 0) || !(dw > 0) || !(dh > 0)) {
        continue;
      }
      /* the corners are stretched; an edge repeats along its own side, and is scaled across it to the border; the middle is scaled by both */
      const bool along_x = col == 1, along_y = row == 1;
      const int mode_x = along_x ? l->border_repeat[0] : 0, mode_y = along_y ? l->border_repeat[1] : 0;
      double scale_x = dw / sw, scale_y = dh / sh;
      if (along_x && mode_x != 0 && !(along_y && mode_y != 0)) {
        scale_x = scale_y; /* repeated along x at the scale across it */
      }
      else if (along_y && mode_y != 0 && !(along_x && mode_x != 0)) {
        scale_y = scale_x;
      }
      else if (along_x && mode_x != 0 && along_y && mode_y != 0) {
        scale_x = scale_y = (scale_x < scale_y ? scale_x : scale_y);
      }
      double fx[256], tx[256], fy[256], ty[256];
      const size_t nx = border_axis(mode_x, sw, dw, scale_x, fx, tx, 256), ny = border_axis(mode_y, sh, dh, scale_y, fy, ty, 256);
      if (nx > 256 || ny > 256) {
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "a mask border is drawn in more pieces than the lowering draws");
      }
      for (size_t j = 0; j < ny; j++) {
        for (size_t i = 0; i < nx; i++) {
          border_cell cell;
          cell.sx = src_x[col];
          cell.sy = src_y[row];
          cell.sw = sw;
          cell.sh = sh;
          cell.dx = dst_x[col] + fx[i];
          cell.dy = dst_y[row] + fy[j];
          cell.dw = tx[i] - fx[i];
          cell.dh = ty[j] - fy[j];
          cell.cx0 = dst_x[col] + (fx[i] > 0 ? fx[i] : 0);
          cell.cy0 = dst_y[row] + (fy[j] > 0 ? fy[j] : 0);
          cell.cx1 = dst_x[col] + (tx[i] < dw ? tx[i] : dw);
          cell.cy1 = dst_y[row] + (ty[j] < dh ? ty[j] : dh);
          r = border_cell_node(&b, &cell);
          if (r != GVEC_OK) {
            return r;
          }
        }
      }
    }
  }
  *top = holder->content ? holder : NULL;
  *ok = true;
  return GVEC_OK;
}

/*
 * `image-set()` (CSS Images 4, 3.3) and `image()`: the `url()` of the image to use, and how many of its pixels make a CSS pixel. A target has one pixel for each CSS pixel, so
 * the candidate is the one of the least resolution that is at least 1x, or else the greatest; a candidate is `<url> | <string>`, then a resolution (`2x`, `192dpi`, `1dppx`), and
 * a `type()` the loader decides by the bytes, not the label. `image()` is its first argument. *ok is false for anything this does not read.
 */
static GVEC_Result image_function(gvec_lw_ctx * ctx, span text, span * out, double * resolution, bool * ok) {
  *ok = false;
  *resolution = 1;
  const bool set = starts(text, "image-set(") || starts(text, "-webkit-image-set(");
  const size_t open = starts(text, "-webkit-image-set(") ? 18 : starts(text, "image-set(") ? 10 : 6;
  if ((!set && !starts(text, "image(")) || text.n < open + 1 || text.p[text.n - 1] != ')') {
    return GVEC_OK;
  }
  span args[17];
  const size_t count = split(trim((span) {text.p + open, text.n - open - 1}), false, args, 17);
  if (count == 0 || count > 16 || (!set && count != 1 && count != 2)) {
    return GVEC_OK;
  }
  double best = -1;
  span best_source = {NULL, 0};
  for (size_t i = 0; i < (set ? count : 1); i++) {
    span tokens[4];
    const size_t n = split(args[i], true, tokens, 4);
    if (n == 0 || n > 3) {
      return GVEC_OK;
    }
    double resolution_of = 1;
    for (size_t k = 1; k < n; k++) {
      double v;
      span unit;
      if (starts(tokens[k], "type(")) {
        continue;
      }
      if (!set || !number_unit(tokens[k], &v, &unit) || !(v > 0)) {
        return GVEC_OK;
      }
      resolution_of = is(unit, "x") || is(unit, "dppx") ? v : is(unit, "dpi") ? v / 96.0 : is(unit, "dpcm") ? v * 2.54 / 96.0 : 0;
      if (!(resolution_of > 0)) {
        return GVEC_OK;
      }
    }
    const bool better = best < 0 || (resolution_of >= 1 ? (best < 1 || resolution_of < best) : (best < 1 && resolution_of > best));
    if (better) {
      best = resolution_of;
      best_source = tokens[0];
    }
  }
  span source = best_source;
  if (source.n >= 2 && (source.p[0] == '"' || source.p[0] == '\'') && source.p[source.n - 1] == source.p[0]) {
    /* a string is the url of an image: `url("...")` */
    char * made = gvec_arena_alloc(&ctx->arena, source.n + 6, 1);
    if (!made) {
      return GVEC_ERR_OOM;
    }
    memcpy(made, "url(", 4);
    memcpy(made + 4, source.p, source.n);
    made[4 + source.n] = ')';
    made[5 + source.n] = 0;
    source = (span) {made, source.n + 5};
  }
  if (!starts(source, "url(")) {
    return GVEC_OK;
  }
  *out = source;
  *resolution = best;
  *ok = true;
  return GVEC_OK;
}

static GVEC_Result parse_layers(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * masked, layer * layers, size_t * count, bool * ok) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(masked);
  *ok = false;
  span images, modes, repeats, positions, sizes, composites, origins, clips, border;
  GVEC_Result r = text_of(ctx, masked, GVEC_SVG_PROP_MASK, &images);
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_MODE, &modes);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_REPEAT, &repeats);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_POSITION, &positions);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_SIZE, &sizes);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_COMPOSITE, &composites);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_ORIGIN, &origins);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_CLIP, &clips);
  }
  if (r == GVEC_OK) {
    r = text_of(ctx, masked, GVEC_SVG_PROP_MASK_BORDER_SOURCE, &border);
  }
  if (r != GVEC_OK) {
    return r;
  }
  layer border_layer;
  bool has_border = false;
  if (border.n && !is(trim(border), "none")) {
    r = parse_border(ctx, masked, trim(border), &border_layer, ok);
    if (r != GVEC_OK || !*ok) {
      return r;
    }
    *ok = false;
    has_border = true;
  }
  span image_list[LAYERS_MAX + 1], mode_list[LAYERS_MAX + 1], repeat_list[LAYERS_MAX + 1], position_list[LAYERS_MAX + 1], size_list[LAYERS_MAX + 1],
      composite_list[LAYERS_MAX + 1], origin_list[LAYERS_MAX + 1], clip_list[LAYERS_MAX + 1];
  const size_t n = split(trim(images), false, image_list, LAYERS_MAX + 1);
  if (n == 0 || n > LAYERS_MAX) {
    return n ? gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask has more layers than the lowering keeps") : GVEC_OK;
  }
  size_t nm = split(trim(modes), false, mode_list, LAYERS_MAX + 1), nr = split(trim(repeats), false, repeat_list, LAYERS_MAX + 1),
         np = split(trim(positions), false, position_list, LAYERS_MAX + 1), ns = split(trim(sizes), false, size_list, LAYERS_MAX + 1),
         nc = split(trim(composites), false, composite_list, LAYERS_MAX + 1), no = split(trim(origins), false, origin_list, LAYERS_MAX + 1),
         nk = split(trim(clips), false, clip_list, LAYERS_MAX + 1);
  nm = nm > LAYERS_MAX ? LAYERS_MAX : nm;
  nr = nr > LAYERS_MAX ? LAYERS_MAX : nr;
  np = np > LAYERS_MAX ? LAYERS_MAX : np;
  ns = ns > LAYERS_MAX ? LAYERS_MAX : ns;
  nc = nc > LAYERS_MAX ? LAYERS_MAX : nc;
  no = no > LAYERS_MAX ? LAYERS_MAX : no;
  nk = nk > LAYERS_MAX ? LAYERS_MAX : nk;
  for (size_t i = 0; i < n; i++) {
    layer * l = &layers[i];
    memset(l, 0, sizeof *l);
    span image = image_list[i];
    l->image = image;
    if (is(image, "none")) {
      l->kind = LAYER_NONE;
    }
    else if (starts(image, "image-set(") || starts(image, "-webkit-image-set(") || starts(image, "image(")) {
      bool picked;
      GVEC_Result pr = image_function(ctx, image, &l->image, &l->resolution, &picked);
      if (pr != GVEC_OK) {
        return pr;
      }
      if (!picked) {
        return refuse_layer(ctx, e, "an image-set() or image() of a mask that this does not read is not lowered");
      }
      l->kind = LAYER_IMAGE;
    }
    else if (starts(image, "url(")) {
      l->kind = local_url(image) ? LAYER_ELEMENT : LAYER_IMAGE;
    }
    else if (starts(image, "linear-gradient(") || starts(image, "radial-gradient(") || starts(image, "repeating-linear-gradient(") ||
        starts(image, "repeating-radial-gradient(") || starts(image, "conic-gradient(") || starts(image, "repeating-conic-gradient(")) {
      l->kind = LAYER_GRADIENT;
    }
    else {
      return refuse_layer(ctx, e, "a mask image that is not a url, a gradient or none is not lowered");
    }
    span mode = trim(entry(mode_list, nm, i));
    l->named_mode = is(mode, "alpha") ? 1 : is(mode, "luminance") ? 2 : 0;
    l->mode = l->named_mode == 1 ? 1 : l->named_mode == 2 ? 0 : l->kind == LAYER_ELEMENT ? 0 : 1;
    span rep[2];
    span rp = trim(entry(repeat_list, nr, i));
    size_t rn = split(rp, true, rep, 2);
    for (size_t a = 0; a < 2; a++) {
      span w = rn == 1 ? rep[0] : a < rn ? rep[a] : rep[0];
      l->repeat[a] = is(w, "repeat") ? 0 : is(w, "no-repeat") ? 1 : is(w, "space") ? 2 : is(w, "round") ? 3 : -1;
      if (l->repeat[a] < 0) {
        return refuse_layer(ctx, e, "a mask-repeat that this does not read is not lowered");
      }
    }
    span pos[5];
    size_t pn = split(trim(entry(position_list, np, i)), true, pos, 5);
    if (pn == 0 || pn > 4 || !position_value(pos, pn, l->position)) {
      return refuse_layer(ctx, e, "a mask-position that is not a CSS position is not lowered");
    }
    span size = trim(entry(size_list, ns, i));
    if (is(size, "contain")) {
      l->size_keyword = 1;
    }
    else if (is(size, "cover")) {
      l->size_keyword = 2;
    }
    else {
      span parts[3];
      size_t sn = split(size, true, parts, 3);
      if (sn == 0 || sn > 2) {
        return refuse_layer(ctx, e, "a mask-size that this does not read is not lowered");
      }
      for (size_t a = 0; a < 2; a++) {
        span part = a < sn ? parts[a] : (span) {"auto", 4};
        if (is(part, "auto")) {
          l->size[a] = (amount) {0, 0, 0};
        }
        else {
          double v;
          bool percent;
          if (!length_or_percent(part, &v, &percent) || v < 0) {
            return refuse_layer(ctx, e, "a mask-size that is not a length, a percentage or auto is not lowered");
          }
          l->size[a] = (amount) {percent ? 2 : 1, v, 0};
        }
      }
    }
    l->origin = box_keyword(trim(entry(origin_list, no, i)));
    l->clip = box_keyword(trim(entry(clip_list, nk, i)));
    if (l->origin < 0 || l->origin == 3 || l->clip < 0) {
      return refuse_layer(ctx, e, "a mask-origin or mask-clip that this does not read is not lowered");
    }
    span comp = trim(entry(composite_list, nc, i));
    l->composite = is(comp, "subtract") ? 1 : is(comp, "intersect") ? 2 : is(comp, "exclude") ? 3 : 0;
  }
  *count = n;
  if (has_border) {
    /* The mask border is the top layer, over the others (it is composited with them by `add`). */
    if (n >= LAYERS_MAX) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the mask has more layers than the lowering keeps");
    }
    for (size_t i = n; i > 0; i--) {
      layers[i] = layers[i - 1];
    }
    layers[0] = border_layer;
    *count = n + 1;
  }
  *ok = true;
  return GVEC_OK;
}

GVEC_Result gvec_lw_mask_layers(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * masked, const GSHGEOM_Map * local, gvec_lw_mask_state * state, const GSHSCENE_Resource ** out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(masked);
  *state = GVEC_LW_MASK_REFUSED;
  *out = NULL;
  if (ctx->picture_depth + 1 > ctx->caps.picture_depth || ctx->levels + 4 > ctx->caps.node_depth) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the masks nest deeper than the scene's limits allow");
  }
  layer layers[LAYERS_MAX];
  size_t count = 0;
  bool ok;
  GVEC_Result r = parse_layers(ctx, masked, layers, &count, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  /* The boxes the layers use, read once for each kind. */
  GSHGEOM_Bounds boxes[3];
  bool known[3] = {false, false, false}, have[3] = {false, false, false};
  size_t live = 0;
  for (size_t i = 0; i < count; i++) {
    if (layers[i].kind == LAYER_NONE) {
      continue;
    }
    live++;
    const int need[2] = {layers[i].origin, layers[i].clip == 3 ? 1 : layers[i].clip};
    for (size_t k = 0; k < 2; k++) {
      const int which = need[k];
      if (!have[which]) {
        r = box_of(ctx, masked, which, &boxes[which], &known[which]);
        if (r != GVEC_OK) {
          return r;
        }
        have[which] = true;
      }
      if (!known[which]) {
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_MASK, "a mask in a box of an element that holds more than shapes is not lowered yet");
      }
    }
  }
  if (live == 0) {
    *state = GVEC_LW_MASK_NONE;
    return GVEC_OK;
  }
  /* The layers, the last first: each is a group that composites it with what is under it. */
  const bool single = live == 1;
  gvec_lw_node * accumulated = NULL;
  gvec_lw_node * holder_content = NULL;
  uint32_t final_mode = 1;
  for (size_t i = count; i-- > 0;) {
    const layer * l = &layers[i];
    if (l->kind == LAYER_NONE) {
      continue;
    }
    const GSHGEOM_Bounds * origin_box = &boxes[l->origin];
    const GSHGEOM_Bounds * clip_box = &boxes[l->clip == 3 ? 1 : l->clip];
    if (origin_box->empty || clip_box->empty) {
      continue;
    }
    gvec_lw_node * art = NULL;
    if (l->kind == LAYER_ELEMENT) {
      /* A `mask` element names a mask of its own: white where that mask lets through, under the layer's clip. */
      span text = trim(l->image);
      span inner = trim((span) {text.p + 4, text.n - 5});
      if (inner.n >= 2 && (inner.p[0] == '"' || inner.p[0] == '\'')) {
        inner.p++;
        inner.n -= 2;
      }
      if (!(inner.n > 1 && inner.p[0] == '#')) {
        return refuse_layer(ctx, e, "a mask that names an element of another document, in a list of masks, is not lowered");
      }
      char * id = gvec_arena_alloc(&ctx->arena, inner.n, 1);
      if (!id) {
        return GVEC_ERR_OOM;
      }
      memcpy(id, inner.p + 1, inner.n - 1);
      id[inner.n - 1] = 0;
      const GVEC_Svg_Element * target = gvec_svg_find_by_id(ctx->document, id);
      if (!target || gvec_svg_element_kind(target) != GVEC_SVG_KIND_MASK || gvec_lw_mask_is_open(ctx, target)) {
        continue; /* names nothing a mask can be made of, or a mask that is being made (a loop): the layer is not there */
      }
      gvec_lw_mask_state inner_state;
      const GSHSCENE_Resource * element_mask = NULL;
      r = gvec_lw_mask_of_element(ctx, masked, local, target, l->named_mode == 1 ? GSHSCENE_MASK_ALPHA : l->named_mode == 2 ? GSHSCENE_MASK_LUMINANCE : 0, &inner_state, &element_mask);
      if (r != GVEC_OK) {
        return r;
      }
      if (inner_state == GVEC_LW_MASK_ALL) {
        continue;
      }
      if (inner_state != GVEC_LW_MASK_NODE) {
        *state = inner_state;
        return GVEC_OK;
      }
      if (single) {
        *out = element_mask;
        *state = GVEC_LW_MASK_NODE;
        return GVEC_OK;
      }
      GSHSCENE_Paint white;
      memset(&white, 0, sizeof white);
      white.kind = GSHSCENE_PAINT_SOLID;
      white.rgba[0] = white.rgba[1] = white.rgba[2] = white.rgba[3] = 1.0;
      gvec_lw_node * fill;
      bool made;
      r = rect_node(ctx, e, local, clip_box->min.x, clip_box->min.y, clip_box->max.x - clip_box->min.x, clip_box->max.y - clip_box->min.y, &white, (uint64_t) i * 100003 + 7, &fill, &made);
      if (r != GVEC_OK || !made) {
        return r;
      }
      gvec_lw_node * group;
      r = new_node(ctx, e, GVEC_LW_GROUP, GVEC_LW_ROLE_MASK_LAYER, (uint64_t) i * 2, &group);
      if (r != GVEC_OK) {
        return r;
      }
      group->opacity = 1;
      group->mask = element_mask;
      r = gvec_lw_attach(ctx, group, fill);
      if (r != GVEC_OK) {
        return r;
      }
      art = group;
    }
    else {
      bool layer_ok;
      r = l->kind == LAYER_BORDER ? border_art(ctx, masked, local, l, i, origin_box, &art, &layer_ok)
                                  : layer_art(ctx, masked, local, l, i, origin_box, clip_box, &art, &layer_ok);
      if (r != GVEC_OK || !layer_ok) {
        return r;
      }
      if (!art) {
        continue;
      }
      if (single) {
        final_mode = l->mode ? GSHSCENE_MASK_ALPHA : GSHSCENE_MASK_LUMINANCE;
        accumulated = art;
        break;
      }
      if (!l->mode) {
        /* A luminance layer is made into the alpha its luminance is: white, masked by the layer. */
        const GSHSCENE_Resource * luminance = NULL;
        r = mask_of_art(ctx, e, art, GSHSCENE_MASK_LUMINANCE, (uint64_t) i * 2 + 1000, &luminance);
        if (r != GVEC_OK) {
          return r;
        }
        GSHSCENE_Paint white;
        memset(&white, 0, sizeof white);
        white.kind = GSHSCENE_PAINT_SOLID;
        white.rgba[0] = white.rgba[1] = white.rgba[2] = white.rgba[3] = 1.0;
        gvec_lw_node * fill;
        bool made;
        r = rect_node(ctx, e, local, clip_box->min.x, clip_box->min.y, clip_box->max.x - clip_box->min.x, clip_box->max.y - clip_box->min.y, &white, (uint64_t) i * 100003 + 9, &fill, &made);
        if (r != GVEC_OK || !made) {
          return r;
        }
        gvec_lw_node * group;
        r = new_node(ctx, e, GVEC_LW_GROUP, GVEC_LW_ROLE_MASK_LAYER, (uint64_t) i * 2 + 2000, &group);
        if (r != GVEC_OK) {
          return r;
        }
        group->opacity = 1;
        group->mask = luminance;
        r = gvec_lw_attach(ctx, group, fill);
        if (r != GVEC_OK) {
          return r;
        }
        art = group;
      }
    }
    /* The layer over what is under it. */
    if (!accumulated) {
      accumulated = art;
      holder_content = art;
      continue;
    }
    gvec_lw_node * over;
    r = new_node(ctx, e, GVEC_LW_GROUP, GVEC_LW_ROLE_MASK_LAYER, (uint64_t) i * 2 + 3000, &over);
    if (r != GVEC_OK) {
      return r;
    }
    over->opacity = 1;
    over->isolate = true;
    over->composite = l->composite == 1 ? GSHSCENE_COMPOSITE_SRC_OUT : l->composite == 2 ? GSHSCENE_COMPOSITE_SRC_IN : l->composite == 3 ? GSHSCENE_COMPOSITE_XOR : GSHSCENE_COMPOSITE_SRC_OVER;
    r = gvec_lw_attach(ctx, over, art);
    if (r != GVEC_OK) {
      return r;
    }
    /* Both are the children of one picture: the accumulated layers, then this one over them. */
    gvec_lw_node * together;
    r = new_node(ctx, e, GVEC_LW_GROUP, GVEC_LW_ROLE_MASK_LAYER, (uint64_t) i * 2 + 4000, &together);
    if (r != GVEC_OK) {
      return r;
    }
    together->opacity = 1;
    together->isolate = true;
    r = gvec_lw_attach(ctx, together, accumulated);
    if (r == GVEC_OK) {
      r = gvec_lw_attach(ctx, together, over);
    }
    if (r != GVEC_OK) {
      return r;
    }
    accumulated = together;
    holder_content = together;
  }
  (void) holder_content;
  if (!accumulated) {
    *state = GVEC_LW_MASK_ALL; /* Layers that cover nothing: nothing of the element is seen. */
    return GVEC_OK;
  }
  const GSHSCENE_Resource * resource = NULL;
  r = mask_of_art(ctx, e, accumulated, final_mode, 0, &resource);
  if (r != GVEC_OK) {
    return r;
  }
  *out = resource;
  *state = GVEC_LW_MASK_NODE;
  return GVEC_OK;
}
