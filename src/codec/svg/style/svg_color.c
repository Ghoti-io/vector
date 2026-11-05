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

/* A computed colour of `inst`, one level deep: `currentColor` is the element's `color`, which is never itself `currentColor` (computed
 * `color: currentcolor` is `inherit`). */
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
      default:
        return refuse(context, where, (uint32_t) c->space,
            "a color() space the shape waist cannot carry (a CIE or perceptual space, or a custom one); not converted, because that would approximate it");
      }
    }
    default:
      return refuse(context, where, (uint32_t) c->space,
          "lab(), lch(), oklab() and oklch() are colors the shape waist cannot carry; not converted, because that would approximate them");
    }
  }
  case GTEXT_CSS_COLOR_MIX:
  case GTEXT_CSS_COLOR_LIGHT_DARK:
  case GTEXT_CSS_COLOR_CONTRAST:
  case GTEXT_CSS_COLOR_ALPHA:
  case GTEXT_CSS_COLOR_DEVICE_CMYK:
    return refuse(context, where, 0, "color-mix(), relative colors and the other colors that need a conversion are not carried by the shape waist");
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
