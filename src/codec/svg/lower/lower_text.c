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
 * `text`: the content of an element, spans and all, shaped with the fonts the caller gave, into glyph runs of the waist. This is the text of
 * a `text` element with no children but character data (the model of chunks, spans and positions is built on it): the white space of
 * `xml:space`, the face chosen character by character from the `font-family` list, shaping with fallback, the pen's advance, and
 * `text-anchor`. A run is one face (a glyph run of the waist has one), so text that falls back to another face is several runs.
 */

#include <ghoti.io/vector/macros.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/shape-geometry/map.h>
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
 * value the nearest element that gives one has, in `out`, read from its attribute (when `attributes`) and from its `style`; an `inherit`
 * looks further up. False when no element gives one. */
static bool chain_value(const GVEC_Svg_Instance * inst, const char * name, bool attributes, bool inherit, char * out, size_t capacity) {
  size_t name_length = strlen(name);
  for (bool first = true; inst && (first || inherit); first = false, inst = gvec_svg_instance_parent(inst)) {
    const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
    const char * found = NULL;
    size_t found_length = 0;
    /* The `font` shorthand in an element's style resets the properties it covers that this reads. */
    for (size_t i = 0; i < e->attribute_count; i++) {
      const GVEC_Svg_Attribute * a = &e->attributes[i];
      size_t n;
      const char * t = gvec_svg_attribute_raw(a, &n);
      if (a->ns || !t || strcmp(a->name, "style") != 0 || (strcmp(name, "font-kerning") != 0 && strcmp(name, "font-variant") != 0)) {
        continue;
      }
      for (size_t k = 0; k + 4 < n; k++) {
        if (strncmp(t + k, "font", 4) == 0 && (k == 0 || is_space(t[k - 1]) || t[k - 1] == ';')) {
          size_t v = k + 4;
          while (v < n && is_space(t[v])) {
            v++;
          }
          if (v < n && t[v] == ':') {
            return false;
          }
        }
      }
    }
    for (size_t i = 0; i < e->attribute_count; i++) {
      const GVEC_Svg_Attribute * a = &e->attributes[i];
      size_t n;
      const char * t = gvec_svg_attribute_raw(a, &n);
      if (a->ns || !t) {
        continue;
      }
      if (attributes && strcmp(a->name, name) == 0) {
        found = t, found_length = n;
      }
      else if (strcmp(a->name, "style") == 0) {
        for (size_t k = 0; k + name_length < n; k++) {
          if (strncmp(t + k, name, name_length) == 0 && (k == 0 || !(t[k - 1] == '-' || (t[k - 1] >= 'a' && t[k - 1] <= 'z')))) {
            size_t v = k + name_length;
            while (v < n && is_space(t[v])) {
              v++;
            }
            if (v >= n || t[v] != ':') {
              continue;
            }
            v++;
            size_t end = v;
            while (end < n && t[end] != ';') {
              end++;
            }
            found = t + v, found_length = end - v;
          }
        }
      }
    }
    if (found) {
      while (found_length && is_space(*found)) {
        found++, found_length--;
      }
      while (found_length && is_space(found[found_length - 1])) {
        found_length--;
      }
      if (!(found_length == 7 && strncmp(found, "inherit", 7) == 0)) {
        size_t n = found_length < capacity - 1 ? found_length : capacity - 1;
        memcpy(out, found, n);
        out[n] = 0;
        return true;
      }
    }
  }
  return false;
}

/* The length a `letter-spacing` or `word-spacing` of the cascade says, in user units: it comes as written (`5`, `1mm`, `5%`, `normal`). A
 * percentage is of the font size. False for what this does not read. */
static bool spacing_of(const GVEC_Svg_Instance * inst, const GVEC_Svg_Style_Value * v, double font_size, bool percent_of_font, double * out) {
  *out = 0;
  if (v->kind == GVEC_SVG_STYLE_LENGTH || v->kind == GVEC_SVG_STYLE_NUMBER) {
    *out = v->number;
    return true;
  }
  if (v->kind != GVEC_SVG_STYLE_KEYWORD || !v->keyword || gvec_lw_keyword_is(v, "normal")) {
    return v->kind != GVEC_SVG_STYLE_KEYWORD || !v->keyword || gvec_lw_keyword_is(v, "normal");
  }
  size_t length = strlen(v->keyword);
  size_t pos = 0;
  double number;
  if (!gvec_svg_scan_number(v->keyword, length, &pos, &number, NULL)) {
    return false;
  }
  const char * unit = v->keyword + pos;
  GVEC_Svg_Length l;
  l.value = number;
  if (strcmp(unit, "") == 0) {
    l.unit = GVEC_SVG_UNIT_NONE;
  }
  else if (strcmp(unit, "px") == 0) {
    l.unit = GVEC_SVG_UNIT_PX;
  }
  else if (strcmp(unit, "mm") == 0) {
    l.unit = GVEC_SVG_UNIT_MM;
  }
  else if (strcmp(unit, "cm") == 0) {
    l.unit = GVEC_SVG_UNIT_CM;
  }
  else if (strcmp(unit, "in") == 0) {
    l.unit = GVEC_SVG_UNIT_IN;
  }
  else if (strcmp(unit, "pt") == 0) {
    l.unit = GVEC_SVG_UNIT_PT;
  }
  else if (strcmp(unit, "pc") == 0) {
    l.unit = GVEC_SVG_UNIT_PC;
  }
  else if (strcmp(unit, "%") == 0) {
    double vw, vh;
    gvec_svg_instance_viewport(inst, &vw, &vh);
    *out = number * (percent_of_font ? font_size : vw) / 100.0;
    return true;
  }
  else if (strcmp(unit, "em") == 0) {
    *out = number * font_size;
    return true;
  }
  else {
    return false;
  }
  return gvec_lw_absolute_length(&l, out);
}

/* What a later story of the milestone lowers is refused, not drawn without it: the element is refused whole when any of these is not its
 * initial value. `*clear` is false when it was refused. */
static GVEC_Result unsupported(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, bool * clear) {
  static const struct {
    GVEC_Svg_Property prop;
    const char * initial[4]; ///< The keywords that mean the initial value; a length or a number of zero does too.
    const char * what;
  } checks[] = {
      {GVEC_SVG_PROP_WRITING_MODE, {"horizontal-tb", "lr-tb", "lr", NULL}, "writing-mode"},
      {GVEC_SVG_PROP_DIRECTION, {"ltr", NULL, NULL, NULL}, "direction"},
      {GVEC_SVG_PROP_UNICODE_BIDI, {"normal", NULL, NULL, NULL}, "unicode-bidi"},
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
    bool initial = ((v.kind == GVEC_SVG_STYLE_LENGTH || v.kind == GVEC_SVG_STYLE_NUMBER) && v.number == 0) ||
        v.kind == GVEC_SVG_STYLE_NONE;
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
  {
    char value[32];
    if (chain_value(inst, "font-variant", true, true, value, sizeof value) && strcmp(value, "normal") != 0 && strcmp(value, "small-caps") != 0) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "this font-variant on text is not lowered yet");
    }
    /* `writing-mode` is read as written as well: the legacy values (`tb`) are not values of the property, and the cascade drops them. */
    if (chain_value(inst, "writing-mode", true, true, value, sizeof value) && strcmp(value, "lr-tb") != 0 && strcmp(value, "lr") != 0 &&
        strcmp(value, "horizontal-tb") != 0) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "writing-mode on text is not lowered yet");
    }
    if (chain_value(inst, "kerning", true, true, value, sizeof value) && strcmp(value, "auto") != 0 && strcmp(value, "0") != 0) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a kerning other than auto and 0 on text is not lowered yet");
    }
  }
  *clear = true;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// The text's content: characters, each with the span it is in and the positions the lists give it
//-----------------------------------------------------------------------------

/** What a span says about the face and the line: read once for each element of the text. */
typedef struct tstyle {
  const GVEC_Svg_Instance * inst;
  const char ** families;
  size_t family_count;
  double size, weight, width, anchor;
  double letter_spacing, word_spacing;
  int dominant, alignment; ///< The baseline the text is set on, and the one a span is aligned by (-1: its parent's): see baseline_kind().
  double shift_length;     ///< The baseline-shifts of the span and its ancestors that are lengths, up positive, in user units.
  int shift_sub, shift_super; ///< How many of them are `sub` and `super`: the face says how far each is.
  int italic;
  bool visible, small_caps, kerning_off;
} tstyle;

/* Whether two spans ask for one font: their characters are shaped together, so kerning and joining run across the boundary. */
static bool same_font(const tstyle * a, const tstyle * b) {
  if (a->size != b->size || a->weight != b->weight || a->width != b->width || a->italic != b->italic || a->family_count != b->family_count || a->small_caps != b->small_caps || a->kerning_off != b->kerning_off || a->dominant != b->dominant || a->alignment != b->alignment ||
      a->shift_length != b->shift_length || a->shift_sub != b->shift_sub || a->shift_super != b->shift_super) {
    return false;
  }
  for (size_t i = 0; i < a->family_count; i++) {
    if (strcmp(a->families[i], b->families[i]) != 0) {
      return false;
    }
  }
  return true;
}

typedef struct tchar {
  uint32_t cp;
  uint32_t style;
  bool has_x, has_y, has_dy, has_rotate;
  double x, y, dx, dy;
  double rotate; ///< Degrees, about the glyph's origin.
} tchar;

/** A list of a span, applied to the characters of its content once the content is known (the tree's order, outer first, so that an inner list wins). */
typedef struct tlist {
  size_t start, end;   ///< The characters of the element's content (end is set when the content is known).
  const GVEC_Svg_Length * items;
  const double * numbers; ///< Kind 4: the angles.
  size_t count;
  int kind; ///< 0 x, 1 y, 2 dx, 3 dy, 4 rotate.
  const GVEC_Svg_Instance * inst;
} tlist;

/** A `textLength` of an element: the content from `start` to `end` is to take `length`. */
typedef struct tlength {
  size_t start, end;
  double length;
  bool glyphs; ///< `lengthAdjust` is `spacingAndGlyphs`: the glyphs are stretched too.
  const GVEC_Svg_Instance * inst;
  GSHGEOM_Map map; ///< The stretch, once laid out (spacingAndGlyphs).
} tlength;

/** A `text-decoration` of an element (or of an ancestor of the text): the lines are drawn across its content, with its own paint and font. */
typedef struct tdecor {
  const GVEC_Svg_Instance * inst;
  uint32_t style;
  size_t start, end;
  unsigned flags; ///< 1 underline, 2 overline, 4 line-through.
} tdecor;

typedef struct tctx {
  gvec_lw_ctx * lw;
  tstyle * styles;
  size_t style_count, style_capacity;
  tchar * chars;
  size_t count, capacity;
  tlist * lists;
  size_t list_count, list_capacity;
  tlength * lengths;
  size_t length_count, length_capacity;
  tdecor * decors;
  size_t decor_count, decor_capacity;
  bool last_space;
  bool refused;
} tctx;

#define TEXT_DEPTH_MAX 64

/* The baseline a keyword names: 0 alphabetic, then central, middle, hanging, mathematical, ideographic, before-edge, after-edge; -1 for the keywords
 * that name no baseline of their own (`auto`, `baseline`, `use-script`, `no-change`, `reset-size`) and for what is not one. */
static int baseline_kind(const GVEC_Svg_Style_Value * v) {
  static const struct {
    const char * word;
    int kind;
  } words[] = {{"alphabetic", 0}, {"central", 1}, {"middle", 2}, {"hanging", 3}, {"mathematical", 4}, {"ideographic", 5}, {"text-before-edge", 6},
      {"before-edge", 6}, {"text-after-edge", 7}, {"after-edge", 7}};
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    if (gvec_lw_keyword_is(v, words[i].word)) {
      return words[i].kind;
    }
  }
  return -1;
}

static GVEC_Result span_style(tctx * t, const GVEC_Svg_Instance * inst, const tstyle * parent, uint32_t * index) {
  gvec_lw_ctx * ctx = t->lw;
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  if (t->style_count == t->style_capacity) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text has more spans than the lowering keeps");
  }
  tstyle * st = &t->styles[t->style_count];
  memset(st, 0, sizeof *st);
  st->inst = inst;
  GVEC_Svg_Style_Value v;
  bool ok;
  GVEC_Result r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_FAMILY, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  r = families_of(ctx, v.kind == GVEC_SVG_STYLE_KEYWORD ? v.keyword : NULL, &st->families, &st->family_count);
  if (r != GVEC_OK) {
    return r;
  }
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_SIZE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->size = v.kind == GVEC_SVG_STYLE_LENGTH || v.kind == GVEC_SVG_STYLE_NUMBER ? v.number : 12;
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_WEIGHT, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->weight = weight_of(&v);
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_STYLE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->italic = gvec_lw_keyword_is(&v, "italic") ? 1 : (v.kind == GVEC_SVG_STYLE_KEYWORD && v.keyword && strncmp(v.keyword, "oblique", 7) == 0) ? 2 : 0;
  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_STRETCH, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->width = stretch_of(&v);
  r = read_style(ctx, inst, GVEC_SVG_PROP_TEXT_ANCHOR, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->anchor = gvec_lw_keyword_is(&v, "middle") ? 0.5 : gvec_lw_keyword_is(&v, "end") ? 1.0 : 0.0;
  r = read_style(ctx, inst, GVEC_SVG_PROP_VISIBILITY, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->visible = !(gvec_lw_keyword_is(&v, "hidden") || gvec_lw_keyword_is(&v, "collapse"));
  r = read_style(ctx, inst, GVEC_SVG_PROP_LETTER_SPACING, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  bool good = spacing_of(inst, &v, st->size, false, &st->letter_spacing);
  if (good) {
    r = read_style(ctx, inst, GVEC_SVG_PROP_WORD_SPACING, &v, &ok);
    if (r != GVEC_OK || !ok) {
      t->refused = true;
      return r;
    }
    good = spacing_of(inst, &v, st->size, false, &st->word_spacing);
  }
  if (!good) {
    t->refused = true;
    return gvec_lw_refuse(ctx, gvec_svg_instance_element(inst), GVEC_LOWER_TEXT, "a letter-spacing or word-spacing in a unit this does not read");
  }
  char value[32];
  st->small_caps = chain_value(inst, "font-variant", true, true, value, sizeof value) && strcmp(value, "small-caps") == 0;
  /* `font-kerning` counts only in a style (a presentation attribute of it is ignored, as the references do), and `kerning` of 0 is the same. */
  st->kerning_off = (chain_value(inst, "font-kerning", false, true, value, sizeof value) && strcmp(value, "none") == 0) ||
      (chain_value(inst, "kerning", true, true, value, sizeof value) && strcmp(value, "0") == 0);
  r = read_style(ctx, inst, GVEC_SVG_PROP_DOMINANT_BASELINE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->dominant = baseline_kind(&v) < 0 ? 0 : baseline_kind(&v);
  {
    /* The SVG 1.1 values (`text-before-edge`, `text-after-edge`) are not values of the property the cascade knows, which drops them: they are
     * read as written, the nearest element that gives one deciding (`dominant-baseline` is inherited, `alignment-baseline` is not). */
    char word[32];
    GVEC_Svg_Style_Value raw;
    memset(&raw, 0, sizeof raw);
    raw.kind = GVEC_SVG_STYLE_KEYWORD;
    raw.keyword = word;
    if (chain_value(inst, "dominant-baseline", true, true, word, sizeof word) && baseline_kind(&raw) >= 0) {
      st->dominant = baseline_kind(&raw);
    }
  }
  r = read_style(ctx, inst, GVEC_SVG_PROP_ALIGNMENT_BASELINE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  st->alignment = baseline_kind(&v);
  {
    char word[32];
    GVEC_Svg_Style_Value raw;
    memset(&raw, 0, sizeof raw);
    raw.kind = GVEC_SVG_STYLE_KEYWORD;
    raw.keyword = word;
    if (chain_value(inst, "alignment-baseline", true, false, word, sizeof word) && baseline_kind(&raw) >= 0) {
      st->alignment = baseline_kind(&raw);
    }
  }
  /* `baseline-shift`: not inherited, but a span's shift is on top of its parent's. */
  st->shift_length = parent ? parent->shift_length : 0;
  st->shift_sub = parent ? parent->shift_sub : 0;
  st->shift_super = parent ? parent->shift_super : 0;
  r = read_style(ctx, inst, GVEC_SVG_PROP_BASELINE_SHIFT, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  if (!parent) {
    /* The shift of the text element itself does nothing: it is a shift of a span from its parent's baseline. */
  }
  else if (gvec_lw_keyword_is(&v, "sub")) {
    st->shift_sub++;
  }
  else if (gvec_lw_keyword_is(&v, "super")) {
    st->shift_super++;
  }
  else if (!gvec_lw_keyword_is(&v, "baseline")) {
    double shift;
    if (spacing_of(inst, &v, st->size, true, &shift)) {
      st->shift_length += shift;
    }
  }
  *index = (uint32_t) t->style_count++;
  return GVEC_OK;
}

/* The `xml:space` that applies to an element: its own, else its parent's. */
static bool preserves(const GVEC_Svg_Element * e, bool inherited) {
  const GVEC_Svg_Attribute * space = gvec_svg_element_find_attribute(e, "xml:space");
  for (size_t i = 0; !space && i < e->attribute_count; i++) {
    const GVEC_Svg_Attribute * a = &e->attributes[i];
    if (a->ns && strcmp(a->name, "space") == 0) {
      space = a;
    }
  }
  if (!space) {
    return inherited;
  }
  size_t n;
  const char * text = gvec_svg_attribute_raw(space, &n);
  if (text && n == 8 && strncmp(text, "preserve", 8) == 0) {
    return true;
  }
  return text && n == 7 && strncmp(text, "default", 7) == 0 ? false : inherited;
}

static void add_characters(tctx * t, const char * text, size_t n, bool preserve, uint32_t style) {
  for (size_t i = 0; i < n;) {
    unsigned char c = (unsigned char) text[i];
    uint32_t cp;
    size_t len;
    if (c < 0x80) {
      cp = c, len = 1;
    }
    else if (c >= 0xC2 && c < 0xE0 && i + 1 < n && ((unsigned char) text[i + 1] & 0xC0) == 0x80) {
      cp = ((uint32_t) (c & 0x1F) << 6) | ((unsigned char) text[i + 1] & 0x3F), len = 2;
    }
    else if (c >= 0xE0 && c < 0xF0 && i + 2 < n && ((unsigned char) text[i + 1] & 0xC0) == 0x80 && ((unsigned char) text[i + 2] & 0xC0) == 0x80) {
      cp = ((uint32_t) (c & 0x0F) << 12) | ((uint32_t) ((unsigned char) text[i + 1] & 0x3F) << 6) | ((unsigned char) text[i + 2] & 0x3F), len = 3;
    }
    else if (c >= 0xF0 && c < 0xF5 && i + 3 < n && ((unsigned char) text[i + 1] & 0xC0) == 0x80 && ((unsigned char) text[i + 2] & 0xC0) == 0x80 &&
        ((unsigned char) text[i + 3] & 0xC0) == 0x80) {
      cp = ((uint32_t) (c & 0x07) << 18) | ((uint32_t) ((unsigned char) text[i + 1] & 0x3F) << 12) | ((uint32_t) ((unsigned char) text[i + 2] & 0x3F) << 6) |
          ((unsigned char) text[i + 3] & 0x3F), len = 4;
    }
    else {
      cp = 0xFFFD, len = 1;
    }
    i += len;
    if (cp == '\r') {
      continue;
    }
    if (cp == '\n' && !preserve) {
      continue; /* A newline is removed unless the white space is preserved (SVG 1.1, 10.15). */
    }
    if (cp == '\n' || cp == '\t' || cp == ' ') {
      cp = ' ';
      if (!preserve && (t->last_space || t->count == 0)) {
        continue; /* The leading white space of the text, and each run after a space (a preserved one too), are dropped. */
      }
      t->last_space = true;
    }
    else {
      t->last_space = false;
    }
    t->chars[t->count].cp = cp;
    t->chars[t->count].style = style;
    t->chars[t->count].has_x = t->chars[t->count].has_y = t->chars[t->count].has_dy = t->chars[t->count].has_rotate = false;
    t->chars[t->count].x = t->chars[t->count].y = t->chars[t->count].dx = t->chars[t->count].dy = t->chars[t->count].rotate = 0;
    t->count++;
  }
}

static GVEC_Result record_list(tctx * t, const GVEC_Svg_Instance * inst, const char * name, int kind, size_t start) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, name);
  const GVEC_Svg_Length * items;
  size_t count;
  if (!a || gvec_svg_attribute_class(a) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_lengths(a, &items, &count) || count == 0) {
    return GVEC_OK;
  }
  if (t->list_count == t->list_capacity) {
    return gvec_lw_refuse(t->lw, e, GVEC_LOWER_LIMIT, "the text has more lists of positions than the lowering keeps");
  }
  tlist * l = &t->lists[t->list_count++];
  l->start = start;
  l->end = SIZE_MAX;
  l->items = items;
  l->numbers = NULL;
  l->count = count;
  l->kind = kind;
  l->inst = inst;
  return GVEC_OK;
}

static GVEC_Result record_rotate(tctx * t, const GVEC_Svg_Instance * inst, size_t start) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, "rotate");
  const double * numbers;
  size_t count;
  if (!a || gvec_svg_attribute_class(a) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_numbers(a, &numbers, &count) || count == 0) {
    return GVEC_OK;
  }
  if (t->list_count == t->list_capacity) {
    return gvec_lw_refuse(t->lw, e, GVEC_LOWER_LIMIT, "the text has more lists of positions than the lowering keeps");
  }
  tlist * l = &t->lists[t->list_count++];
  l->start = start;
  l->end = SIZE_MAX;
  l->items = NULL;
  l->numbers = numbers;
  l->count = count;
  l->kind = 4;
  l->inst = inst;
  return GVEC_OK;
}

static bool length_of(const GVEC_Svg_Instance * inst, const GVEC_Svg_Length * length, bool horizontal, double * out) {
  if (length->unit == GVEC_SVG_UNIT_PERCENT) {
    double vw, vh;
    gvec_svg_instance_viewport(inst, &vw, &vh);
    *out = length->value * (horizontal ? vw : vh) / 100.0;
    return true;
  }
  return gvec_lw_absolute_length(length, out);
}

/* The kinds of line a `text-decoration` of an element asks for: its computed value is a list of `underline`, `overline` and `line-through`,
 * or `none`. */
static GVEC_Result decoration_flags(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, unsigned * flags) {
  GVEC_Svg_Style_Value v;
  bool ok;
  *flags = 0;
  GVEC_Result r = read_style(ctx, inst, GVEC_SVG_PROP_TEXT_DECORATION, &v, &ok);
  if (r != GVEC_OK || !ok || v.kind != GVEC_SVG_STYLE_KEYWORD || !v.keyword) {
    return r;
  }
  const char * k = v.keyword;
  while (*k) {
    while (*k == ' ') {
      k++;
    }
    const char * word = k;
    while (*k && *k != ' ') {
      k++;
    }
    size_t n = (size_t) (k - word);
    if (n == 9 && strncmp(word, "underline", 9) == 0) {
      *flags |= 1;
    }
    else if (n == 8 && strncmp(word, "overline", 8) == 0) {
      *flags |= 2;
    }
    else if (n == 12 && strncmp(word, "line-through", 12) == 0) {
      *flags |= 4;
    }
  }
  return GVEC_OK;
}

static GVEC_Result walk(tctx * t, const GVEC_Svg_Instance * inst, bool inherited_preserve, size_t depth, const tstyle * parent) {
  gvec_lw_ctx * ctx = t->lw;
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  if (depth > TEXT_DEPTH_MAX) {
    t->refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text's spans nest deeper than the lowering allows");
  }
  bool clear;
  GVEC_Result r = unsupported(ctx, inst, &clear);
  if (r != GVEC_OK || !clear) {
    t->refused = true;
    return r;
  }
  uint32_t style = 0;
  r = span_style(t, inst, parent, &style);
  if (r != GVEC_OK || t->refused) {
    return r;
  }
  bool preserve = preserves(e, inherited_preserve);
  size_t start = t->count;
  size_t first_list = t->list_count;
  static const char * const names[4] = {"x", "y", "dx", "dy"};
  for (int k = 0; r == GVEC_OK && k < 4; k++) {
    r = record_list(t, inst, names[k], k, start);
  }
  if (r == GVEC_OK) {
    r = record_rotate(t, inst, start);
  }
  if (r != GVEC_OK) {
    t->refused = true;
    return r;
  }
  for (const GVEC_Svg_Node * node = gvec_svg_element_first_node(e); node; node = gvec_svg_node_next(node)) {
    size_t n;
    const char * text = gvec_svg_node_text(node, &n);
    if (text) {
      add_characters(t, text, n, preserve, style);
      continue;
    }
    const GVEC_Svg_Element * child = gvec_svg_node_element(node);
    if (!child) {
      continue;
    }
    GVEC_Svg_Kind kind = gvec_svg_element_kind(child);
    if (kind != GVEC_SVG_KIND_TSPAN && kind != GVEC_SVG_KIND_A) {
      if (kind == GVEC_SVG_KIND_TEXT_PATH || strcmp(child->name, "tref") == 0) {
        t->refused = true;
        return gvec_lw_refuse(ctx, child, GVEC_LOWER_TEXT, "text on a path and tref are not lowered yet");
      }
      continue; /* title, desc, metadata and what SVG does not put in text carry no characters. */
    }
    if (gvec_svg_element_conditional_false(child)) {
      continue;
    }
    const GVEC_Svg_Instance * ci = NULL;
    r = gvec_svg_instance_child(ctx->style, inst, child, &ci);
    if (r == GVEC_ERR_LIMIT) {
      t->refused = true;
      return gvec_lw_refuse(ctx, child, GVEC_LOWER_LIMIT, "more elements are drawn than the limits allow");
    }
    if (r != GVEC_OK) {
      return r;
    }
    GVEC_Svg_Style_Value dv;
    bool ok;
    r = read_style(ctx, ci, GVEC_SVG_PROP_DISPLAY, &dv, &ok);
    if (r != GVEC_OK || !ok) {
      t->refused = true;
      return r;
    }
    if (gvec_lw_keyword_is(&dv, "none")) {
      continue; /* A span that is not displayed has no characters and takes no room. */
    }
    r = walk(t, ci, preserve, depth + 1, &t->styles[style]);
    if (r != GVEC_OK || t->refused) {
      return r;
    }
  }
  for (size_t i = first_list; i < t->list_count; i++) {
    if (t->lists[i].inst == inst) {
      t->lists[i].end = t->count; /* The element's content is done: its lists apply to these characters and no others. */
    }
  }
  {
    unsigned flags;
    r = decoration_flags(ctx, inst, &flags);
    if (r != GVEC_OK) {
      t->refused = true;
      return r;
    }
    if (flags && t->count > start && t->decor_count < t->decor_capacity) {
      tdecor * d = &t->decors[t->decor_count++];
      d->inst = inst;
      d->style = style;
      d->start = start;
      d->end = t->count;
      d->flags = flags;
    }
  }
  const GVEC_Svg_Attribute * tl = gvec_svg_element_find_attribute(e, "textLength");
  GVEC_Svg_Length length;
  if (tl && t->count > start && gvec_svg_attribute_class(tl) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_length(tl, &length)) {
    double value;
    if (!length_of(inst, &length, true, &value)) {
      t->refused = true;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_STYLE, "a textLength in a unit that needs the font or the window is not lowered");
    }
    if (value > 0 && t->length_count < t->length_capacity) {
      tlength * entry = &t->lengths[t->length_count++];
      entry->start = start;
      entry->end = t->count;
      entry->length = value;
      size_t n;
      const GVEC_Svg_Attribute * adjust = gvec_svg_element_find_attribute(e, "lengthAdjust");
      const char * a = adjust ? gvec_svg_attribute_raw(adjust, &n) : NULL;
      entry->glyphs = a && n == 16 && strncmp(a, "spacingAndGlyphs", 16) == 0;
      entry->inst = inst;
    }
  }
  return GVEC_OK;
}

static GVEC_Result apply_lists(tctx * t) {
  for (size_t i = 0; i < t->list_count; i++) {
    const tlist * l = &t->lists[i];
    if (l->kind == 4) {
      /* Every character of the element's content has an angle: the list's values in order, then its last one. */
      for (size_t j = l->start; j < l->end && j < t->count; j++) {
        tchar * c = &t->chars[j];
        c->has_rotate = true;
        c->rotate = l->numbers[j - l->start < l->count ? j - l->start : l->count - 1];
      }
      continue;
    }
    for (size_t j = 0; j < l->count && l->start + j < l->end && l->start + j < t->count; j++) {
      double v;
      if (!length_of(l->inst, &l->items[j], l->kind == 0 || l->kind == 2, &v)) {
        return gvec_lw_refuse(t->lw, gvec_svg_instance_element(l->inst), GVEC_LOWER_STYLE, "a text position that needs the font or the window is not lowered");
      }
      tchar * c = &t->chars[l->start + j];
      switch (l->kind) {
      case 0: c->has_x = true, c->x = v; break;
      case 1: c->has_y = true, c->y = v; break;
      case 2: c->dx = v; break;
      default: c->has_dy = true, c->dy = v; break;
      }
    }
  }
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// Layout: chunks, runs of one span, glyphs
//-----------------------------------------------------------------------------

/* Whether a character is of a cursive script, whose letters are joined: letter-spacing does not apply to it (CSS Text 3, 7.2). */
static bool cursive(uint32_t cp) {
  return (cp >= 0x0600 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF);
}

/* Whether a character is a combining mark, which belongs to the grapheme cluster of the character before it. */
static bool combining(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x064B && cp <= 0x065F) ||
      cp == 0x0670 || (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x0900 && cp <= 0x0903) || (cp >= 0x093A && cp <= 0x094F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
      (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

typedef struct tglyph {
  const GFNT_Face * face;
  uint32_t id;
  double x, y;
  double cell_x0, cell_x1, cell_y0, cell_y1; ///< The glyph's cell: its advance wide, the face's ascent above its baseline and descent below.
  uint32_t style;
  size_t ch;        ///< The character the glyph is of (its cluster's first).
  double base;      ///< The baseline the glyph sits on.
  double rotate;    ///< Degrees about the origin of the glyph at the start of the glyph's cluster.
  size_t pivot;     ///< That glyph's index in the text's glyphs.
  uint32_t group;   ///< One more than the textLength entry that stretches the glyphs, or 0.
  double size;      ///< The em size the glyph is drawn at (a small capital is smaller).
} tglyph;

/* Shape the characters [from, to) of one span: the faces its characters need (the first name of the list that maps each), shaped with fallback;
 * the glyphs are appended to `glyphs` in visual order, positioned from `pen` and with the dx and dy of their characters, which move the pen. */
static GVEC_Result shape_run(tctx * t, const GVEC_Fonts * fonts, size_t from, size_t to, double * pen_x, double * pen_y, tglyph * glyphs, size_t * glyph_count) {
  gvec_lw_ctx * ctx = t->lw;
  const tstyle * st = &t->styles[t->chars[from].style];
  const GVEC_Svg_Element * e = gvec_svg_instance_element(st->inst);
  size_t count = to - from;
  if (!(st->size > 0) || st->size - st->size != 0) {
    return GVEC_OK; /* A size of nothing draws nothing and takes no room. */
  }
  const double size = st->size;
  uint32_t * cps = gvec_arena_array(&ctx->arena, count, sizeof *cps, sizeof *cps);
  if (!cps) {
    return GVEC_ERR_OOM;
  }
  for (size_t i = 0; i < count; i++) {
    cps[i] = t->chars[from + i].cp;
  }
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
      GVEC_Result r = gvec_fonts_face(fonts, st->families, st->family_count, st->weight, st->italic, st->width, cps[i], &face, &found);
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
  GFNT_ShapeFeature features[4];
  size_t feature_count = 0;
  if (st->kerning_off) {
    features[feature_count++] = (GFNT_ShapeFeature) {GFNT_TAG('k', 'e', 'r', 'n'), 0, 0, GFNT_SHAPE_END};
  }
  if (st->small_caps) {
    /* Small capitals are the face's own (`smcp`), as the references draw them. */
    features[feature_count++] = (GFNT_ShapeFeature) {GFNT_TAG('s', 'm', 'c', 'p'), 1, 0, GFNT_SHAPE_END};
  }
  bool spaced = false;
  for (size_t i = from; i < to; i++) {
    spaced = spaced || (t->styles[t->chars[i].style].letter_spacing != 0 && !cursive(t->chars[i].cp));
  }
  if (spaced) {
    /* Letters that are spaced apart are not joined into ligatures (CSS Text 3, 7.2). */
    features[feature_count++] = (GFNT_ShapeFeature) {GFNT_TAG('l', 'i', 'g', 'a'), 0, 0, GFNT_SHAPE_END};
    features[feature_count++] = (GFNT_ShapeFeature) {GFNT_TAG('c', 'l', 'i', 'g'), 0, 0, GFNT_SHAPE_END};
  }
  shape.features = feature_count ? features : NULL;
  shape.feature_count = feature_count;
  GFNT_FaceRuns runs;
  memset(&runs, 0, sizeof runs);
  GFNT_Result sr = gfnt_faces_shape_bidi(faces, face_count, cps, count, GFNT_BIDI_LTR, &shape, NULL, &runs, NULL);
  if (sr != GFNT_OK) {
    return sr == GFNT_ERR_OOM ? GVEC_ERR_OOM : gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "the text could not be shaped with the fonts given");
  }
  GVEC_Result result = GVEC_OK;
  size_t last_base_ch = SIZE_MAX, last_base_index = 0;
  for (size_t i = 0; result == GVEC_OK && i < runs.count; i++) {
    const GFNT_FaceRun * fr = &runs.runs[i];
    uint16_t upem = 0;
    if (gfnt_face_units_per_em(faces[fr->face], &upem, NULL) != GFNT_OK || upem == 0) {
      result = gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a face of the fonts has no size to scale by");
      break;
    }
    double s = size / (double) upem;
    GFNT_LineMetrics line;
    if (gfnt_face_line_metrics(faces[fr->face], GFNT_LINE_METRICS_FONT, NULL, &line, NULL) != GFNT_OK) {
      line.ascent = upem;
      line.descent = 0;
    }
    /* The baseline the run sits on: the span's dominant baseline is put at the text position (or the baseline it is aligned by, when that is
     * not its parent's), then the baseline-shifts lift it. The positions of the baselines come from the face (CSS Inline 3, the fallbacks of
     * 5.1): the ascent and descent, half the ascent for the mathematical, four fifths of it for the hanging, half the x-height for the middle. */
    const GFNT_Os2 * os2 = NULL;
    if (gfnt_face_os2(faces[fr->face], &os2, NULL) != GFNT_OK) {
      os2 = NULL;
    }
    double ascent = (double) line.ascent * s, descent = (double) line.descent * s;
    double x_height = os2 && os2->x_height > 0 ? (double) os2->x_height * s : 0.5 * size;
    int baseline = st->alignment >= 0 ? st->alignment : st->dominant;
    double down = 0;
    switch (baseline) {
    case 1: down = (ascent + descent) / 2; break;
    case 2: down = x_height / 2; break;
    case 3: down = 0.8 * ascent; break;
    case 4: down = ascent / 2; break;
    case 5: down = descent; break;
    case 6: down = ascent; break;
    case 7: down = descent; break;
    default: break;
    }
    double lift = st->shift_length;
    lift += (double) st->shift_super * (os2 && os2->superscript_y_offset != 0 ? (double) os2->superscript_y_offset * s : size / 3.0);
    lift -= (double) st->shift_sub * (os2 && os2->subscript_y_offset != 0 ? (double) os2->subscript_y_offset * s : size / 5.0);
    down -= lift;
    for (size_t g = 0; g < fr->run.count; g++) {
      const GFNT_ShapedGlyph * sg = &fr->run.glyphs[g];
      size_t ch = from + sg->cluster;
      if (ch >= to) {
        ch = to - 1;
      }
      *pen_x += t->chars[ch].dx;
      *pen_y += t->chars[ch].dy;
      t->chars[ch].dx = t->chars[ch].dy = 0; /* A character shaped into several glyphs moves the pen once. */
      tglyph * out = &glyphs[(*glyph_count)++];
      out->face = faces[fr->face];
      out->id = sg->glyph;
      out->x = *pen_x + (double) sg->x_offset * s;
      out->y = *pen_y + down - (double) sg->y_offset * s;
      out->style = t->chars[ch].style; /* The paint of a glyph is that of the span its character is in. */
      out->cell_x0 = *pen_x;
      out->cell_x1 = *pen_x + (double) sg->x_advance * s;
      out->cell_y0 = *pen_y + down - (double) line.ascent * s;
      out->cell_y1 = *pen_y + down - (double) line.descent * s;
      out->ch = ch;
      out->base = *pen_y + down;
      size_t head = ch;
      while (head > from && combining(t->chars[head].cp)) {
        head--; /* A mark turns with the character it is on: the angle is that of the start of the cluster. */
      }
      out->rotate = t->chars[head].has_rotate ? t->chars[head].rotate : 0;
      size_t own_index = (size_t) (out - glyphs);
      if (head == ch) {
        last_base_ch = ch;
        last_base_index = own_index;
        out->pivot = own_index;
      }
      else {
        out->pivot = last_base_ch == head ? last_base_index : own_index;
      }
      out->group = 0;
      out->size = size;
      *pen_x += (double) sg->x_advance * s;
      /* The spacing after a character (a cluster of glyphs takes it once, after its last): letter-spacing, and word-spacing after a space. */
      bool last_of_cluster = g + 1 >= fr->run.count || fr->run.glyphs[g + 1].cluster != sg->cluster;
      if (last_of_cluster) {
        const tstyle * cs = &t->styles[t->chars[ch].style];
        if (!cursive(t->chars[ch].cp)) {
          *pen_x += cs->letter_spacing;
        }
        if (t->chars[ch].cp == ' ' || t->chars[ch].cp == 0xA0) {
          *pen_x += cs->word_spacing;
        }
        out->cell_x1 = *pen_x;
      }
    }
  }
  gfnt_face_runs_free(&runs);
  return result;
}

GVEC_Result gvec_lw_text_layout(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_text * out, bool * usable) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  memset(out, 0, sizeof *out);
  *usable = false;
  const GVEC_Fonts * fonts = ctx->options ? ctx->options->fonts : NULL;
  if (!fonts) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "no fonts were given, so text cannot be drawn");
  }
  /* The room the content can take: its bytes bound its characters, its elements bound its spans and lists. */
  size_t bytes = 0, elements = 0;
  {
    const GVEC_Svg_Element * stack[TEXT_DEPTH_MAX + 2];
    size_t depth = 0;
    stack[depth++] = e;
    while (depth) {
      const GVEC_Svg_Element * cur = stack[--depth];
      elements++;
      for (const GVEC_Svg_Node * node = gvec_svg_element_first_node(cur); node; node = gvec_svg_node_next(node)) {
        size_t n;
        if (gvec_svg_node_text(node, &n)) {
          bytes += n;
        }
        else if (gvec_svg_node_element(node) && depth < TEXT_DEPTH_MAX) {
          stack[depth++] = gvec_svg_node_element(node);
        }
      }
    }
  }
  if (bytes > TEXT_CHARACTERS_MAX) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text has more characters than the lowering shapes");
  }
  tctx t;
  memset(&t, 0, sizeof t);
  t.lw = ctx;
  t.style_capacity = elements + 1 + TEXT_DEPTH_MAX;
  t.list_capacity = 5 * (elements + 1);
  t.capacity = bytes + 1;
  t.styles = gvec_arena_array(&ctx->arena, t.style_capacity, sizeof *t.styles, sizeof(void *));
  t.lists = gvec_arena_array(&ctx->arena, t.list_capacity, sizeof *t.lists, sizeof(void *));
  t.length_capacity = elements + 1;
  t.lengths = gvec_arena_array(&ctx->arena, t.length_capacity, sizeof *t.lengths, sizeof(void *));
  t.chars = gvec_arena_array(&ctx->arena, t.capacity, sizeof *t.chars, sizeof(double));
  if (!t.styles || !t.lists || !t.chars || !t.lengths) {
    return GVEC_ERR_OOM;
  }
  t.decor_capacity = elements + TEXT_DEPTH_MAX + 2;
  t.decors = gvec_arena_array(&ctx->arena, t.decor_capacity, sizeof *t.decors, sizeof(void *));
  if (!t.decors) {
    return GVEC_ERR_OOM;
  }
  GVEC_Result r = walk(&t, inst, false, 0, NULL);
  if (r != GVEC_OK || t.refused) {
    return r;
  }
  /* The decorations of the elements the text is inside (a `g` that says `underline`) cover all of it, outermost first. */
  {
    const GVEC_Svg_Instance * chain[TEXT_DEPTH_MAX];
    size_t depth = 0;
    for (const GVEC_Svg_Instance * up = gvec_svg_instance_parent(inst); up && depth < TEXT_DEPTH_MAX; up = gvec_svg_instance_parent(up)) {
      chain[depth++] = up;
    }
    size_t outer = t.decor_count;
    for (size_t k = depth; r == GVEC_OK && k-- > 0;) {
      unsigned flags;
      r = decoration_flags(ctx, chain[k], &flags);
      if (r != GVEC_OK || !flags || t.decor_count >= t.decor_capacity || false) {
        continue;
      }
      /* A decoration of an element outside the text is drawn with the text's own paint and font (the references do so). */
      tdecor * d = &t.decors[t.decor_count++];
      d->inst = inst;
      d->style = 0;
      d->start = 0;
      d->end = t.count;
      d->flags = flags;
    }
    if (r != GVEC_OK) {
      return r;
    }
    /* The ancestors come before the text's own, whose lines are on top of theirs. */
    (void) outer;
  }
  if (t.count > 0 && t.chars[t.count - 1].cp == ' ' && !preserves(e, false)) {
    t.count--; /* The trailing white space of the text is dropped. */
  }
  r = apply_lists(&t);
  if (r != GVEC_OK) {
    return r;
  }
  if (t.count == 0) {
    *usable = true;
    return GVEC_OK;
  }
  tglyph * glyphs = gvec_arena_array(&ctx->arena, t.count * 4 + 4, sizeof *glyphs, sizeof(double));
  size_t glyph_capacity = t.count * 4 + 4;
  if (!glyphs) {
    return GVEC_ERR_OOM;
  }
  typedef struct drect {
    double x0, x1, center, thickness;
    const GVEC_Svg_Instance * inst;
    bool over_text; ///< A line-through: drawn after the text, where an underline and an overline are before it.
  } drect;
  drect * rects = NULL;
  size_t rect_count = 0, rect_capacity = 0;
  size_t glyph_count = 0;
  double pen_x = 0, pen_y = 0;
  size_t chunk_first_glyph = 0;
  for (size_t i = 0; r == GVEC_OK && i < t.count;) {
    /* A text chunk begins at an absolute x or y (and at the start of the text). */
    size_t j = i + 1;
    while (j < t.count && !t.chars[j].has_x && !t.chars[j].has_y) {
      j++;
    }
    if (t.chars[i].has_x) {
      pen_x = t.chars[i].x;
    }
    if (t.chars[i].has_y) {
      pen_y = t.chars[i].y;
    }
    double start_x = pen_x;
    chunk_first_glyph = glyph_count;
    for (size_t k = i; r == GVEC_OK && k < j;) {
      size_t m = k + 1;
      while (m < j && (t.chars[m].style == t.chars[k].style || same_font(&t.styles[t.chars[m].style], &t.styles[t.chars[k].style]))) {
        m++;
      }
      if (glyph_count + (m - k) * 4 + 4 > glyph_capacity) {
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text has more glyphs than the lowering keeps");
      }
      r = shape_run(&t, fonts, k, m, &pen_x, &pen_y, glyphs, &glyph_count);
      k = m;
    }
    if (r != GVEC_OK) {
      return r;
    }
    /* The textLength of an element in this chunk: its glyphs take the length asked for (the spacing between them, or the glyphs too). */
    for (size_t k = 0; r == GVEC_OK && k < t.length_count; k++) {
      tlength * tl = &t.lengths[k];
      if (tl->end <= i || tl->start >= j) {
        continue;
      }
      if (tl->start < i || tl->end > j) {
        return gvec_lw_refuse(ctx, gvec_svg_instance_element(tl->inst), GVEC_LOWER_TEXT, "a textLength over several text chunks is not lowered");
      }
      size_t ga = SIZE_MAX, gb = 0;
      for (size_t g = chunk_first_glyph; g < glyph_count; g++) {
        if (glyphs[g].ch >= tl->start && glyphs[g].ch < tl->end) {
          if (ga == SIZE_MAX) {
            ga = g;
          }
          gb = g + 1;
        }
      }
      if (ga == SIZE_MAX) {
        continue;
      }
      double x0 = glyphs[ga].cell_x0;
      double width = glyphs[gb - 1].cell_x1 - x0;
      if (!(width > 0)) {
        continue;
      }
      double grow = tl->length - width;
      if (tl->glyphs) {
        double factor = tl->length / width;
        GSHGEOM_Map move, stretch, back, partial;
        if (gshgeom_map_translate(2, x0, 0, 0, &move) != GSHGEOM_OK || gshgeom_map_scale(2, factor, 1, 1, &stretch) != GSHGEOM_OK ||
            gshgeom_map_translate(2, -x0, 0, 0, &back) != GSHGEOM_OK || !gvec_lw_compose(&move, &stretch, &partial) || !gvec_lw_compose(&partial, &back, &tl->map)) {
          return gvec_lw_refuse(ctx, gvec_svg_instance_element(tl->inst), GVEC_LOWER_LIMIT, "a textLength stretch is past the range the waist can carry");
        }
        for (size_t g = ga; g < gb; g++) {
          glyphs[g].group = (uint32_t) (k + 1);
          glyphs[g].cell_x0 = x0 + (glyphs[g].cell_x0 - x0) * factor;
          glyphs[g].cell_x1 = x0 + (glyphs[g].cell_x1 - x0) * factor;
        }
      }
      else if (gb - ga > 1) {
        double extra = grow / (double) (gb - ga - 1);
        for (size_t g = ga; g < gb; g++) {
          double shift = extra * (double) (g - ga);
          glyphs[g].x += shift;
          glyphs[g].cell_x0 += shift;
          glyphs[g].cell_x1 += shift;
        }
      }
      for (size_t g = gb; g < glyph_count; g++) {
        glyphs[g].x += grow;
        glyphs[g].cell_x0 += grow;
        glyphs[g].cell_x1 += grow;
      }
      pen_x += grow;
    }
    /* The anchor of the chunk's first character moves the whole chunk by its whole advance. */
    double shift = -t.styles[t.chars[i].style].anchor * (pen_x - start_x);
    for (size_t g = chunk_first_glyph; shift != 0 && g < glyph_count; g++) {
      glyphs[g].x += shift;
      glyphs[g].cell_x0 += shift;
      glyphs[g].cell_x1 += shift;
    }
    /* The lines of the decorations that cover characters of this chunk, across the glyphs of those characters. */
    for (size_t d = 0; d < t.decor_count; d++) {
      const tdecor * dec = &t.decors[d];
      size_t ca = dec->start > i ? dec->start : i, cb = dec->end < j ? dec->end : j;
      if (ca >= cb) {
        continue;
      }
      size_t ga = SIZE_MAX, gb = 0;
      for (size_t g = chunk_first_glyph; g < glyph_count; g++) {
        if (glyphs[g].ch >= ca && glyphs[g].ch < cb) {
          if (ga == SIZE_MAX) {
            ga = g;
          }
          gb = g + 1;
        }
      }
      if (ga == SIZE_MAX) {
        continue;
      }
      for (size_t g = ga; g < gb; g++) {
        if (glyphs[g].rotate != 0) {
          return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a text decoration over rotated glyphs is not lowered yet");
        }
      }
      for (size_t c = ca + 1; c < cb; c++) {
        if (t.chars[c].has_dy || t.chars[c].has_y) {
          return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a text decoration over glyphs that change baseline is not lowered yet");
        }
      }
      /* The face and the size of the element that declares the decoration give the lines' place and thickness. */
      const tstyle * ds = &t.styles[dec->style];
      const GFNT_Face * face = NULL;
      bool found = false;
      r = gvec_fonts_face(fonts, ds->families, ds->family_count, ds->weight, ds->italic, ds->width, t.chars[dec->start].cp, &face, &found);
      if (r != GVEC_OK) {
        return r;
      }
      uint16_t upem = 0;
      if (!found || gfnt_face_units_per_em(face, &upem, NULL) != GFNT_OK || upem == 0 || !(ds->size > 0)) {
        continue;
      }
      double sc = ds->size / (double) upem;
      GFNT_LineMetrics line;
      const GFNT_Post * post = NULL;
      const GFNT_Os2 * os2 = NULL;
      if (gfnt_face_line_metrics(face, GFNT_LINE_METRICS_FONT, NULL, &line, NULL) != GFNT_OK) {
        line.ascent = upem;
        line.descent = 0;
      }
      if (gfnt_face_post(face, &post, NULL) != GFNT_OK) {
        post = NULL;
      }
      if (gfnt_face_os2(face, &os2, NULL) != GFNT_OK) {
        os2 = NULL;
      }
      double under_position = post && post->underline_thickness ? (double) post->underline_position * sc : -0.1 * ds->size;
      double under_thickness = post && post->underline_thickness ? (double) post->underline_thickness * sc : 0.05 * ds->size;
      double strike_position = os2 && os2->strikeout_size ? (double) os2->strikeout_position * sc : 0.3 * ds->size;
      double strike_thickness = os2 && os2->strikeout_size ? (double) os2->strikeout_size * sc : under_thickness;
      /* The references draw a line for each span: it is cut where the span of the characters changes. */
      for (size_t sa = ga; sa < gb;) {
      size_t sb = sa + 1;
      while (sb < gb && glyphs[sb].style == glyphs[sa].style) {
        sb++;
      }
      for (unsigned kind = 1; kind <= 4; kind <<= 1) {
        if (!(dec->flags & kind)) {
          continue;
        }
        if (rect_count == rect_capacity) {
          size_t capacity = rect_capacity ? rect_capacity * 2 : 8;
          drect * grown = gvec_arena_array(&ctx->arena, capacity, sizeof *grown, sizeof(double));
          if (!grown) {
            return GVEC_ERR_OOM;
          }
          if (rect_count) {
            memcpy(grown, rects, rect_count * sizeof *grown);
          }
          rects = grown;
          rect_capacity = capacity;
        }
        drect * dr = &rects[rect_count++];
        dr->x0 = glyphs[sa].cell_x0;
        dr->x1 = glyphs[sb - 1].cell_x1;
        double base = glyphs[sa].base;
        dr->center = kind == 1 ? base - under_position : kind == 2 ? base - (double) line.ascent * sc : base - strike_position;
        dr->thickness = kind == 4 ? strike_thickness : under_thickness;
        dr->inst = dec->inst;
        dr->over_text = kind == 4;
      }
      sa = sb;
      }
    }
    i = j;
  }
  /* The box the text's paint is placed in: the union of its glyphs' cells (the text's height is the font's, not the outlines'). */
  out->box.empty = 1;
  for (size_t g = 0; g < glyph_count; g++) {
    if (out->box.empty) {
      out->box.min.x = glyphs[g].cell_x0, out->box.max.x = glyphs[g].cell_x1;
      out->box.min.y = glyphs[g].cell_y0, out->box.max.y = glyphs[g].cell_y1;
      out->box.empty = 0;
      continue;
    }
    if (glyphs[g].cell_x0 < out->box.min.x) out->box.min.x = glyphs[g].cell_x0;
    if (glyphs[g].cell_x1 > out->box.max.x) out->box.max.x = glyphs[g].cell_x1;
    if (glyphs[g].cell_y0 < out->box.min.y) out->box.min.y = glyphs[g].cell_y0;
    if (glyphs[g].cell_y1 > out->box.max.y) out->box.max.y = glyphs[g].cell_y1;
  }
  /* One run for each stretch of glyphs of one span and one face (a rotated glyph is a run of its own), between the lines that go under the
   * text and the lines that go over it. */
  out->pieces = gvec_arena_array(&ctx->arena, glyph_count + rect_count + 1, sizeof *out->pieces, sizeof(void *));
  if (!out->pieces) {
    return GVEC_ERR_OOM;
  }
  for (int pass = 0; r == GVEC_OK && pass < 3; pass++) {
    if (pass != 1) {
      for (size_t k = 0; r == GVEC_OK && k < rect_count; k++) {
        if (rects[k].over_text != (pass == 2) || !(rects[k].thickness > 0)) {
          continue;
        }
        gvec_lw_geometry geometry;
        const GSHSCENE_Resource * rect = NULL;
        r = gvec_lw_rect_path(ctx, rects[k].x0, rects[k].center - rects[k].thickness / 2, rects[k].x1 - rects[k].x0, rects[k].thickness, &geometry, &rect);
        if (r != GVEC_OK) {
          break;
        }
        if (geometry != GVEC_LW_GEOMETRY_PATH) {
          continue;
        }
        out->pieces[out->count].run = rect;
        out->pieces[out->count].inst = rects[k].inst;
        out->pieces[out->count].decoration = true;
        gshgeom_map_identity(2, &out->pieces[out->count].map);
        out->count++;
      }
      continue;
    }
    for (size_t g = 0; r == GVEC_OK && g < glyph_count;) {
      size_t h = g + 1;
      while (h < glyph_count && glyphs[h].style == glyphs[g].style && glyphs[h].face == glyphs[g].face && glyphs[h].group == glyphs[g].group &&
          glyphs[h].size == glyphs[g].size && glyphs[h].rotate == glyphs[g].rotate && (glyphs[g].rotate == 0 || glyphs[h].pivot == glyphs[g].pivot)) {
        h++;
      }
      const tstyle * st = &t.styles[glyphs[g].style];
      if (!st->visible) {
        g = h;
        continue;
      }
      GSHSCENE_Glyph * list = gvec_arena_array(&ctx->arena, h - g, sizeof *list, sizeof(double));
      if (!list) {
        return GVEC_ERR_OOM;
      }
      for (size_t k = g; k < h; k++) {
        list[k - g].id = glyphs[k].id;
        list[k - g].x = glyphs[k].x;
        list[k - g].y = glyphs[k].y;
        list[k - g].outline = NULL;
      }
      const GSHSCENE_Resource * face_resource = NULL;
      r = gvec_fonts_scene_face(fonts, glyphs[g].face, ctx->store, &face_resource);
      if (r != GVEC_OK) {
        break;
      }
      GSHSCENE_Glyph_Run run;
      memset(&run, 0, sizeof run);
      run.source = GSHSCENE_GLYPHS_FROM_FONT;
      run.face = face_resource;
      run.size = glyphs[g].size;
      run.hint_mode = GSHSCENE_HINT_NONE;
      run.glyph_count = h - g;
      run.glyphs = list;
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
      gvec_lw_text_piece * piece = &out->pieces[out->count];
      piece->run = resource;
      piece->inst = st->inst;
      piece->decoration = false;
      if (glyphs[g].group) {
        piece->map = t.lengths[glyphs[g].group - 1].map;
      }
      else {
        gshgeom_map_identity(2, &piece->map);
      }
      if (glyphs[g].rotate != 0) {
        /* A rotated glyph is turned about its own origin. */
        GSHGEOM_Map move, back, turn, partial, whole;
        if (glyphs[g].group || gshgeom_map_translate(2, glyphs[glyphs[g].pivot].x, glyphs[glyphs[g].pivot].y, 0, &move) != GSHGEOM_OK ||
            gshgeom_map_translate(2, -glyphs[glyphs[g].pivot].x, -glyphs[glyphs[g].pivot].y, 0, &back) != GSHGEOM_OK ||
            gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, glyphs[g].rotate, &turn) != GSHGEOM_OK || !gvec_lw_compose(&move, &turn, &partial) ||
            !gvec_lw_compose(&partial, &back, &whole)) {
          r = gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a rotation of a glyph that the length of its text also stretches is not lowered");
          break;
        }
        piece->map = whole;
      }
      out->count++;
      g = h;
    }
  }
  if (r != GVEC_OK) {
    return r;
  }
  *usable = true;
  return GVEC_OK;
}
