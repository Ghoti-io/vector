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
 * @file svg_transform.c
 *
 * `transform`, `transform-origin` and `transform-box` (C9): the computed transform list as a map, exact (AD-22) through the shape
 * family's constructors, with the percentages that depend on a bounding box left symbolic until the lowering has the box.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/elementary.h>

#include "svg_style.h"
#include "svg_used.h"

#define PI 3.14159265358979323846

static bool name_is(const GTEXT_CSS_Value * f, const char * name) {
  if (f->text.length != strlen(name)) {
    return false;
  }
  for (size_t i = 0; i < f->text.length; i++) {
    char c = f->text.data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char) (c - 'A' + 'a');
    }
    if (c != name[i]) {
      return false;
    }
  }
  return true;
}

/* A number argument. */
static bool number_of(const GTEXT_CSS_Value * v, double * out) {
  switch (v->kind) {
  case GTEXT_CSS_VAL_NUMBER:
  case GTEXT_CSS_VAL_INTEGER:
    *out = gvec_svg_clamp_number(v->number);
    return true;
  case GTEXT_CSS_VAL_ZERO:
    *out = 0;
    return true;
  case GTEXT_CSS_VAL_PERCENTAGE:
    *out = gvec_svg_clamp_number(v->number / 100.0); /* `scale(50%)` is 0.5 (Transforms 1, 12). */
    return true;
  default:
    return false;
  }
}

/* An angle argument in degrees; the engine made it `deg` already. */
static bool angle_of(const GTEXT_CSS_Value * v, double * out) {
  if (v->kind == GTEXT_CSS_VAL_ZERO) {
    *out = 0;
    return true;
  }
  if (v->kind == GTEXT_CSS_VAL_DIMENSION && gtext_css_unit_type(v->unit) == GTEXT_CSS_UNIT_TYPE_ANGLE) {
    if (v->unit == GTEXT_CSS_UNIT_DEG) {
      *out = gvec_svg_clamp_number(v->number);
      return true;
    }
    if (!gtext_css_unit_to_canonical(v->unit, v->number, out)) {
      return false;
    }
    *out = gvec_svg_clamp_number(*out);
    return true;
  }
  return false;
}

/* A length-percentage: a length in user units, or a fraction. */
static bool length_or_fraction(const GTEXT_CSS_Value * v, double * out, bool * percent) {
  *percent = false;
  switch (v->kind) {
  case GTEXT_CSS_VAL_ZERO:
    *out = 0;
    return true;
  case GTEXT_CSS_VAL_NUMBER:
  case GTEXT_CSS_VAL_INTEGER:
    *out = gvec_svg_clamp_number(v->number);
    return true;
  case GTEXT_CSS_VAL_PERCENTAGE:
    *out = gvec_svg_clamp_number(v->number / 100.0);
    *percent = true;
    return true;
  case GTEXT_CSS_VAL_DIMENSION: {
    bool is_auto;
    return gvec_svg_length_of_value(v, 0, out, &is_auto);
  }
  case GTEXT_CSS_VAL_FUNCTION:
    if (v->math) {
      double value;
      bool pct = false;
      GTEXT_CSS_Unit unit;
      if (gtext_css_math_numeric(v->math, &value, &unit, &pct)) {
        *out = pct ? value / 100.0 : value;
        *percent = pct;
        return true;
      }
    }
    return false;
  default:
    return false;
  }
}

static bool keyword_lower(const GTEXT_CSS_Value * v, const char * word) {
  if (v->kind != GTEXT_CSS_VAL_KEYWORD || v->text.length != strlen(word)) {
    return false;
  }
  for (size_t i = 0; i < v->text.length; i++) {
    char c = v->text.data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char) (c - 'A' + 'a');
    }
    if (c != word[i]) {
      return false;
    }
  }
  return true;
}

/* The arguments of a function, in order: the engine nests them in the lists its grammar made (`translate(10, 20)` is one list holding
 * 10 and a list of the comma and 20), so the values that are not commas are collected in a walk of fixed depth. A function of a 2D
 * drawing has at most six. */
#define MAX_ARGS 6
static size_t flatten(const GTEXT_CSS_Value * v, const GTEXT_CSS_Value ** out, size_t n, unsigned depth) {
  if (depth > 8 || n > MAX_ARGS) {
    return MAX_ARGS + 1;
  }
  if (v->kind == GTEXT_CSS_VAL_LIST) {
    for (size_t i = 0; i < v->child_count; i++) {
      n = flatten(&v->children[i], out, n, depth + 1);
    }
    return n;
  }
  if (v->kind == GTEXT_CSS_VAL_TOKEN) {
    return n;
  }
  if (n >= MAX_ARGS) {
    return MAX_ARGS + 1;
  }
  out[n] = v;
  return n + 1;
}

/* One function of the list into an op; false for a function a 2D drawing does not take. */
static bool read_function(const GTEXT_CSS_Value * f, GVEC_Svg_Transform_Op * op) {
  memset(op, 0, sizeof *op);
  if (f->kind != GTEXT_CSS_VAL_FUNCTION) {
    return false;
  }
  const GTEXT_CSS_Value * arguments[MAX_ARGS];
  size_t n = 0;
  for (size_t i = 0; i < f->child_count; i++) {
    n = flatten(&f->children[i], arguments, n, 0);
  }
  if (n > MAX_ARGS) {
    return false;
  }
  const GTEXT_CSS_Value * a[MAX_ARGS];
  for (size_t i = 0; i < n; i++) {
    a[i] = arguments[i];
  }
  /* The branches below read a[i] as a value; make them read through the pointer. */
  if (name_is(f, "matrix") && n == 6) {
    op->kind = GVEC_SVG_TRANSFORM_MATRIX;
    for (size_t i = 0; i < 6; i++) {
      if (!number_of(a[i], &op->v[i])) {
        return false;
      }
    }
    return true;
  }
  if (name_is(f, "translate") && (n == 1 || n == 2)) {
    op->kind = GVEC_SVG_TRANSFORM_TRANSLATE;
    return length_or_fraction(a[0], &op->v[0], &op->percent[0]) && (n == 1 || length_or_fraction(a[1], &op->v[1], &op->percent[1]));
  }
  if (name_is(f, "translatex") && n == 1) {
    op->kind = GVEC_SVG_TRANSFORM_TRANSLATE;
    return length_or_fraction(a[0], &op->v[0], &op->percent[0]);
  }
  if (name_is(f, "translatey") && n == 1) {
    op->kind = GVEC_SVG_TRANSFORM_TRANSLATE;
    return length_or_fraction(a[0], &op->v[1], &op->percent[1]);
  }
  if (name_is(f, "scale") && (n == 1 || n == 2)) {
    op->kind = GVEC_SVG_TRANSFORM_SCALE;
    if (!number_of(a[0], &op->v[0])) {
      return false;
    }
    if (n == 2) {
      return number_of(a[1], &op->v[1]);
    }
    op->v[1] = op->v[0];
    return true;
  }
  if (name_is(f, "scalex") && n == 1) {
    op->kind = GVEC_SVG_TRANSFORM_SCALE;
    op->v[1] = 1;
    return number_of(a[0], &op->v[0]);
  }
  if (name_is(f, "scaley") && n == 1) {
    op->kind = GVEC_SVG_TRANSFORM_SCALE;
    op->v[0] = 1;
    return number_of(a[0], &op->v[1]);
  }
  if ((name_is(f, "rotate") || name_is(f, "rotatez")) && n == 1) {
    op->kind = GVEC_SVG_TRANSFORM_ROTATE;
    return angle_of(a[0], &op->v[0]);
  }
  if (name_is(f, "skewx") && n == 1) {
    op->kind = GVEC_SVG_TRANSFORM_SKEW_X;
    return angle_of(a[0], &op->v[0]);
  }
  if (name_is(f, "skewy") && n == 1) {
    op->kind = GVEC_SVG_TRANSFORM_SKEW_Y;
    return angle_of(a[0], &op->v[0]);
  }
  return false;
}

static GVEC_Result map_of_op(const GVEC_Svg_Transform_Op * op, double box_w, double box_h, GSHGEOM_Map * out) {
  GSHGEOM_Result g;
  switch (op->kind) {
  case GVEC_SVG_TRANSFORM_MATRIX: {
    /* matrix(a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f. */
    double m[4] = {op->v[0], op->v[2], op->v[1], op->v[3]};
    double t[2] = {op->v[4], op->v[5]};
    g = gshgeom_map_from_matrix(2, m, t, out);
    break;
  }
  case GVEC_SVG_TRANSFORM_TRANSLATE:
    g = gshgeom_map_translate(2, op->percent[0] ? op->v[0] * box_w : op->v[0], op->percent[1] ? op->v[1] * box_h : op->v[1], 0, out);
    break;
  case GVEC_SVG_TRANSFORM_SCALE:
    g = gshgeom_map_scale(2, op->v[0], op->v[1], 1, out);
    break;
  case GVEC_SVG_TRANSFORM_ROTATE:
    g = gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, op->v[0], out);
    break;
  case GVEC_SVG_TRANSFORM_SKEW_X:
  case GVEC_SVG_TRANSFORM_SKEW_Y: {
    /* A skew has no exact constructor; the angle goes through math's tangent. */
    double t = gmath_tan(op->v[0] * (PI / 180.0));
    double m[4] = {1, 0, 0, 1};
    if (op->kind == GVEC_SVG_TRANSFORM_SKEW_X) {
      m[1] = t;
    }
    else {
      m[2] = t;
    }
    double zero[2] = {0, 0};
    g = gshgeom_map_from_matrix(2, m, zero, out);
    break;
  }
  default:
    return GVEC_ERR_INTERNAL;
  }
  return g == GSHGEOM_OK ? GVEC_OK : GVEC_ERR_UNSUPPORTED;
}

GVEC_Result gvec_svg_transform_resolve(const GVEC_Svg_Transform * transform, double box_x, double box_y, double box_width,
    double box_height, GSHGEOM_Map * out_map) {
  if (!transform || !out_map) {
    return GVEC_ERR_INVALID;
  }
  if (!transform->depends_on_box) {
    *out_map = transform->map;
    return GVEC_OK;
  }
  double ox = box_x + transform->origin[0] + transform->origin_fraction[0] * box_width;
  double oy = box_y + transform->origin[1] + transform->origin_fraction[1] * box_height;
  GSHGEOM_Map result, step;
  if (gshgeom_map_translate(2, ox, oy, 0, &result) != GSHGEOM_OK) {
    return GVEC_ERR_UNSUPPORTED;
  }
  for (size_t i = 0; i < transform->op_count; i++) {
    GVEC_Result r = map_of_op(&transform->ops[i], box_width, box_height, &step);
    if (r != GVEC_OK || gshgeom_map_compose(&result, &step, &result) != GSHGEOM_OK) {
      return GVEC_ERR_UNSUPPORTED;
    }
  }
  if (gshgeom_map_translate(2, -ox, -oy, 0, &step) != GSHGEOM_OK || gshgeom_map_compose(&result, &step, &result) != GSHGEOM_OK) {
    return GVEC_ERR_UNSUPPORTED;
  }
  *out_map = result;
  return GVEC_OK;
}

/* The origin: one to three values, each a keyword, a length or a percentage (Transforms 1, `transform-origin`). */
static bool read_origin(const GTEXT_CSS_Value * v, double origin[2], double fraction[2]) {
  origin[0] = origin[1] = fraction[0] = fraction[1] = 0;
  if (!v) {
    return true;
  }
  const GTEXT_CSS_Value * items = v;
  size_t n = 1;
  if (v->kind == GTEXT_CSS_VAL_LIST) {
    items = v->children;
    n = v->child_count;
  }
  if (n < 1 || n > 3) {
    return false;
  }
  for (size_t i = 0; i < 2 && i < n; i++) {
    const GTEXT_CSS_Value * c = &items[i];
    double value = 0;
    bool percent = false;
    if (keyword_lower(c, "left") || keyword_lower(c, "top")) {
      fraction[i] = 0;
    }
    else if (keyword_lower(c, "center")) {
      fraction[i] = 0.5;
    }
    else if (keyword_lower(c, "right") || keyword_lower(c, "bottom")) {
      fraction[i] = 1;
    }
    else if (length_or_fraction(c, &value, &percent)) {
      if (percent) {
        fraction[i] = value;
      }
      else {
        origin[i] = value;
      }
    }
    else {
      return false;
    }
  }
  if (n == 1) {
    fraction[1] = 0.5; /* One value: the other is `center`. */
    /* `top` and `bottom` alone name the vertical axis. */
    if (keyword_lower(&items[0], "top") || keyword_lower(&items[0], "bottom")) {
      fraction[1] = fraction[0];
      fraction[0] = 0.5;
    }
  }
  return true;
}

GVEC_Result gvec_svg_used_transform(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Svg_Transform * out) {
  memset(out, 0, sizeof *out);
  const GTEXT_CSS_Computed_Style * style = NULL;
  const GTEXT_CSS_Value *list = NULL, *origin = NULL, *box = NULL;
  GVEC_Result r = gvec_svg_computed(context, instance, GVEC_SVG_PROP_TRANSFORM, &style, &list);
  if (r == GVEC_OK) {
    r = gvec_svg_computed(context, instance, GVEC_SVG_PROP_TRANSFORM_ORIGIN, NULL, &origin);
  }
  if (r == GVEC_OK) {
    r = gvec_svg_computed(context, instance, GVEC_SVG_PROP_TRANSFORM_BOX, NULL, &box);
  }
  if (r != GVEC_OK) {
    return r;
  }
  const GVEC_Svg_Element * e = instance->element;
  /* The ops. */
  GVEC_Svg_Transform_Op * ops = NULL;
  size_t count = 0;
  if (list && !keyword_lower(list, "none")) {
    const GTEXT_CSS_Value * items = list;
    size_t n = 1;
    if (list->kind == GTEXT_CSS_VAL_LIST) {
      items = list->children;
      n = list->child_count;
    }
    ops = gvec_arena_array(&context->arena, n * 2 + 1, sizeof *ops, 8);
    if (!ops) {
      return GVEC_ERR_OOM;
    }
    for (size_t i = 0; i < n; i++) {
      const GTEXT_CSS_Value * f = &items[i];
      if (f->kind == GTEXT_CSS_VAL_FUNCTION && name_is(f, "skew")) {
        const GTEXT_CSS_Value * args[MAX_ARGS];
        size_t na = 0;
        for (size_t k = 0; k < f->child_count; k++) {
          na = flatten(&f->children[k], args, na, 0);
        }
        double ax, ay = 0;
        if ((na != 1 && na != 2) || !angle_of(args[0], &ax) || (na == 2 && !angle_of(args[1], &ay))) {
          goto invalid;
        }
        ops[count].kind = GVEC_SVG_TRANSFORM_SKEW_X;
        ops[count++].v[0] = ax;
        if (na == 2) {
          ops[count].kind = GVEC_SVG_TRANSFORM_SKEW_Y;
          ops[count++].v[0] = ay;
        }
        continue;
      }
      if (!read_function(f, &ops[count])) {
        goto invalid;
      }
      count++;
    }
  }
  double ox[2], frac[2];
  if (!read_origin(origin, ox, frac)) {
    goto invalid;
  }
  out->box = GVEC_SVG_BOX_VIEW;
  if (box) {
    if (keyword_lower(box, "fill-box")) {
      out->box = GVEC_SVG_BOX_FILL;
    }
    else if (keyword_lower(box, "stroke-box")) {
      out->box = GVEC_SVG_BOX_STROKE;
    }
    else if (keyword_lower(box, "content-box")) {
      out->box = GVEC_SVG_BOX_CONTENT;
    }
    else if (keyword_lower(box, "border-box")) {
      out->box = GVEC_SVG_BOX_BORDER;
    }
  }
  bool percent = frac[0] != 0 || frac[1] != 0;
  for (size_t i = 0; i < count; i++) {
    percent = percent || ops[i].percent[0] || ops[i].percent[1];
  }
  out->ops = ops;
  out->op_count = count;
  out->origin[0] = ox[0];
  out->origin[1] = ox[1];
  out->origin_fraction[0] = frac[0];
  out->origin_fraction[1] = frac[1];
  if (percent && out->box != GVEC_SVG_BOX_VIEW) {
    out->depends_on_box = true;
    gshgeom_map_identity(2, &out->map);
    return GVEC_OK;
  }
  if (percent) {
    /* The reference box is the nearest viewport, which this instance knows: resolve every percentage now. */
    for (size_t i = 0; i < count; i++) {
      if (ops[i].percent[0]) {
        ops[i].v[0] *= instance->viewport_width;
        ops[i].percent[0] = false;
      }
      if (ops[i].percent[1]) {
        ops[i].v[1] *= instance->viewport_height;
        ops[i].percent[1] = false;
      }
    }
    out->origin[0] += frac[0] * instance->viewport_width;
    out->origin[1] += frac[1] * instance->viewport_height;
    out->origin_fraction[0] = out->origin_fraction[1] = 0;
  }
  GVEC_Svg_Transform resolved = *out;
  resolved.depends_on_box = true; /* resolve() composes the ops; the box is irrelevant now. */
  r = gvec_svg_transform_resolve(&resolved, 0, 0, 0, 0, &out->map);
  if (r != GVEC_OK) {
    gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_STYLE_INVALID, 0, e, gvec_svg_element_find_attribute(e, "transform"),
        "a transform that is not finite or past the waist's coordinate limit; the element is refused");
    return GVEC_ERR_UNSUPPORTED;
  }
  return GVEC_OK;
invalid:
  gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_STYLE_INVALID, 0, e, gvec_svg_element_find_attribute(e, "transform"),
      "a transform function a two-dimensional drawing does not take (a 3D or perspective function); the element is refused");
  return GVEC_ERR_UNSUPPORTED;
}
