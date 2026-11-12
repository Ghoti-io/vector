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
      {GVEC_SVG_PROP_FONT_SIZE_ADJUST, {"none", NULL, NULL, NULL}, "font-size-adjust"},
      {GVEC_SVG_PROP_OPACITY, {"1", NULL, NULL, NULL}, "opacity on a span"},
  };
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  bool span = gvec_svg_element_kind(e) != GVEC_SVG_KIND_TEXT;
  *clear = false;
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (checks[i].prop == GVEC_SVG_PROP_OPACITY && !span) {
      continue; /* The text element's own opacity is the group around everything. */
    }
    GVEC_Svg_Style_Value v;
    bool ok;
    GVEC_Result r = read_style(ctx, inst, checks[i].prop, &v, &ok);
    if (r != GVEC_OK || !ok) {
      return r;
    }
    bool initial = ((v.kind == GVEC_SVG_STYLE_LENGTH || v.kind == GVEC_SVG_STYLE_NUMBER) && v.number == (checks[i].prop == GVEC_SVG_PROP_OPACITY ? 1 : 0)) ||
        v.kind == GVEC_SVG_STYLE_NONE;
    for (size_t k = 0; !initial && k < 4 && checks[i].initial[k]; k++) {
      initial = gvec_lw_keyword_is(&v, checks[i].initial[k]);
    }
    if (!initial && v.kind == GVEC_SVG_STYLE_KEYWORD && v.keyword) {
      /* A length that the cascade left as it was written (`0px`) is the initial value when it is zero. */
      double zero;
      initial = leading_number(v.keyword, &zero) && zero == (checks[i].prop == GVEC_SVG_PROP_OPACITY ? 1 : 0);
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
    if (!a->ns && (strcmp(a->name, "rotate") == 0 || strcmp(a->name, "textLength") == 0 || strcmp(a->name, "lengthAdjust") == 0)) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "rotation and length adjustment of text are not lowered yet");
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
  int italic;
  bool visible;
} tstyle;

/* Whether two spans ask for one font: their characters are shaped together, so kerning and joining run across the boundary. */
static bool same_font(const tstyle * a, const tstyle * b) {
  if (a->size != b->size || a->weight != b->weight || a->width != b->width || a->italic != b->italic || a->family_count != b->family_count) {
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
  bool has_x, has_y;
  double x, y, dx, dy;
} tchar;

/** A list of a span, applied to the characters of its content once the content is known (the tree's order, outer first, so that an inner list wins). */
typedef struct tlist {
  size_t start, end;   ///< The characters of the element's content (end is set when the content is known).
  const GVEC_Svg_Length * items;
  size_t count;
  int kind; ///< 0 x, 1 y, 2 dx, 3 dy.
  const GVEC_Svg_Instance * inst;
} tlist;

typedef struct tctx {
  gvec_lw_ctx * lw;
  tstyle * styles;
  size_t style_count, style_capacity;
  tchar * chars;
  size_t count, capacity;
  tlist * lists;
  size_t list_count, list_capacity;
  bool last_space;
  bool refused;
} tctx;

#define TEXT_DEPTH_MAX 64

static GVEC_Result span_style(tctx * t, const GVEC_Svg_Instance * inst, uint32_t * index) {
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
    t->chars[t->count].has_x = t->chars[t->count].has_y = false;
    t->chars[t->count].x = t->chars[t->count].y = t->chars[t->count].dx = t->chars[t->count].dy = 0;
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
  l->count = count;
  l->kind = kind;
  l->inst = inst;
  return GVEC_OK;
}

static GVEC_Result walk(tctx * t, const GVEC_Svg_Instance * inst, bool inherited_preserve, size_t depth) {
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
  r = span_style(t, inst, &style);
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
    r = walk(t, ci, preserve, depth + 1);
    if (r != GVEC_OK || t->refused) {
      return r;
    }
  }
  for (size_t i = first_list; i < t->list_count; i++) {
    if (t->lists[i].inst == inst) {
      t->lists[i].end = t->count; /* The element's content is done: its lists apply to these characters and no others. */
    }
  }
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

static GVEC_Result apply_lists(tctx * t) {
  for (size_t i = 0; i < t->list_count; i++) {
    const tlist * l = &t->lists[i];
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
      default: c->dy = v; break;
      }
    }
  }
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// Layout: chunks, runs of one span, glyphs
//-----------------------------------------------------------------------------

typedef struct tglyph {
  const GFNT_Face * face;
  uint32_t id;
  double x, y;
  uint32_t style;
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
  GFNT_FaceRuns runs;
  memset(&runs, 0, sizeof runs);
  GFNT_Result sr = gfnt_faces_shape_bidi(faces, face_count, cps, count, GFNT_BIDI_LTR, &shape, NULL, &runs, NULL);
  if (sr != GFNT_OK) {
    return sr == GFNT_ERR_OOM ? GVEC_ERR_OOM : gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "the text could not be shaped with the fonts given");
  }
  GVEC_Result result = GVEC_OK;
  for (size_t i = 0; result == GVEC_OK && i < runs.count; i++) {
    const GFNT_FaceRun * fr = &runs.runs[i];
    uint16_t upem = 0;
    if (gfnt_face_units_per_em(faces[fr->face], &upem, NULL) != GFNT_OK || upem == 0) {
      result = gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, "a face of the fonts has no size to scale by");
      break;
    }
    double s = st->size / (double) upem;
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
      out->y = *pen_y - (double) sg->y_offset * s;
      out->style = t->chars[ch].style; /* The paint of a glyph is that of the span its character is in. */
      *pen_x += (double) sg->x_advance * s;
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
  t.style_capacity = elements + 1;
  t.list_capacity = 4 * (elements + 1);
  t.capacity = bytes + 1;
  t.styles = gvec_arena_array(&ctx->arena, t.style_capacity, sizeof *t.styles, sizeof(void *));
  t.lists = gvec_arena_array(&ctx->arena, t.list_capacity, sizeof *t.lists, sizeof(void *));
  t.chars = gvec_arena_array(&ctx->arena, t.capacity, sizeof *t.chars, sizeof(double));
  if (!t.styles || !t.lists || !t.chars) {
    return GVEC_ERR_OOM;
  }
  GVEC_Result r = walk(&t, inst, false, 0);
  if (r != GVEC_OK || t.refused) {
    return r;
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
    /* The anchor of the chunk's first character moves the whole chunk by its whole advance. */
    double shift = -t.styles[t.chars[i].style].anchor * (pen_x - start_x);
    for (size_t g = chunk_first_glyph; shift != 0 && g < glyph_count; g++) {
      glyphs[g].x += shift;
    }
    i = j;
  }
  /* One run for each stretch of glyphs of one span and one face. */
  out->pieces = gvec_arena_array(&ctx->arena, glyph_count ? glyph_count : 1, sizeof *out->pieces, sizeof(void *));
  if (!out->pieces) {
    return GVEC_ERR_OOM;
  }
  for (size_t g = 0; r == GVEC_OK && g < glyph_count;) {
    size_t h = g + 1;
    while (h < glyph_count && glyphs[h].style == glyphs[g].style && glyphs[h].face == glyphs[g].face) {
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
    run.size = st->size;
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
    out->pieces[out->count].run = resource;
    out->pieces[out->count].inst = st->inst;
    out->count++;
    g = h;
  }
  if (r != GVEC_OK) {
    return r;
  }
  *usable = true;
  return GVEC_OK;
}
