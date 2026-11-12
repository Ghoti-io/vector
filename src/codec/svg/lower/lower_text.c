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
 * @file lower_text.c
 *
 * `text`: the character data of an element shaped with the fonts the caller gave, into glyph runs of the waist. This is the text of
 * a `text` element with no children but character data (the model of chunks, spans and positions is built on it): the white space of
 * `xml:space`, the face chosen character by character from the `font-family` list, shaping with fallback, the pen's advance, and
 * `text-anchor`. A run is one face (a glyph run of the waist has one), so text that falls back to another face is several runs.
 */

#include <ghoti.io/vector/macros.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/shape.h>

#include "../svg_grammar.h"
#include "lower_internal.h"

/* The most characters of one text this lowering shapes. */
#define TEXT_CHARACTERS_MAX ((size_t) 1 << 20)

/* The number a text starts with, read without the locale; false when it does not start with one. */
static bool leading_number(const char * text, double * out) {
  size_t pos = 0;
  return gvec_svg_scan_number(text, strlen(text), &pos, out, NULL);
}

static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* The characters of the element's data nodes, white space handled as `xml:space` says. Nothing but character data may be inside. */
static GVEC_Result gather(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, uint32_t ** out, size_t * count) {
  const GVEC_Svg_Attribute * space = gvec_svg_element_find_attribute(e, "xml:space");
  bool preserve = false;
  for (size_t i = 0; !space && i < e->attribute_count; i++) {
    const GVEC_Svg_Attribute * a = &e->attributes[i];
    if (a->ns && strcmp(a->name, "space") == 0) {
      space = a;
    }
  }
  if (space) {
    size_t n;
    const char * t = gvec_svg_attribute_raw(space, &n);
    preserve = t && n == 8 && strncmp(t, "preserve", 8) == 0;
  }
  size_t bytes = 0;
  for (const GVEC_Svg_Node * node = gvec_svg_element_first_node(e); node; node = gvec_svg_node_next(node)) {
    size_t n;
    if (gvec_svg_node_text(node, &n)) {
      bytes += n;
    }
  }
  char * text = gvec_arena_alloc(&ctx->arena, bytes + 1, 1);
  uint32_t * cps = gvec_arena_array(&ctx->arena, bytes + 1, sizeof *cps, sizeof *cps);
  if (!text || !cps) {
    return GVEC_ERR_OOM;
  }
  size_t at = 0;
  for (const GVEC_Svg_Node * node = gvec_svg_element_first_node(e); node; node = gvec_svg_node_next(node)) {
    size_t n;
    const char * t = gvec_svg_node_text(node, &n);
    for (size_t i = 0; t && i < n; i++) {
      char c = t[i];
      if (c == '\r') {
        continue;
      }
      if (c == '\n' && !preserve) {
        continue; /* A newline is removed (and a tab is a space) unless the white space is preserved (SVG 1.1, 10.15). */
      }
      text[at++] = is_space(c) ? ' ' : c;
    }
  }
  size_t begin = 0, end = at;
  if (!preserve) {
    while (begin < end && text[begin] == ' ') {
      begin++;
    }
    while (end > begin && text[end - 1] == ' ') {
      end--;
    }
  }
  size_t n = 0;
  bool last_space = false;
  for (size_t i = begin; i < end;) {
    unsigned char c = (unsigned char) text[i];
    uint32_t cp;
    size_t len;
    if (c < 0x80) {
      cp = c, len = 1;
    }
    else if (c >= 0xC2 && c < 0xE0 && i + 1 < end && ((unsigned char) text[i + 1] & 0xC0) == 0x80) {
      cp = ((uint32_t) (c & 0x1F) << 6) | ((unsigned char) text[i + 1] & 0x3F), len = 2;
    }
    else if (c >= 0xE0 && c < 0xF0 && i + 2 < end && ((unsigned char) text[i + 1] & 0xC0) == 0x80 && ((unsigned char) text[i + 2] & 0xC0) == 0x80) {
      cp = ((uint32_t) (c & 0x0F) << 12) | ((uint32_t) ((unsigned char) text[i + 1] & 0x3F) << 6) | ((unsigned char) text[i + 2] & 0x3F), len = 3;
    }
    else if (c >= 0xF0 && c < 0xF5 && i + 3 < end && ((unsigned char) text[i + 1] & 0xC0) == 0x80 && ((unsigned char) text[i + 2] & 0xC0) == 0x80 &&
        ((unsigned char) text[i + 3] & 0xC0) == 0x80) {
      cp = ((uint32_t) (c & 0x07) << 18) | ((uint32_t) ((unsigned char) text[i + 1] & 0x3F) << 12) | ((uint32_t) ((unsigned char) text[i + 2] & 0x3F) << 6) |
          ((unsigned char) text[i + 3] & 0x3F), len = 4;
    }
    else {
      cp = 0xFFFD, len = 1;
    }
    i += len;
    if (!preserve && cp == ' ') {
      if (last_space) {
        continue;
      }
      last_space = true;
    }
    else {
      last_space = false;
    }
    cps[n++] = cp;
  }
  *out = cps;
  *count = n;
  return GVEC_OK;
}

/* The names of a `font-family` value, as CSS serialises it: separated by commas, a name optionally in quotes. */
static GVEC_Result families_of(gvec_lw_ctx * ctx, const char * value, const char *** out, size_t * count) {
  size_t length = value ? strlen(value) : 0;
  char * copy = gvec_arena_alloc(&ctx->arena, length + 1, 1);
  const char ** names = gvec_arena_array(&ctx->arena, length + 2, sizeof *names, sizeof(void *));
  if (!copy || !names) {
    return GVEC_ERR_OOM;
  }
  size_t n = 0;
  size_t at = 0;
  while (at < length) {
    while (at < length && (value[at] == ' ' || value[at] == ',' )) {
      at++;
    }
    if (at >= length) {
      break;
    }
    char quote = value[at] == '"' || value[at] == '\'' ? value[at] : 0;
    if (quote) {
      at++;
    }
    char * start = copy + (at);
    size_t from = at;
    while (at < length && (quote ? value[at] != quote : value[at] != ',')) {
      at++;
    }
    size_t to = at;
    if (!quote) {
      while (to > from && value[to - 1] == ' ') {
        to--;
      }
    }
    memcpy(start, value + from, to - from);
    start[to - from] = 0;
    if (to > from) {
      names[n++] = start;
    }
    if (quote && at < length) {
      at++;
    }
  }
  *out = names;
  *count = n;
  return GVEC_OK;
}

static double weight_of(const GVEC_Svg_Style_Value * v) {
  if (v->kind == GVEC_SVG_STYLE_NUMBER) {
    return v->number;
  }
  if (gvec_lw_keyword_is(v, "bold")) {
    return 700;
  }
  if (v->kind == GVEC_SVG_STYLE_KEYWORD && v->keyword) {
    double w;
    if (leading_number(v->keyword, &w) && w >= 1 && w <= 1000) {
      return w;
    }
  }
  return 400;
}

static double stretch_of(const GVEC_Svg_Style_Value * v) {
  static const struct {
    const char * word;
    double percent;
  } words[] = {{"ultra-condensed", 50}, {"extra-condensed", 62.5}, {"condensed", 75}, {"semi-condensed", 87.5}, {"normal", 100},
      {"semi-expanded", 112.5}, {"expanded", 125}, {"extra-expanded", 150}, {"ultra-expanded", 200},
      /* The relative keywords of CSS 2 take a step from `normal`, which is what the cascade gives a text whose parents say nothing. */
      {"narrower", 87.5}, {"wider", 112.5}};
  if (v->kind == GVEC_SVG_STYLE_NUMBER && v->number > 0) {
    return v->number;
  }
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    if (gvec_lw_keyword_is(v, words[i].word)) {
      return words[i].percent;
    }
  }
  if (v->kind == GVEC_SVG_STYLE_KEYWORD && v->keyword) {
    double p;
    if (leading_number(v->keyword, &p) && p > 0) {
      return p;
    }
  }
  return 100;
}

static GVEC_Result read_style(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property prop, GVEC_Svg_Style_Value * v, bool * usable) {
  return gvec_lw_read_property(ctx, inst, prop, GVEC_LOWER_STYLE, v, usable);
}

/* A property of the text group with no longhand in the style layer (`font-variant` and `font-kerning` are shorthands or not registered): the
 * attributes of the element and of every element it inherits from, and their `style`, are read for it. True when one gives a value that
 * is not the initial one. */
static bool chain_sets(const GVEC_Svg_Instance * inst, const char * name, const char * initial) {
  size_t name_length = strlen(name);
  for (; inst; inst = gvec_svg_instance_parent(inst)) {
    const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
    for (size_t i = 0; i < e->attribute_count; i++) {
      const GVEC_Svg_Attribute * a = &e->attributes[i];
      size_t n;
      const char * t = gvec_svg_attribute_raw(a, &n);
      if (a->ns || !t) {
        continue;
      }
      if (strcmp(a->name, name) == 0) {
        size_t begin = 0;
        while (begin < n && is_space(t[begin])) {
          begin++;
        }
        size_t end = n;
        while (end > begin && is_space(t[end - 1])) {
          end--;
        }
        if (!(end - begin == strlen(initial) && strncmp(t + begin, initial, end - begin) == 0) && !(end - begin == 7 && strncmp(t + begin, "inherit", 7) == 0)) {
          return true;
        }
      }
      else if (strcmp(a->name, "style") == 0) {
        for (size_t k = 0; k + name_length < n; k++) {
          if (strncmp(t + k, name, name_length) == 0 && (k == 0 || !(t[k - 1] == '-' || (t[k - 1] >= 'a' && t[k - 1] <= 'z')))) {
            size_t v = k + name_length;
            while (v < n && (is_space(t[v]) || t[v] == ':')) {
              v++;
            }
            size_t initial_length = strlen(initial);
            if (!(v + initial_length <= n && strncmp(t + v, initial, initial_length) == 0) && !(v + 7 <= n && strncmp(t + v, "inherit", 7) == 0)) {
              return true;
            }
          }
        }
      }
    }
  }
  return false;
}

/* What a later story of the milestone lowers is refused, not drawn without it: the element is refused whole when any of these is not its
 * initial value. `*clear` is false when it was refused. */
static GVEC_Result unsupported(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, bool * clear) {
  static const struct {
    GVEC_Svg_Property prop;
    const char * initial[4]; ///< The keywords that mean the initial value; a length or a number of zero does too.
    const char * what;
  } checks[] = {
      {GVEC_SVG_PROP_LETTER_SPACING, {"normal", NULL, NULL, NULL}, "letter-spacing"},
      {GVEC_SVG_PROP_WORD_SPACING, {"normal", NULL, NULL, NULL}, "word-spacing"},
      {GVEC_SVG_PROP_TEXT_DECORATION, {"none", NULL, NULL, NULL}, "text-decoration"},
      {GVEC_SVG_PROP_BASELINE_SHIFT, {"baseline", NULL, NULL, NULL}, "baseline-shift"},
      {GVEC_SVG_PROP_DOMINANT_BASELINE, {"auto", NULL, NULL, NULL}, "dominant-baseline"},
      {GVEC_SVG_PROP_ALIGNMENT_BASELINE, {"auto", "baseline", NULL, NULL}, "alignment-baseline"},
      {GVEC_SVG_PROP_WRITING_MODE, {"horizontal-tb", "lr-tb", "lr", NULL}, "writing-mode"},
      {GVEC_SVG_PROP_DIRECTION, {"ltr", NULL, NULL, NULL}, "direction"},
      {GVEC_SVG_PROP_UNICODE_BIDI, {"normal", NULL, NULL, NULL}, "unicode-bidi"},
      {GVEC_SVG_PROP_FONT_VARIANT, {"normal", NULL, NULL, NULL}, "font-variant"},
      {GVEC_SVG_PROP_FONT_SIZE_ADJUST, {"none", NULL, NULL, NULL}, "font-size-adjust"},
  };
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *clear = false;
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    GVEC_Svg_Style_Value v;
    bool ok;
    GVEC_Result r = read_style(ctx, inst, checks[i].prop, &v, &ok);
    if (r != GVEC_OK || !ok) {
      return r;
    }
    bool initial = ((v.kind == GVEC_SVG_STYLE_LENGTH || v.kind == GVEC_SVG_STYLE_NUMBER) && v.number == 0) || v.kind == GVEC_SVG_STYLE_NONE;
    for (size_t k = 0; !initial && k < 4 && checks[i].initial[k]; k++) {
      initial = gvec_lw_keyword_is(&v, checks[i].initial[k]);
    }
    if (!initial && v.kind == GVEC_SVG_STYLE_KEYWORD && v.keyword) {
      /* A length that the cascade left as it was written (`0px`) is the initial value when it is zero. */
      double zero;
      initial = leading_number(v.keyword, &zero) && zero == 0;
    }
    if (!initial) {
      char message[96];
      snprintf(message, sizeof message, "%s on text is not lowered yet", checks[i].what);
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, message);
    }
  }
  if (chain_sets(inst, "font-variant", "normal") || chain_sets(inst, "font-kerning", "auto")) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "font-variant and font-kerning on text are not lowered yet");
  }
  for (size_t i = 0; i < e->attribute_count; i++) {
    const GVEC_Svg_Attribute * a = &e->attributes[i];
    if (a->ns) {
      continue;
    }
    if (strcmp(a->name, "dx") == 0 || strcmp(a->name, "dy") == 0 || strcmp(a->name, "rotate") == 0 || strcmp(a->name, "textLength") == 0 ||
        strcmp(a->name, "lengthAdjust") == 0) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "positions, rotation and length adjustment of text are not lowered yet");
    }
    if (strcmp(a->name, "x") == 0 || strcmp(a->name, "y") == 0) {
      const GVEC_Svg_Length * items;
      size_t count;
      if (gvec_svg_attribute_class(a) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_lengths(a, &items, &count) && count > 1) {
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a list of positions on text is not lowered yet");
      }
    }
  }
  *clear = true;
  return GVEC_OK;
}

GVEC_Result gvec_lw_text_layout(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_text * out, bool * usable) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  memset(out, 0, sizeof *out);
  *usable = false;
  const GVEC_Fonts * fonts = ctx->options ? ctx->options->fonts : NULL;
  if (!fonts) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "no fonts were given, so text cannot be drawn");
  }
  if (gvec_svg_element_first_child(e)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "text with spans is not lowered yet");
  }
  bool clear;
  GVEC_Result r = unsupported(ctx, inst, &clear);
  if (r != GVEC_OK || !clear) {
    return r;
  }
  GVEC_Svg_Style_Value v;
  bool ok;
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_FAMILY, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  const char ** families;
  size_t family_count;
  r = families_of(ctx, v.kind == GVEC_SVG_STYLE_KEYWORD ? v.keyword : NULL, &families, &family_count);
  if (r != GVEC_OK) {
    return r;
  }
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_SIZE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  double size = v.kind == GVEC_SVG_STYLE_LENGTH || v.kind == GVEC_SVG_STYLE_NUMBER ? v.number : 12;
  if (!(size > 0) || size - size != 0) {
    return GVEC_OK; /* A size of nothing draws nothing. */
  }
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_WEIGHT, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  double weight = weight_of(&v);
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_STYLE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  int italic = gvec_lw_keyword_is(&v, "italic") ? 1 : (v.kind == GVEC_SVG_STYLE_KEYWORD && v.keyword && strncmp(v.keyword, "oblique", 7) == 0) ? 2 : 0;
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_STRETCH, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  double width = stretch_of(&v);
  r = read_style(ctx, inst, GVEC_SVG_PROP_TEXT_ANCHOR, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  double anchor = gvec_lw_keyword_is(&v, "middle") ? 0.5 : gvec_lw_keyword_is(&v, "end") ? 1.0 : 0.0;

  uint32_t * cps;
  size_t count;
  r = gather(ctx, e, &cps, &count);
  if (r != GVEC_OK) {
    return r;
  }
  if (count == 0) {
    *usable = true;
    return GVEC_OK;
  }
  if (count > TEXT_CHARACTERS_MAX) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text has more characters than the lowering shapes");
  }

  /* The faces: one per character, in the order they are first needed. */
  const GFNT_Face * faces[16];
  size_t face_count = 0;
  const GFNT_Face * previous = NULL;
  uint32_t previous_cp = 0xFFFFFFFFu;
  for (size_t i = 0; i < count; i++) {
    const GFNT_Face * face = NULL;
    bool found;
    if (cps[i] == previous_cp && previous) {
      face = previous;
    }
    else {
      r = gvec_fonts_face(fonts, families, family_count, weight, italic, width, cps[i], &face, &found);
      if (r != GVEC_OK) {
        return r;
      }
      if (!found) {
        face = face_count ? faces[0] : NULL;
      }
      previous = face;
      previous_cp = cps[i];
    }
    if (!face) {
      continue;
    }
    bool have = false;
    for (size_t k = 0; k < face_count; k++) {
      have = have || faces[k] == face;
    }
    if (!have) {
      if (face_count == sizeof faces / sizeof faces[0]) {
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text needs more faces than the lowering keeps");
      }
      faces[face_count++] = face;
    }
  }
  if (face_count == 0) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "no face of the fonts given has the text's characters");
  }

  GFNT_ShapeOptions shape;
  memset(&shape, 0, sizeof shape);
  GFNT_FaceRuns runs;
  memset(&runs, 0, sizeof runs);
  GFNT_Result sr = gfnt_faces_shape_bidi(faces, face_count, cps, count, GFNT_BIDI_LTR, &shape, NULL, &runs, NULL);
  if (sr != GFNT_OK) {
    return sr == GFNT_ERR_OOM ? GVEC_ERR_OOM : gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "the text could not be shaped with the fonts given");
  }

  /* The pen's advance over all of it, then the anchor. */
  double total = 0;
  double scale[16];
  for (size_t i = 0; i < runs.count; i++) {
    uint16_t upem = 0;
    if (gfnt_face_units_per_em(faces[runs.runs[i].face], &upem, NULL) != GFNT_OK || upem == 0) {
      gfnt_face_runs_free(&runs);
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a face of the fonts has no size to scale by");
    }
    scale[runs.runs[i].face] = size / (double) upem;
    for (size_t g = 0; g < runs.runs[i].run.count; g++) {
      total += (double) runs.runs[i].run.glyphs[g].x_advance * scale[runs.runs[i].face];
    }
  }
  double pen = -total * anchor;

  out->pieces = gvec_arena_array(&ctx->arena, runs.count ? runs.count : 1, sizeof *out->pieces, sizeof(void *));
  if (!out->pieces) {
    gfnt_face_runs_free(&runs);
    return GVEC_ERR_OOM;
  }
  for (size_t i = 0; r == GVEC_OK && i < runs.count; i++) {
    const GFNT_FaceRun * fr = &runs.runs[i];
    if (fr->run.count == 0) {
      continue;
    }
    GSHSCENE_Glyph * glyphs = gvec_arena_array(&ctx->arena, fr->run.count, sizeof *glyphs, sizeof(double));
    if (!glyphs) {
      r = GVEC_ERR_OOM;
      break;
    }
    double s = scale[fr->face];
    for (size_t g = 0; g < fr->run.count; g++) {
      const GFNT_ShapedGlyph * sg = &fr->run.glyphs[g];
      glyphs[g].id = sg->glyph;
      glyphs[g].x = pen + (double) sg->x_offset * s;
      glyphs[g].y = -(double) sg->y_offset * s;
      glyphs[g].outline = NULL;
      pen += (double) sg->x_advance * s;
    }
    const GSHSCENE_Resource * face_resource = NULL;
    r = gvec_fonts_scene_face(fonts, faces[fr->face], ctx->store, &face_resource);
    if (r != GVEC_OK) {
      break;
    }
    GSHSCENE_Glyph_Run run;
    memset(&run, 0, sizeof run);
    run.source = GSHSCENE_GLYPHS_FROM_FONT;
    run.face = face_resource;
    run.size = size;
    run.hint_mode = GSHSCENE_HINT_NONE;
    run.glyph_count = fr->run.count;
    run.glyphs = glyphs;
    const GSHSCENE_Resource * resource = NULL;
    GSHSCENE_Result gr = gshscene_store_intern_glyph_run(ctx->store, &run, &resource);
    gshscene_resource_release(face_resource);
    if (gr != GSHSCENE_OK) {
      r = gr == GSHSCENE_ERR_OOM ? GVEC_ERR_OOM : gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text is past what the waist can carry");
      break;
    }
    r = gvec_lw_hold(ctx, resource);
    if (r != GVEC_OK) {
      break;
    }
    out->pieces[out->count++] = resource;
  }
  gfnt_face_runs_free(&runs);
  if (r != GVEC_OK) {
    return r;
  }
  *usable = true;
  return GVEC_OK;
}
