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
 * @file svg_dash.c
 *
 * `stroke-dasharray` (C9): the entries in user units, a percentage being of the normalised diagonal; SVG 2's rules for what the array
 * means (13.5.2): an odd number of values is repeated to make an even number, and an array whose values sum to zero is `none`.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "svg_style.h"
#include "svg_used.h"

/* Add the lengths of a value to `out`, flattening the lists the engine nests (a comma list of space lists). */
static GVEC_Result collect(const GTEXT_CSS_Value * v, double basis, double * out, size_t * count, size_t capacity, bool * ok) {
  if (v->kind == GTEXT_CSS_VAL_LIST) {
    for (size_t i = 0; i < v->child_count; i++) {
      GVEC_Result r = collect(&v->children[i], basis, out, count, capacity, ok);
      if (r != GVEC_OK) {
        return r;
      }
    }
    return GVEC_OK;
  }
  double value;
  bool is_auto;
  if (!gvec_svg_length_of_value(v, basis, &value, &is_auto) || is_auto) {
    *ok = false;
    return GVEC_OK;
  }
  if (*count >= capacity) {
    return GVEC_ERR_LIMIT;
  }
  out[(*count)++] = value;
  return GVEC_OK;
}

static size_t count_leaves(const GTEXT_CSS_Value * v) {
  if (v->kind != GTEXT_CSS_VAL_LIST) {
    return 1;
  }
  size_t n = 0;
  for (size_t i = 0; i < v->child_count; i++) {
    n += count_leaves(&v->children[i]);
  }
  return n;
}

GVEC_Result gvec_svg_used_dasharray(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Value * v,
    GVEC_Svg_Style_Value * out) {
  out->kind = GVEC_SVG_STYLE_NUMBER_LIST;
  if (v->kind == GTEXT_CSS_VAL_KEYWORD) {
    return GVEC_OK; /* `none` */
  }
  size_t leaves = count_leaves(v);
  if (leaves == 0 || leaves > context->limits.max_list_items) {
    return GVEC_OK;
  }
  /* Room for the repeat of an odd count. */
  double * numbers = gvec_arena_array(&context->arena, leaves * 2, sizeof *numbers, 8);
  if (!numbers) {
    return GVEC_ERR_OOM;
  }
  size_t n = 0;
  bool ok = true;
  double basis = gvec_svg_basis(instance, GVEC_SVG_BASIS_DIAGONAL);
  GVEC_Result r = collect(v, basis, numbers, &n, leaves, &ok);
  if (r != GVEC_OK) {
    return r;
  }
  if (!ok || n == 0) {
    return GVEC_OK;
  }
  double sum = 0;
  for (size_t i = 0; i < n; i++) {
    if (!(numbers[i] >= 0) || numbers[i] > 1e300) {
      return GVEC_OK; /* A negative or non-finite entry makes the array invalid: solid. */
    }
    sum += numbers[i];
  }
  if (!(sum > 0) || sum > 1e300) {
    return GVEC_OK; /* All zero: solid (SVG 2, 13.5.2). */
  }
  if (n % 2 == 1) {
    memcpy(numbers + n, numbers, n * sizeof *numbers);
    n *= 2;
  }
  out->numbers = numbers;
  out->number_count = n;
  return GVEC_OK;
}
