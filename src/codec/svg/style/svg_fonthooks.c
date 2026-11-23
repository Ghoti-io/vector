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
 * @file svg_fonthooks.c
 *
 * The font-relative units of a style (`ex`, `ch`, `cap`, `ic`, a `normal` line height), answered from the fonts of a lowering.  They are of the
 * style side: they read a computed `font-family` through the text library's CSS values, which the lowering objects do not reference.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/text/css/css_value.h>

#include "svg_style.h"
#include "../lower/lower_internal.h"

//-----------------------------------------------------------------------------
// The font-relative units of a style
//-----------------------------------------------------------------------------

/* The names of a serialised `font-family`, split in place: commas separate, and a name may be in quotes. */
static size_t split_families(char * text, const char ** names, size_t capacity) {
  size_t n = 0;
  char * at = text;
  while (*at && n < capacity) {
    while (*at == ' ' || *at == ',') {
      at++;
    }
    if (!*at) {
      break;
    }
    char quote = *at == '"' || *at == '\'' ? *at : 0;
    if (quote) {
      at++;
    }
    char * start = at;
    while (*at && (quote ? *at != quote : *at != ',')) {
      at++;
    }
    char * end = at;
    if (!quote) {
      while (end > start && end[-1] == ' ') {
        end--;
      }
    }
    const bool more = *at != 0;
    *end = 0;
    if (end > start) {
      names[n++] = start;
    }
    if (more) {
      at++;
    }
  }
  return n;
}

/* The face the query asks about, for the character `cp`; NULL when the fonts have none. */
static const GFNT_Face * query_face(const GVEC_Fonts * fonts, const GTEXT_CSS_Font_Query * q, uint32_t cp) {
  const char * names[16];
  size_t count = 0;
  char * buffer = NULL;
  if (q->family) {
    size_t length = 0;
    if (gtext_css_serialize_value_buffer(q->family, NULL, &buffer, &length) != GTEXT_CSS_OK) {
      return NULL;
    }
    count = split_families(buffer, names, 16);
  }
  const GFNT_Face * face = NULL;
  bool found = false;
  GVEC_Result r = count ? gvec_fonts_face(fonts, names, count, q->weight, q->italic ? 1 : 0, q->width_percent, cp, 0, &face, &found) : GVEC_OK;
  if (buffer) {
    gtext_css_serialize_buffer_free(NULL, buffer);
  }
  return r == GVEC_OK && found ? face : NULL;
}

/* The advance of the glyph of `cp` in `face` as a fraction of the em; false when the face has none. */
static bool advance_em(const GFNT_Face * face, uint32_t cp, double * em) {
  uint16_t upem = 0;
  uint32_t glyph = 0;
  int32_t advance = 0;
  if (gfnt_face_units_per_em(face, &upem, NULL) != GFNT_OK || upem == 0 || gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) != GFNT_OK ||
      glyph == 0 || gfnt_face_glyph_advance(face, glyph, NULL, &advance, NULL) != GFNT_OK) {
    return false;
  }
  *em = (double) advance / (double) upem;
  return true;
}

/* The metrics a font gives (CSS Values 4, 6.1.1): the x-height and the advance of "0" are half the em when the font has none, the cap height
 * 0.7 em, the advance of U+6C34 one em. */
static bool fonts_metric(void * user, const GTEXT_CSS_Font_Query * q, GTEXT_CSS_Font_Metric metric, double * px) {
  const GVEC_Fonts * fonts = user;
  double em = 0;
  switch (metric) {
  case GTEXT_CSS_METRIC_EX:
  case GTEXT_CSS_METRIC_CH:
  case GTEXT_CSS_METRIC_CAP:
  case GTEXT_CSS_METRIC_IC:
    break;
  default:
    return false;
  }
  const double fallback = metric == GTEXT_CSS_METRIC_CAP ? 0.7 : metric == GTEXT_CSS_METRIC_IC ? 1.0 : 0.5;
  const uint32_t cp = metric == GTEXT_CSS_METRIC_EX ? 'x' : metric == GTEXT_CSS_METRIC_CH ? '0' : metric == GTEXT_CSS_METRIC_CAP ? 'H' : 0x6C34;
  const GFNT_Face * face = query_face(fonts, q, cp);
  em = fallback;
  if (face) {
    uint16_t upem = 0;
    const GFNT_Os2 * os2 = NULL;
    if (metric == GTEXT_CSS_METRIC_CH || metric == GTEXT_CSS_METRIC_IC) {
      double a;
      if (advance_em(face, cp, &a)) {
        em = a;
      }
    }
    else if (gfnt_face_units_per_em(face, &upem, NULL) == GFNT_OK && upem && gfnt_face_os2(face, &os2, NULL) == GFNT_OK && os2) {
      const int16_t v = metric == GTEXT_CSS_METRIC_EX ? os2->x_height : os2->cap_height;
      if (v > 0) {
        em = (double) v / (double) upem;
      }
    }
  }
  *px = q->size_px * em;
  return true;
}

/* The used line height of `normal`: the face's ascent, descent and line gap (CSS Inline 3). */
static bool fonts_line_height(void * user, const GTEXT_CSS_Font_Query * q, double * px) {
  const GVEC_Fonts * fonts = user;
  const GFNT_Face * face = query_face(fonts, q, 'x');
  uint16_t upem = 0;
  GFNT_LineMetrics line;
  if (face && gfnt_face_units_per_em(face, &upem, NULL) == GFNT_OK && upem && gfnt_face_line_metrics(face, GFNT_LINE_METRICS_FONT, NULL, &line, NULL) == GFNT_OK) {
    *px = q->size_px * (double) (line.ascent - line.descent + line.line_gap) / (double) upem;
    return true;
  }
  *px = q->size_px * 1.2;
  return true;
}

void gvec_fonts_style_hooks(const GVEC_Fonts * fonts, gvec_svg_font_hooks * hooks) {
  hooks->user = (void *) fonts;
  hooks->metric = fonts_metric;
  hooks->line_height = fonts_line_height;
  hooks->reuse = NULL;
}
