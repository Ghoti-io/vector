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
 * @file lower_viewport.c
 *
 * The map from a viewBox to its viewport (SVG 2, 8.2; P3, P4), in exact arithmetic.
 *
 * The scale is the correctly rounded quotient of the viewport's size and the box's. The translation is the exact value of
 * `a * viewport - scale * (origin + a * box)`, with `a` 0, 1/2 or 1 for the alignment, rounded once: it is summed as a
 * non-overlapping expansion (Shewchuk's), whose sign and neighbours decide the rounding, so that it does not depend on the
 * order of the terms or on a fused multiply-add (this library is built without one).
 */

#include <ghoti.io/vector/macros.h>

#include <math.h>
#include <string.h>

#include <ghoti.io/math/basic.h>

#include "lower_internal.h"

/* An expansion: non-overlapping doubles in increasing order of magnitude, whose exact sum is the value. */
typedef struct expansion {
  double c[24];
  int n;
} expansion;

static void two_sum(double a, double b, double * s, double * err) {
  double sum = a + b;
  double bb = sum - a;
  *s = sum;
  *err = (a - (sum - bb)) + (b - bb);
}

/* Veltkamp's split: a = hi + lo, each with at most 26 significant bits. */
static void split(double a, double * hi, double * lo) {
  double c = 134217729.0 * a;
  double abig = c - a;
  double h = c - abig;
  *hi = h;
  *lo = a - h;
}

/* a * b = p + err exactly, for inputs whose product neither overflows nor underflows into the subnormals. */
static void two_product(double a, double b, double * p, double * err) {
  double prod = a * b;
  double ah, al, bh, bl;
  split(a, &ah, &al);
  split(b, &bh, &bl);
  double e1 = prod - (ah * bh);
  double e2 = e1 - (al * bh);
  double e3 = e2 - (ah * bl);
  *p = prod;
  *err = (al * bl) - e3;
}

/* Add one double to an expansion, exactly (Grow-Expansion, zero components dropped). */
static bool grow(expansion * e, double b) {
  double q = b;
  double h[24];
  int k = 0;
  for (int i = 0; i < e->n; i++) {
    double sum, err;
    two_sum(q, e->c[i], &sum, &err);
    q = sum;
    if (err != 0) {
      h[k++] = err;
    }
  }
  if (q != 0 || k == 0) {
    h[k++] = q;
  }
  if (k > 24) {
    return false;
  }
  memcpy(e->c, h, (size_t) k * sizeof(double));
  e->n = k;
  if (k == 1 && e->c[0] == 0) {
    e->n = 0;
  }
  return true;
}

static int sign_of(const expansion * e) {
  if (e->n == 0) {
    return 0;
  }
  double top = e->c[e->n - 1];
  return top > 0 ? 1 : top < 0 ? -1 : 0;
}

/* The sign of `e - (a + b) / 2`, exactly. */
static int sign_against_midpoint(const expansion * e, double a, double b) {
  expansion t = *e;
  if (!grow(&t, -a / 2) || !grow(&t, -b / 2)) {
    return 0;
  }
  return sign_of(&t);
}

static bool is_even(double d) {
  uint64_t bits;
  memcpy(&bits, &d, sizeof bits);
  return (bits & 1) == 0;
}

/* The double nearest the expansion's exact sum (ties to even). */
static double round_expansion(const expansion * e) {
  if (e->n == 0) {
    return 0;
  }
  double t = 0;
  for (int i = 0; i < e->n; i++) {
    t += e->c[i];
  }
  /* t is within an ulp of the sum; move it to the neighbour the exact sum is nearest. */
  for (int step = 0; step < 4; step++) {
    double up = gmath_nextafter(t, INFINITY);
    double down = gmath_nextafter(t, -INFINITY);
    int toward_up = sign_against_midpoint(e, t, up);
    int toward_down = sign_against_midpoint(e, t, down);
    if (toward_up > 0 || (toward_up == 0 && !is_even(t) && is_even(up))) {
      t = up;
    }
    else if (toward_down < 0 || (toward_down == 0 && !is_even(t) && is_even(down))) {
      t = down;
    }
    else {
      break;
    }
  }
  return t == 0 ? 0.0 : t;
}

static bool in_range(double v) {
  if (v == 0) {
    return true;
  }
  double m = gmath_fabs(v);
  return m >= 0x1p-200 && m <= 0x1p200;
}

/* The exact value of `align * extent - scale * (origin + align * size)`, rounded once. */
static bool translation(double align, double extent, double scale, double origin, double size, double * out) {
  expansion e;
  e.n = 0;
  double p, err;
  /* align * extent: align is 0, 1/2 or 1, so the product is exact. */
  if (!grow(&e, align * extent)) {
    return false;
  }
  two_product(scale, origin, &p, &err);
  if (!grow(&e, -err) || !grow(&e, -p)) {
    return false;
  }
  two_product(align * scale, size, &p, &err);
  if (!grow(&e, -err) || !grow(&e, -p)) {
    return false;
  }
  *out = round_expansion(&e);
  return true;
}

bool gvec_lw_viewbox_map(const GVEC_Svg_View_Box * box, const GVEC_Svg_Preserve_Aspect_Ratio * par, double width, double height,
    GSHGEOM_Map * out) {
  if (!(width > 0) || !(height > 0) || !(box->width > 0) || !(box->height > 0) || !in_range(width) || !in_range(height) ||
      !in_range(box->width) || !in_range(box->height) || !in_range(box->x) || !in_range(box->y)) {
    return false;
  }
  double sx = width / box->width;
  double sy = height / box->height;
  double m[4] = {sx, 0, 0, sy};
  double t[2];
  if (par->align == GVEC_SVG_ALIGN_NONE) {
    t[0] = -(box->x * sx);
    t[1] = -(box->y * sy);
  }
  else {
    double s = par->slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    unsigned index = (unsigned) par->align - 1;
    double ax = (double) (index % 3) / 2.0;
    double ay = (double) (index / 3) / 2.0;
    m[0] = m[3] = s;
    if (!in_range(s) || !translation(ax, width, s, box->x, box->width, &t[0]) ||
        !translation(ay, height, s, box->y, box->height, &t[1])) {
      return false;
    }
  }
  if (!(gmath_fabs(m[0]) <= 0x1p100) || !(gmath_fabs(m[3]) <= 0x1p100)) {
    return false;
  }
  GSHGEOM_Map made;
  if (gshgeom_map_from_matrix(2, m, t, &made) != GSHGEOM_OK) {
    return false;
  }
  return gvec_lw_normal(&made, out);
}
