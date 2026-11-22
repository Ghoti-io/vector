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
 * feGaussianBlur, feDropShadow, feMorphology, feConvolveMatrix, feTile, feDisplacementMap, feTurbulence, the two lightings and feImage of an
 * image (a `data:` URI or a file under the options' image root: lower_image.c) or of an element of the document (a slot of the filter's picture,
 * FL25); an element that names a filter with any other primitive is refused whole.  The waist's filter is in the space of the group's items, the list's, so the filter's lengths and
 * regions are put into it through the element's own map, which must be axis aligned (a scale, a mirror, a quarter turn and a translation:
 * the filter of a rotated or skewed element needs canonical space, which no sink draws yet).
 */

#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/filter.h>

#include "../../../core/core_internal.h"
#include "../svg_grammar.h"
#include "../style/svg_used.h"
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
static bool length_attr(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const char * name, bool box_units, double basis, bool * present, double * out,
    GVEC_Result * error) {
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
  bool usable;
  *error = gvec_lw_unit_length(ctx, e, &len, out, &usable);
  return *error == GVEC_OK && usable;
}

/* --- the filter ------------------------------------------------------------------------------------------------------- */

typedef struct fbuild {
  gvec_lw_ctx * ctx;
  const GVEC_Svg_Element * target; /* the element the filter is on, for the refusals */
  fmap map;
  GSHGEOM_Map local;               /* the element's map into the list: what `map` is made of */
  double region[4];                /* the filter region in the element's space: x y width height (feImage fills it by default) */
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
  const GSHSCENE_Resource * slot_pictures[GSHSCENE_FILTER_MAX_PRIMITIVES]; /* the pictures of the elements the feImages draw, one a slot (FL25), each held here */
  GSHGEOM_Map slot_maps[GSHSCENE_FILTER_MAX_PRIMITIVES];                   /* and where each is placed: the element's space to the list's */
  size_t slot_count;
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

/*
 * `currentColor` where the element has no `color` of its own: the foreground the lowering is given, black when it sets none.  A filter holds its
 * colors (it is interned by its content), so unlike a fill it cannot follow a foreground that the application changes later; the color is fixed
 * when the document is lowered, and a notice says so.
 */
static GVEC_Result foreground_color(fbuild * b, const GVEC_Svg_Element * el, const GVEC_Svg_Color * c, GSHSCENE_Color * out) {
  memset(out, 0, sizeof *out);
  const GVEC_Lower_Options * o = b->ctx->options;
  if (o && (o->flags & GVEC_LOWER_SET_FOREGROUND)) {
    *out = o->foreground;
  }
  else {
    out->rgba[3] = 1;
  }
  out->rgba[3] *= c->color.rgba[3];
  return gvec_diag_add(b->ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_STYLE_INVALID, 0, el->node.line, el->node.column, el->node.offset, el->name,
      "currentColor in a filter is the foreground at the time of lowering: a filter holds its colors");
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
  if (!c) {
    return refuse_with(b, "a flood color that is not a plain color is not lowered");
  }
  if (c->kind == GVEC_SVG_COLOR_FOREGROUND) {
    r = foreground_color(b, el, c, &p->color);
    if (r != GVEC_OK) {
      return r;
    }
  }
  else {
    p->color.rgba[0] = c->color.rgba[0];
    p->color.rgba[1] = c->color.rgba[1];
    p->color.rgba[2] = c->color.rgba[2];
    p->color.rgba[3] = c->color.rgba[3];
    p->color.space = c->color.space;
  }
  double o = opacity.kind == GVEC_SVG_STYLE_NUMBER ? opacity.number : 1;
  p->opacity = o < 0 ? 0 : (o > 1 ? 1 : o);
  return GVEC_OK;
}

/** The color of the `lighting-color` property of an element, as the primitive's `color` (opaque: its alpha is not a light's). */
static GVEC_Result lighting_color_of(fbuild * b, const GVEC_Svg_Element * el, GSHSCENE_Filter_Primitive * p) {
  const GVEC_Svg_Instance * inst;
  GVEC_Result r = gvec_lw_instance_of(b->ctx, el, &inst);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Style_Value color;
  bool usable;
  r = gvec_lw_read_property(b->ctx, inst, GVEC_SVG_PROP_LIGHTING_COLOR, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &color, &usable);
  if (r != GVEC_OK || !usable) {
    b->refused = true;
    return r;
  }
  const GVEC_Svg_Color * c = color.kind == GVEC_SVG_STYLE_COLOR ? &color.color : color.kind == GVEC_SVG_STYLE_PAINT ? &color.paint.color : NULL;
  if (!c) {
    return refuse_with(b, "a lighting color that is not a plain color is not lowered");
  }
  if (c->kind == GVEC_SVG_COLOR_FOREGROUND) {
    r = foreground_color(b, el, c, &p->color);
    p->color.rgba[3] = 1;
    return r;
  }
  p->color.rgba[0] = c->color.rgba[0];
  p->color.rgba[1] = c->color.rgba[1];
  p->color.rgba[2] = c->color.rgba[2];
  p->color.rgba[3] = 1;
  p->color.space = c->color.space;
  return GVEC_OK;
}

/** The light source child of a lighting primitive, in the list's space; false when there is none or it is invalid. */
static bool light_of(const fbuild * b, const GVEC_Svg_Element * el, GSHSCENE_Filter_Light * lt) {
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(el); c; c = gvec_svg_element_next_sibling(c)) {
    const GVEC_Svg_Kind k = gvec_svg_element_kind(c);
    memset(lt, 0, sizeof *lt);
    if (k == GVEC_SVG_KIND_FE_DISTANT_LIGHT) {
      lt->kind = GSHSCENE_FILTER_LIGHT_DISTANT;
      return number_attr(c, "azimuth", 0, &lt->azimuth) && number_attr(c, "elevation", 0, &lt->elevation);
    }
    if (k == GVEC_SVG_KIND_FE_POINT_LIGHT || k == GVEC_SVG_KIND_FE_SPOT_LIGHT) {
      double pos[3], at[3] = {0, 0, 0};
      if (!number_attr(c, "x", 0, &pos[0]) || !number_attr(c, "y", 0, &pos[1]) || !number_attr(c, "z", 0, &pos[2])) {
        return false;
      }
      double sx, sy;
      fmap_lengths(&b->map, 1, 1, &sx, &sy);
      const double zs = gmath_sqrt(sx * sy);
      double lx, ly;
      fmap_vector(&b->map, pos[0], pos[1], &lx, &ly);
      lt->kind = k == GVEC_SVG_KIND_FE_POINT_LIGHT ? GSHSCENE_FILTER_LIGHT_POINT : GSHSCENE_FILTER_LIGHT_SPOT;
      lt->position[0] = lx + b->map.e;
      lt->position[1] = ly + b->map.f;
      lt->position[2] = pos[2] * zs;
      if (k == GVEC_SVG_KIND_FE_SPOT_LIGHT) {
        if (!number_attr(c, "pointsAtX", 0, &at[0]) || !number_attr(c, "pointsAtY", 0, &at[1]) || !number_attr(c, "pointsAtZ", 0, &at[2]) ||
            !number_attr(c, "specularExponent", 1, &lt->spot_exponent)) {
          return false;
        }
        if (lt->spot_exponent < 0) {
          lt->spot_exponent = 1; /* a negative exponent is the default, as resvg, Chromium and Firefox draw it */
        }
        if (lt->spot_exponent == 0) {
          return false;
        }
        fmap_vector(&b->map, at[0], at[1], &lx, &ly);
        lt->points_at[0] = lx + b->map.e;
        lt->points_at[1] = ly + b->map.f;
        lt->points_at[2] = at[2] * zs;
        if (gvec_svg_element_find_attribute(c, "limitingConeAngle")) {
          lt->has_cone = 1;
          if (!number_attr(c, "limitingConeAngle", 0, &lt->cone_angle)) {
            return false;
          }
        }
      }
      return true;
    }
  }
  return false;
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
/* An empty result: transparent black, which is what a feImage with nothing to draw is (a dummy flood, as the reference does). */
static void transparent_result(GSHSCENE_Filter_Primitive * p) {
  p->kind = GSHSCENE_FILTER_FLOOD;
  p->input_count = 0;
  p->inputs = NULL;
  memset(&p->color, 0, sizeof p->color);
  p->opacity = 0;
}

/*
 * feImage: the image of its `href` (a `data:` URI, or a file under the options' image root) fitted into the primitive subregion by
 * `preserveAspectRatio` (default xMidYMid meet) and drawn by the waist's IMAGE primitive under a placement map into the list's space.
 * The subregion is the filter region where the element gives none of x, y, width and height, and is what clips a slice (FL4, FL17).
 *
 * An element of the document as the `href` (`#id`) is drawn as a `use` of it would be (element_primitive). A reference to nothing, an image
 * that cannot be used and a subregion of no size are an empty result.
 *
 * @param sub The subregion's x, y, width and height in the element's space, those the element gives (`p->subregion_mask`).
 */
/* What a lowering inside a slot's picture changes of the context, and puts back (the picture is a list of its own, one level and one picture deeper). */
typedef struct slot_scope {
  uint64_t chain;
  size_t open, levels;
  bool context_set;
  const GVEC_Svg_Instance * context_inst;
  const GVEC_Svg_Instance * owner;
  bool rendered, cycle;
  GSHGEOM_Map item_local;
  bool item_crisp;
} slot_scope;

static void scope_enter(gvec_lw_ctx * ctx, const GVEC_Svg_Element * el, slot_scope * s) {
  s->chain = ctx->chain;
  s->open = ctx->open;
  s->levels = ctx->levels;
  s->context_set = ctx->context_set;
  s->context_inst = ctx->context_inst;
  s->owner = ctx->filter_owner;
  s->rendered = ctx->filter_rendered;
  s->cycle = ctx->filter_cycle;
  s->item_local = ctx->item_local;
  s->item_crisp = ctx->item_crisp;
  uint8_t buf[16];
  for (int i = 0; i < 8; i++) {
    buf[i] = (uint8_t) (s->chain >> (8 * i));
    buf[8 + i] = (uint8_t) (el->key >> (8 * i));
  }
  ctx->levels += 2;
  ctx->picture_depth++;
  ctx->open = 0;
  ctx->use_depth++;
  ctx->chain = gvec_svg_hash64(buf, sizeof buf);
  ctx->context_set = false;
  ctx->context_inst = NULL;
  ctx->filter_cycle = false;
}

/* Put the context back; true when a filter being built was met again inside (a cycle). */
static bool scope_leave(gvec_lw_ctx * ctx, const slot_scope * s) {
  const bool cycled = ctx->filter_cycle;
  ctx->filter_cycle = s->cycle;
  ctx->filter_rendered = s->rendered;
  ctx->filter_owner = s->owner;
  ctx->context_set = s->context_set;
  ctx->context_inst = s->context_inst;
  ctx->item_local = s->item_local;
  ctx->item_crisp = s->item_crisp;
  ctx->chain = s->chain;
  ctx->use_depth--;
  ctx->open = s->open;
  ctx->picture_depth--;
  ctx->levels = s->levels;
  return cycled;
}

/* The hidden holder of a slot's picture. */
static GVEC_Result slot_holder(fbuild * b, const GVEC_Svg_Element * el, gvec_lw_node ** out) {
  gvec_lw_ctx * ctx = b->ctx;
  if (b->slot_count >= GSHSCENE_FILTER_MAX_PRIMITIVES || ctx->levels + 3 > ctx->caps.node_depth || ctx->picture_depth + 1 > ctx->caps.picture_depth) {
    *out = NULL;
    return refuse_with(b, "the element of an feImage nests pictures deeper than the scene's limits allow");
  }
  gvec_lw_node * holder = gvec_lw_new_node(ctx, GVEC_LW_USE);
  if (!holder) {
    return GVEC_ERR_OOM;
  }
  holder->key = gvec_lw_key(el->key, ctx->chain, GVEC_LW_ROLE_FILTER_IMAGE);
  holder->site = gvec_lw_new_site(ctx, el, holder->key);
  if (holder->site == GVEC_LW_NO_SITE) {
    return GVEC_ERR_OOM;
  }
  *out = holder;
  return GVEC_OK;
}

/* The holder's content as the picture of a new slot of the primitive, placed by `placed`; an empty holder, or a cycle, is a transparent result. */
static GVEC_Result slot_finish(fbuild * b, GSHSCENE_Filter_Primitive * p, gvec_lw_node * holder, bool cycled, const GSHGEOM_Map * placed) {
  gvec_lw_ctx * ctx = b->ctx;
  if (cycled || !holder->first) {
    transparent_result(p); /* a cycle through this filter, or content that draws nothing */
    return GVEC_OK;
  }
  const GSHSCENE_Resource * picture = NULL;
  GVEC_Result r = gvec_lw_emit_picture(ctx, holder, &picture);
  if (r != GVEC_OK) {
    return r;
  }
  if (!picture) {
    return refuse_with(b, "the content of an feImage is past what the waist can carry");
  }
  p->kind = GSHSCENE_FILTER_IMAGE;
  p->use_picture = 1;
  p->slot = (uint32_t) b->slot_count;
  b->slot_pictures[b->slot_count] = picture;
  b->slot_maps[b->slot_count] = *placed;
  b->slot_count++;
  return GVEC_OK;
}

/*
 * feImage of an element (Filter Effects 1, FL25): the element is lowered as a `use` of it would be, into a picture of its own, in the user space of
 * the filtered element moved by the subregion's `x` and `y` (the filter region's where the element gives none): the primitive draws a slot of the
 * filter's picture, which is the instance of that picture under that placement.  The element's own `transform`, opacity, clip, mask and filter apply
 * as they do anywhere.  An element that draws nothing, or whose own filter is one of those being built (a cycle through the filter), is an empty
 * result; nesting past GVEC_LW_FILTER_NESTING filters or the scene's limits is refused.
 */
static GVEC_Result element_primitive(fbuild * b, const GVEC_Svg_Element * el, GSHSCENE_Filter_Primitive * p, const GVEC_Svg_Element * target,
    const double sub[4]) {
  gvec_lw_ctx * ctx = b->ctx;
  const double x0 = (p->subregion_mask & 1u) ? sub[0] : b->region[0];
  const double y0 = (p->subregion_mask & 2u) ? sub[1] : b->region[1];
  if (x0 - x0 != 0 || y0 - y0 != 0) {
    transparent_result(p); /* an edge that is not finite */
    return GVEC_OK;
  }
  GSHGEOM_Map shift, placed;
  if (gshgeom_map_translate(2, x0, y0, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&b->local, &shift, &placed)) {
    return refuse_with(b, "the element of an feImage is placed past the range the waist can carry");
  }
  const GVEC_Svg_Instance * inst = NULL;
  GVEC_Result r = gvec_lw_instance_of(ctx, target, &inst);
  if (r != GVEC_OK) {
    return r;
  }
  if (!inst) {
    transparent_result(p);
    return GVEC_OK;
  }
  gvec_lw_node * holder;
  r = slot_holder(b, el, &holder);
  if (r != GVEC_OK || !holder) {
    return r;
  }
  slot_scope scope;
  scope_enter(ctx, el, &scope);
  GSHGEOM_Map identity;
  r = gshgeom_map_identity(2, &identity) == GSHGEOM_OK ? gvec_lw_element(ctx, inst, &identity, holder) : GVEC_ERR_INTERNAL;
  const bool cycled = scope_leave(ctx, &scope);
  if (r != GVEC_OK) {
    return r;
  }
  return slot_finish(b, p, holder, cycled, &placed);
}

/*
 * feImage of an SVG document (R3): the document is lowered by a context of its own into a picture, clipped to its own size and placed into the
 * subregion by `preserveAspectRatio` as a raster is; the slot is that picture under the element's map.
 */
static GVEC_Result document_primitive(fbuild * b, const GVEC_Svg_Element * el, GSHSCENE_Filter_Primitive * p, const gvec_lw_image * image,
    const GSHGEOM_Map * placed) {
  gvec_lw_ctx * ctx = b->ctx;
  gvec_lw_node * holder;
  GVEC_Result r = slot_holder(b, el, &holder);
  if (r != GVEC_OK || !holder) {
    return r;
  }
  slot_scope scope;
  scope_enter(ctx, el, &scope);
  GSHGEOM_Map identity;
  gvec_lw_node * use = NULL;
  r = gshgeom_map_identity(2, &identity) == GSHGEOM_OK ? gvec_lw_svg_image_node(ctx, el, &identity, image, placed, &use) : GVEC_ERR_INTERNAL;
  if (r == GVEC_OK && use) {
    r = gvec_lw_attach(ctx, holder, use);
  }
  const bool cycled = scope_leave(ctx, &scope);
  if (r != GVEC_OK) {
    return r;
  }
  return slot_finish(b, p, holder, cycled, &b->local);
}

/*
 * D-FL10 bounds a length, a factor and a coefficient of a filter to GSHSCENE_FILTER_VALUE_MAX in magnitude.  A document may say more
 * (`values="99999"` of a saturate, a region of 100000 units), and what it means is the same wherever the numbers are beyond the bound: a
 * coefficient that large saturates the clamped result, an edge that far out is past the canvas.  So a magnitude past the bound is held to it,
 * and one notice says so; the filter is not refused for it.
 */
static void hold(double * x, bool * hit) {
  const double max = GSHSCENE_FILTER_VALUE_MAX;
  if (*x > max) {
    *x = max;
    *hit = true;
  }
  else if (*x < -max) {
    *x = -max;
    *hit = true;
  }
}

static void hold_all(double * x, size_t n, bool * hit) {
  for (size_t i = 0; i < n; i++) {
    hold(&x[i], hit);
  }
}

static void hold_primitive(GSHSCENE_Filter_Primitive * p, bool * hit) {
  hold_all(p->subregion, 4, hit);
  hold(&p->dx, hit);
  hold(&p->dy, hit);
  hold_all(p->k, 4, hit);
  hold(&p->sx, hit);
  hold(&p->sy, hit);
  hold(&p->surface_scale, hit);
  hold(&p->constant, hit);
  hold(&p->divisor, hit);
  hold(&p->bias, hit);
  hold(&p->scale, hit);
  hold_all(p->light.position, 3, hit);
  hold_all(p->light.points_at, 3, hit);
  for (int c = 0; c < 4; c++) {
    GSHSCENE_Filter_Transfer * t = &p->transfer[c];
    hold(&t->slope, hit);
    hold(&t->intercept, hit);
    hold(&t->amplitude, hit);
    hold(&t->exponent, hit);
    hold(&t->offset, hit);
  }
}

/* The filter's picture: an instance of each element's picture under its placement, in the order of the slots (FL25); NULL for none. */
static GVEC_Result slots_picture(fbuild * b, const GSHSCENE_Resource ** out) {
  gvec_lw_ctx * ctx = b->ctx;
  *out = NULL;
  if (b->slot_count == 0) {
    return GVEC_OK;
  }
  GSHSCENE_List * list = NULL;
  GSHSCENE_Result g = gshscene_list_create(ctx->scene_allocator, ctx->scene_limits, ctx->store, &list);
  for (size_t i = 0; g == GSHSCENE_OK && i < b->slot_count; i++) {
    g = gshscene_list_instance(list, b->slot_pictures[i], &b->slot_maps[i], 0, 0.0);
  }
  const GSHSCENE_Resource * picture = NULL;
  if (g == GSHSCENE_OK) {
    g = gshscene_list_close(list);
  }
  if (g == GSHSCENE_OK) {
    g = gshscene_store_intern_picture(ctx->store, list, &picture);
  }
  gshscene_list_destroy(list);
  if (g == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (g != GSHSCENE_OK) {
    return refuse_with(b, "the elements of the feImages are past what the waist can carry");
  }
  *out = picture;
  return GVEC_OK;
}

static void release_slots(fbuild * b) {
  for (size_t i = 0; i < b->slot_count; i++) {
    gshscene_resource_release(b->slot_pictures[i]);
  }
  b->slot_count = 0;
}

static GVEC_Result image_primitive(fbuild * b, const GVEC_Svg_Element * el, GSHSCENE_Filter_Primitive * p, const double sub[4]) {
  gvec_lw_ctx * ctx = b->ctx;
  const GVEC_Svg_Attribute * href = gvec_svg_element_find_attribute(el, "href");
  if (!href) {
    href = gvec_svg_element_find_attribute(el, "xlink:href");
  }
  GVEC_Svg_Reference ref;
  if (href && gvec_svg_attribute_class(href) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_reference(href, &ref) && ref.kind == GVEC_SVG_REF_INTERNAL) {
    const GVEC_Svg_Element * target = gvec_svg_find_by_id(ctx->document, ref.fragment);
    if (target) {
      return element_primitive(b, el, p, target, sub);
    }
    transparent_result(p);
    return GVEC_OK;
  }
  gvec_lw_image_state state;
  gvec_lw_image image;
  GVEC_Result r = gvec_lw_image_load(ctx, el, &state, &image);
  if (r != GVEC_OK) {
    return r;
  }
  if (state == GVEC_LW_IMAGE_REFUSED) {
    b->refused = true;
    return GVEC_OK;
  }
  if (state == GVEC_LW_IMAGE_NOTHING) {
    transparent_result(p);
    return GVEC_OK;
  }
  /* the viewport: the subregion as the waist reads it (FL4), the filter region where a value is not given */
  const double x0 = (p->subregion_mask & 1u) ? sub[0] : b->region[0];
  const double y0 = (p->subregion_mask & 2u) ? sub[1] : b->region[1];
  const double x1 = (p->subregion_mask & 4u) ? x0 + sub[2] : x0 + b->region[2]; /* a size not given is the region's, wherever x puts it */
  const double y1 = (p->subregion_mask & 8u) ? y0 + sub[3] : y0 + b->region[3];
  if (!(x1 - x0 > 0) || !(y1 - y0 > 0) || x1 - x1 != 0 || y1 - y1 != 0) { /* no area, or an edge that is not finite */
    transparent_result(p);
    return GVEC_OK;
  }
  GVEC_Svg_Preserve_Aspect_Ratio par = {GVEC_SVG_ALIGN_X_MID_Y_MID, 0, 0};
  const GVEC_Svg_Attribute * pa = gvec_svg_element_find_attribute(el, "preserveAspectRatio");
  if (pa && gvec_svg_attribute_class(pa) == GVEC_SVG_ATTR_TYPED) {
    gvec_svg_attribute_preserve_aspect_ratio(pa, &par);
  }
  GSHGEOM_Map placed;
  if (!gvec_lw_image_place(&image, &par, x0, y0, x1 - x0, y1 - y0, &placed)) {
    return refuse_with(b, "the image of an feImage is placed past the range the waist can carry");
  }
  if (image.svg) {
    return document_primitive(b, el, p, &image, &placed);
  }
  if (!gvec_lw_compose(&b->local, &placed, &p->placement)) {
    return refuse_with(b, "the image of an feImage is placed past the range the waist can carry");
  }
  const GVEC_Svg_Instance * inst;
  r = gvec_lw_instance_of(ctx, el, &inst);
  if (r != GVEC_OK) {
    return r;
  }
  uint32_t magnify, minify;
  bool usable;
  r = gvec_lw_image_filters(ctx, inst, &magnify, &minify, &usable);
  if (r != GVEC_OK || !usable) {
    b->refused = !usable;
    return r;
  }
  (void) minify;
  p->kind = GSHSCENE_FILTER_IMAGE;
  p->image = image.resource;
  p->rendering = magnify;
  return GVEC_OK;
}

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
  const bool none = kind == GVEC_SVG_KIND_FE_FLOOD || kind == GVEC_SVG_KIND_FE_TURBULENCE || kind == GVEC_SVG_KIND_FE_IMAGE;
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
  }
  else if (!none) {
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
  double sub[4] = {0, 0, 0, 0}; /* in the element's space: what feImage fits its image into */
  {
    static const char * const names[4] = {"x", "y", "width", "height"};
    for (int k = 0; k < 4; k++) {
      bool present;
      GVEC_Result unit_error = GVEC_OK;
      if (!length_attr(b->ctx, el, names[k], b->prim_box, (k & 1) ? b->vh : b->vw, &present, &sub[k], &unit_error)) {
        if (unit_error != GVEC_OK) {
          return unit_error;
        }
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
      if (merge_count == 0) { /* no feMergeNode: nothing to merge is transparent black, which the next primitive then takes as its input */
        transparent_result(p);
        break;
      }
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
            continue; /* a list that does not parse is no list: the function is the identity, as in every engine we probed */
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
      if (!number_attr(el, "divisor", (sum > -1e-9 && sum < 1e-9) ? 1 : sum, &divisor) || divisor == 0 || !number_attr(el, "bias", 0, &bias)) {
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
    case GVEC_SVG_KIND_FE_TURBULENCE: {
      const char * s;
      size_t n;
      double freq[2], octaves, seed;
      p->kind = GSHSCENE_FILTER_TURBULENCE;
      if (!pair_attr(el, "baseFrequency", 0, freq) || !number_attr(el, "numOctaves", 1, &octaves) || !number_attr(el, "seed", 0, &seed)) {
        b->invalid = true;
        return GVEC_OK;
      }
      if (freq[0] < 0 || freq[1] < 0) {
        b->invalid = true; /* a negative frequency is an error: the element is not rendered */
        return GVEC_OK;
      }
      /* The noise is a function of the point in the element's user space.  The waist's primitive is in the list's space, so its frequency is
       * per unit of that space (the user one divided by the map's scale) and its origin is where the user space's origin lies in it.  Only a
       * positive scale and a translation keep the noise lattice the lattice of the list's axes: a turn or a mirror would turn it. */
      if (b->map.a <= 0 || b->map.d <= 0) { /* a quarter turn has a == 0 */
        return refuse_with(b, "a turbulence under a map that turns or mirrors its element is not lowered: its noise lattice would turn with it");
      }
      p->sx = freq[0] / b->map.a;
      p->sy = freq[1] / b->map.d;
      p->dx = b->map.e;
      p->dy = b->map.f;
      p->octaves = octaves < 0 ? 0 : (octaves > GSHSCENE_FILTER_MAX_OCTAVES ? GSHSCENE_FILTER_MAX_OCTAVES : (uint32_t) octaves);
      p->seed = seed;
      p->mode = raw(el, "type", &s, &n) && word_is(s, n, "fractalNoise") ? GSHSCENE_FILTER_NOISE_FRACTAL : GSHSCENE_FILTER_NOISE_TURBULENCE;
      p->stitch_tiles = raw(el, "stitchTiles", &s, &n) && word_is(s, n, "stitch") ? 1u : 0u;
      break;
    }
    case GVEC_SVG_KIND_FE_DIFFUSE_LIGHTING:
    case GVEC_SVG_KIND_FE_SPECULAR_LIGHTING: {
      const bool spec = kind == GVEC_SVG_KIND_FE_SPECULAR_LIGHTING;
      p->kind = spec ? GSHSCENE_FILTER_SPECULAR_LIGHTING : GSHSCENE_FILTER_DIFFUSE_LIGHTING;
      if (!number_attr(el, "surfaceScale", 1, &p->surface_scale) || !number_attr(el, spec ? "specularConstant" : "diffuseConstant", 1, &p->constant) ||
          p->constant < 0 || (spec && (!number_attr(el, "specularExponent", 1, &p->exponent) || p->exponent < 1 || p->exponent > 128)) ||
          !light_of(b, el, &p->light)) {
        b->invalid = true;
        return GVEC_OK;
      }
      r = lighting_color_of(b, el, p);
      if (r != GVEC_OK || b->refused) {
        return r;
      }
      break;
    }
    case GVEC_SVG_KIND_FE_IMAGE:
      r = image_primitive(b, el, p, sub);
      if (r != GVEC_OK || b->refused) {
        return r;
      }
      break;
    default:
      return refuse_with(b, "a filter primitive the waist's sinks do not draw yet is not lowered");
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
      GVEC_Result unit_error = GVEC_OK;
      if (!length_attr(ctx, attr_owner(ctx, filter, names[k]), names[k], region_box, (k & 1) ? vh : vw, &present, &v, &unit_error)) {
        if (unit_error != GVEC_OK) {
          return unit_error;
        }
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
  bool held = false;
  {
    /* an edge of the region past the bound is held to it, and the size follows from the edges */
    const double x1 = list_region[0] + list_region[2], y1 = list_region[1] + list_region[3];
    double e[4] = {list_region[0], list_region[1], x1, y1};
    hold_all(e, 4, &held);
    list_region[0] = e[0];
    list_region[1] = e[1];
    list_region[2] = e[2] - e[0];
    list_region[3] = e[3] - e[1];
    hold_all(list_region + 2, 2, &held); /* the size is a length too: the region then ends short of the edge, still past any canvas */
  }
  memcpy(b.region, region, sizeof b.region);
  b.local = *local;
  /* an feImage of an element lowers that element, whose own filter may be this one: a filter met again is a cycle (the feImage draws nothing) */
  for (size_t i = 0; i < ctx->filter_active_count; i++) {
    if (ctx->filter_active[i] == filter) {
      ctx->filter_cycle = true;
      *state = GVEC_LW_FILTER_ALL;
      return GVEC_OK;
    }
  }
  if (ctx->filter_active_count >= GVEC_LW_FILTER_NESTING) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "filters nest deeper through their feImages than the lowering allows");
  }
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
  ctx->filter_active[ctx->filter_active_count++] = filter;
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(primitive_owner(ctx, filter)); c && r == GVEC_OK && !b.invalid && !b.refused;
      c = gvec_svg_element_next_sibling(c)) {
    if (is_filter_primitive(gvec_svg_element_kind(c))) {
      r = primitive(&b, c, filter);
    }
  }
  ctx->filter_active_count--;
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
      for (size_t i = 0; i < b.count; i++) {
        hold_primitive(&b.prims[i], &held);
      }
      hold_all(b.values, b.values_used, &held);
      if (held) {
        r = gvec_diag_add(ctx->diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_STYLE_INVALID, 0, e->node.line, e->node.column, e->node.offset, e->name,
            "a length, factor or coefficient of the filter past the waist's bound is held to it");
      }
      const GSHSCENE_Resource * slots = NULL;
      if (r == GVEC_OK) {
        r = slots_picture(&b, &slots);
      }
      f.picture = slots;
      GSHSCENE_Result sr = r == GVEC_OK ? gshscene_store_intern_filter(ctx->store, &f, &resource) : GSHSCENE_OK;
      gshscene_resource_release(slots);
      if (r != GVEC_OK) {
        /* the slots' refusal is already recorded */
      }
      else if (sr == GSHSCENE_ERR_OOM) {
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
  release_slots(&b);
  gvec_free(ctx->allocator, b.prims);
  gvec_free(ctx->allocator, b.inputs);
  gvec_free(ctx->allocator, b.merge_inputs);
  gvec_free(ctx->allocator, b.values);
  return r;
}

/* --- CSS filter functions ---------------------------------------------------------------------------------------------- */

/** A CSS number at @p *pos of @p s: sign, digits, a point and digits (no exponent); false when there is none. */
static bool css_number(const char * s, size_t n, size_t * pos, double * out) {
  size_t i = *pos;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    i++;
  }
  double v = 0, scale = 1;
  bool digits = false, point = false;
  for (; i < n; i++) {
    if (s[i] >= '0' && s[i] <= '9') {
      digits = true;
      if (point) {
        scale /= 10;
        v += (s[i] - '0') * scale;
      }
      else {
        v = v * 10 + (s[i] - '0');
      }
    }
    else if (s[i] == '.' && !point) {
      point = true;
    }
    else {
      break;
    }
  }
  if (!digits) {
    return false;
  }
  *out = neg ? -v : v;
  *pos = i;
  return true;
}

/** A CSS number or percentage (`0.5`, `50%`) as a fraction; false for anything else. */
static bool css_amount(const char * s, size_t n, double fallback, double * out) {
  while (n > 0 && is_wsp(s[0])) {
    s++;
    n--;
  }
  while (n > 0 && is_wsp(s[n - 1])) {
    n--;
  }
  *out = fallback;
  if (n == 0) {
    return true;
  }
  size_t pos = 0;
  double v;
  if (!css_number(s, n, &pos, &v)) {
    return false;
  }
  if (pos < n && s[pos] == '%') {
    v /= 100.0;
    pos++;
  }
  if (pos != n) {
    return false;
  }
  *out = v;
  return true;
}

/** A CSS length in pixels (`2px`, `0`) at @p *pos of @p s; false when it is not one. */
static bool css_length(const char * s, size_t n, size_t * pos, double * out) {
  while (*pos < n && is_wsp(s[*pos])) {
    (*pos)++;
  }
  if (!css_number(s, n, pos, out)) {
    return false;
  }
  if (*pos + 2 <= n && s[*pos] == 'p' && s[*pos + 1] == 'x') {
    *pos += 2;
    return true;
  }
  return *out == 0.0;
}

static int hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/** A color of a `drop-shadow()`: `#rgb`, `#rrggbb`, `#rrggbbaa` or one of a few names; false for the rest. */
static bool css_color(const char * s, size_t n, double rgba[4]) {
  static const struct {
    const char * name;
    double c[4];
  } names[] = {{"black", {0, 0, 0, 1}}, {"currentcolor", {0, 0, 0, 1}}, {"currentColor", {0, 0, 0, 1}}, {"white", {1, 1, 1, 1}}, {"red", {1, 0, 0, 1}}, {"blue", {0, 0, 1, 1}}, {"transparent", {0, 0, 0, 0}},
      {"green", {0, 128.0 / 255, 0, 1}}, {"gray", {128.0 / 255, 128.0 / 255, 128.0 / 255, 1}}, {"grey", {128.0 / 255, 128.0 / 255, 128.0 / 255, 1}}};
  while (n > 0 && is_wsp(s[0])) {
    s++;
    n--;
  }
  while (n > 0 && is_wsp(s[n - 1])) {
    n--;
  }
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (word_is(s, n, names[i].name)) {
      memcpy(rgba, names[i].c, sizeof names[i].c);
      return true;
    }
  }
  if (n > 5 && strncmp(s, "rgb", 3) == 0) {
    /* rgb(r g b), rgb(r, g, b), rgba(r, g, b, a) and rgb(r g b / a): the computed form a style engine writes a color in */
    size_t i = s[3] == 'a' ? 5 : 4;
    double c[4] = {0, 0, 0, 1};
    for (int k = 0; k < 4; k++) {
      while (i < n && (is_wsp(s[i]) || s[i] == ',' || s[i] == '/')) {
        i++;
      }
      if (i >= n || s[i] == ')') {
        if (k < 3) {
          return false;
        }
        break;
      }
      if (!css_number(s, n, &i, &c[k])) {
        return false;
      }
      if (i < n && s[i] == '%') {
        c[k] = k < 3 ? c[k] * 255.0 / 100.0 : c[k] / 100.0;
        i++;
      }
    }
    for (int k = 0; k < 3; k++) {
      rgba[k] = (c[k] < 0 ? 0 : (c[k] > 255 ? 255 : c[k])) / 255.0;
    }
    rgba[3] = c[3] < 0 ? 0 : (c[3] > 1 ? 1 : c[3]);
    return true;
  }
  if (n < 2 || s[0] != '#') {
    return false;
  }
  const size_t d = n - 1;
  if (d != 3 && d != 6 && d != 8) {
    return false;
  }
  for (int k = 0; k < 4; k++) {
    rgba[k] = 1;
  }
  const size_t per = d == 3 ? 1 : 2;
  for (size_t k = 0; k * per < d; k++) {
    int hi = hex_digit(s[1 + k * per]), lo = per == 2 ? hex_digit(s[2 + k * per]) : hi;
    if (hi < 0 || lo < 0) {
      return false;
    }
    rgba[k] = (double) (hi * 16 + lo) / 255.0;
  }
  return true;
}

/** Whether a drop-shadow() color argument is the keyword `currentcolor`: the computed value writes it so, in lower case, first. */
static bool is_current_color(const char * s, size_t n) {
  return word_is(s, n, "currentcolor");
}

/** A color of a `drop-shadow()` that the subset of `css_color` does not read (a named color, `oklch()`, `lab()`, `color(display-p3 ...)`, a system color, a mix):
 * the style layer reads the text as it reads a property. `*ok` is false, with nothing refused, when the text is no color; the element is refused (as for any
 * color the waist cannot carry) when it is one that cannot be had. */
static GVEC_Result typed_color(fbuild * b, const char * text, size_t n, double rgba[4], GSHSCENE_Color_Space * space, bool * typed, bool * ok) {
  *ok = false;
  const GVEC_Svg_Instance * inst;
  GVEC_Result r = gvec_lw_instance_of(b->ctx, b->target, &inst);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Color c;
  memset(&c, 0, sizeof c);
  bool valid = false;
  r = gvec_svg_style_color_of_text(b->ctx->style, inst, b->target, text, n, &c, &valid);
  if (r == GVEC_ERR_UNSUPPORTED) {
    b->refused = true;
    return gvec_lw_refuse(b->ctx, b->target, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, "a color of a drop-shadow() is in a space the waist cannot carry");
  }
  if (r != GVEC_OK || !valid) {
    return r;
  }
  if (c.kind == GVEC_SVG_COLOR_FOREGROUND) {
    GSHSCENE_Color fg;
    r = foreground_color(b, b->target, &c, &fg);
    if (r != GVEC_OK || b->refused) {
      return r;
    }
    c.color = fg;
  }
  for (int k = 0; k < 4; k++) {
    rgba[k] = c.color.rgba[k];
  }
  *space = c.color.space;
  *typed = true;
  *ok = true;
  return GVEC_OK;
}

/** The `color` property of the filtered element, which a drop-shadow() with no color, or with `currentcolor`, takes (Filter Effects 1: "the missing used color is taken from the color property"). */
static GVEC_Result element_color(fbuild * b, double rgba[4]) {
  const GVEC_Svg_Instance * inst;
  GVEC_Result r = gvec_lw_instance_of(b->ctx, b->target, &inst);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Style_Value v;
  bool usable;
  r = gvec_lw_read_property(b->ctx, inst, GVEC_SVG_PROP_COLOR, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    b->refused = true;
    return r;
  }
  const GVEC_Svg_Color * c = v.kind == GVEC_SVG_STYLE_COLOR ? &v.color : v.kind == GVEC_SVG_STYLE_PAINT ? &v.paint.color : NULL;
  if (!c) {
    return GVEC_OK; /* not a color: black stays */
  }
  if (c->kind == GVEC_SVG_COLOR_FOREGROUND) {
    GSHSCENE_Color fg;
    r = foreground_color(b, b->target, c, &fg);
    for (int k = 0; k < 4; k++) {
      rgba[k] = fg.rgba[k];
    }
    return r;
  }
  for (int k = 0; k < 4; k++) {
    rgba[k] = c->color.rgba[k];
  }
  return GVEC_OK;
}

/** One function of a `filter` list as primitives of the build; false when it is not one the waist's primitives can say. */
static GVEC_Result css_function(fbuild * b, const char * text, bool * ok) {
  *ok = false;
  size_t n = strlen(text);
  const char * open = memchr(text, '(', n);
  if (!open || text[n - 1] != ')') {
    return GVEC_OK;
  }
  const size_t name_len = (size_t) (open - text);
  const char * args = open + 1;
  const size_t arg_len = n - name_len - 2;
  GSHSCENE_Filter_Primitive * p;
  long at = add_prim(b, &p);
  if (at < 0) {
    return refuse_with(b, "the filter list has more primitives than the waist carries");
  }
  p->color_space = GSHSCENE_FILTER_SPACE_SRGB; /* the functions work in sRGB */
  GSHSCENE_Filter_Input * slot = b->inputs[at];
  slot[0].kind = b->prev < 0 ? GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC : GSHSCENE_FILTER_INPUT_RESULT;
  slot[0].index = b->prev < 0 ? 0 : (uint32_t) b->prev;
  p->input_count = 1;
  p->inputs = slot;
  double v;
  if (word_is(text, name_len, "blur")) {
    size_t pos = 0;
    double len = 0;
    if (arg_len > 0 && !css_length(args, arg_len, &pos, &len)) {
      return GVEC_OK;
    }
    if (len < 0) {
      return GVEC_OK;
    }
    double pair[2] = {len, len};
    lengths_to_list(b, pair);
    p->kind = GSHSCENE_FILTER_GAUSSIAN_BLUR;
    p->sx = pair[0];
    p->sy = pair[1];
  }
  else if (word_is(text, name_len, "brightness") || word_is(text, name_len, "contrast") || word_is(text, name_len, "invert") ||
           word_is(text, name_len, "opacity")) {
    if (!css_amount(args, arg_len, 1.0, &v) || v < 0) {
      return GVEC_OK;
    }
    p->kind = GSHSCENE_FILTER_COMPONENT_TRANSFER;
    if (text[0] == 'b' || text[0] == 'c') { /* brightness: slope n; contrast: slope n, intercept (1 - n) / 2 */
      for (int c = 0; c < 3; c++) {
        p->transfer[c].type = GSHSCENE_FILTER_TRANSFER_LINEAR;
        p->transfer[c].slope = v;
        p->transfer[c].intercept = text[0] == 'c' ? 0.5 - 0.5 * v : 0.0;
      }
    }
    else if (text[0] == 'i') { /* invert: the table (n, 1 - n) */
      v = v > 1 ? 1 : v;
      if (b->values_used + 2 > VALUE_POOL) {
        return refuse_with(b, "the filter's values are past what the waist carries");
      }
      b->values[b->values_used] = v;
      b->values[b->values_used + 1] = 1 - v;
      for (int c = 0; c < 3; c++) {
        p->transfer[c].type = GSHSCENE_FILTER_TRANSFER_TABLE;
        p->transfer[c].count = 2;
        p->transfer[c].table = b->values + b->values_used;
      }
      b->values_used += 2;
    }
    else { /* opacity: slope n on the alpha */
      p->transfer[3].type = GSHSCENE_FILTER_TRANSFER_LINEAR;
      p->transfer[3].slope = v > 1 ? 1 : v;
    }
  }
  else if (word_is(text, name_len, "grayscale") || word_is(text, name_len, "sepia")) {
    if (!css_amount(args, arg_len, 1.0, &v) || v < 0) {
      return GVEC_OK;
    }
    v = v > 1 ? 1 : v;
    const double s1 = 1 - v;
    const bool gray = text[0] == 'g';
    double m[3][3];
    if (gray) {
      const double g[3][3] = {{0.2126 + 0.7874 * s1, 0.7152 - 0.7152 * s1, 0.0722 - 0.0722 * s1},
          {0.2126 - 0.2126 * s1, 0.7152 + 0.2848 * s1, 0.0722 - 0.0722 * s1}, {0.2126 - 0.2126 * s1, 0.7152 - 0.7152 * s1, 0.0722 + 0.9278 * s1}};
      memcpy(m, g, sizeof m);
    }
    else {
      const double q[3][3] = {{0.393 + 0.607 * s1, 0.769 - 0.769 * s1, 0.189 - 0.189 * s1},
          {0.349 - 0.349 * s1, 0.686 + 0.314 * s1, 0.168 - 0.168 * s1}, {0.272 - 0.272 * s1, 0.534 - 0.534 * s1, 0.131 + 0.869 * s1}};
      memcpy(m, q, sizeof m);
    }
    if (b->values_used + 20 > VALUE_POOL) {
      return refuse_with(b, "the filter's values are past what the waist carries");
    }
    double * m20 = b->values + b->values_used;
    memset(m20, 0, 20 * sizeof *m20);
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 3; c++) {
        m20[5 * r + c] = m[r][c];
      }
    }
    m20[18] = 1;
    b->values_used += 20;
    p->kind = GSHSCENE_FILTER_COLOR_MATRIX;
    p->mode = GSHSCENE_FILTER_MATRIX_MATRIX;
    p->value_count = 20;
    p->values = m20;
  }
  else if (word_is(text, name_len, "saturate") || word_is(text, name_len, "hue-rotate")) {
    const bool hue = text[0] == 'h';
    double amount;
    if (hue) {
      GVEC_Svg_Length angle;
      gvec_svg_parse_error err;
      if (arg_len == 0) {
        amount = 0;
      }
      else if (gvec_svg_parse_angle(args, arg_len, &angle, &err) == GVEC_SVG_PARSE_OK) {
        amount = gvec_svg_angle_degrees(&angle);
      }
      else {
        return GVEC_OK;
      }
    }
    else if (!css_amount(args, arg_len, 1.0, &amount) || amount < 0) {
      return GVEC_OK;
    }
    if (b->values_used + 1 > VALUE_POOL) {
      return refuse_with(b, "the filter's values are past what the waist carries");
    }
    b->values[b->values_used] = amount;
    p->kind = GSHSCENE_FILTER_COLOR_MATRIX;
    p->mode = hue ? GSHSCENE_FILTER_MATRIX_HUE_ROTATE : GSHSCENE_FILTER_MATRIX_SATURATE;
    p->value_count = 1;
    p->values = b->values + b->values_used++;
  }
  else if (word_is(text, name_len, "drop-shadow")) {
    size_t pos = 0;
    double len[3] = {0, 0, 0};
    size_t got = 0;
    double rgba[4] = {0, 0, 0, 1};
    bool color_given = false;
    bool typed = false, ok_typed = false;
    GVEC_Result r;
    GSHSCENE_Color_Space typed_space;
    memset(&typed_space, 0, sizeof typed_space);
    bool current = false; /* the color is `currentcolor` (the computed value always writes one, first): the element's `color` */
    /* the color may come first, as a style engine writes the computed value */
    while (pos < arg_len && is_wsp(args[pos])) {
      pos++;
    }
    if (pos < arg_len && !((args[pos] >= '0' && args[pos] <= '9') || args[pos] == '-' || args[pos] == '+' || args[pos] == '.')) {
      size_t end = pos;
      int depth = 0;
      while (end < arg_len && (depth > 0 || !is_wsp(args[end]))) {
        depth += args[end] == '(' ? 1 : args[end] == ')' ? -1 : 0;
        end++;
      }
      if (!css_color(args + pos, end - pos, rgba)) {
        r = typed_color(b, args + pos, end - pos, rgba, &typed_space, &typed, &ok_typed);
        if (r != GVEC_OK || b->refused || !ok_typed) {
          return r;
        }
      }
      current = is_current_color(args + pos, end - pos);
      color_given = true;
      pos = end;
    }
    while (got < 3) {
      size_t save = pos;
      if (!css_length(args, arg_len, &pos, &len[got])) {
        pos = save;
        break;
      }
      got++;
    }
    if (got < 2 || (got == 3 && len[2] < 0)) {
      return GVEC_OK;
    }
    if (!color_given && pos < arg_len && !css_color(args + pos, arg_len - pos, rgba)) {
      r = typed_color(b, args + pos, arg_len - pos, rgba, &typed_space, &typed, &ok_typed);
      if (r != GVEC_OK || b->refused || !ok_typed) {
        return r; /* not a color: the list is not lowered */
      }
    }
    if (current) {
      GVEC_Result cr = element_color(b, rgba);
      if (cr != GVEC_OK || b->refused) {
        return cr;
      }
    }
    p->kind = GSHSCENE_FILTER_DROP_SHADOW;
    fmap_vector(&b->map, len[0], len[1], &p->dx, &p->dy);
    double dev[2] = {len[2], len[2]}; /* the third length is the standard deviation itself, not box-shadow's blur radius (Filter Effects 1, drop-shadow()) */
    lengths_to_list(b, dev);
    p->sx = dev[0];
    p->sy = dev[1];
    for (int k = 0; k < 4; k++) {
      p->color.rgba[k] = rgba[k];
    }
    p->opacity = 1;
    p->color.space.primaries = GCOL_PRIMARIES_SRGB;
    p->color.space.transfer = GCOL_TRANSFER_SRGB;
    if (typed) {
      p->color.space = typed_space; /* a color the CSS subset above does not read, in the space the style layer made it in */
    }
  }
  else {
    return GVEC_OK;
  }
  b->names[b->count] = NULL;
  b->name_len[b->count] = 0;
  b->prev = (long) b->count;
  b->count++;
  *ok = true;
  return GVEC_OK;
}

/** The filter of the CSS filter functions `v->references[first .. first + count)`: an `AUTO` region and the functions' primitives in order, in sRGB. */
static GVEC_Result make_function_filter(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * local, const GVEC_Svg_Style_Value * v,
    size_t first, size_t count, gvec_lw_filter_state * state, const GSHSCENE_Resource ** out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *state = GVEC_LW_FILTER_REFUSED;
  *out = NULL;
  fbuild b;
  memset(&b, 0, sizeof b);
  b.ctx = ctx;
  b.target = e;
  b.prev = -1;
  if (!fmap_make(local, &b.map)) {
    return GVEC_ERR_INTERNAL;
  }
  b.prims = gvec_realloc(ctx->allocator, NULL, GSHSCENE_FILTER_MAX_PRIMITIVES * sizeof *b.prims);
  b.inputs = gvec_realloc(ctx->allocator, NULL, GSHSCENE_FILTER_MAX_PRIMITIVES * sizeof *b.inputs);
  b.values = gvec_realloc(ctx->allocator, NULL, VALUE_POOL * sizeof *b.values);
  if (!b.prims || !b.inputs || !b.values) {
    gvec_free(ctx->allocator, b.prims);
    gvec_free(ctx->allocator, b.inputs);
    gvec_free(ctx->allocator, b.values);
    return GVEC_ERR_OOM;
  }
  GVEC_Result r = GVEC_OK;
  bool ok = true;
  for (size_t i = first; r == GVEC_OK && ok && !b.refused && i < first + count; i++) {
    r = css_function(&b, v->references[i].text, &ok);
  }
  const GSHSCENE_Resource * resource = NULL;
  if (r == GVEC_OK && !b.refused && !ok) {
    r = gvec_lw_refuse(ctx, e, GVEC_LOWER_FILTER, "a filter function with arguments this library does not read (a color it does not know, a unit other than px) is not lowered");
  }
  else if (r == GVEC_OK && !b.refused) {
    GSHSCENE_Filter f;
    memset(&f, 0, sizeof f);
    f.region_mode = GSHSCENE_FILTER_REGION_AUTO;
    f.color_space = GSHSCENE_FILTER_SPACE_SRGB;
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
  gvec_free(ctx->allocator, b.prims);
  gvec_free(ctx->allocator, b.inputs);
  gvec_free(ctx->allocator, b.values);
  return r;
}

/** One step of a filter list: a `url()` of a filter element, or a run of CSS functions (made into one filter resource). */
typedef struct filter_step {
  bool is_url;
  size_t first;
  size_t count;
} filter_step;

GVEC_Result gvec_lw_filter_begin(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * frame, gvec_lw_filter_state * state,
    gvec_lw_filter_set * set) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *state = GVEC_LW_FILTER_NONE;
  memset(set, 0, sizeof *set);
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
  /* A list is applied left to right, each filter to the result of the one before (Filter Effects 1): the steps are nested groups, the
   * first innermost.  A run of functions is one step, since one resource holds them in order. */
  filter_step steps[GVEC_LW_FILTER_LIST_MAX];
  size_t step_count = 0;
  for (size_t i = 0; i < v.reference_count; i++) {
    if (!v.references[i].is_url && step_count && !steps[step_count - 1].is_url) {
      steps[step_count - 1].count++;
      continue;
    }
    if (step_count == GVEC_LW_FILTER_LIST_MAX) {
      *state = GVEC_LW_FILTER_REFUSED;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the filter list is longer than the lowering nests");
    }
    steps[step_count].is_url = v.references[i].is_url;
    steps[step_count].first = i;
    steps[step_count].count = 1;
    step_count++;
  }
  const GVEC_Svg_Element * targets[GVEC_LW_FILTER_LIST_MAX] = {NULL};
  for (size_t k = 0; k < step_count; k++) {
    if (!steps[k].is_url) {
      continue;
    }
    const GVEC_Svg_Style_Reference * ref = &v.references[steps[k].first];
    if (!ref->local) {
      *state = GVEC_LW_FILTER_REFUSED;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_FILTER, "a filter that names an element the resolver does not supply is not lowered");
    }
    const GVEC_Svg_Element * target = ref->target;
    if (!target && ref->cut && ref->id) {
      target = gvec_svg_find_by_id(ctx->document, ref->id);
    }
    if (!target || gvec_svg_element_kind(target) != GVEC_SVG_KIND_FILTER) {
      /* Filter Effects 1, `filter`: "If the filter references a non-existent object or the referenced object is not a filter element, then
         the whole filter chain is ignored."  The element is drawn as it is, whatever else the list holds (Firefox and Chromium draw a lone
         reference so; SVG 1.1 and resvg do not render it, and the reference of the corpus is resvg's). */
      *state = GVEC_LW_FILTER_NONE;
      return GVEC_OK;
    }
    targets[k] = target;
  }
  const GVEC_Svg_Style_Value filter_value = v;
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &v, &usable);
  if (r != GVEC_OK || !usable) {
    *state = GVEC_LW_FILTER_REFUSED;
    return r;
  }
  GSHGEOM_Map own_map = v.transform.map;
  if (v.transform.depends_on_box) {
    bool known;
    r = gvec_lw_box_transform(ctx, inst, &v.transform, &own_map, &known);
    if (r != GVEC_OK || !known) {
      *state = GVEC_LW_FILTER_REFUSED;
      return r != GVEC_OK ? r : gvec_lw_refuse(ctx, e, GVEC_LOWER_TRANSFORM_BOX, "a transform with percentages of a bounding box of an element that holds what the lowering does not box");
    }
  }
  GSHGEOM_Map local;
  if (!gvec_lw_compose(frame, &own_map, &local)) {
    *state = GVEC_LW_FILTER_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the element's transform leaves the range the waist can carry");
  }
  fmap probe;
  const bool plain = fmap_make(&local, &probe);
  GSHGEOM_Map identity;
  if (gshgeom_map_identity(2, &identity) != GSHGEOM_OK) {
    return GVEC_ERR_INTERNAL;
  }
  /* A rotated or skewed element is filtered in a picture of its own, whose space is the element's: the filter is then in that space and the
   * sink filters in canonical space (D-FL8); an axis-aligned one is filtered in the list's through its map. */
  const GSHSCENE_Resource * resources[GVEC_LW_FILTER_LIST_MAX];
  for (size_t k = 0; k < step_count; k++) {
    const GSHGEOM_Map * at = plain ? &local : &identity;
    gvec_lw_filter_state one;
    r = steps[k].is_url ? make_filter(ctx, inst, at, targets[k], &one, &resources[k])
                        : make_function_filter(ctx, inst, at, &filter_value, steps[k].first, steps[k].count, &one, &resources[k]);
    if (r != GVEC_OK || one != GVEC_LW_FILTER_NODE) {
      *state = one;
      return r;
    }
  }
  *state = GVEC_LW_FILTER_NODE;
  if (!plain) {
    gvec_lw_node * use = gvec_lw_new_node(ctx, GVEC_LW_USE);
    if (!use) {
      return GVEC_ERR_OOM;
    }
    use->map = local;
    use->key = gvec_lw_key(e->key, hash2(ctx->chain, 1), GVEC_LW_ROLE_INSTANCE);
    use->container_key = gvec_lw_key(e->key, hash2(ctx->chain, 1), GVEC_LW_ROLE_CONTAINER);
    use->site = gvec_lw_new_site(ctx, e, use->key);
    use->container_site = gvec_lw_new_site(ctx, e, use->container_key);
    if (use->site == GVEC_LW_NO_SITE || use->container_site == GVEC_LW_NO_SITE) {
      return GVEC_ERR_OOM;
    }
    set->outer = use;
  }
  /* The last step is the outermost group, which carries the opacity; the first is innermost, which holds the content. */
  for (size_t j = 0; j < step_count; j++) {
    const size_t k = step_count - 1 - j;
    gvec_lw_node * g = gvec_lw_new_node(ctx, GVEC_LW_GROUP);
    if (!g) {
      return GVEC_ERR_OOM;
    }
    g->opacity = 1;
    g->filter = resources[k];
    g->key = gvec_lw_key(e->key, hash2(ctx->chain, j == 0 ? 0 : 16u + (uint64_t) j), GVEC_LW_ROLE_FILTER);
    g->site = gvec_lw_new_site(ctx, e, g->key);
    if (g->site == GVEC_LW_NO_SITE) {
      return GVEC_ERR_OOM;
    }
    set->groups[set->count++] = g;
  }
  return GVEC_OK;
}
