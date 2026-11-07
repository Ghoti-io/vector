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
 * @file lower_basic_shape.c
 *
 * `clip-path` written as a basic shape (H11): `circle()`, `ellipse()`, `inset()`, `polygon()` and `path()`, with a reference box. The
 * value is read as text (the style layer keeps a basic shape as written), resolved against the reference box of the element in its
 * own space, and made an exact primitive or path of the waist: a circle is an ellipse, `inset()` a rounded rectangle whose corners
 * are scaled as CSS Backgrounds says when they would overlap, and `polygon()` and `path()` paths. The result is one clip region
 * in the element's space; what this cannot read is told apart as invalid (the property is then as if not specified, CSS) or valid
 * and refused (`rect()`, `xywh()`, `shape()`, an `em` length that needs the font).
 *
 * The reference boxes: `fill-box` is the bounds of the element's geometry; `stroke-box` those bounds taken through the waist's stroke
 * widened by half the stroke when the element has one (a rectangle, circle or ellipse; no other geometry is refused for a box it cannot name); `view-box` the nearest viewport, which must start at the origin. `border-box` and `margin-box`
 * are `stroke-box`, `content-box` and `padding-box` are `fill-box` (SVG 2), and `border-box` is the default. An element with no
 * geometry of its own (a group) has no box to name and is refused.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/primitive.h>
#include <ghoti.io/shape-scene/stroke.h>

#include "../svg_grammar.h"
#include "lower_internal.h"

typedef struct box {
  double x, y, w, h;
} box;

typedef struct cur {
  const char * p;
  const char * end;
} cur;

static void ws(cur * c) {
  while (c->p < c->end && gvec_svg_is_wsp((unsigned char) *c->p)) {
    c->p++;
  }
}

static bool eat(cur * c, char ch) {
  ws(c);
  if (c->p < c->end && *c->p == ch) {
    c->p++;
    return true;
  }
  return false;
}

static bool at_end(cur * c) {
  ws(c);
  return c->p == c->end;
}

static bool is_letter(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

/* A word of letters and hyphens, lowercased into `buf`; false (nothing consumed) when the next thing is not one. */
static bool word(cur * c, char * buf, size_t size) {
  ws(c);
  const char * q = c->p;
  size_t n = 0;
  while (q < c->end && (is_letter(*q) || *q == '-')) {
    if (n + 1 >= size) {
      return false;
    }
    buf[n++] = (char) ((*q >= 'A' && *q <= 'Z') ? *q + 32 : *q);
    q++;
  }
  if (n == 0 || !is_letter(buf[0])) {
    return false;
  }
  buf[n] = 0;
  c->p = q;
  return true;
}

/* Whether the next thing is a word (not consumed). */
static bool next_is_word(cur * c) {
  ws(c);
  return c->p < c->end && is_letter(*c->p);
}

typedef enum lp_status { LP_OK, LP_NO, LP_REFUSE } lp_status;

/* One length-percentage with no function around it; a percentage is of `basis`. */
static lp_status lp_term(cur * c, double basis, double * out) {
  ws(c);
  const char * q = c->p;
  while (q < c->end && !gvec_svg_is_wsp((unsigned char) *q) && *q != ',' && *q != '/' && *q != ')' && *q != '(') {
    q++;
  }
  if (q == c->p) {
    return LP_NO;
  }
  GVEC_Svg_Length length;
  gvec_svg_parse_error err;
  if (gvec_svg_parse_length(c->p, (size_t) (q - c->p), &length, &err) != GVEC_SVG_PARSE_OK) {
    return LP_NO;
  }
  if (length.unit == GVEC_SVG_UNIT_NONE && length.value != 0) {
    return LP_NO; /* A bare number is a length only when it is zero. */
  }
  if (length.unit == GVEC_SVG_UNIT_PERCENT) {
    *out = length.value * basis / 100.0;
  }
  else if (!gvec_lw_absolute_length(&length, out)) {
    return LP_REFUSE;
  }
  c->p = q;
  return LP_OK;
}

/* A length-percentage; a percentage is of `basis`. The CSS engine writes a position such as `right 10px` as `calc(100% - 10px)`, so
 * `calc()` of lengths and percentages joined by `+` and `-` is read too. LP_NO leaves the cursor; LP_REFUSE is a valid length this
 * does not resolve. */
static lp_status lp(cur * c, double basis, double * out) {
  ws(c);
  static const char calc[] = "calc(";
  if ((size_t) (c->end - c->p) < sizeof calc - 1 || strncmp(c->p, calc, sizeof calc - 1)) {
    return lp_term(c, basis, out);
  }
  cur save = *c;
  c->p += sizeof calc - 1;
  double sum;
  lp_status s = lp_term(c, basis, &sum);
  while (s == LP_OK) {
    ws(c);
    if (c->p < c->end && *c->p == ')') {
      c->p++;
      *out = sum;
      return LP_OK;
    }
    if (c->p >= c->end || (*c->p != '+' && *c->p != '-')) {
      s = LP_NO;
      break;
    }
    char op = *c->p++;
    double term;
    s = lp_term(c, basis, &term);
    if (s == LP_OK) {
      sum = op == '+' ? sum + term : sum - term;
    }
  }
  if (s != LP_REFUSE) {
    *c = save;
  }
  return s;
}

typedef enum parse_status { PARSE_OK, PARSE_INVALID, PARSE_REFUSED } parse_status;

/* `<position>`: one to four values, as CSS Values gives them; the result is the point in user space. */
static parse_status position(cur * c, const box * b, double * px, double * py) {
  double x = b->x + b->w / 2, y = b->y + b->h / 2;
  double v1, v2;
  /* Without a keyword first: a length-percentage for x, then a length-percentage or a vertical keyword for y. */
  cur save = *c;
  lp_status s1 = lp(c, b->w, &v1);
  if (s1 == LP_REFUSE) {
    return PARSE_REFUSED;
  }
  if (s1 == LP_OK) {
    x = b->x + v1;
    lp_status s2 = lp(c, b->h, &v2);
    if (s2 == LP_REFUSE) {
      return PARSE_REFUSED;
    }
    if (s2 == LP_OK) {
      y = b->y + v2;
    }
    else if (next_is_word(c)) {
      cur before = *c;
      char w[16];
      if (!word(c, w, sizeof w)) {
        return PARSE_INVALID;
      }
      if (!strcmp(w, "top")) {
        y = b->y;
      }
      else if (!strcmp(w, "bottom")) {
        y = b->y + b->h;
      }
      else if (strcmp(w, "center")) {
        *c = before; /* Not a keyword of the position: what follows it. */
      }
    }
    *px = x;
    *py = y;
    return PARSE_OK;
  }
  *c = save;
  /* Keywords, each with an optional offset: `left 10px top 20px`, `right`, `top`, `center`. */
  bool have_x = false, have_y = false;
  int centers = 0, groups = 0;
  while (groups < 2 && next_is_word(c)) {
    char w[16];
    cur before = *c;
    if (!word(c, w, sizeof w)) {
      return PARSE_INVALID;
    }
    bool left = !strcmp(w, "left"), right = !strcmp(w, "right"), top = !strcmp(w, "top"), bottom = !strcmp(w, "bottom");
    if (!strcmp(w, "center")) {
      centers++;
      groups++;
      continue;
    }
    if (!left && !right && !top && !bottom) {
      *c = before; /* Not a keyword of the position: what follows it. */
      break;
    }
    bool horizontal = left || right;
    double offset = 0;
    lp_status so = lp(c, horizontal ? b->w : b->h, &offset);
    if (so == LP_REFUSE) {
      return PARSE_REFUSED;
    }
    if (so != LP_OK) {
      offset = 0;
    }
    if (horizontal) {
      if (have_x) {
        return PARSE_INVALID;
      }
      x = left ? b->x + offset : b->x + b->w - offset;
      have_x = true;
    }
    else {
      if (have_y) {
        return PARSE_INVALID;
      }
      y = top ? b->y + offset : b->y + b->h - offset;
      have_y = true;
    }
    groups++;
  }
  if (groups == 0 || centers + (have_x ? 1 : 0) + (have_y ? 1 : 0) > 2) {
    return PARSE_INVALID;
  }
  *px = x;
  *py = y;
  return PARSE_OK;
}

typedef enum side_mode { SIDE_NONE, SIDE_CLOSEST, SIDE_FARTHEST } side_mode;

static double min2(double a, double b) {
  return a < b ? a : b;
}

static double max2(double a, double b) {
  return a > b ? a : b;
}

static double abs1(double a) {
  return a < 0 ? -a : a;
}

typedef struct shape {
  GSHSCENE_Primitive primitive;
  bool is_primitive;
  bool empty;                      ///< The region has no area: nothing gets through.
  const GVEC_Svg_Path_Segment * segments;
  size_t segment_count;
  double * points;
  size_t point_count;
  GSHGEOM_Fill_Rule rule;
  double ox, oy;                   ///< Where a `path()` is placed: the reference box's origin.
} shape;

static parse_status circle(cur * c, const box * b, shape * out) {
  double basis = gmath_sqrt((b->w * b->w + b->h * b->h) / 2.0);
  side_mode mode = SIDE_CLOSEST;
  double radius = 0;
  if (next_is_word(c)) {
    cur save = *c;
    char w[16];
    if (!word(c, w, sizeof w)) {
      return PARSE_INVALID;
    }
    if (!strcmp(w, "closest-side")) {
      mode = SIDE_CLOSEST;
    }
    else if (!strcmp(w, "farthest-side")) {
      mode = SIDE_FARTHEST;
    }
    else {
      *c = save; /* `at`, or something invalid that the end test catches. */
    }
  }
  else if (!at_end(c)) {
    lp_status s = lp(c, basis, &radius);
    if (s == LP_REFUSE) {
      return PARSE_REFUSED;
    }
    if (s == LP_OK) {
      if (radius < 0) {
        return PARSE_INVALID;
      }
      mode = SIDE_NONE;
    }
  }
  double cx = b->x + b->w / 2, cy = b->y + b->h / 2;
  if (next_is_word(c)) {
    cur save = *c;
    char w[16];
    if (word(c, w, sizeof w) && !strcmp(w, "at")) {
      parse_status p = position(c, b, &cx, &cy);
      if (p != PARSE_OK) {
        return p;
      }
    }
    else {
      *c = save;
      return PARSE_INVALID;
    }
  }
  if (!at_end(c)) {
    return PARSE_INVALID;
  }
  if (mode != SIDE_NONE) {
    double l = abs1(cx - b->x), r = abs1(b->x + b->w - cx), t = abs1(cy - b->y), bt = abs1(b->y + b->h - cy);
    radius = mode == SIDE_CLOSEST ? min2(min2(l, r), min2(t, bt)) : max2(max2(l, r), max2(t, bt));
  }
  if (!(radius > 0)) {
    out->empty = true;
    return PARSE_OK;
  }
  memset(&out->primitive, 0, sizeof out->primitive);
  out->primitive.kind = GSHSCENE_PRIMITIVE_ELLIPSE;
  out->primitive.cx = cx;
  out->primitive.cy = cy;
  out->primitive.rx = radius;
  out->primitive.ry = radius;
  out->is_primitive = true;
  return PARSE_OK;
}

/* One radius of ellipse(): a length-percentage or a side keyword; `*present` is false when the next thing is neither. */
static parse_status radius_of(cur * c, double basis, side_mode * mode, double * r, bool * present) {
  *present = false;
  if (next_is_word(c)) {
    cur save = *c;
    char w[16];
    if (!word(c, w, sizeof w)) {
      return PARSE_INVALID;
    }
    if (!strcmp(w, "closest-side") || !strcmp(w, "farthest-side")) {
      *mode = !strcmp(w, "closest-side") ? SIDE_CLOSEST : SIDE_FARTHEST;
      *present = true;
      return PARSE_OK;
    }
    *c = save;
    return PARSE_OK;
  }
  if (at_end(c)) {
    return PARSE_OK;
  }
  lp_status s = lp(c, basis, r);
  if (s == LP_REFUSE) {
    return PARSE_REFUSED;
  }
  if (s == LP_OK) {
    if (*r < 0) {
      return PARSE_INVALID;
    }
    *mode = SIDE_NONE;
    *present = true;
  }
  return PARSE_OK;
}

static parse_status ellipse(cur * c, const box * b, shape * out) {
  side_mode mx = SIDE_CLOSEST, my = SIDE_CLOSEST;
  double rx = 0, ry = 0;
  bool px, py;
  parse_status ps = radius_of(c, b->w, &mx, &rx, &px);
  if (ps != PARSE_OK) {
    return ps;
  }
  if (px) {
    ps = radius_of(c, b->h, &my, &ry, &py);
    if (ps != PARSE_OK) {
      return ps;
    }
    if (!py) {
      return PARSE_INVALID; /* One radius is not a form of ellipse(). */
    }
  }
  double cx = b->x + b->w / 2, cy = b->y + b->h / 2;
  if (next_is_word(c)) {
    char w[16];
    if (!word(c, w, sizeof w) || strcmp(w, "at")) {
      return PARSE_INVALID;
    }
    ps = position(c, b, &cx, &cy);
    if (ps != PARSE_OK) {
      return ps;
    }
  }
  if (!at_end(c)) {
    return PARSE_INVALID;
  }
  double l = abs1(cx - b->x), r = abs1(b->x + b->w - cx), t = abs1(cy - b->y), bt = abs1(b->y + b->h - cy);
  if (mx == SIDE_CLOSEST) {
    rx = min2(l, r);
  }
  else if (mx == SIDE_FARTHEST) {
    rx = max2(l, r);
  }
  if (my == SIDE_CLOSEST) {
    ry = min2(t, bt);
  }
  else if (my == SIDE_FARTHEST) {
    ry = max2(t, bt);
  }
  if (!(rx > 0) || !(ry > 0)) {
    out->empty = true;
    return PARSE_OK;
  }
  memset(&out->primitive, 0, sizeof out->primitive);
  out->primitive.kind = GSHSCENE_PRIMITIVE_ELLIPSE;
  out->primitive.cx = cx;
  out->primitive.cy = cy;
  out->primitive.rx = rx;
  out->primitive.ry = ry;
  out->is_primitive = true;
  return PARSE_OK;
}

/* One to four length-percentages of the shorthand forms of CSS (top, right, bottom, left; or the corners): returns the count. */
static parse_status shorthand(cur * c, double basis_a, double basis_b, double v[4], int * count, bool nonneg) {
  *count = 0;
  while (*count < 4) {
    /* The first and third values are along the first basis, the second and fourth along the second. */
    double basis = (*count % 2 == 0) ? basis_a : basis_b;
    cur save = *c;
    lp_status s = lp(c, basis, &v[*count]);
    if (s == LP_REFUSE) {
      return PARSE_REFUSED;
    }
    if (s != LP_OK) {
      *c = save;
      break;
    }
    if (nonneg && v[*count] < 0) {
      return PARSE_INVALID;
    }
    (*count)++;
  }
  return *count == 0 ? PARSE_INVALID : PARSE_OK;
}

static void expand4(const double in[4], int count, double out[4]) {
  switch (count) {
  case 1:
    out[0] = out[1] = out[2] = out[3] = in[0];
    break;
  case 2:
    out[0] = out[2] = in[0];
    out[1] = out[3] = in[1];
    break;
  case 3:
    out[0] = in[0];
    out[1] = out[3] = in[1];
    out[2] = in[2];
    break;
  default:
    memcpy(out, in, 4 * sizeof(double));
    break;
  }
}

static parse_status inset(cur * c, const box * b, shape * out) {
  double raw[4], sides[4];
  int count;
  /* top, right, bottom, left: the first and third are heights, the second and fourth widths. */
  parse_status s = shorthand(c, b->h, b->w, raw, &count, false);
  if (s != PARSE_OK) {
    return s;
  }
  expand4(raw, count, sides);
  double hr[4] = {0, 0, 0, 0}, vr[4] = {0, 0, 0, 0};
  if (next_is_word(c)) {
    char w[16];
    if (!word(c, w, sizeof w) || strcmp(w, "round")) {
      return PARSE_INVALID;
    }
    double h_raw[4], v_raw[4];
    int hc, vc;
    /* Corner radii: top-left, top-right, bottom-right, bottom-left; the first set is horizontal, the second (after `/`) vertical. */
    cur radii = *c;
    s = shorthand(c, b->w, b->w, h_raw, &hc, true);
    if (s != PARSE_OK) {
      return s;
    }
    if (eat(c, '/')) {
      s = shorthand(c, b->h, b->h, v_raw, &vc, true);
    }
    else {
      /* Without `/` each corner is circular, and a percentage is of the height for the vertical radius: read the values again. */
      cur end = *c;
      s = shorthand(&radii, b->h, b->h, v_raw, &vc, true);
      *c = end;
    }
    if (s != PARSE_OK) {
      return s;
    }
    expand4(h_raw, hc, hr);
    expand4(v_raw, vc, vr);
  }
  if (!at_end(c)) {
    return PARSE_INVALID;
  }
  double x = b->x + sides[3], y = b->y + sides[0];
  double w = b->w - sides[3] - sides[1], h = b->h - sides[0] - sides[2];
  if (!(w > 0) || !(h > 0)) {
    out->empty = true;
    return PARSE_OK;
  }
  /* CSS Backgrounds 5.5: when two radii on one side add up to more than the side, all of them are scaled by one factor. */
  double f = 1;
  double top = hr[0] + hr[1], bottom = hr[3] + hr[2], left = vr[0] + vr[3], right = vr[1] + vr[2];
  if (top > w) {
    f = min2(f, w / top);
  }
  if (bottom > w) {
    f = min2(f, w / bottom);
  }
  if (left > h) {
    f = min2(f, h / left);
  }
  if (right > h) {
    f = min2(f, h / right);
  }
  memset(&out->primitive, 0, sizeof out->primitive);
  out->primitive.x = x;
  out->primitive.y = y;
  out->primitive.w = w;
  out->primitive.h = h;
  bool round_any = false;
  for (int i = 0; i < 4; i++) {
    out->primitive.r[i].rx = hr[i] * f;
    out->primitive.r[i].ry = vr[i] * f;
    round_any = round_any || (hr[i] > 0 && vr[i] > 0);
  }
  out->primitive.kind = round_any ? GSHSCENE_PRIMITIVE_ROUNDED_RECT : GSHSCENE_PRIMITIVE_RECT;
  out->is_primitive = true;
  return PARSE_OK;
}

static bool fill_rule_word(cur * c, GSHGEOM_Fill_Rule * rule) {
  cur save = *c;
  char w[16];
  if (next_is_word(c) && word(c, w, sizeof w)) {
    if (!strcmp(w, "nonzero") || !strcmp(w, "evenodd")) {
      if (eat(c, ',')) {
        *rule = !strcmp(w, "evenodd") ? GSHGEOM_FILL_EVEN_ODD : GSHGEOM_FILL_NONZERO;
        return true;
      }
    }
  }
  *c = save;
  return false;
}

static parse_status polygon(gvec_lw_ctx * ctx, cur * c, const box * b, shape * out) {
  out->rule = GSHGEOM_FILL_NONZERO;
  fill_rule_word(c, &out->rule);
  size_t capacity = 8, n = 0;
  double * pts = gvec_arena_array(&ctx->arena, capacity * 2, sizeof(double), sizeof(double));
  if (!pts) {
    return PARSE_REFUSED;
  }
  for (;;) {
    double x, y;
    lp_status sx = lp(c, b->w, &x);
    if (sx == LP_REFUSE) {
      return PARSE_REFUSED;
    }
    lp_status sy = sx == LP_OK ? lp(c, b->h, &y) : LP_NO;
    if (sy == LP_REFUSE) {
      return PARSE_REFUSED;
    }
    if (sx != LP_OK || sy != LP_OK) {
      return PARSE_INVALID;
    }
    if (n == capacity) {
      double * bigger = gvec_arena_array(&ctx->arena, capacity * 4, sizeof(double), sizeof(double));
      if (!bigger) {
        return PARSE_REFUSED;
      }
      memcpy(bigger, pts, n * 2 * sizeof(double));
      pts = bigger;
      capacity *= 2;
    }
    pts[2 * n] = b->x + x;
    pts[2 * n + 1] = b->y + y;
    n++;
    if (eat(c, ',')) {
      continue;
    }
    break;
  }
  if (!at_end(c)) {
    return PARSE_INVALID;
  }
  out->points = pts;
  out->point_count = n;
  return PARSE_OK;
}

static parse_status path_function(gvec_lw_ctx * ctx, cur * c, const box * b, shape * out) {
  out->rule = GSHGEOM_FILL_NONZERO;
  fill_rule_word(c, &out->rule);
  ws(c);
  if (c->p >= c->end || (*c->p != '"' && *c->p != '\'')) {
    return PARSE_INVALID;
  }
  char quote = *c->p++;
  const char * start = c->p;
  while (c->p < c->end && *c->p != quote) {
    if (*c->p == '\\') {
      return PARSE_REFUSED; /* An escape in the string is not read. */
    }
    c->p++;
  }
  if (c->p >= c->end) {
    return PARSE_INVALID;
  }
  size_t n = (size_t) (c->p - start);
  c->p++;
  if (!at_end(c)) {
    return PARSE_INVALID;
  }
  gvec_svg_parse_context pc;
  memset(&pc, 0, sizeof pc);
  pc.arena = &ctx->arena;
  pc.allocator = ctx->allocator;
  pc.max_list_items = ctx->document->limits.max_list_items;
  pc.max_path_segments = ctx->document->limits.max_path_segments;
  gvec_svg_path parsed;
  gvec_svg_parse_error err;
  gvec_svg_parse_status st = gvec_svg_parse_path(start, n, &pc, &parsed, &err);
  if (st == GVEC_SVG_PARSE_OOM) {
    return PARSE_REFUSED;
  }
  if (st == GVEC_SVG_PARSE_INVALID || parsed.segment_count == 0) {
    return PARSE_INVALID;
  }
  out->segments = parsed.segments;
  out->segment_count = parsed.segment_count;
  out->ox = b->x;
  out->oy = b->y;
  return PARSE_OK;
}

//-----------------------------------------------------------------------------
// The reference box
//-----------------------------------------------------------------------------

typedef enum box_kind { BOX_FILL, BOX_STROKE, BOX_VIEW } box_kind;

static bool is_geometry(GVEC_Svg_Kind kind) {
  return kind == GVEC_SVG_KIND_PATH || kind == GVEC_SVG_KIND_POLYGON || kind == GVEC_SVG_KIND_POLYLINE || kind == GVEC_SVG_KIND_LINE ||
      kind == GVEC_SVG_KIND_RECT || kind == GVEC_SVG_KIND_CIRCLE || kind == GVEC_SVG_KIND_ELLIPSE;
}

typedef enum box_status { BOX_OK, BOX_NONE, BOX_REFUSED, BOX_ERROR } box_status;

static box_status reference_box(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, box_kind kind, box * out, GVEC_Result * result) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *result = GVEC_OK;
  if (kind == BOX_VIEW) {
    for (const GVEC_Svg_Element * a = e; a; a = gvec_svg_element_parent(a)) {
      GVEC_Svg_Kind k = gvec_svg_element_kind(a);
      if (k == GVEC_SVG_KIND_SVG || k == GVEC_SVG_KIND_SYMBOL) {
        const GVEC_Svg_Attribute * vb = gvec_svg_element_find_attribute(a, "viewBox");
        GVEC_Svg_View_Box v;
        if (vb && gvec_svg_attribute_class(vb) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_view_box(vb, &v) && (v.x != 0 || v.y != 0)) {
          return BOX_REFUSED; /* The viewport's origin is not the user space's. */
        }
      }
    }
    double w, h;
    gvec_svg_instance_viewport(inst, &w, &h);
    out->x = out->y = 0;
    out->w = w;
    out->h = h;
    return BOX_OK;
  }
  if (!is_geometry(gvec_svg_element_kind(e))) {
    return BOX_REFUSED;
  }
  gvec_lw_geometry geometry;
  const GSHSCENE_Resource * resource = NULL;
  bool usable;
  GVEC_Result r = gvec_lw_centerline_of(ctx, inst, &geometry, &resource, &usable);
  if (r != GVEC_OK || !usable) {
    *result = r;
    return r != GVEC_OK ? BOX_ERROR : BOX_NONE;
  }
  GSHGEOM_Bounds bounds;
  if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
    return BOX_REFUSED;
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH || gshgeom_path_bounds(gshscene_resource_path(resource), &bounds) != GSHGEOM_OK || bounds.empty) {
    return BOX_NONE; /* The element draws nothing, so what clips it does not matter. */
  }
  if (kind == BOX_STROKE) {
    gvec_lw_stroke stroke;
    r = gvec_lw_stroke_of(ctx, inst, &stroke);
    if (r != GVEC_OK) {
      *result = r;
      return BOX_ERROR;
    }
    if (stroke.decision == GVEC_LW_PAINT_REFUSED) {
      return BOX_ERROR; /* Refused already, and reported. */
    }
    if (stroke.decision == GVEC_LW_PAINT_DRAW) {
      /* The stroke's box is the geometry's box widened by half the width on every side. That is exact for a rectangle (a miter, a
       * bevel or a round join all leave the sides where they are), a circle and an ellipse; for any other geometry a join or a cap
       * can reach further, and the box is not one this computes. */
      GVEC_Svg_Kind element_kind = gvec_svg_element_kind(e);
      GSHSCENE_Stroke_Style style;
      if ((element_kind != GVEC_SVG_KIND_RECT && element_kind != GVEC_SVG_KIND_CIRCLE && element_kind != GVEC_SVG_KIND_ELLIPSE) ||
          gshscene_resource_stroke_style(stroke.style, &style) != GSHSCENE_OK) {
        return BOX_REFUSED;
      }
      double half = style.width / 2;
      bounds.min.x -= half;
      bounds.min.y -= half;
      bounds.max.x += half;
      bounds.max.y += half;
    }
  }
  out->x = bounds.min.x;
  out->y = bounds.min.y;
  out->w = bounds.max.x - bounds.min.x;
  out->h = bounds.max.y - bounds.min.y;
  return BOX_OK;
}

//-----------------------------------------------------------------------------
// Entry
//-----------------------------------------------------------------------------

static bool box_word(const char * w, size_t n, box_kind * kind) {
  struct {
    const char * name;
    box_kind kind;
  } const words[] = {{"fill-box", BOX_FILL}, {"content-box", BOX_FILL}, {"padding-box", BOX_FILL}, {"stroke-box", BOX_STROKE},
      {"border-box", BOX_STROKE}, {"margin-box", BOX_STROKE}, {"view-box", BOX_VIEW}};
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    if (strlen(words[i].name) == n && !strncmp(words[i].name, w, n)) {
      *kind = words[i].kind;
      return true;
    }
  }
  return false;
}

static GVEC_Result refused(const char * why, gvec_lw_shape * out) {
  out->kind = GVEC_LW_SHAPE_REFUSED;
  out->why = why;
  return GVEC_OK;
}

GVEC_Result gvec_lw_basic_shape(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const char * text, gvec_lw_shape * out) {
  memset(out, 0, sizeof *out);
  out->kind = GVEC_LW_SHAPE_INVALID;
  cur c = {text, text + strlen(text)};
  box_kind bk = BOX_STROKE;
  bool have_box = false;
  const char * fn = NULL;
  size_t fn_length = 0;
  const char * args = NULL;
  size_t args_length = 0;
  /* The value is a shape function and a box keyword in either order. */
  while (!at_end(&c)) {
    const char * start = c.p;
    while (c.p < c.end && (is_letter(*c.p) || *c.p == '-')) {
      c.p++;
    }
    size_t n = (size_t) (c.p - start);
    if (n == 0) {
      return GVEC_OK; /* Not a shape: invalid, so as if not specified. */
    }
    if (c.p < c.end && *c.p == '(') {
      if (fn) {
        return GVEC_OK;
      }
      fn = start;
      fn_length = n;
      c.p++;
      args = c.p;
      char quote = 0;
      int depth = 0; /* `calc()` inside the arguments has parentheses of its own. */
      while (c.p < c.end && (quote || depth > 0 || *c.p != ')')) {
        if (quote) {
          quote = *c.p == quote ? 0 : quote;
        }
        else if (*c.p == '"' || *c.p == '\'') {
          quote = *c.p;
        }
        else if (*c.p == '(') {
          depth++;
        }
        else if (*c.p == ')') {
          depth--;
        }
        c.p++;
      }
      if (c.p >= c.end) {
        return GVEC_OK;
      }
      args_length = (size_t) (c.p - args);
      c.p++;
    }
    else {
      box_kind k;
      if (have_box || !box_word(start, n, &k)) {
        return GVEC_OK;
      }
      bk = k;
      have_box = true;
    }
  }
  if (!fn) {
    return GVEC_OK;
  }
  enum { F_CIRCLE, F_ELLIPSE, F_INSET, F_POLYGON, F_PATH } which;
  if (fn_length == 6 && !strncmp(fn, "circle", 6)) {
    which = F_CIRCLE;
  }
  else if (fn_length == 7 && !strncmp(fn, "ellipse", 7)) {
    which = F_ELLIPSE;
  }
  else if (fn_length == 5 && !strncmp(fn, "inset", 5)) {
    which = F_INSET;
  }
  else if (fn_length == 7 && !strncmp(fn, "polygon", 7)) {
    which = F_POLYGON;
  }
  else if (fn_length == 4 && !strncmp(fn, "path", 4)) {
    which = F_PATH;
  }
  else if ((fn_length == 4 && (!strncmp(fn, "rect", 4) || !strncmp(fn, "xywh", 4))) || (fn_length == 5 && !strncmp(fn, "shape", 5))) {
    return refused("a clip-path of rect(), xywh() or shape() is not lowered", out);
  }
  else {
    return GVEC_OK;
  }
  box ref;
  GVEC_Result result;
  box_status bs = reference_box(ctx, inst, bk, &ref, &result);
  if (bs == BOX_ERROR) {
    out->kind = GVEC_LW_SHAPE_ERROR;
    return result;
  }
  if (bs == BOX_REFUSED) {
    return refused("the reference box of a clip-path shape is not one this lowering has for the element", out);
  }
  if (bs == BOX_NONE) {
    out->kind = GVEC_LW_SHAPE_NONE;
    return GVEC_OK;
  }
  shape s;
  memset(&s, 0, sizeof s);
  s.rule = GSHGEOM_FILL_NONZERO;
  cur a = {args, args + args_length};
  parse_status ps;
  switch (which) {
  case F_CIRCLE:
    ps = circle(&a, &ref, &s);
    break;
  case F_ELLIPSE:
    ps = ellipse(&a, &ref, &s);
    break;
  case F_INSET:
    ps = inset(&a, &ref, &s);
    break;
  case F_POLYGON:
    ps = polygon(ctx, &a, &ref, &s);
    break;
  default:
    ps = path_function(ctx, &a, &ref, &s);
    break;
  }
  if (ps == PARSE_REFUSED) {
    return refused("a length of a clip-path shape needs the font or the window, or the value is not read", out);
  }
  if (ps == PARSE_INVALID) {
    return GVEC_OK;
  }
  if (s.empty) {
    out->kind = GVEC_LW_SHAPE_ALL;
    return GVEC_OK;
  }
  gvec_lw_geometry geometry = GVEC_LW_GEOMETRY_LIMIT;
  const GSHSCENE_Resource * resource = NULL;
  if (s.is_primitive) {
    GSHSCENE_Result g = gshscene_store_intern_primitive(ctx->store, &s.primitive, &resource);
    if (g == GSHSCENE_ERR_OOM) {
      return GVEC_ERR_OOM;
    }
    if (g == GSHSCENE_OK) {
      GVEC_Result held = gvec_lw_hold(ctx, resource);
      if (held != GVEC_OK) {
        return held;
      }
      geometry = GVEC_LW_GEOMETRY_PATH;
    }
  }
  else if (s.segments) {
    GVEC_Result r = gvec_lw_segments_path(ctx, s.segments, s.segment_count, &geometry, &resource);
    if (r != GVEC_OK) {
      return r;
    }
  }
  else {
    GVEC_Result r = gvec_lw_points_path(ctx, s.points, 2 * s.point_count, &geometry, &resource);
    if (r != GVEC_OK) {
      return r;
    }
  }
  if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
    return refused("the clip-path shape is past the range the waist can carry", out);
  }
  if (geometry != GVEC_LW_GEOMETRY_PATH) {
    out->kind = GVEC_LW_SHAPE_ALL; /* No area: nothing gets through. */
    return GVEC_OK;
  }
  out->kind = GVEC_LW_SHAPE_PATH;
  out->path = resource;
  out->rule = s.rule;
  out->ox = s.ox;
  out->oy = s.oy;
  return GVEC_OK;
}
