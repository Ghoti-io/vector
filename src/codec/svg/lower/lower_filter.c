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

/*
 * The `filter` property: a reference to a `filter` element becomes a filter resource of the waist (shape-scene filter.h) and a group that
 * names it around the element's content, inside the clip and the mask (Filter Effects: the filter comes first, then the clip, the mask and
 * the opacity).  The primitives lowered are feOffset, feFlood, feMerge, feBlend, feComposite, feColorMatrix, feComponentTransfer,
 * feGaussianBlur, feDropShadow, feMorphology, feConvolveMatrix, feTile and feDisplacementMap; an element that names a filter with any
 * other primitive is refused whole.  The waist's filter is in the space of the group's items, the list's, so the filter's lengths and
 * regions are put into it through the element's own map, which must be axis aligned (a scale, a mirror, a quarter turn and a translation:
 * the filter of a rotated or skewed element needs canonical space, which no sink draws yet).
 */

#include <string.h>

#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/filter.h>

#include "../../../core/core_internal.h"
#include "../svg_grammar.h"
#include "lower_internal.h"

/* --- the element's map into the list's space ---------------------------------------------------------------------------- */

typedef struct fmap {
  double a, b, c, d, e, f; /* x' = a x + b y + e, y' = c x + d y + f */
  bool swap;
} fmap;

static bool fmap_make(const GSHGEOM_Map * m, fmap * out) {
  if (m->dim != 2) {
    return false;
  }
  out->a = m->m[0][0];
  out->b = m->m[0][1];
  out->c = m->m[1][0];
  out->d = m->m[1][1];
  out->e = m->t[0];
  out->f = m->t[1];
  out->swap = out->a == 0 && out->d == 0;
  return ((out->b == 0 && out->c == 0) || out->swap) && (out->a * out->d - out->b * out->c) != 0;
}

static double dabs(double x) {
  return x < 0 ? -x : x;
}

/** A pair of lengths along the element's axes as lengths along the list's. */
static void fmap_lengths(const fmap * m, double ux, double uy, double * lx, double * ly) {
  if (!m->swap) {
    *lx = dabs(m->a) * ux;
    *ly = dabs(m->d) * uy;
  }
  else {
    *lx = dabs(m->b) * uy;
    *ly = dabs(m->c) * ux;
  }
}

static void fmap_vector(const fmap * m, double ux, double uy, double * lx, double * ly) {
  *lx = m->a * ux + m->b * uy;
  *ly = m->c * ux + m->d * uy;
}

/** A rectangle `x y w h` of the element's space as one of the list's. */
static void fmap_rect(const fmap * m, const double r[4], double out[4]) {
  const double xs[2] = {r[0], r[0] + r[2]}, ys[2] = {r[1], r[1] + r[3]};
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  for (int i = 0; i < 4; i++) {
    double px, py;
    fmap_vector(m, xs[i & 1], ys[i >> 1], &px, &py);
    px += m->e;
    py += m->f;
    if (i == 0 || px < x0) {
      x0 = px;
    }
    if (i == 0 || px > x1) {
      x1 = px;
    }
    if (i == 0 || py < y0) {
      y0 = py;
    }
    if (i == 0 || py > y1) {
      y1 = py;
    }
  }
  out[0] = x0;
  out[1] = y0;
  out[2] = x1 - x0;
  out[3] = y1 - y0;
}

/* --- reading attributes -------------------------------------------------------------------------------------------------- */

static bool is_wsp(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/** The attribute's text trimmed; false when the element has none. */
static bool raw(const GVEC_Svg_Element * e, const char * name, const char ** s, size_t * n) {
  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, name);
  if (!a) {
    return false;
  }
  *s = gvec_svg_attribute_raw(a, n);
  while (*n > 0 && is_wsp((*s)[0])) {
    (*s)++;
    (*n)--;
  }
  while (*n > 0 && is_wsp((*s)[*n - 1])) {
    (*n)--;
  }
  return *s != NULL;
}

static bool word_is(const char * s, size_t n, const char * word) {
  const size_t w = strlen(word);
  return n == w && strncmp(s, word, n) == 0;
}

/** A list of numbers separated by white space and commas; false on anything else or past @p max. */
static bool numbers(const char * s, size_t n, double * out, size_t max, size_t * count) {
  size_t pos = 0;
  *count = 0;
  for (;;) {
    while (pos < n && (is_wsp(s[pos]) || s[pos] == ',')) {
      pos++;
    }
    if (pos >= n) {
      return true;
    }
    gvec_svg_parse_error err;
    double v;
    if (*count >= max || !gvec_svg_scan_number(s, n, &pos, &v, &err)) {
      return false;
    }
    out[(*count)++] = v;
  }
}

/** A number attribute, @p fallback when it is absent; false when it is present and not a number. */
static bool number_attr(const GVEC_Svg_Element * e, const char * name, double fallback, double * out) {
  const char * s;
  size_t n;
  *out = fallback;
  if (!raw(e, name, &s, &n)) {
    return true;
  }
  double v[1];
  size_t count;
  if (!numbers(s, n, v, 1, &count) || count != 1) {
    return true; /* a value that is not a number is an error: the attribute is as if it were not there (SVG 2's error rule) */
  }
  *out = v[0];
  return true;
}

/** One number, or two (the second the first's when there is one only): `stdDeviation`, `radius`, `order`. */
static bool pair_attr(const GVEC_Svg_Element * e, const char * name, double fallback, double out[2]) {
  const char * s;
  size_t n;
  out[0] = out[1] = fallback;
  if (!raw(e, name, &s, &n)) {
    return true;
  }
  double v[2];
  size_t count;
  if (!numbers(s, n, v, 2, &count) || count < 1) {
    return true; /* as if the attribute were not there */
  }
  out[0] = v[0];
  out[1] = count == 2 ? v[1] : v[0];
  return true;
}

/** A length of the region or a subregion: the fraction of the box (`percent / 100` for a percentage) in the box's units, else user units. */
static bool length_attr(const GVEC_Svg_Element * e, const char * name, bool box_units, double basis, bool * present, double * out) {
  const char * s;
  size_t n;
  *present = raw(e, name, &s, &n);
  if (!*present) {
    return true;
  }
  GVEC_Svg_Length len;
  gvec_svg_parse_error err;
  if (gvec_svg_parse_length(s, n, &len, &err) != GVEC_SVG_PARSE_OK) {
    return false;
  }
  if (len.unit == GVEC_SVG_UNIT_PERCENT) {
    *out = box_units ? len.value / 100.0 : len.value * basis / 100.0;
    return true;
  }
  if (box_units && len.unit == GVEC_SVG_UNIT_NONE) {
    *out = len.value;
    return true;
  }
  return gvec_lw_absolute_length(&len, out);
}

/* --- the filter ------------------------------------------------------------------------------------------------------- */

typedef struct fbuild {
  gvec_lw_ctx * ctx;
  const GVEC_Svg_Element * target; /* the element the filter is on, for the refusals */
  fmap map;
  bool prim_box;                   /* primitiveUnits is objectBoundingBox */
  double box[4];                   /* its box: x y w h, in the element's space */
  GSHSCENE_Filter_Primitive * prims;
  GSHSCENE_Filter_Input (*inputs)[2];
  GSHSCENE_Filter_Input * merge_inputs;
  size_t merge_used;
  double * values;
  size_t values_used;
  size_t count;
  const char * names[GSHSCENE_FILTER_MAX_PRIMITIVES];
  size_t name_len[GSHSCENE_FILTER_MAX_PRIMITIVES];
  double vw, vh;                   /* the viewport, for the percentages of user units */
  long prev;                       /* the last real primitive, or -1 */
  bool invalid;                    /* the filter is in error: the element is not rendered */
  bool refused;
} fbuild;

#define MERGE_POOL (GSHSCENE_FILTER_MAX_PRIMITIVES * 4u)
#define VALUE_POOL (GSHSCENE_FILTER_MAX_VALUES)

static GVEC_Result refuse_with(fbuild * b, const char * why) {
  b->refused = true;
  return gvec_lw_refuse(b->ctx, b->target, GVEC_LOWER_FILTER, why);
}

/** Add a primitive; the index, or -1 when full (the filter is refused). */
static long add_prim(fbuild * b, GSHSCENE_Filter_Primitive ** out) {
  if (b->count >= GSHSCENE_FILTER_MAX_PRIMITIVES) {
    return -1;
  }
  GSHSCENE_Filter_Primitive * p = &b->prims[b->count];
  memset(p, 0, sizeof *p);
  *out = p;
  return (long) b->count;
}

/** The input an `in` attribute names for the primitive about to be added at index `b->count` (and an empty flood for the removed keywords). */
static GVEC_Result input_of(fbuild * b, const GVEC_Svg_Element * el, const char * attribute, GSHSCENE_Filter_Input * in, bool * ok) {
  const char * s;
  size_t n;
  *ok = true;
  in->index = 0;
  if (!raw(el, attribute, &s, &n) || n == 0) {
    if (b->prev < 0) {
      in->kind = GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC;
    }
    else {
      in->kind = GSHSCENE_FILTER_INPUT_RESULT;
      in->index = (uint32_t) b->prev;
    }
    return GVEC_OK;
  }
  if (word_is(s, n, "SourceGraphic")) {
    in->kind = GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC;
    return GVEC_OK;
  }
  if (word_is(s, n, "SourceAlpha")) {
    in->kind = GSHSCENE_FILTER_INPUT_SOURCE_ALPHA;
    return GVEC_OK;
  }
  if (word_is(s, n, "BackgroundImage") || word_is(s, n, "BackgroundAlpha") || word_is(s, n, "FillPaint") || word_is(s, n, "StrokePaint")) {
    /* Filter Effects 1 removed these inputs: transparent black, here an empty flood of their own. */
    GSHSCENE_Filter_Primitive * p;
    long at = add_prim(b, &p);
    if (at < 0) {
      *ok = false;
      return refuse_with(b, "the filter has more primitives than the waist carries");
    }
    p->kind = GSHSCENE_FILTER_FLOOD;
    p->color_space = GSHSCENE_FILTER_SPACE_LINEAR_RGB;
    b->names[b->count] = NULL;
    b->name_len[b->count] = 0;
    b->count++;
    in->kind = GSHSCENE_FILTER_INPUT_RESULT;
    in->index = (uint32_t) at;
    return GVEC_OK;
  }
  for (size_t i = b->count; i-- > 0;) {
    if (b->names[i] && b->name_len[i] == n && strncmp(b->names[i], s, n) == 0) {
      in->kind = GSHSCENE_FILTER_INPUT_RESULT;
      in->index = (uint32_t) i;
      return GVEC_OK;
    }
  }
  /* a name no earlier primitive has: the previous result, as the browsers and resvg take it */
  if (b->prev < 0) {
    in->kind = GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC;
  }
  else {
    in->kind = GSHSCENE_FILTER_INPUT_RESULT;
    in->index = (uint32_t) b->prev;
  }
  return GVEC_OK;
}

/** The flood color and opacity of an element's `flood-color` and `flood-opacity` (or the properties given), as the primitive's `color`. */
static GVEC_Result flood_of(fbuild * b, const GVEC_Svg_Element * el, GSHSCENE_Filter_Primitive * p) {
  const GVEC_Svg_Instance * inst;
  GVEC_Result r = gvec_lw_instance_of(b->ctx, el, &inst);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Style_Value color, opacity;
  bool usable;
  r = gvec_lw_read_property(b->ctx, inst, GVEC_SVG_PROP_FLOOD_COLOR, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &color, &usable);
  if (r != GVEC_OK || !usable) {
    b->refused = true;
    return r;
  }
  r = gvec_lw_read_property(b->ctx, inst, GVEC_SVG_PROP_FLOOD_OPACITY, GVEC_LOWER_STYLE, &opacity, &usable);
  if (r != GVEC_OK || !usable) {
    b->refused = true;
    return r;
  }
  const GVEC_Svg_Color * c = color.kind == GVEC_SVG_STYLE_COLOR ? &color.color : color.kind == GVEC_SVG_STYLE_PAINT ? &color.paint.color : NULL;
  if (!c || c->kind == GVEC_SVG_COLOR_FOREGROUND) {
    return refuse_with(b, "a flood color that is not a plain color is not lowered");
  }
  p->color.rgba[0] = c->color.rgba[0];
  p->color.rgba[1] = c->color.rgba[1];
  p->color.rgba[2] = c->color.rgba[2];
  p->color.rgba[3] = c->color.rgba[3];
  p->color.space = c->color.space;
  double o = opacity.kind == GVEC_SVG_STYLE_NUMBER ? opacity.number : 1;
  p->opacity = o < 0 ? 0 : (o > 1 ? 1 : o);
  return GVEC_OK;
}

/** `color-interpolation-filters` of an element. */
static GVEC_Result space_of(fbuild * b, const GVEC_Svg_Element * el, uint32_t * space) {
  const GVEC_Svg_Instance * inst;
  GVEC_Result r = gvec_lw_instance_of(b->ctx, el, &inst);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Style_Value v;
  bool usable;
  r = gvec_lw_read_property(b->ctx, inst, GVEC_SVG_PROP_COLOR_INTERPOLATION_FILTERS, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK) {
    return r;
  }
  *space = usable && gvec_lw_keyword_is(&v, "srgb") ? GSHSCENE_FILTER_SPACE_SRGB : GSHSCENE_FILTER_SPACE_LINEAR_RGB;
  return GVEC_OK;
}

static uint32_t blend_mode(const char * s, size_t n, bool * ok) {
  static const struct {
    const char * word;
    uint32_t op;
  } modes[] = {{"normal", GSHSCENE_COMPOSITE_SRC_OVER}, {"multiply", GSHSCENE_COMPOSITE_MULTIPLY}, {"screen", GSHSCENE_COMPOSITE_SCREEN},
      {"overlay", GSHSCENE_COMPOSITE_OVERLAY}, {"darken", GSHSCENE_COMPOSITE_DARKEN}, {"lighten", GSHSCENE_COMPOSITE_LIGHTEN},
      {"color-dodge", GSHSCENE_COMPOSITE_COLOR_DODGE}, {"color-burn", GSHSCENE_COMPOSITE_COLOR_BURN},
      {"hard-light", GSHSCENE_COMPOSITE_HARD_LIGHT}, {"soft-light", GSHSCENE_COMPOSITE_SOFT_LIGHT},
      {"difference", GSHSCENE_COMPOSITE_DIFFERENCE}, {"exclusion", GSHSCENE_COMPOSITE_EXCLUSION}, {"hue", GSHSCENE_COMPOSITE_HUE},
      {"saturation", GSHSCENE_COMPOSITE_SATURATION}, {"color", GSHSCENE_COMPOSITE_COLOR}, {"luminosity", GSHSCENE_COMPOSITE_LUMINOSITY}};
  *ok = true;
  for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
    if (word_is(s, n, modes[i].word)) {
      return modes[i].op;
    }
  }
  *ok = false;
  return 0;
}

/** The values of an attribute into the build's pool; false when it is not a list of numbers or the pool is full. */
static bool values_attr(fbuild * b, const GVEC_Svg_Element * el, const char * name, const double ** out, size_t * count) {
  const char * s;
  size_t n;
  *out = NULL;
  *count = 0;
  if (!raw(el, name, &s, &n)) {
    return true;
  }
  double * at = b->values + b->values_used;
  size_t got;
  if (!numbers(s, n, at, VALUE_POOL - b->values_used, &got)) {
    return false;
  }
  b->values_used += got;
  *out = at;
  *count = got;
  return true;
}

/** Scale a pair of user-unit lengths into the list's through the map and the box units; the pair in `v`. */
static void lengths_to_list(const fbuild * b, double v[2]) {
  if (b->prim_box) {
    v[0] *= b->box[2];
    v[1] *= b->box[3];
  }
  double lx, ly;
  fmap_lengths(&b->map, v[0], v[1], &lx, &ly);
  v[0] = lx;
  v[1] = ly;
}

/** One `feXxx` element as a primitive (and the helpers its inputs add). */
static GVEC_Result primitive(fbuild * b, const GVEC_Svg_Element * el, const GVEC_Svg_Element * filter) {
  (void) filter;
  const GVEC_Svg_Kind kind = gvec_svg_element_kind(el);
  GSHSCENE_Filter_Input in0 = {0, 0}, in1 = {0, 0};
  bool ok = true;
  GVEC_Result r = GVEC_OK;
  uint32_t space;
  r = space_of(b, el, &space);
  if (r != GVEC_OK) {
    return r;
  }
  /* the inputs first: a removed keyword adds its flood before this primitive */
  const bool two = kind == GVEC_SVG_KIND_FE_BLEND || kind == GVEC_SVG_KIND_FE_COMPOSITE || kind == GVEC_SVG_KIND_FE_DISPLACEMENT_MAP;
  const bool none = kind == GVEC_SVG_KIND_FE_FLOOD;
  size_t merge_first = b->merge_used;
  size_t merge_count = 0;
  if (kind == GVEC_SVG_KIND_FE_MERGE) {
    for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(el); c; c = gvec_svg_element_next_sibling(c)) {
      if (gvec_svg_element_kind(c) != GVEC_SVG_KIND_FE_MERGE_NODE) {
        continue;
      }
      if (b->merge_used >= MERGE_POOL || merge_count >= GSHSCENE_FILTER_MAX_MERGE) {
        return refuse_with(b, "an feMerge has more nodes than the waist carries");
      }
      r = input_of(b, c, "in", &b->merge_inputs[b->merge_used], &ok);
      if (r != GVEC_OK || !ok) {
        return r;
      }
      b->merge_used++;
      merge_count++;
    }
    if (merge_count == 0) {
      GSHSCENE_Filter_Input none_in = {GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC, 0};
      b->merge_inputs[b->merge_used++] = none_in;
      merge_count = 1;
    }
  }
  else if (!none && kind != GVEC_SVG_KIND_FE_TURBULENCE) {
    r = input_of(b, el, "in", &in0, &ok);
    if (r != GVEC_OK || !ok) {
      return r;
    }
    if (two) {
      r = input_of(b, el, "in2", &in1, &ok);
      if (r != GVEC_OK || !ok) {
        return r;
      }
    }
  }
  GSHSCENE_Filter_Primitive * p;
  long at = add_prim(b, &p);
  if (at < 0) {
    return refuse_with(b, "the filter has more primitives than the waist carries");
  }
  p->color_space = space;
  GSHSCENE_Filter_Input * slot = b->inputs[at];
  slot[0] = in0;
  slot[1] = in1;
  if (!none) {
    p->input_count = two ? 2 : 1;
    p->inputs = slot;
  }
  /* the subregion, put into the list's space: coordinate by coordinate under a positive scale, whole under a mirror or a quarter turn */
  {
    static const char * const names[4] = {"x", "y", "width", "height"};
    double sub[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; k++) {
      bool present;
      if (!length_attr(el, names[k], b->prim_box, (k & 1) ? b->vh : b->vw, &present, &sub[k])) {
        b->invalid = true;
        return GVEC_OK;
      }
      if (present) {
        p->subregion_mask |= 1u << k;
      }
    }
    if (b->prim_box) {
      for (int k = 0; k < 4; k++) {
        if (p->subregion_mask & (1u << k)) {
          sub[k] = k == 0 ? b->box[0] + sub[k] * b->box[2] : k == 1 ? b->box[1] + sub[k] * b->box[3] : k == 2 ? sub[k] * b->box[2] : sub[k] * b->box[3];
        }
      }
    }
    if (((p->subregion_mask & 4u) && sub[2] < 0) || ((p->subregion_mask & 8u) && sub[3] < 0)) {
      b->invalid = true;
      return GVEC_OK;
    }
    if (p->subregion_mask) {
      const bool plain = !b->map.swap && b->map.a > 0 && b->map.d > 0;
      if (plain) {
        p->subregion[0] = b->map.a * sub[0] + b->map.e;
        p->subregion[1] = b->map.d * sub[1] + b->map.f;
        p->subregion[2] = b->map.a * sub[2];
        p->subregion[3] = b->map.d * sub[3];
      }
      else if (p->subregion_mask == 15u) {
        fmap_rect(&b->map, sub, p->subregion);
      }
      else {
        return refuse_with(b, "a partial primitive subregion under a mirrored or turned map is not lowered");
      }
    }
  }
  double v2[2];
  switch (kind) {
    case GVEC_SVG_KIND_FE_OFFSET: {
      double dx, dy;
      p->kind = GSHSCENE_FILTER_OFFSET;
      if (!number_attr(el, "dx", 0, &dx) || !number_attr(el, "dy", 0, &dy)) {
        b->invalid = true;
        return GVEC_OK;
      }
      if (b->prim_box) {
        dx *= b->box[2];
        dy *= b->box[3];
      }
      fmap_vector(&b->map, dx, dy, &p->dx, &p->dy);
      break;
    }
    case GVEC_SVG_KIND_FE_FLOOD:
      p->kind = GSHSCENE_FILTER_FLOOD;
      r = flood_of(b, el, p);
      if (r != GVEC_OK || b->refused) {
        return r;
      }
      break;
    case GVEC_SVG_KIND_FE_MERGE:
      p->kind = GSHSCENE_FILTER_MERGE;
      p->input_count = merge_count;
      p->inputs = &b->merge_inputs[merge_first];
      break;
    case GVEC_SVG_KIND_FE_BLEND: {
      const char * s;
      size_t n;
      p->kind = GSHSCENE_FILTER_BLEND;
      p->mode = GSHSCENE_COMPOSITE_SRC_OVER;
      if (raw(el, "mode", &s, &n)) {
        p->mode = blend_mode(s, n, &ok);
        if (!ok) {
          p->mode = GSHSCENE_COMPOSITE_SRC_OVER; /* an unknown mode is normal */
        }
      }
      break;
    }
    case GVEC_SVG_KIND_FE_COMPOSITE: {
      const char * s;
      size_t n;
      p->kind = GSHSCENE_FILTER_COMPOSITE;
      p->mode = GSHSCENE_FILTER_COMPOSITE_OVER;
      if (raw(el, "operator", &s, &n)) {
        p->mode = word_is(s, n, "in") ? GSHSCENE_FILTER_COMPOSITE_IN : word_is(s, n, "out") ? GSHSCENE_FILTER_COMPOSITE_OUT
            : word_is(s, n, "atop") ? GSHSCENE_FILTER_COMPOSITE_ATOP : word_is(s, n, "xor") ? GSHSCENE_FILTER_COMPOSITE_XOR
            : word_is(s, n, "arithmetic") ? GSHSCENE_FILTER_COMPOSITE_ARITHMETIC : GSHSCENE_FILTER_COMPOSITE_OVER;
      }
      if (p->mode == GSHSCENE_FILTER_COMPOSITE_ARITHMETIC) {
        static const char * const k[4] = {"k1", "k2", "k3", "k4"};
        for (int i = 0; i < 4; i++) {
          if (!number_attr(el, k[i], 0, &p->k[i])) {
            b->invalid = true;
            return GVEC_OK;
          }
        }
      }
      break;
    }
    case GVEC_SVG_KIND_FE_COLOR_MATRIX: {
      const char * s;
      size_t n;
      const double * vals;
      size_t count;
      p->kind = GSHSCENE_FILTER_COLOR_MATRIX;
      p->mode = GSHSCENE_FILTER_MATRIX_MATRIX;
      if (raw(el, "type", &s, &n)) {
        p->mode = word_is(s, n, "saturate") ? GSHSCENE_FILTER_MATRIX_SATURATE : word_is(s, n, "hueRotate") ? GSHSCENE_FILTER_MATRIX_HUE_ROTATE
            : word_is(s, n, "luminanceToAlpha") ? GSHSCENE_FILTER_MATRIX_LUMINANCE_TO_ALPHA : GSHSCENE_FILTER_MATRIX_MATRIX;
      }
      if (!values_attr(b, el, "values", &vals, &count)) {
        b->invalid = true;
        return GVEC_OK;
      }
      if (p->mode == GSHSCENE_FILTER_MATRIX_MATRIX) {
        static const double identity[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
        if (count == 20) {
          p->value_count = 20;
          p->values = vals;
        }
        else {
          if (b->values_used + 20 > VALUE_POOL) {
            return refuse_with(b, "the filter's values are past what the waist carries");
          }
          memcpy(b->values + b->values_used, identity, sizeof identity);
          p->value_count = 20;
          p->values = b->values + b->values_used;
          b->values_used += 20; /* a matrix of the wrong length is the identity (an error the browsers and resvg answer so) */
        }
      }
      else if (p->mode == GSHSCENE_FILTER_MATRIX_SATURATE || p->mode == GSHSCENE_FILTER_MATRIX_HUE_ROTATE) {
        if (b->values_used + 1 > VALUE_POOL) {
          return refuse_with(b, "the filter's values are past what the waist carries");
        }
        double v = count >= 1 ? vals[0] : (p->mode == GSHSCENE_FILTER_MATRIX_SATURATE ? 1 : 0);
        if (p->mode == GSHSCENE_FILTER_MATRIX_SATURATE && v < 0) {
          v = 0;
        }
        b->values[b->values_used] = v;
        p->value_count = 1;
        p->values = b->values + b->values_used;
        b->values_used++;
      }
      break;
    }
    case GVEC_SVG_KIND_FE_COMPONENT_TRANSFER: {
      p->kind = GSHSCENE_FILTER_COMPONENT_TRANSFER;
      for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(el); c; c = gvec_svg_element_next_sibling(c)) {
        const GVEC_Svg_Kind ck = gvec_svg_element_kind(c);
        const int ch = ck == GVEC_SVG_KIND_FE_FUNC_R ? 0 : ck == GVEC_SVG_KIND_FE_FUNC_G ? 1 : ck == GVEC_SVG_KIND_FE_FUNC_B ? 2
            : ck == GVEC_SVG_KIND_FE_FUNC_A ? 3 : -1;
        if (ch < 0) {
          continue;
        }
        GSHSCENE_Filter_Transfer * t = &p->transfer[ch];
        memset(t, 0, sizeof *t);
        const char * s;
        size_t n;
        if (!raw(c, "type", &s, &n)) {
          continue;
        }
        if (word_is(s, n, "table") || word_is(s, n, "discrete")) {
          const double * vals;
          size_t count;
          if (!values_attr(b, c, "tableValues", &vals, &count)) {
            b->invalid = true;
            return GVEC_OK;
          }
          if (count > 0) { /* an empty table is the identity */
            t->type = word_is(s, n, "table") ? GSHSCENE_FILTER_TRANSFER_TABLE : GSHSCENE_FILTER_TRANSFER_DISCRETE;
            t->count = count;
            t->table = vals;
          }
        }
        else if (word_is(s, n, "linear")) {
          t->type = GSHSCENE_FILTER_TRANSFER_LINEAR;
          if (!number_attr(c, "slope", 1, &t->slope) || !number_attr(c, "intercept", 0, &t->intercept)) {
            b->invalid = true;
            return GVEC_OK;
          }
        }
        else if (word_is(s, n, "gamma")) {
          t->type = GSHSCENE_FILTER_TRANSFER_GAMMA;
          if (!number_attr(c, "amplitude", 1, &t->amplitude) || !number_attr(c, "exponent", 1, &t->exponent) ||
              !number_attr(c, "offset", 0, &t->offset) || t->exponent < 0) {
            b->invalid = true;
            return GVEC_OK;
          }
        }
      }
      break;
    }
    case GVEC_SVG_KIND_FE_GAUSSIAN_BLUR:
    case GVEC_SVG_KIND_FE_DROP_SHADOW: {
      const bool shadow = kind == GVEC_SVG_KIND_FE_DROP_SHADOW;
      p->kind = shadow ? GSHSCENE_FILTER_DROP_SHADOW : GSHSCENE_FILTER_GAUSSIAN_BLUR;
      if (!pair_attr(el, "stdDeviation", shadow ? 2 : 0, v2)) {
        b->invalid = true;
        return GVEC_OK;
      }
      if (v2[0] < 0 || v2[1] < 0) {
        v2[0] = v2[1] = 0; /* a negative deviation disables the effect: the result is the input (Filter Effects) */
      }
      lengths_to_list(b, v2);
      p->sx = v2[0];
      p->sy = v2[1];
      if (shadow) {
        double dx, dy;
        if (!number_attr(el, "dx", 2, &dx) || !number_attr(el, "dy", 2, &dy)) {
          b->invalid = true;
          return GVEC_OK;
        }
        if (b->prim_box) {
          dx *= b->box[2];
          dy *= b->box[3];
        }
        fmap_vector(&b->map, dx, dy, &p->dx, &p->dy);
        r = flood_of(b, el, p);
        if (r != GVEC_OK || b->refused) {
          return r;
        }
      }
      else {
        const char * s;
        size_t n;
        if (raw(el, "edgeMode", &s, &n)) {
          p->edge_mode = word_is(s, n, "duplicate") ? GSHSCENE_FILTER_EDGE_DUPLICATE : word_is(s, n, "wrap") ? GSHSCENE_FILTER_EDGE_WRAP
              : GSHSCENE_FILTER_EDGE_NONE;
        }
      }
      break;
    }
    case GVEC_SVG_KIND_FE_MORPHOLOGY: {
      const char * s;
      size_t n;
      p->kind = GSHSCENE_FILTER_MORPHOLOGY;
      p->mode = raw(el, "operator", &s, &n) && word_is(s, n, "dilate") ? GSHSCENE_FILTER_MORPHOLOGY_DILATE : GSHSCENE_FILTER_MORPHOLOGY_ERODE;
      if (!pair_attr(el, "radius", 0, v2)) {
        b->invalid = true;
        return GVEC_OK;
      }
      if (v2[0] < 0 || v2[1] < 0) {
        v2[0] = v2[1] = 0; /* a negative radius disables the effect: the result is the input */
      }
      lengths_to_list(b, v2);
      p->sx = v2[0];
      p->sy = v2[1];
      break;
    }
    case GVEC_SVG_KIND_FE_TILE:
      p->kind = GSHSCENE_FILTER_TILE;
      break;
    case GVEC_SVG_KIND_FE_DISPLACEMENT_MAP: {
      const char * s;
      size_t n;
      double scale;
      p->kind = GSHSCENE_FILTER_DISPLACEMENT_MAP;
      if (b->prim_box) {
        return refuse_with(b, "a displacement map in the units of the bounding box is not lowered");
      }
      if (!number_attr(el, "scale", 0, &scale)) {
        b->invalid = true;
        return GVEC_OK;
      }
      p->scale = scale;
      p->x_channel = p->y_channel = GSHSCENE_FILTER_CHANNEL_A;
      static const char * const sel[2] = {"xChannelSelector", "yChannelSelector"};
      for (int k = 0; k < 2; k++) {
        uint32_t * ch = k == 0 ? &p->x_channel : &p->y_channel;
        if (raw(el, sel[k], &s, &n)) {
          *ch = word_is(s, n, "R") ? GSHSCENE_FILTER_CHANNEL_R : word_is(s, n, "G") ? GSHSCENE_FILTER_CHANNEL_G
              : word_is(s, n, "B") ? GSHSCENE_FILTER_CHANNEL_B : GSHSCENE_FILTER_CHANNEL_A;
        }
      }
      break;
    }
    case GVEC_SVG_KIND_FE_CONVOLVE_MATRIX: {
      const char * s;
      size_t n;
      double order[2], ox, oy, divisor, bias;
      const double * kernel;
      size_t count;
      p->kind = GSHSCENE_FILTER_CONVOLVE_MATRIX;
      if (b->prim_box) {
        /* kernelUnitLength is not used: no unit to scale */
      }
      if (!pair_attr(el, "order", 3, order) || order[0] != (double) (long) order[0] || order[1] != (double) (long) order[1] || order[0] < 1 ||
          order[1] < 1 || order[0] * order[1] > GSHSCENE_FILTER_MAX_KERNEL) {
        b->invalid = true;
        return GVEC_OK;
      }
      ox = order[0];
      oy = order[1];
      if (!values_attr(b, el, "kernelMatrix", &kernel, &count) || count != (size_t) (ox * oy)) {
        b->invalid = true;
        return GVEC_OK;
      }
      double sum = 0;
      for (size_t i = 0; i < count; i++) {
        sum += kernel[i];
      }
      if (!number_attr(el, "divisor", sum == 0 ? 1 : sum, &divisor) || divisor == 0 || !number_attr(el, "bias", 0, &bias)) {
        b->invalid = true;
        return GVEC_OK;
      }
      double tx, ty;
      if (!number_attr(el, "targetX", (double) (long) (ox / 2), &tx) || !number_attr(el, "targetY", (double) (long) (oy / 2), &ty) ||
          tx < 0 || ty < 0 || tx >= ox || ty >= oy || tx != (double) (long) tx || ty != (double) (long) ty) {
        b->invalid = true;
        return GVEC_OK;
      }
      p->order_x = (uint32_t) ox;
      p->order_y = (uint32_t) oy;
      p->target_x = (uint32_t) tx;
      p->target_y = (uint32_t) ty;
      p->divisor = divisor;
      p->bias = bias;
      p->value_count = count;
      p->values = kernel;
      p->edge_mode = GSHSCENE_FILTER_EDGE_DUPLICATE;
      if (raw(el, "edgeMode", &s, &n)) {
        p->edge_mode = word_is(s, n, "wrap") ? GSHSCENE_FILTER_EDGE_WRAP : word_is(s, n, "none") ? GSHSCENE_FILTER_EDGE_NONE
            : GSHSCENE_FILTER_EDGE_DUPLICATE;
      }
      p->preserve_alpha = raw(el, "preserveAlpha", &s, &n) && word_is(s, n, "true") ? 1u : 0u;
      break;
    }
    default:
      return refuse_with(b, "a filter primitive the waist's sinks do not draw yet (feImage, feTurbulence, lighting) is not lowered");
  }
  if (p->subregion_mask == 0) {
    memset(p->subregion, 0, sizeof p->subregion);
  }
  b->names[b->count] = NULL;
  b->name_len[b->count] = 0;
  {
    const char * s;
    size_t n;
    if (raw(el, "result", &s, &n) && n > 0) {
      b->names[b->count] = s;
      b->name_len[b->count] = n;
    }
  }
  b->prev = (long) b->count;
  b->count++;
  return GVEC_OK;
}


static bool is_filter_primitive(GVEC_Svg_Kind k) {
  return k >= GVEC_SVG_KIND_FE_BLEND && k <= GVEC_SVG_KIND_FE_TURBULENCE && k != GVEC_SVG_KIND_FE_DISTANT_LIGHT &&
      k != GVEC_SVG_KIND_FE_FUNC_A && k != GVEC_SVG_KIND_FE_FUNC_B && k != GVEC_SVG_KIND_FE_FUNC_G && k != GVEC_SVG_KIND_FE_FUNC_R &&
      k != GVEC_SVG_KIND_FE_MERGE_NODE && k != GVEC_SVG_KIND_FE_POINT_LIGHT && k != GVEC_SVG_KIND_FE_SPOT_LIGHT;
}

/** The filter an `href` (or `xlink:href`) of a `filter` element names in this document; NULL for none. */
static const GVEC_Svg_Element * href_of(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e) {
  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, "href");
  if (!a) {
    a = gvec_svg_element_find_attribute(e, "xlink:href");
  }
  if (!a) {
    return NULL;
  }
  size_t n;
  const char * t = gvec_svg_attribute_raw(a, &n);
  while (n > 0 && is_wsp(t[0])) {
    t++;
    n--;
  }
  if (n < 2 || t[0] != '#' || n > 255) {
    return NULL;
  }
  char id[256];
  memcpy(id, t + 1, n - 1);
  id[n - 1] = 0;
  return gvec_svg_find_by_id(ctx->document, id);
}

/** A `filter` element's attribute follows `href` to the filters it refers to: the element in the chain that has it, else the first. */
static const GVEC_Svg_Element * attr_owner(gvec_lw_ctx * ctx, const GVEC_Svg_Element * f, const char * name) {
  const GVEC_Svg_Element * e = f;
  for (int hops = 0; e && hops < 16; hops++, e = href_of(ctx, e)) {
    if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_FILTER && gvec_svg_element_find_attribute(e, name)) {
      return e;
    }
  }
  return f;
}

/** The element of the `href` chain whose children are the primitives: the first filter that has any. */
static const GVEC_Svg_Element * primitive_owner(gvec_lw_ctx * ctx, const GVEC_Svg_Element * f) {
  const GVEC_Svg_Element * e = f;
  for (int hops = 0; e && hops < 16; hops++, e = href_of(ctx, e)) {
    if (gvec_svg_element_kind(e) != GVEC_SVG_KIND_FILTER) {
      break;
    }
    for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(e); c; c = gvec_svg_element_next_sibling(c)) {
      if (is_filter_primitive(gvec_svg_element_kind(c))) {
        return e;
      }
    }
  }
  return f;
}

static bool units_box(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const char * name, bool fallback) {
  e = attr_owner(ctx, e, name);
  const char * s;
  size_t n;
  if (!raw(e, name, &s, &n)) {
    return fallback;
  }
  return word_is(s, n, "objectBoundingBox") ? true : word_is(s, n, "userSpaceOnUse") ? false : fallback;
}

static uint64_t hash2(uint64_t a, uint64_t b) {
  uint8_t buf[16];
  uint64_t parts[2] = {a, b};
  for (int p = 0; p < 2; p++) {
    for (int i = 0; i < 8; i++) {
      buf[p * 8 + i] = (uint8_t) (parts[p] >> (8 * i));
    }
  }
  return gvec_svg_hash64(buf, sizeof buf);
}

/** The filter element's resource for the element of @p inst, whose own map into the list is @p local. */
static GVEC_Result make_filter(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * local, const GVEC_Svg_Element * filter,
    gvec_lw_filter_state * state, const GSHSCENE_Resource ** out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *state = GVEC_LW_FILTER_REFUSED;
  *out = NULL;
  fbuild b;
  memset(&b, 0, sizeof b);
  b.ctx = ctx;
  b.target = e;
  b.prev = -1;
  if (!fmap_make(local, &b.map)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_FILTER, "a filter on an element whose map rotates or skews needs canonical space, which no sink draws yet");
  }
  const bool region_box = units_box(ctx, filter, "filterUnits", true);
  b.prim_box = units_box(ctx, filter, "primitiveUnits", false);
  GSHGEOM_Bounds box;
  box.empty = 1;
  GVEC_Result r = GVEC_OK;
  if (region_box || b.prim_box) {
    bool known;
    r = gvec_lw_inner_box(ctx, inst, &box, &known);
    if (r != GVEC_OK) {
      return r;
    }
    if (!known) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_FILTER, "a filter in the units of the bounding box of an element that holds more than shapes is not lowered yet");
    }
    if (box.empty || !(box.max.x > box.min.x) || !(box.max.y > box.min.y)) {
      *state = GVEC_LW_FILTER_ALL; /* The box cannot place the filter: the element is not rendered. */
      return GVEC_OK;
    }
    b.box[0] = box.min.x;
    b.box[1] = box.min.y;
    b.box[2] = box.max.x - box.min.x;
    b.box[3] = box.max.y - box.min.y;
  }
  double vw, vh;
  gvec_svg_instance_viewport(inst, &vw, &vh);
  b.vw = vw;
  b.vh = vh;
  /* the region, in the element's space (-10% and 120% by default) */
  double region[4];
  {
    static const char * const names[4] = {"x", "y", "width", "height"};
    static const double fallback[4] = {-10, -10, 120, 120};
    for (int k = 0; k < 4; k++) {
      bool present;
      double v;
      if (!length_attr(attr_owner(ctx, filter, names[k]), names[k], region_box, (k & 1) ? vh : vw, &present, &v)) {
        *state = GVEC_LW_FILTER_ALL;
        return GVEC_OK;
      }
      if (!present) {
        v = region_box ? fallback[k] / 100.0 : fallback[k] * ((k & 1) ? vh : vw) / 100.0;
      }
      region[k] = v;
    }
    if (region_box) {
      region[0] = b.box[0] + region[0] * b.box[2];
      region[1] = b.box[1] + region[1] * b.box[3];
      region[2] *= b.box[2];
      region[3] *= b.box[3];
    }
  }
  if (!(region[2] > 0) || !(region[3] > 0)) {
    *state = GVEC_LW_FILTER_ALL; /* A region of no size draws nothing. */
    return GVEC_OK;
  }
  double list_region[4];
  fmap_rect(&b.map, region, list_region);
  /* the primitives */
  b.prims = gvec_realloc(ctx->allocator, NULL, GSHSCENE_FILTER_MAX_PRIMITIVES * sizeof *b.prims);
  b.inputs = gvec_realloc(ctx->allocator, NULL, GSHSCENE_FILTER_MAX_PRIMITIVES * sizeof *b.inputs);
  b.merge_inputs = gvec_realloc(ctx->allocator, NULL, MERGE_POOL * sizeof *b.merge_inputs);
  b.values = gvec_realloc(ctx->allocator, NULL, VALUE_POOL * sizeof *b.values);
  if (!b.prims || !b.inputs || !b.merge_inputs || !b.values) {
    gvec_free(ctx->allocator, b.prims);
    gvec_free(ctx->allocator, b.inputs);
    gvec_free(ctx->allocator, b.merge_inputs);
    gvec_free(ctx->allocator, b.values);
    return GVEC_ERR_OOM;
  }
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(primitive_owner(ctx, filter)); c && r == GVEC_OK && !b.invalid && !b.refused;
      c = gvec_svg_element_next_sibling(c)) {
    if (is_filter_primitive(gvec_svg_element_kind(c))) {
      r = primitive(&b, c, filter);
    }
  }
  const GSHSCENE_Resource * resource = NULL;
  if (r == GVEC_OK && !b.refused) {
    if (b.invalid || b.count == 0) {
      /* A filter in error, or with no primitives, draws the element as nothing (Filter Effects). */
      *state = GVEC_LW_FILTER_ALL;
    }
    else {
      GSHSCENE_Filter f;
      memset(&f, 0, sizeof f);
      f.region_mode = GSHSCENE_FILTER_REGION_EXPLICIT;
      memcpy(f.region, list_region, sizeof f.region);
      f.color_space = GSHSCENE_FILTER_SPACE_LINEAR_RGB;
      f.primitive_count = b.count;
      f.primitives = b.prims;
      GSHSCENE_Result sr = gshscene_store_intern_filter(ctx->store, &f, &resource);
      if (sr == GSHSCENE_ERR_OOM) {
        r = GVEC_ERR_OOM;
      }
      else if (sr != GSHSCENE_OK) {
        r = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the filter is past what the waist can carry");
      }
      else {
        r = gvec_lw_hold(ctx, resource);
        if (r == GVEC_OK) {
          *out = resource;
          *state = GVEC_LW_FILTER_NODE;
        }
      }
    }
  }
  gvec_free(ctx->allocator, b.prims);
  gvec_free(ctx->allocator, b.inputs);
  gvec_free(ctx->allocator, b.merge_inputs);
  gvec_free(ctx->allocator, b.values);
  return r;
}

GVEC_Result gvec_lw_filter_begin(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_filter_state * state,
    gvec_lw_node ** out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *state = GVEC_LW_FILTER_NONE;
  *out = NULL;
  GVEC_Svg_Style_Value v;
  bool usable;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_FILTER, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK) {
    return r;
  }
  if (!usable) {
    *state = GVEC_LW_FILTER_REFUSED;
    return GVEC_OK;
  }
  if (v.reference_count == 0) {
    return GVEC_OK;
  }
  if (v.reference_count != 1 || !v.references[0].is_url || !v.references[0].local) {
    *state = GVEC_LW_FILTER_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_FILTER, "a filter function, a list of filters or a filter of another document is not lowered yet");
  }
  const GVEC_Svg_Style_Reference * ref = &v.references[0];
  const GVEC_Svg_Element * target = ref->target;
  if (!target && ref->cut && ref->id) {
    target = gvec_svg_find_by_id(ctx->document, ref->id);
  }
  if (!target || gvec_svg_element_kind(target) != GVEC_SVG_KIND_FILTER) {
    *state = GVEC_LW_FILTER_ALL; /* A reference to what is no filter: the element is not rendered. */
    return GVEC_OK;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    *state = GVEC_LW_FILTER_REFUSED;
    return r;
  }
  if (v.transform.depends_on_box) {
    *state = GVEC_LW_FILTER_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TRANSFORM_BOX, "a transform with percentages of a bounding box is not lowered yet");
  }
  GSHGEOM_Map local;
  if (!gvec_lw_compose(frame, &v.transform.map, &local)) {
    *state = GVEC_LW_FILTER_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's transform leaves the range the waist can carry");
  }
  const GSHSCENE_Resource * resource = NULL;
  r = make_filter(ctx, inst, &local, target, state, &resource);
  if (r != GVEC_OK || *state != GVEC_LW_FILTER_NODE) {
    return r;
  }
  gvec_lw_node * g = gvec_lw_new_node(ctx, GVEC_LW_GROUP);
  if (!g) {
    return GVEC_ERR_OOM;
  }
  g->opacity = 1;
  g->filter = resource;
  g->key = gvec_lw_key(e->key, hash2(ctx->chain, 0), GVEC_LW_ROLE_FILTER);
  g->site = gvec_lw_new_site(ctx, e, g->key);
  if (g->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  *out = g;
  return GVEC_OK;
}
