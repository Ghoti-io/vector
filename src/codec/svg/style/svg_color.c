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
 * @file svg_color.c
 *
 * Colours (C7, C8): `currentColor`, and a CSS colour as the waist carries it, never as 8 bits.
 *
 * The engine of `text` parses a colour and does not convert it. The spaces the waist has a gamut and a transfer for are carried as
 * they are: sRGB, linear sRGB, Display P3, linear Display P3 and Adobe RGB (1998). Rec. 2020 and ProPhoto, whose transfer curves or
 * white point the waist does not take, are converted here, through the `color` library's matrices, to linear Rec. 2020, a gamut that
 * holds both. `hsl()` and `hwb()` are converted in `double` (the engine rounds them to 8 bits for its serialisation). Every other colour
 * function (`lab()`, `lch()`, `oklab()`, `oklch()`, `xyz`, `color-mix()`, relative colours ...) is parsed and kept by the engine and refused
 * here, reported with its location, because converting it would approximate a colour the waist cannot yet carry.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>
#include <strings.h>

#include <ghoti.io/color/color.h>
#include <ghoti.io/color/math.h>
#include <ghoti.io/math/basic.h>
#include <ghoti.io/math/elementary.h>

#include "svg_style.h"
#include "svg_used.h"

static double clamp01(double v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

static double saturate(double v) {
  return v > GSHSCENE_COLOR_COMPONENT_MAX ? GSHSCENE_COLOR_COMPONENT_MAX : v < -GSHSCENE_COLOR_COMPONENT_MAX ? -GSHSCENE_COLOR_COMPONENT_MAX : v;
}

/* The numeric value of a channel; `scale` is what 100% is. false for a channel that needs an environment (a keyword of a relative colour,
 * a calculation that is not a number). */
static bool channel(const GTEXT_CSS_Color_Component * c, double scale, double * out) {
  switch (c->kind) {
  case GTEXT_CSS_COMP_NUMBER:
  case GTEXT_CSS_COMP_ANGLE:
    *out = c->value;
    return true;
  case GTEXT_CSS_COMP_PERCENT:
    *out = c->value * scale / 100.0;
    return true;
  case GTEXT_CSS_COMP_NONE:
    *out = 0; /* A missing component is 0 when the colour is used (Color 4, 4.4). */
    return true;
  case GTEXT_CSS_COMP_MATH: {
    double value;
    bool percent = false;
    GTEXT_CSS_Unit unit;
    if (!c->math || !gtext_css_math_numeric(c->math, &value, &unit, &percent)) {
      return false;
    }
    *out = percent ? value * scale / 100.0 : value;
    return true;
  }
  case GTEXT_CSS_COMP_SYMBOL:
    return false;
  }
  return false;
}

static bool alpha_of(const GTEXT_CSS_Color * c, double * out) {
  if (!c->alpha_written) {
    *out = 1;
    return true;
  }
  double a;
  if (!channel(&c->alpha, 1.0, &a)) {
    return false;
  }
  *out = clamp01(a);
  return true;
}

/*
 * HSL to sRGB, CSS Color 4 section 7. The sample code there divides the hue by 30 and takes a modulus of 12; the same arithmetic is done
 * here in degrees, where the modulus is exact and the division by 30 is the only rounding, so that each channel is within a few units in
 * the last place of the exact value.
 */
static double hsl_f(double n, double hue, double sat, double light) {
  double k = gmath_fmod(n * 30.0 + hue, 360.0);          /* 30 * k of the sample code, exactly */
  double a = sat * (light < 1 - light ? light : 1 - light);
  double t = (k - 90.0) / 30.0 < (270.0 - k) / 30.0 ? (k - 90.0) / 30.0 : (270.0 - k) / 30.0;
  t = t < 1 ? t : 1;
  t = t > -1 ? t : -1;
  return light - a * t;
}

static void hsl_to_rgb(double hue, double sat, double light, double rgb[3]) {
  hue = gmath_fmod(hue, 360.0);
  if (hue < 0) {
    hue += 360.0;
  }
  rgb[0] = hsl_f(0, hue, sat, light);
  rgb[1] = hsl_f(8, hue, sat, light);
  rgb[2] = hsl_f(4, hue, sat, light);
}

static void hwb_to_rgb(double hue, double white, double black, double rgb[3]) {
  if (white + black >= 1) {
    double gray = white / (white + black);
    rgb[0] = rgb[1] = rgb[2] = gray;
    return;
  }
  hsl_to_rgb(hue, 1.0, 0.5, rgb);
  for (int i = 0; i < 3; i++) {
    rgb[i] = rgb[i] * (1 - white - black) + white;
  }
}

static void set_space(GSHSCENE_Color * c, GCOL_Primaries primaries, GCOL_Transfer transfer, double gamma) {
  c->space.primaries = primaries;
  c->space.transfer = transfer;
  c->space.gamma = gamma;
}

static GVEC_Result refuse(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * where, uint32_t detail, const char * message) {
  if (where && gvec_svg_once_first(context, GVEC_ONCE_COLOR, where)) {
    GVEC_Result r = gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_UNSUPPORTED_COLOR_SPACE, detail, where, NULL, message);
    if (r == GVEC_ERR_OOM) {
      return r;
    }
  }
  return GVEC_ERR_UNSUPPORTED;
}

/* CSS Color 4 section 10: the transfer function of Rec. 2020, and of ProPhoto RGB, to linear light (odd functions). */
static double rec2020_decode(double v) {
  const double alpha = 1.09929682680944, beta = 0.018053968510807;
  double a = gmath_fabs(v);
  double s = v < 0 ? -1.0 : 1.0;
  if (a < beta * 4.5) {
    return v / 4.5;
  }
  return s * gmath_pow((a + alpha - 1.0) / alpha, 1.0 / 0.45);
}

static double prophoto_decode(double v) {
  double a = gmath_fabs(v);
  double s = v < 0 ? -1.0 : 1.0;
  if (a <= 16.0 / 512.0) {
    return v / 16.0;
  }
  return s * gmath_pow(a, 1.8);
}

/* ProPhoto RGB (D50) to linear Rec. 2020 (D65): the gamut's matrix to XYZ, the Bradford adaptation, and the inverse of Rec. 2020's. */
static bool prophoto_to_rec2020(const double in[3], double out[3]) {
  GCOL_Gamut pro, rec;
  double to_xyz[9], adapt[9], from_xyz[9];
  if (!gcol_gamut_named(GCOL_PRIMARIES_PROPHOTO, &pro) || !gcol_gamut_named(GCOL_PRIMARIES_BT2020, &rec) ||
      gcol_gamut_to_xyz(&pro, to_xyz) != GCOL_OK || gcol_chromatic_adapt(pro.white, rec.white, GCOL_CAT_BRADFORD, adapt) != GCOL_OK ||
      gcol_xyz_to_gamut(&rec, from_xyz) != GCOL_OK) {
    return false;
  }
  double xyz[3], adapted[3];
  for (int i = 0; i < 3; i++) {
    xyz[i] = to_xyz[3 * i] * in[0] + to_xyz[3 * i + 1] * in[1] + to_xyz[3 * i + 2] * in[2];
  }
  for (int i = 0; i < 3; i++) {
    adapted[i] = adapt[3 * i] * xyz[0] + adapt[3 * i + 1] * xyz[1] + adapt[3 * i + 2] * xyz[2];
  }
  for (int i = 0; i < 3; i++) {
    out[i] = from_xyz[3 * i] * adapted[0] + from_xyz[3 * i + 1] * adapted[1] + from_xyz[3 * i + 2] * adapted[2];
  }
  return true;
}

/* CIE XYZ to linear Rec. 2020 (D65): taken from the white point `white` of the XYZ (D50 for CIE Lab and the xyz-d50 space, D65 otherwise) by the
 * Bradford adaptation, then by the inverse of Rec. 2020's matrix. Rec. 2020 is the widest gamut the shape waist carries beside ProPhoto, and a
 * color outside it is carried with a component out of [0, 1], not clipped (the waist's conversion to a smaller target does that). */
static bool xyz_to_rec2020(const double xyz[3], GCOL_Primaries white_of, double out[3]) {
  GCOL_Gamut from, rec;
  double adapt[9], from_xyz[9];
  if (!gcol_gamut_named(white_of, &from) || !gcol_gamut_named(GCOL_PRIMARIES_BT2020, &rec) ||
      gcol_chromatic_adapt(from.white, rec.white, GCOL_CAT_BRADFORD, adapt) != GCOL_OK || gcol_xyz_to_gamut(&rec, from_xyz) != GCOL_OK) {
    return false;
  }
  double adapted[3];
  for (int i = 0; i < 3; i++) {
    adapted[i] = adapt[3 * i] * xyz[0] + adapt[3 * i + 1] * xyz[1] + adapt[3 * i + 2] * xyz[2];
  }
  for (int i = 0; i < 3; i++) {
    out[i] = from_xyz[3 * i] * adapted[0] + from_xyz[3 * i + 1] * adapted[1] + from_xyz[3 * i + 2] * adapted[2];
  }
  return true;
}

/* CSS Color 4, 9.1 and 9.3: CIE Lab (D50) to XYZ (D50). */
static void lab_to_xyz_d50(double l, double a, double b, double xyz[3]) {
  const double eps = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
  const double fy = (l + 16.0) / 116.0, fx = a / 500.0 + fy, fz = fy - b / 200.0;
  const double x = fx * fx * fx > eps ? fx * fx * fx : (116.0 * fx - 16.0) / kappa;
  const double y = l > kappa * eps ? fy * fy * fy : l / kappa;
  const double z = fz * fz * fz > eps ? fz * fz * fz : (116.0 * fz - 16.0) / kappa;
  xyz[0] = x * 0.3457 / 0.3585;  /* the D50 white of CSS Color 4: x 0.3457, y 0.3585 */
  xyz[1] = y;
  xyz[2] = z * (1.0 - 0.3457 - 0.3585) / 0.3585;
}

/* CSS Color 4, 9.4: Oklab to XYZ (D65). */
static void oklab_to_xyz_d65(double l, double a, double b, double xyz[3]) {
  const double lp = l + 0.3963377773761749 * a + 0.2158037573099136 * b;
  const double mp = l - 0.1055613458156586 * a - 0.0638541728258133 * b;
  const double sp = l - 0.0894841775298119 * a - 1.2914855480194092 * b;
  const double lms[3] = {lp * lp * lp, mp * mp * mp, sp * sp * sp};
  xyz[0] = 1.2268798758459243 * lms[0] - 0.5578149944602171 * lms[1] + 0.2813910456659647 * lms[2];
  xyz[1] = -0.0405757452148008 * lms[0] + 1.1122868032803170 * lms[1] - 0.0717110580655164 * lms[2];
  xyz[2] = -0.0763729366746601 * lms[0] - 0.4214933324022432 * lms[1] + 1.5869240198367816 * lms[2];
}

/* lab(), lch(), oklab() and oklch(): the three channels as the grammar gives them (the references of a percentage are 100 and 125 for Lab,
 * 100 and 150 for LCH, 1 and 0.4 for Oklab, 1 and 0.4 for Oklch), to linear Rec. 2020. */
static bool cie_color(GTEXT_CSS_Color_Function fn, const GTEXT_CSS_Color * c, double rgb[3]) {
  const bool ok_space = fn == GTEXT_CSS_COLOR_FN_OKLAB || fn == GTEXT_CSS_COLOR_FN_OKLCH;
  const bool polar = fn == GTEXT_CSS_COLOR_FN_LCH || fn == GTEXT_CSS_COLOR_FN_OKLCH;
  double v0, v1, v2;
  if (!channel(&c->components[0], ok_space ? 1.0 : 100.0, &v0) || !channel(&c->components[1], polar ? (ok_space ? 0.4 : 150.0) : (ok_space ? 0.4 : 125.0), &v1) ||
      !channel(&c->components[2], polar ? 360.0 : (ok_space ? 0.4 : 125.0), &v2)) {
    return false;
  }
  v0 = v0 < 0 ? 0 : v0 > (ok_space ? 1.0 : 100.0) ? (ok_space ? 1.0 : 100.0) : v0;
  if (polar) {
    const double h = gmath_fmod(v2, 360.0) * 3.14159265358979323846264338327950288 / 180.0;
    const double chroma = v1 < 0 ? 0 : v1;
    v1 = chroma * gmath_cos(h);
    v2 = chroma * gmath_sin(h);
  }
  double xyz[3];
  if (ok_space) {
    oklab_to_xyz_d65(v0, v1, v2, xyz);
    return xyz_to_rec2020(xyz, GCOL_PRIMARIES_SRGB, rgb);  /* sRGB's white is D65 */
  }
  lab_to_xyz_d50(v0, v1, v2, xyz);
  return xyz_to_rec2020(xyz, GCOL_PRIMARIES_PROPHOTO, rgb); /* ProPhoto's white is D50 */
}

/* The keywords of a `color-scheme` value, which the engine nests in the lists of its grammar. */
static void scan_scheme(const GTEXT_CSS_Value * v, bool * light, bool * dark, unsigned depth) {
  if (depth > 8) {
    return;
  }
  if (v->kind == GTEXT_CSS_VAL_LIST) {
    for (size_t i = 0; i < v->child_count; i++) {
      scan_scheme(&v->children[i], light, dark, depth + 1);
    }
  }
  else if (v->kind == GTEXT_CSS_VAL_KEYWORD) {
    if (v->text.length == 5 && memcmp(v->text.data, "light", 5) == 0) {
      *light = true;
    }
    else if (v->text.length == 4 && memcmp(v->text.data, "dark", 4) == 0) {
      *dark = true;
    }
  }
}

/* The used color scheme of an element: the `color-scheme` it computes to is a list of `light`, `dark` and `only`; with both, the
 * preference decides; with `dark` alone, dark; otherwise light (`normal` is light). */
static bool used_scheme_is_dark(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance) {
  const GTEXT_CSS_Computed_Style * style = NULL;
  if (gvec_svg_instance_style(context, instance, &style) != GVEC_OK) {
    return false;
  }
  const GTEXT_CSS_Value * v = gtext_css_computed_root(style, context->engine->reg.color_scheme);
  if (!v) {
    return false;
  }
  bool light = false, dark = false;
  scan_scheme(v, &light, &dark, 0);
  return dark && (!light || context->color_scheme == GVEC_COLOR_SCHEME_DARK);
}

static bool is_name(const GTEXT_CSS_Color * c, const char * name) {
  return c->name.length == strlen(name) && memcmp(c->name.data, name, c->name.length) == 0;
}

static void foreground(GVEC_Svg_Color * out, double alpha) {
  memset(out, 0, sizeof *out);
  out->kind = GVEC_SVG_COLOR_FOREGROUND;
  gshscene_color_init(&out->color);
  out->color.rgba[0] = out->color.rgba[1] = out->color.rgba[2] = 0;
  out->color.rgba[3] = alpha;
}

static void solid(GVEC_Svg_Color * out, const double rgba[4], GCOL_Primaries primaries, GCOL_Transfer transfer, double gamma) {
  memset(out, 0, sizeof *out);
  out->kind = GVEC_SVG_COLOR_SOLID;
  for (int i = 0; i < 3; i++) {
    out->color.rgba[i] = saturate(rgba[i]);
  }
  out->color.rgba[3] = clamp01(rgba[3]);
  set_space(&out->color, primaries, transfer, gamma);
}

/* A color of a `color-mix()`, `light-dark()`, `contrast-color()`, `alpha()` or a relative color that holds `currentcolor` (which the engine cannot
 * compute from the color alone) is made from the color with the element's own `color` in the place of each `currentcolor`; the engine writes the
 * color it computes to (`color(srgb 0.5 0 0.5)`, `lch(...)`), and that text is read back as the color it is. */
typedef struct subst {
  GTEXT_CSS_Color pool[8];
  size_t used;
  const GTEXT_CSS_Color * ptrs[16];
  size_t pused;
  const GTEXT_CSS_Color * current;
} subst;

static bool subst_color(subst * s, const GTEXT_CSS_Color * c, unsigned depth, const GTEXT_CSS_Color ** out) {
  if (depth > 6) {
    return false;
  }
  if (c->kind == GTEXT_CSS_COLOR_KEYWORD && c->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR) {
    *out = s->current;
    return s->current != NULL;
  }
  if (!c->origin && c->item_count == 0) {
    *out = c;
    return true;
  }
  if (s->used >= sizeof s->pool / sizeof s->pool[0]) {
    return false;
  }
  GTEXT_CSS_Color * copy = &s->pool[s->used++];
  *copy = *c;
  if (c->origin && !subst_color(s, c->origin, depth + 1, &copy->origin)) {
    return false;
  }
  if (c->item_count) {
    if (s->pused + c->item_count > sizeof s->ptrs / sizeof s->ptrs[0]) {
      return false;
    }
    const GTEXT_CSS_Color ** items = &s->ptrs[s->pused];
    s->pused += c->item_count;
    for (size_t i = 0; i < c->item_count; i++) {
      if (!subst_color(s, c->items[i], depth + 1, &items[i])) {
        return false;
      }
    }
    copy->items = (const GTEXT_CSS_Color * const *) items;
  }
  *out = copy;
  return true;
}

/* A decimal number as the engine writes one (an optional sign, digits, a fraction, an exponent), without the locale: the part of `*text` read,
 * advanced past it; false when none is there. */
static bool read_number(const char ** text, double * out) {
  const char * p = *text;
  double sign = 1;
  if (*p == '-' || *p == '+') {
    sign = *p == '-' ? -1 : 1;
    p++;
  }
  double mantissa = 0;
  int exponent = 0;
  bool digits = false;
  for (; *p >= '0' && *p <= '9'; p++) {
    digits = true;
    if (mantissa < 1e17) {
      mantissa = mantissa * 10 + (*p - '0');
    }
    else {
      exponent++;
    }
  }
  if (*p == '.') {
    for (p++; *p >= '0' && *p <= '9'; p++) {
      digits = true;
      if (mantissa < 1e17) {
        mantissa = mantissa * 10 + (*p - '0');
        exponent--;
      }
    }
  }
  if (!digits) {
    return false;
  }
  if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
    int sign_e = p[1] == '-' ? -1 : 1;
    const char * q = p + ((p[1] == '-' || p[1] == '+') ? 2 : 1);
    int e = 0;
    for (; *q >= '0' && *q <= '9'; q++) {
      e = e * 10 + (*q - '0');
      if (e > 400) {
        e = 400;
      }
    }
    exponent += sign_e * e;
    p = q;
  }
  *out = sign * mantissa * gmath_pow(10.0, (double) exponent);
  *text = p;
  return true;
}

/* The color the engine wrote (`function(args)` or `color(space args)`, `none` for a missing channel, `/ alpha`) as a color of this library's
 * reading: the function, the space and three numbers. false for what is not one of these. */
static bool read_computed(const char * text, GTEXT_CSS_Color * c, GTEXT_CSS_Color_Component comps[3]) {
  static const struct {
    const char * name;
    GTEXT_CSS_Color_Function fn;
  } functions[] = {{"rgb", GTEXT_CSS_COLOR_FN_RGB}, {"hsl", GTEXT_CSS_COLOR_FN_HSL}, {"hwb", GTEXT_CSS_COLOR_FN_HWB}, {"lab", GTEXT_CSS_COLOR_FN_LAB},
      {"lch", GTEXT_CSS_COLOR_FN_LCH}, {"oklab", GTEXT_CSS_COLOR_FN_OKLAB}, {"oklch", GTEXT_CSS_COLOR_FN_OKLCH}, {"color", GTEXT_CSS_COLOR_FN_COLOR}};
  const char * open = strchr(text, '(');
  if (!open) {
    return false;
  }
  memset(c, 0, sizeof *c);
  c->kind = GTEXT_CSS_COLOR_FUNCTION;
  bool found = false;
  for (size_t i = 0; i < sizeof functions / sizeof functions[0]; i++) {
    if ((size_t)(open - text) == strlen(functions[i].name) && memcmp(text, functions[i].name, open - text) == 0) {
      c->function = functions[i].fn;
      found = true;
    }
  }
  if (!found) {
    return false;
  }
  const char * p = open + 1;
  while (*p == ' ') {
    p++;
  }
  if (c->function == GTEXT_CSS_COLOR_FN_COLOR) {
    const char * end = p;
    while (*end && *end != ' ') {
      end++;
    }
    bool space_found = false;
    for (int sp = 0; sp < (int) GTEXT_CSS_SPACE_CUSTOM; sp++) {
      const char * name = gtext_css_color_space_name((GTEXT_CSS_Color_Space) sp);
      if (name && strlen(name) == (size_t)(end - p) && memcmp(name, p, end - p) == 0) {
        c->space = (GTEXT_CSS_Color_Space) sp;
        space_found = true;
        break;
      }
    }
    if (!space_found) {
      return false;
    }
    p = end;
  }
  c->components = comps;
  c->component_count = 3;
  for (int i = 0; i < 4; i++) {
    while (*p == ' ') {
      p++;
    }
    if (i == 3) {
      if (*p == '/') {
        p++;
        while (*p == ' ') {
          p++;
        }
        c->alpha_written = true;
      }
      else {
        break;
      }
    }
    GTEXT_CSS_Color_Component * k = i == 3 ? &c->alpha : &comps[i];
    memset(k, 0, sizeof *k);
    if (strncmp(p, "none", 4) == 0) {
      k->kind = GTEXT_CSS_COMP_NONE;
      p += 4;
      continue;
    }
    double v;
    if (!read_number(&p, &v)) {
      return false;
    }
    k->kind = GTEXT_CSS_COMP_NUMBER;
    k->value = v;
    if (*p == '%') {
      k->kind = GTEXT_CSS_COMP_PERCENT;
      p++;
    }
    else if (strncmp(p, "deg", 3) == 0) {
      k->kind = GTEXT_CSS_COMP_ANGLE;
      p += 3;
    }
  }
  return true;
}

/* A computed colour of `inst`, one level deep: `currentColor` is the element's `color`, which is never itself `currentColor` (computed
 * `color: currentcolor` is `inherit`). */
static GVEC_Result color_of(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Color * c,
    const GVEC_Svg_Element * where, bool allow_current, GVEC_Svg_Color * out);

/* The color of `c` through the text the engine computes it to, with the element's own color where `currentcolor` is written. */
static GVEC_Result computed_color(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Color * c,
    const GVEC_Svg_Element * where, GVEC_Svg_Color * out) {
  subst s;
  memset(&s, 0, sizeof s);
  const GTEXT_CSS_Value * v = NULL;
  GVEC_Result r = gvec_svg_computed(context, instance, GVEC_SVG_PROP_COLOR, NULL, &v);
  if (r != GVEC_OK) {
    return r;
  }
  s.current = v && v->kind == GTEXT_CSS_VAL_COLOR ? v->color : NULL;
  const GTEXT_CSS_Color * replaced;
  if (!subst_color(&s, c, 0, &replaced)) {
    return refuse(context, where, 0, "a color holds a currentcolor, a system color or too much nesting for it to be computed");
  }
  char text[GTEXT_CSS_COLOR_COMPUTED_TEXT_MAX + 1];
  size_t length = 0;
  if (gtext_css_color_compute_text(replaced, used_scheme_is_dark(context, instance), NULL, text, &length) != GTEXT_CSS_OK || length == 0) {
    return refuse(context, where, 0, "a color cannot be computed from itself (a system color, a custom space, a unit that needs an element)");
  }
  GTEXT_CSS_Color synthetic;
  GTEXT_CSS_Color_Component comps[3];
  if (!read_computed(text, &synthetic, comps)) {
    return refuse(context, where, 0, "the color the engine computes is of a form this library does not read");
  }
  return color_of(context, instance, &synthetic, where, false, out);
}

static GVEC_Result color_of(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Color * c,
    const GVEC_Svg_Element * where, bool allow_current, GVEC_Svg_Color * out) {
  double alpha;
  double rgba[4];
  switch (c->kind) {
  case GTEXT_CSS_COLOR_KEYWORD:
    switch (c->keyword) {
    case GTEXT_CSS_COLOR_KW_NAMED:
      for (int i = 0; i < 3; i++) {
        rgba[i] = c->components[i].value / 255.0;
      }
      rgba[3] = 1;
      solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
      return GVEC_OK;
    case GTEXT_CSS_COLOR_KW_TRANSPARENT:
      memset(rgba, 0, sizeof rgba);
      solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
      return GVEC_OK;
    case GTEXT_CSS_COLOR_KW_CURRENTCOLOR: {
      if (!allow_current) {
        return refuse(context, where, 0, "currentColor has no color to be the value of");
      }
      const GTEXT_CSS_Value * v = NULL;
      GVEC_Result r = gvec_svg_computed(context, instance, GVEC_SVG_PROP_COLOR, NULL, &v);
      if (r != GVEC_OK) {
        return r;
      }
      if (!v || v->kind != GTEXT_CSS_VAL_COLOR || !v->color) {
        return refuse(context, where, 0, "the color property has no color value");
      }
      return color_of(context, instance, v->color, where, false, out);
    }
    case GTEXT_CSS_COLOR_KW_SYSTEM:
    case GTEXT_CSS_COLOR_KW_DEPRECATED:
      if (is_name(c, "canvastext")) {
        foreground(out, 1.0);
        return GVEC_OK;
      }
      if (is_name(c, "canvas")) {
        /* The document's background: opaque white in the light scheme, opaque black in the dark, the scheme being the one the element's
         * `color-scheme` and the load options' preference make (CSS Color Adjust 1). */
        double v = used_scheme_is_dark(context, instance) ? 0.0 : 1.0;
        rgba[0] = rgba[1] = rgba[2] = v;
        rgba[3] = 1;
        solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
        return GVEC_OK;
      }
      return refuse(context, where, 0, "a system color other than Canvas and CanvasText has no value without a user agent");
    case GTEXT_CSS_COLOR_KW_NONE:
      break;
    }
    return refuse(context, where, 0, "a color keyword this library does not know");
  case GTEXT_CSS_COLOR_HEX:
    for (int i = 0; i < 3; i++) {
      rgba[i] = c->components[i].value / 255.0;
    }
    if (!alpha_of(c, &alpha)) {
      return refuse(context, where, 0, "the alpha of a color needs an environment");
    }
    rgba[3] = alpha;
    solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
    return GVEC_OK;
  case GTEXT_CSS_COLOR_FUNCTION: {
    if (c->has_unresolved && c->origin) {
      return computed_color(context, instance, c, where, out);
    }
    if (c->has_unresolved || !alpha_of(c, &alpha)) {
      return refuse(context, where, (uint32_t) c->space, "a color channel needs an environment and could not be computed");
    }
    double ch[3];
    switch (c->function) {
    case GTEXT_CSS_COLOR_FN_RGB:
      for (int i = 0; i < 3; i++) {
        if (!channel(&c->components[i], 255.0, &ch[i])) {
          return refuse(context, where, 0, "a color channel needs an environment and could not be computed");
        }
        rgba[i] = clamp01(ch[i] / 255.0);
      }
      rgba[3] = alpha;
      solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
      return GVEC_OK;
    case GTEXT_CSS_COLOR_FN_HSL:
    case GTEXT_CSS_COLOR_FN_HWB: {
      double h, a, b;
      if (!channel(&c->components[0], 360.0, &h) || !channel(&c->components[1], 100.0, &a) || !channel(&c->components[2], 100.0, &b)) {
        return refuse(context, where, 0, "a color channel needs an environment and could not be computed");
      }
      a = clamp01(a / 100.0);
      b = clamp01(b / 100.0);
      if (c->function == GTEXT_CSS_COLOR_FN_HSL) {
        hsl_to_rgb(h, a, b, ch);
      }
      else {
        hwb_to_rgb(h, a, b, ch);
      }
      rgba[0] = clamp01(ch[0]);
      rgba[1] = clamp01(ch[1]);
      rgba[2] = clamp01(ch[2]);
      rgba[3] = alpha;
      solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
      return GVEC_OK;
    }
    case GTEXT_CSS_COLOR_FN_COLOR: {
      if (c->component_count != 3) {
        return refuse(context, where, (uint32_t) c->space, "a color() function of another number of channels");
      }
      for (int i = 0; i < 3; i++) {
        if (!channel(&c->components[i], 1.0, &ch[i])) {
          return refuse(context, where, (uint32_t) c->space, "a color channel needs an environment and could not be computed");
        }
      }
      rgba[3] = alpha;
      switch (c->space) {
      case GTEXT_CSS_SPACE_SRGB:
        memcpy(rgba, ch, sizeof ch);
        solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
        return GVEC_OK;
      case GTEXT_CSS_SPACE_SRGB_LINEAR:
        memcpy(rgba, ch, sizeof ch);
        solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_LINEAR, 0);
        return GVEC_OK;
      case GTEXT_CSS_SPACE_DISPLAY_P3:
        memcpy(rgba, ch, sizeof ch);
        solid(out, rgba, GCOL_PRIMARIES_DISPLAY_P3, GCOL_TRANSFER_SRGB, 0);
        return GVEC_OK;
      case GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR:
        memcpy(rgba, ch, sizeof ch);
        solid(out, rgba, GCOL_PRIMARIES_DISPLAY_P3, GCOL_TRANSFER_LINEAR, 0);
        return GVEC_OK;
      case GTEXT_CSS_SPACE_A98_RGB:
        /* Adobe RGB (1998) is a pure power law of 563/256 (CSS Color 4, 10.3). */
        memcpy(rgba, ch, sizeof ch);
        solid(out, rgba, GCOL_PRIMARIES_ADOBE_RGB, GCOL_TRANSFER_GAMMA, 563.0 / 256.0);
        return GVEC_OK;
      case GTEXT_CSS_SPACE_REC2020:
        for (int i = 0; i < 3; i++) {
          rgba[i] = rec2020_decode(ch[i]);
        }
        solid(out, rgba, GCOL_PRIMARIES_BT2020, GCOL_TRANSFER_LINEAR, 0);
        return GVEC_OK;
      case GTEXT_CSS_SPACE_PROPHOTO_RGB: {
        double lin[3], conv[3];
        for (int i = 0; i < 3; i++) {
          lin[i] = prophoto_decode(ch[i]);
        }
        if (!prophoto_to_rec2020(lin, conv)) {
          return refuse(context, where, (uint32_t) c->space, "the conversion from ProPhoto RGB is not available");
        }
        memcpy(rgba, conv, sizeof conv);
        solid(out, rgba, GCOL_PRIMARIES_BT2020, GCOL_TRANSFER_LINEAR, 0);
        return GVEC_OK;
      }
      case GTEXT_CSS_SPACE_XYZ_D50:
      case GTEXT_CSS_SPACE_XYZ_D65: {
        double conv[3];
        if (!xyz_to_rec2020(ch, c->space == GTEXT_CSS_SPACE_XYZ_D50 ? GCOL_PRIMARIES_PROPHOTO : GCOL_PRIMARIES_SRGB, conv)) {
          return refuse(context, where, (uint32_t) c->space, "the conversion from CIE XYZ is not available");
        }
        memcpy(rgba, conv, sizeof conv);
        solid(out, rgba, GCOL_PRIMARIES_BT2020, GCOL_TRANSFER_LINEAR, 0);
        return GVEC_OK;
      }
      default:
        return refuse(context, where, (uint32_t) c->space, "a color() space the shape waist cannot carry (a custom one, named by a `@color-profile`)");
      }
    }
    case GTEXT_CSS_COLOR_FN_LAB:
    case GTEXT_CSS_COLOR_FN_LCH:
    case GTEXT_CSS_COLOR_FN_OKLAB:
    case GTEXT_CSS_COLOR_FN_OKLCH: {
      double conv[3];
      if (!cie_color(c->function, c, conv)) {
        return refuse(context, where, (uint32_t) c->space, "a color channel needs an environment and could not be computed");
      }
      memcpy(rgba, conv, sizeof conv);
      rgba[3] = alpha;
      solid(out, rgba, GCOL_PRIMARIES_BT2020, GCOL_TRANSFER_LINEAR, 0);
      return GVEC_OK;
    }
    default:
      return refuse(context, where, (uint32_t) c->space, "a color function this library does not know");
    }
  }
  case GTEXT_CSS_COLOR_MIX:
  case GTEXT_CSS_COLOR_LIGHT_DARK:
  case GTEXT_CSS_COLOR_CONTRAST:
  case GTEXT_CSS_COLOR_ALPHA:
    return computed_color(context, instance, c, where, out);
  case GTEXT_CSS_COLOR_DEVICE_CMYK: {
    /* CSS Color 5, 5.1: no color management is assumed, so the conversion is the naive one: each of red, green and blue is one less the cyan,
     * magenta or yellow, which black darkens. */
    double cmyk[4];
    if (c->component_count != 4 || !alpha_of(c, &alpha)) {
      return refuse(context, where, 0, "device-cmyk() of another number of channels");
    }
    for (int i = 0; i < 4; i++) {
      if (!channel(&c->components[i], 1.0, &cmyk[i])) {
        return refuse(context, where, 0, "a color channel needs an environment and could not be computed");
      }
      cmyk[i] = clamp01(cmyk[i]);
    }
    for (int i = 0; i < 3; i++) {
      rgba[i] = 1.0 - (cmyk[i] * (1.0 - cmyk[3]) + cmyk[3] < 1.0 ? cmyk[i] * (1.0 - cmyk[3]) + cmyk[3] : 1.0);
    }
    rgba[3] = alpha;
    solid(out, rgba, GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0);
    return GVEC_OK;
  }
  }
  return refuse(context, where, 0, "a color this library does not know");
}

GVEC_Result gvec_svg_used_color(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Value * value,
    const GVEC_Svg_Element * where, GVEC_Svg_Color * out) {
  /* A property whose value is `currentColor` computes to the keyword, not to the color: it is the element's `color`. */
  if (value && value->kind == GTEXT_CSS_VAL_KEYWORD && value->text.length == strlen("currentcolor") &&
      strncasecmp(value->text.data, "currentcolor", value->text.length) == 0) {
    const GTEXT_CSS_Value * current = NULL;
    GVEC_Result r = gvec_svg_computed(context, instance, GVEC_SVG_PROP_COLOR, NULL, &current);
    if (r != GVEC_OK) {
      return r;
    }
    if (!current || current->kind != GTEXT_CSS_VAL_COLOR || !current->color) {
      return refuse(context, where, 0, "the color property has no color value");
    }
    return color_of(context, instance, current->color, where, false, out);
  }
  if (!value || value->kind != GTEXT_CSS_VAL_COLOR || !value->color) {
    return refuse(context, where, 0, "the value is not a color");
  }
  return color_of(context, instance, value->color, where, true, out);
}
