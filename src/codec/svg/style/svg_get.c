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
 * @file svg_get.c
 *
 * The generic reads (C12): one tagged read of a property of an instance, lengths and numbers as used values (C6), and the
 * computed value as text with where the cascade got it.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/math/basic.h>

#include "svg_style.h"
#include "svg_used.h"

const char * gvec_svg_pool_string(GVEC_Svg_Style_Context * context, const char * text, size_t length) {
  return gvec_arena_strndup(&context->arena, text, length);
}

GVEC_Result gvec_svg_computed(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Svg_Property property,
    const GTEXT_CSS_Computed_Style ** out_style, const GTEXT_CSS_Value ** out_value) {
  const GTEXT_CSS_Computed_Style * style = NULL;
  GVEC_Result r = gvec_svg_instance_style(context, instance, &style);
  if (r != GVEC_OK) {
    return r;
  }
  if (out_style) {
    *out_style = style;
  }
  if (out_value) {
    *out_value = gtext_css_computed_root(style, context->engine->reg.reads[property]);
  }
  return GVEC_OK;
}

double gvec_svg_basis(const GVEC_Svg_Instance * instance, gvec_svg_prop_basis basis) {
  double w = instance->viewport_width, h = instance->viewport_height;
  switch (basis) {
  case GVEC_SVG_BASIS_WIDTH:
    return w;
  case GVEC_SVG_BASIS_HEIGHT:
    return h;
  case GVEC_SVG_BASIS_DIAGONAL:
    /* SVG 2, 8.9 "Units": the normalised diagonal, sqrt((w^2 + h^2) / 2). */
    return gmath_sqrt((w * w + h * h) / 2.0);
  default:
    return 0;
  }
}

/* Keywords are ASCII case-insensitive; `word` is lower case. */
static bool is_keyword(const GTEXT_CSS_Value * v, const char * word) {
  if (v->kind != GTEXT_CSS_VAL_KEYWORD || strlen(word) != v->text.length) {
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

bool gvec_svg_length_of_value(const GTEXT_CSS_Value * v, double basis, double * out, bool * out_auto) {
  *out_auto = false;
  switch (v->kind) {
  case GTEXT_CSS_VAL_ZERO:
    *out = 0;
    return true;
  case GTEXT_CSS_VAL_NUMBER:
  case GTEXT_CSS_VAL_INTEGER:
    *out = gvec_svg_clamp_number(v->number);
    return true;
  case GTEXT_CSS_VAL_PERCENTAGE:
    *out = gvec_svg_clamp_number(v->number * basis / 100.0);
    return true;
  case GTEXT_CSS_VAL_DIMENSION:
    if (gtext_css_unit_type(v->unit) != GTEXT_CSS_UNIT_TYPE_LENGTH) {
      return false;
    }
    if (v->unit == GTEXT_CSS_UNIT_PX) {
      *out = gvec_svg_clamp_number(v->number);
      return true;
    }
    /* Anything else that survives to a computed value is an absolute length not yet in px. */
    if (!gtext_css_unit_to_canonical(v->unit, v->number, out)) {
      return false;
    }
    *out = gvec_svg_clamp_number(*out);
    return true;
  case GTEXT_CSS_VAL_KEYWORD:
    if (is_keyword(v, "auto")) {
      *out = 0;
      *out_auto = true;
      return true;
    }
    return false;
  case GTEXT_CSS_VAL_LIST:
    return v->child_count == 1 && gvec_svg_length_of_value(&v->children[0], basis, out, out_auto);
  case GTEXT_CSS_VAL_FUNCTION:
    if (v->math) {
      GTEXT_CSS_Math_Env env;
      memset(&env, 0, sizeof env);
      env.has_percent_basis = true;
      env.percent_basis = basis;
      bool resolved = false;
      double value = 0;
      if (gtext_css_math_resolve(v->math, &env, NULL, &value, &resolved) == GTEXT_CSS_OK && resolved) {
        *out = gtext_css_math_clamp(value, v->range_lo, v->range_hi);
        return true;
      }
    }
    return false;
  default:
    return false;
  }
}

GVEC_Result gvec_svg_used_length(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Svg_Property property,
    double * out, bool * out_auto) {
  const GTEXT_CSS_Value * v = NULL;
  GVEC_Result r = gvec_svg_computed(context, instance, property, NULL, &v);
  if (r != GVEC_OK) {
    return r;
  }
  bool is_auto = false;
  double value = 0;
  if (!v || !gvec_svg_length_of_value(v, gvec_svg_basis(instance, (gvec_svg_prop_basis) gvec_svg_prop_rows[property].basis), &value, &is_auto)) {
    return GVEC_ERR_UNSUPPORTED;
  }
  *out = value;
  *out_auto = is_auto;
  return GVEC_OK;
}

/* The computed value as the engine writes it, in the context's memory. */
static GVEC_Result serialize_value(GVEC_Svg_Style_Context * context, const GTEXT_CSS_Value * v, const char ** out) {
  char * text = NULL;
  size_t length = 0;
  GTEXT_CSS_Status status = gtext_css_serialize_value_buffer(v, NULL, &text, &length);
  if (status != GTEXT_CSS_OK) {
    return status == GTEXT_CSS_E_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
  }
  const char * kept = gvec_svg_pool_string(context, text, length);
  gtext_css_serialize_buffer_free(NULL, text);
  if (!kept) {
    return GVEC_ERR_OOM;
  }
  *out = kept;
  return GVEC_OK;
}

GVEC_Result gvec_svg_get(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Svg_Property property,
    GVEC_Svg_Style_Value * out_value) {
  if (!context || !instance || !out_value || (unsigned) property >= GVEC_SVG_PROP_COUNT) {
    return GVEC_ERR_INVALID;
  }
  memset(out_value, 0, sizeof *out_value);
  const gvec_svg_prop_row * row = &gvec_svg_prop_rows[property];
  if (row->type == GVEC_SVG_PT_SHORTHAND) {
    /* No value of its own, but the style that has none must be there to say so: an element that has no style says it of every property. */
    const GTEXT_CSS_Computed_Style * style = NULL;
    GVEC_Result shorthand = gvec_svg_instance_style(context, instance, &style);
    out_value->kind = GVEC_SVG_STYLE_NONE;
    return shorthand;
  }
  if (row->type == GVEC_SVG_PT_TRANSFORM) {
    out_value->kind = GVEC_SVG_STYLE_TRANSFORM;
    return gvec_svg_used_transform(context, instance, &out_value->transform);
  }
  const GTEXT_CSS_Value * v = NULL;
  GVEC_Result r = gvec_svg_computed(context, instance, property, NULL, &v);
  if (r != GVEC_OK) {
    return r;
  }
  if (!v) {
    out_value->kind = GVEC_SVG_STYLE_NONE;
    return GVEC_OK;
  }
  switch ((gvec_svg_prop_type) row->type) {
  case GVEC_SVG_PT_KEYWORD:
    out_value->kind = GVEC_SVG_STYLE_KEYWORD;
    return serialize_value(context, v, &out_value->keyword);
  case GVEC_SVG_PT_NUMBER:
    if (v->kind != GTEXT_CSS_VAL_NUMBER && v->kind != GTEXT_CSS_VAL_INTEGER && v->kind != GTEXT_CSS_VAL_ZERO) {
      out_value->kind = GVEC_SVG_STYLE_NONE;
      return GVEC_OK;
    }
    out_value->kind = GVEC_SVG_STYLE_NUMBER;
    out_value->number = v->kind == GTEXT_CSS_VAL_ZERO ? 0 : gvec_svg_clamp_number(v->number);
    return GVEC_OK;
  case GVEC_SVG_PT_LENGTH: {
    double value;
    bool is_auto;
    if (!gvec_svg_length_of_value(v, gvec_svg_basis(instance, (gvec_svg_prop_basis) row->basis), &value, &is_auto)) {
      out_value->kind = GVEC_SVG_STYLE_NONE;
      return GVEC_OK;
    }
    out_value->kind = GVEC_SVG_STYLE_LENGTH;
    out_value->number = value;
    out_value->length_auto = is_auto;
    return GVEC_OK;
  }
  case GVEC_SVG_PT_COLOR:
    out_value->kind = GVEC_SVG_STYLE_COLOR;
    return gvec_svg_used_color(context, instance, v, instance->element, &out_value->color);
  case GVEC_SVG_PT_PAINT:
    out_value->kind = GVEC_SVG_STYLE_PAINT;
    return gvec_svg_used_paint(context, instance, v, &out_value->paint);
  case GVEC_SVG_PT_DASHARRAY:
    return gvec_svg_used_dasharray(context, instance, v, out_value);
  case GVEC_SVG_PT_REFS:
    return gvec_svg_used_references(context, instance, v, out_value);
  case GVEC_SVG_PT_TRANSFORM:
  case GVEC_SVG_PT_SHORTHAND:
  case GVEC_SVG_PT_COUNT:
    break;
  }
  return GVEC_ERR_INTERNAL;
}

GVEC_Result gvec_svg_get_specified(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Svg_Property property,
    GVEC_Svg_Style_Specified * out_specified) {
  if (!context || !instance || !out_specified || (unsigned) property >= GVEC_SVG_PROP_COUNT) {
    return GVEC_ERR_INVALID;
  }
  memset(out_specified, 0, sizeof *out_specified);
  const GTEXT_CSS_Computed_Style * style = NULL;
  GVEC_Result r = gvec_svg_instance_style(context, instance, &style);
  if (r != GVEC_OK) {
    return r;
  }
  GTEXT_CSS_Cascade_Entry entry;
  memset(&entry, 0, sizeof entry);
  if (gtext_css_computed_specified(style, context->engine->reg.reads[property], &entry)) {
    out_specified->source = entry.origin == GTEXT_CSS_ORIGIN_USER_AGENT ? GVEC_SVG_STYLE_FROM_USER_AGENT
                            : entry.element_attached                    ? GVEC_SVG_STYLE_FROM_STYLE_ATTRIBUTE
                                                                        : GVEC_SVG_STYLE_FROM_PRESENTATION;
    out_specified->important = entry.important;
  }
  const GTEXT_CSS_Value * v = gtext_css_computed_root(style, context->engine->reg.reads[property]);
  if (v) {
    return serialize_value(context, v, &out_specified->computed);
  }
  return GVEC_OK;
}
