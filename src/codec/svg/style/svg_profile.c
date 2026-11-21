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
 * @file svg_profile.c
 *
 * `@color-profile` (CSS Color 5, 10): a `color(--name ...)` or a `device-cmyk()` is a color in the space an ICC profile describes. The
 * profile is the `src` of the rule, asked of the caller's resolver like any other resource (GVEC_RESOURCE_COLOR_PROFILE; the library never
 * fetches), parsed by the color library and made a transform to linear Rec. 2020, the gamut the other converted colors are carried in. A profile
 * is had once for a document, whatever the number of colors that name it; one that cannot be had, read or used is remembered as failed, with
 * the reason, so that every color that names it is refused the same way and the reason is reported where the color is read.
 */

#include <ghoti.io/vector/macros.h>

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/color/color.h>
#include <ghoti.io/color/icc.h>
#include <ghoti.io/color/transform.h>

#include "../svg_resource.h"
#include "svg_style.h"

struct gvec_svg_profile {
  char * name;                  ///< The dashed name of the rule, or `device-cmyk`; owned.
  size_t name_length;
  GCOL_ICC_Profile * icc;       ///< The parsed profile, or NULL when it could not be had.
  GCOL_Transform * transform;   ///< To linear Rec. 2020, or NULL.
  unsigned channels;            ///< How many channels a color in this space has: 1, 3 or 4.
  const char * why;             ///< Static: why it cannot be used; NULL when it can.
};

void gvec_svg_style_profiles_free(GVEC_Svg_Style_Context * context) {
  for (size_t i = 0; i < context->profile_count; i++) {
    gvec_svg_profile * p = &context->profiles[i];
    gcol_transform_free(p->transform);
    gcol_icc_free(p->icc);
    gvec_free(context->allocator, p->name);
  }
  gvec_free(context->allocator, context->profiles);
  context->profiles = NULL;
  context->profile_count = context->profile_capacity = 0;
}

static unsigned sig(char a, char b, char c, char d) {
  return ((unsigned) (unsigned char) a << 24) | ((unsigned) (unsigned char) b << 16) | ((unsigned) (unsigned char) c << 8) | (unsigned char) d;
}

static GCOL_Rendering_Intent intent_of(GTEXT_CSS_Rendering_Intent intent) {
  switch (intent) {
  case GTEXT_CSS_INTENT_ABSOLUTE_COLORIMETRIC:
    return GCOL_INTENT_ABSOLUTE_COLORIMETRIC;
  case GTEXT_CSS_INTENT_PERCEPTUAL:
    return GCOL_INTENT_PERCEPTUAL;
  case GTEXT_CSS_INTENT_SATURATION:
    return GCOL_INTENT_SATURATION;
  case GTEXT_CSS_INTENT_RELATIVE_COLORIMETRIC:
  default:
    return GCOL_INTENT_RELATIVE_COLORIMETRIC;
  }
}

/* Make the transform of a profile whose bytes are in hand. */
static void make_transform(gvec_svg_profile * p, const GTEXT_CSS_Color_Profile * rule, const unsigned char * bytes, size_t size) {
  if (gcol_icc_parse(NULL, NULL, bytes, size, &p->icc) != GCOL_OK) {
    p->why = "the profile of a @color-profile is not an ICC profile this library reads";
    return;
  }
  const unsigned space = gcol_icc_data_space(p->icc);
  GCOL_Pixel_Model model = GCOL_PIXEL_RGB;
  if (space == sig('R', 'G', 'B', ' ')) {
    p->channels = 3;
  }
  else if (space == sig('C', 'M', 'Y', 'K')) {
    p->channels = 4;
    model = GCOL_PIXEL_CMYK;
  }
  else if (space == sig('G', 'R', 'A', 'Y')) {
    p->channels = 1;
    model = GCOL_PIXEL_GRAY;
  }
  else {
    p->why = "the profile of a @color-profile is of a color space this library does not convert (it is not RGB, CMYK or gray)";
    return;
  }
  if (rule->component_count != 0 && rule->component_count != p->channels) {
    p->why = "the `components` of a @color-profile are not as many as the channels of its profile";
    return;
  }
  GCOL_Color_Info dst;
  gcol_color_info_default(&dst);
  gcol_color_info_set_gamut(&dst, GCOL_PRIMARIES_BT2020);
  dst.transfer = GCOL_TRANSFER_LINEAR;
  dst.reference = GCOL_REFERENCE_DISPLAY;
  dst.sample_scale = GCOL_SAMPLE_SCALE_RELATIVE;
  const GCOL_Rendering_Intent intent = intent_of(rule->intent);
  GCOL_Result r;
  GCOL_Color_Info src;
  if (model != GCOL_PIXEL_CMYK && gcol_icc_is_matrix_trc(p->icc) && gcol_icc_to_color_info(p->icc, &src) == GCOL_OK) {
    r = gcol_transform_create_model(NULL, &src, model, &dst, GCOL_PIXEL_RGB, intent, 0, &p->transform);
  }
  else {
    /* A table profile; a CMYK one is read as inks, 0 being none (CSS Color 5 writes the channels of device-cmyk() so). */
    r = gcol_transform_create_icc(NULL, p->icc, &dst, intent, 0, GCOL_CMYK_POLARITY_REFLECTION, &p->transform);
  }
  if (r != GCOL_OK || !p->transform) {
    p->transform = NULL;
    p->why = "the profile of a @color-profile cannot be converted at its rendering intent (it has no table for it)";
  }
}

static GVEC_Result have_profile(GVEC_Svg_Style_Context * context, const char * name, size_t length, gvec_svg_profile ** out) {
  for (size_t i = 0; i < context->profile_count; i++) {
    gvec_svg_profile * p = &context->profiles[i];
    if (p->name_length == length && memcmp(p->name, name, length) == 0) {
      *out = p;
      return GVEC_OK;
    }
  }
  if (context->profile_count == context->profile_capacity) {
    size_t capacity = context->profile_capacity ? context->profile_capacity * 2 : 4;
    gvec_svg_profile * grown = gvec_alloc(context->allocator, capacity * sizeof *grown);
    if (!grown) {
      return GVEC_ERR_OOM;
    }
    if (context->profile_count) {
      memcpy(grown, context->profiles, context->profile_count * sizeof *grown);
    }
    gvec_free(context->allocator, context->profiles);
    context->profiles = grown;
    context->profile_capacity = capacity;
  }
  gvec_svg_profile * p = &context->profiles[context->profile_count];
  memset(p, 0, sizeof *p);
  p->name = gvec_alloc(context->allocator, length + 1);
  if (!p->name) {
    return GVEC_ERR_OOM;
  }
  memcpy(p->name, name, length);
  p->name[length] = 0;
  p->name_length = length;
  context->profile_count++;
  *out = p;
  const GTEXT_CSS_Color_Profile * rule = gvec_svg_style_color_profile(context, name, length);
  if (!rule || !rule->src.data || rule->src.length == 0) {
    p->why = "a color names a @color-profile that has no src";
    return GVEC_OK;
  }
  if (!context->res || !gvec_res_enabled(context->res)) {
    p->why = "the profile of a @color-profile is not fetched (there is no resolver)";
    return GVEC_OK;
  }
  char * reference = gvec_alloc(context->allocator, rule->src.length + 1);
  if (!reference) {
    return GVEC_ERR_OOM;
  }
  memcpy(reference, rule->src.data, rule->src.length);
  reference[rule->src.length] = 0;
  gvec_res_outcome outcome;
  gvec_res_entry * entry = NULL;
  GVEC_Result r = gvec_res_fetch(context->res, GVEC_RESOURCE_COLOR_PROFILE, context->base, reference, context->limits.max_image_bytes,
      context->res_depth + 1, &outcome, &entry);
  gvec_free(context->allocator, reference);
  if (r != GVEC_OK) {
    return r;
  }
  switch (outcome) {
  case GVEC_RES_OK:
    make_transform(p, rule, entry->bytes, entry->size);
    break;
  case GVEC_RES_NO_RESOLVER:
  case GVEC_RES_REFUSED:
    p->why = "the profile of a @color-profile is refused by the resolver";
    break;
  case GVEC_RES_NOT_FOUND:
    p->why = "the profile of a @color-profile is not found by the resolver";
    break;
  case GVEC_RES_FAILED:
    p->why = "the resolver failed to give the profile of a @color-profile";
    break;
  case GVEC_RES_TOO_BIG:
    p->why = "the profile of a @color-profile is longer than max_image_bytes or max_resource_bytes allows";
    break;
  case GVEC_RES_LIMIT_COUNT:
    p->why = "the profile of a @color-profile would pass max_resources";
    break;
  case GVEC_RES_LIMIT_DEPTH:
    p->why = "the profile of a @color-profile is nested deeper than max_resource_depth";
    break;
  }
  return GVEC_OK;
}

GVEC_Result gvec_svg_style_profile_color(GVEC_Svg_Style_Context * context, const char * name, size_t length, const double * channels, size_t count,
    double rgb[3], const char ** why) {
  *why = NULL;
  gvec_svg_profile * p;
  GVEC_Result r = have_profile(context, name, length, &p);
  if (r != GVEC_OK) {
    return r;
  }
  if (p->why) {
    *why = p->why;
    return GVEC_ERR_UNSUPPORTED;
  }
  if (count != p->channels) {
    *why = "a color has other channels than the profile of its @color-profile";
    return GVEC_ERR_UNSUPPORTED;
  }
  GCOL_Sample_Layout in = {GCOL_SAMPLE_FLOAT64, (unsigned) count, p->channels == 4 ? GCOL_PIXEL_CMYK : p->channels == 1 ? GCOL_PIXEL_GRAY : GCOL_PIXEL_RGB};
  GCOL_Sample_Layout out = {GCOL_SAMPLE_FLOAT64, 3, GCOL_PIXEL_RGB};
  double src[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < count && i < 4; i++) {
    src[i] = channels[i] < 0 ? 0 : channels[i] > 1 ? 1 : channels[i];
  }
  if (gcol_transform_apply(p->transform, &in, src, &out, rgb, 1) != GCOL_OK) {
    *why = "a color of a @color-profile cannot be converted";
    return GVEC_ERR_UNSUPPORTED;
  }
  return GVEC_OK;
}
