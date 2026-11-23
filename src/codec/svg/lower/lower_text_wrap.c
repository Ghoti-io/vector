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
 * @file lower_text_wrap.c
 *
 * Wrapped text (SVG 2, 11.4.1 and 11.7): a `text` with an `inline-size` is laid out in a rectangle that size wide (tall, in vertical text) and as tall as it
 * needs. The content is measured as one line, the lines are broken by the rules of UAX 14 as CSS Text 3 tailors them (`line-break`, `word-break`,
 * `overflow-wrap`, `hyphens`, `text-wrap-mode`, `text-wrap-style`), each line box is as tall as the `line-height` of what is on it, and each line then
 * begins a text chunk of its own at the place the area puts it. The layout that follows is the one of any text: it shapes each chunk, and `text-anchor`
 * moves each line within the rectangle.
 *
 * What is dropped from the lines, as CSS Text 3 drops it: the spaces at a line's end (they hang), and a line feed that the white space keeps as a break.
 * What the spec says of wrapped text is applied: the `x` and `y` of the first character put the area, and the `dx`, `dy`, `rotate`, `textLength` and the other
 * absolute positions are ignored.
 */

#include <ghoti.io/vector/macros.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/char.h>

#include "../svg_grammar.h"
#include "lower_internal.h"
#include "lower_text_types.h"

/** What the properties of the text element say about breaking its lines. */
typedef struct wrap_props {
  double size;             ///< `inline-size`.
  bool nowrap;             ///< `text-wrap-mode: nowrap`: only a line feed that is kept ends a line.
  bool balance;            ///< `text-wrap-style: balance`.
  bool emergency;          ///< `overflow-wrap: break-word` or `anywhere` (or `word-break: break-word`): a word too long for a line is broken.
  int word_break;          ///< 0 normal, 1 `break-all`, 2 `keep-all`.
  bool hyphens_none;       ///< `hyphens: none`: a soft hyphen is no break.
  GUNI_LineBreakTailoring tailoring;
} wrap_props;

/** The text measured as one line, and where it may be broken. */
typedef struct wrap_text {
  tctx * t;
  size_t n;
  const wrap_props * props;
  uint32_t * cps;
  double * prefix;   ///< prefix[i]: the advance of the characters before i (their cells, the spacing after each included).
  double * trail;    ///< The spacing after the character, which the end of a line does not keep.
  bool * start;      ///< A glyph's cluster begins at the character: a line can end before it.
  bool * allowed;    ///< A line may end before the character (the rules of line breaking, tailored).
  bool * grapheme;   ///< A grapheme cluster begins at the character (where a word too long is broken).
  double * hyphen;   ///< For a soft hyphen: the advance of the hyphen it becomes where a line ends at it (0 for the others).
  uint32_t * hyphen_cp; ///< ...and that hyphen: U+2010 when the span's face has it, else U+002D.
} wrap_text;

/** A line of the text: the characters from `from` to `to` are on it, and `next` is where the following line begins (the spaces and the line feed at its end are not on it). */
typedef struct wrap_line {
  size_t from, to, next;
  double left, width; ///< The stretch of the line it is in.
  double baseline;
} wrap_line;


/* A length of the text element, read as its `inline-size` is: a percentage is of the viewport's width, or of its height in vertical text. */
static bool inline_size_of(tctx * t, const GVEC_Svg_Instance * inst, const GVEC_Svg_Style_Value * v, double * out) {
  *out = 0;
  if (v->kind == GVEC_SVG_STYLE_LENGTH || v->kind == GVEC_SVG_STYLE_NUMBER) {
    *out = v->number;
    return true;
  }
  if (v->kind != GVEC_SVG_STYLE_KEYWORD || !v->keyword) {
    return false;
  }
  const size_t length = strlen(v->keyword);
  if (length > 0 && v->keyword[length - 1] == '%') {
    size_t at = 0;
    double number;
    if (!gvec_svg_scan_number(v->keyword, length, &at, &number, NULL) || at + 1 != length) {
      return false;
    }
    double vw, vh;
    gvec_svg_instance_viewport(inst, &vw, &vh);
    *out = number * (t->vertical ? vh : vw) / 100.0;
    return true;
  }
  return gvec_lw_text_spacing_of(inst, v, 0, false, out);
}

static GVEC_Result keyword_of(tctx * t, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, GVEC_Svg_Style_Value * v, bool * ok) {
  GVEC_Result r = gvec_lw_read_property(t->lw, inst, property, GVEC_LOWER_STYLE, v, ok);
  if (r != GVEC_OK || !*ok) {
    t->refused = true;
  }
  return r;
}

/* The `inline-size` of the text element. `*on` is false when it has none (or one of nothing). */
static GVEC_Result read_size(tctx * t, const GVEC_Svg_Instance * inst, wrap_props * p, bool * on) {
  GVEC_Svg_Style_Value v;
  bool ok;
  *on = false;
  memset(p, 0, sizeof *p);
  GVEC_Result r = keyword_of(t, inst, GVEC_SVG_PROP_INLINE_SIZE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  if (gvec_lw_keyword_is(&v, "auto")) {
    return GVEC_OK;
  }
  double size;
  if (!inline_size_of(t, inst, &v, &size) || !(size > 0) || size - size != 0) {
    return GVEC_OK; /* zero turns the area off (SVG 2, 11.4.1), and a value that is not a length is not one */
  }
  p->size = size;
  *on = true;
  return GVEC_OK;
}

/* The other properties of the text element that bear on its lines. */
static GVEC_Result read_flow(tctx * t, const GVEC_Svg_Instance * inst, wrap_props * p) {
  GVEC_Svg_Style_Value v;
  bool ok;
  GVEC_Result r = keyword_of(t, inst, GVEC_SVG_PROP_TEXT_WRAP_MODE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  p->nowrap = gvec_lw_keyword_is(&v, "nowrap");
  r = keyword_of(t, inst, GVEC_SVG_PROP_TEXT_WRAP_STYLE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  p->balance = gvec_lw_keyword_is(&v, "balance");
  r = keyword_of(t, inst, GVEC_SVG_PROP_OVERFLOW_WRAP, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  p->emergency = gvec_lw_keyword_is(&v, "break-word") || gvec_lw_keyword_is(&v, "anywhere");
  r = keyword_of(t, inst, GVEC_SVG_PROP_WORD_BREAK, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  if (gvec_lw_keyword_is(&v, "break-all")) {
    p->word_break = 1;
  }
  else if (gvec_lw_keyword_is(&v, "keep-all")) {
    p->word_break = 2;
  }
  else if (gvec_lw_keyword_is(&v, "break-word")) {
    p->emergency = true;
  }
  r = keyword_of(t, inst, GVEC_SVG_PROP_LINE_BREAK, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  p->tailoring = gvec_lw_keyword_is(&v, "loose") ? GUNI_LINE_BREAK_LOOSE
      : gvec_lw_keyword_is(&v, "anywhere")       ? GUNI_LINE_BREAK_ANYWHERE
      : gvec_lw_keyword_is(&v, "strict")         ? GUNI_LINE_BREAK_STRICT
                                                 : GUNI_LINE_BREAK_NORMAL;
  r = keyword_of(t, inst, GVEC_SVG_PROP_HYPHENS, &v, &ok);
  if (r != GVEC_OK || !ok) {
    return r;
  }
  p->hyphens_none = gvec_lw_keyword_is(&v, "none");
  return GVEC_OK;
}

static bool is_letter_or_number(uint32_t cp) {
  return (guni_general_category_mask(cp) & (GUNI_GC_MASK_L | GUNI_GC_MASK_N | GUNI_GC_MASK_M)) != 0;
}

/* The letters that `word-break: keep-all` keeps from breaking among themselves: the typographic letter units of the classes AL, AI, NU and ID (CSS Text 3, 5.2), and
 * the conditional starters and the Hangul that go with them. */
static bool cjk_letter(uint32_t cp) {
  switch (guni_line_break(cp)) {
  case GUNI_LB_AL:
  case GUNI_LB_AI:
  case GUNI_LB_NU:
  case GUNI_LB_ID:
  case GUNI_LB_CJ:
  case GUNI_LB_H2:
  case GUNI_LB_H3:
  case GUNI_LB_JL:
  case GUNI_LB_JV:
  case GUNI_LB_JT:
    return true;
  default:
    return false;
  }
}

/* The boundaries of `kind` in the characters, as a flag for each: a boundary before the character. */
static GVEC_Result boundaries(tctx * t, const wrap_text * w, GUNI_BreakKind kind, GUNI_LineBreakTailoring tailoring, bool * flags) {
  size_t * at = gvec_arena_array(&t->lw->arena, w->n + 1, sizeof *at, sizeof *at);
  if (!at) {
    return GVEC_ERR_OOM;
  }
  GUNI_BreakOptions options;
  memset(&options, 0, sizeof options);
  options.kind = kind;
  options.tailoring = tailoring;
  size_t count = 0;
  if (guni_break_all_codepoints(&options, w->cps, w->n, at, w->n + 1, &count) != GUNI_OK) {
    return GVEC_ERR_LIMIT;
  }
  for (size_t i = 0; i < count; i++) {
    if (at[i] > 0 && at[i] < w->n) {
      flags[at[i]] = true;
    }
  }
  return GVEC_OK;
}

/** The advance of a run of characters, spacing included but for the spacing after the last: the width of a line made of them, whose spaces at its end are left off. */
static double span_width(const wrap_text * w, size_t from, size_t to, bool hyphen) {
  while (to > from && w->cps[to - 1] == ' ') {
    to--;
  }
  if (to <= from) {
    return 0;
  }
  /* A line that ends at a soft hyphen shows a hyphen, which is as wide as the hyphen is. */
  const double shown = hyphen && w->cps[to - 1] == 0xAD ? w->hyphen[to - 1] : 0;
  return w->prefix[to] - w->prefix[from] - w->trail[to - 1] + shown;
}

/* Where the line that begins at `from` ends in a width of `width`: `*to` is the end of what is on it (its spaces left off), `*next` where the next line begins. A first piece
 * wider than the width makes a line that overflows when `overflow` (a rectangle has no better place to put it); else the line is not made, and false is returned, so
 * that the text is put where it fits. */
static bool fill_line(const wrap_text * w, size_t from, double width, bool overflow, size_t * to, size_t * next) {
  const wrap_props * p = w->props;
  const double slack = 1e-9 * (width > 1 ? width : 1);
  size_t best = 0;
  bool have = false;
  for (size_t b = from + 1; b <= w->n; b++) {
    const bool hard = w->t->chars[b - 1].newline;
    if (!(b == w->n || hard || (!p->nowrap && w->allowed[b]))) {
      continue;
    }
    if (span_width(w, from, b, b < w->n && !hard) <= width + slack) {
      best = b;
      have = true;
      if (hard) {
        break;
      }
      continue;
    }
    if (have) {
      break;
    }
    if (b < w->n && !hard && w->cps[b - 1] == 0xAD) {
      continue; /* the hyphen does not fit: the word is broken elsewhere, or not at all */
    }
    /* The first piece is wider than the line. A line that may not be broken inside a word is as long as the piece (it overflows); one that may is broken
     * where the most of it fits, at least by a cluster. */
    best = b;
    bool cut_fits = false;
    if (p->emergency && !p->nowrap) {
      size_t cut = 0;
      for (size_t k = from + 1; k < b; k++) {
        if (!w->grapheme[k] || !w->start[k]) {
          continue;
        }
        if (cut == 0 || span_width(w, from, k, false) <= width + slack) {
          cut = k;
        }
        if (span_width(w, from, k, false) > width + slack) {
          break;
        }
      }
      if (cut) {
        best = cut;
        cut_fits = span_width(w, from, cut, false) <= width + slack;
      }
    }
    if (!overflow && !cut_fits) {
      return false;
    }
    break;
  }
  if (!have && best == 0) {
    best = w->n;
  }
  *next = best;
  size_t end = best;
  while (end > from && w->cps[end - 1] == ' ') {
    end--;
  }
  *to = end;
  return true;
}

/* How many lines the text takes in `width`. */
static size_t count_lines(const wrap_text * w, double width) {
  size_t lines = 0;
  for (size_t at = 0; at < w->n;) {
    size_t to, next;
    (void) fill_line(w, at, width, true, &to, &next);
    at = next > at ? next : at + 1;
    lines++;
  }
  return lines;
}

/* The ascent and the descent that a span's line box has about its baseline: the face's own, with the half-leading `line-height` makes of the rest. */
static GVEC_Result style_box(tctx * t, uint32_t style, double * above, double * below) {
  const tstyle * st = &t->styles[style];
  double ascent = 0.8 * st->size, descent = 0.2 * st->size, gap = 0;
  const GFNT_Face * face = NULL;
  bool found = false;
  size_t rank = 0;
  GVEC_Result r = gvec_lw_font_face(t->lw, st->families, st->family_count, st->weight, st->italic, st->width, ' ', st->language, &face, &found, &rank);
  if (r != GVEC_OK) {
    return r;
  }
  uint16_t upem = 0;
  GFNT_LineMetrics line;
  if (found && face && gfnt_face_units_per_em(face, &upem, NULL) == GFNT_OK && upem != 0 &&
      gfnt_face_line_metrics(face, GFNT_LINE_METRICS_FONT, NULL, &line, NULL) == GFNT_OK) {
    const double s = st->size / (double) upem;
    ascent = (double) line.ascent * s;
    descent = -(double) line.descent * s;
    gap = (double) line.line_gap * s;
  }
  if (st->line_normal) {
    *above = ascent + gap / 2;
    *below = descent + gap / 2;
  }
  else {
    const double leading = st->line_height - (ascent + descent);
    *above = ascent + leading / 2;
    *below = descent + leading / 2;
  }
  return GVEC_OK;
}

/** What a band is placed with. */
typedef struct wrap_band {
  const wrap_text * w;
  tctx * t;
  tarea * area;
  const wrap_props * props;
  bool rtl;
  bool overflow;       ///< A piece wider than its stretch overflows it (a rectangle); else it is put where it fits.
  double left;         ///< The rectangle's left edge, when there is no area.
  double width;        ///< The width the text is filled to in a rectangle (`inline-size`, or narrower when it is balanced).
  double tallest;      ///< The tallest line box the text has.
  wrap_line * lines;
} wrap_band;

#define SPANS_PER_BAND 64u ///< How many stretches of a band the text is filled into.

/* Fill the stretches of the band that begins at `top` with the text from `at`, into `lines` from `first`: `*placed` is how many lines it made, and `*cursor` where
 * the text goes on. Nothing is placed when no stretch can take the next piece. */
static GVEC_Result place_band(const wrap_band * b, double top, size_t at, size_t first, size_t * placed, size_t * cursor) {
  tspan spans[SPANS_PER_BAND];
  size_t span_count = 1;
  *placed = 0;
  *cursor = at;
  if (b->area) {
    GVEC_Result r = gvec_lw_text_area_band(b->t, b->area, top, top + b->tallest, spans, SPANS_PER_BAND, &span_count);
    if (r != GVEC_OK) {
      return r;
    }
  }
  else {
    spans[0].from = b->left;
    spans[0].to = b->left + b->props->size;
  }
  for (size_t k = 0; k < span_count && *cursor < b->w->n; k++) {
    const tspan sp = spans[b->rtl ? span_count - 1 - k : k];
    wrap_line * line = &b->lines[first + *placed];
    /* a balanced text is filled to its narrower width, and aligned in the whole of the stretch */
    if (!fill_line(b->w, *cursor, b->area ? sp.to - sp.from : b->width, b->overflow, &line->to, &line->next)) {
      continue; /* nothing of what is next fits in this stretch */
    }
    line->from = *cursor;
    line->left = sp.from;
    line->width = sp.to - sp.from;
    (*placed)++;
    const bool forced = b->t->chars[line->next - 1].newline;
    *cursor = line->next > *cursor ? line->next : *cursor + 1;
    if (forced) {
      break; /* a line that ends the line: the stretches after it are left empty */
    }
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_text_wrap(tctx * t, const GVEC_Svg_Instance * inst, tglyph * glyphs, size_t glyph_capacity, bool * wrapped) {
  gvec_lw_ctx * ctx = t->lw;
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *wrapped = false;
  wrap_props props;
  bool size_on;
  GVEC_Result r = read_size(t, inst, &props, &size_on);
  if (r != GVEC_OK || t->refused || t->tpath_count > 0 || t->count == 0) {
    return r; /* text on a path is not wrapped: a path is its own line */
  }
  /* The area is the shape the text flows into if it has one (it is what `shape-inside` is for, whatever the `inline-size`), else the rectangle. */
  tarea * area = NULL;
  r = gvec_lw_text_area_make(t, inst, &area);
  if (r != GVEC_OK || t->refused || (!size_on && !area)) {
    return r;
  }
  r = read_flow(t, inst, &props);
  if (r != GVEC_OK || t->refused) {
    return r;
  }
  const size_t n = t->count;
  wrap_text w;
  memset(&w, 0, sizeof w);
  w.t = t;
  w.n = n;
  w.props = &props;
  w.cps = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.cps, sizeof *w.cps);
  w.prefix = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.prefix, sizeof(double));
  w.trail = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.trail, sizeof(double));
  w.start = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.start, 1);
  w.allowed = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.allowed, 1);
  w.grapheme = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.grapheme, 1);
  w.hyphen = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.hyphen, sizeof(double));
  w.hyphen_cp = gvec_arena_array(&ctx->arena, n + 1, sizeof *w.hyphen_cp, sizeof *w.hyphen_cp);
  double * advance = gvec_arena_array(&ctx->arena, n + 1, sizeof *advance, sizeof(double));
  if (!w.cps || !w.prefix || !w.trail || !w.start || !w.allowed || !w.grapheme || !w.hyphen || !w.hyphen_cp || !advance) {
    return GVEC_ERR_OOM;
  }
  /* The place the area is put, which the first character's own position gives; the other positions are ignored (SVG 2, 11.2.2). */
  const double x0 = t->chars[0].has_x ? t->chars[0].x : 0;
  const double y0 = t->chars[0].has_y ? t->chars[0].y : 0;
  for (size_t i = 0; i < n; i++) {
    tchar * c = &t->chars[i];
    c->has_x = c->has_y = c->has_dy = c->has_rotate = c->chunk_start = false;
    c->x = c->y = c->dx = c->dy = c->rotate = 0;
    w.cps[i] = c->cp;
  }
  t->length_count = 0; /* textLength is not applied to wrapped text (SVG 2, 11.2.1) */
  /* The text as one line: how far each character is from the one before. */
  size_t glyph_count = 0;
  double pen_x = 0, pen_y = 0;
  for (size_t k = 0; r == GVEC_OK && k < n;) {
    size_t m = k + 1;
    while (m < n && (t->chars[m].style == t->chars[k].style || same_font(&t->styles[t->chars[m].style], &t->styles[t->chars[k].style]))) {
      m++;
    }
    if (glyph_count + (m - k) * 4 + 4 > glyph_capacity) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the text has more glyphs than the lowering keeps");
    }
    r = gvec_lw_text_shape_run(t, ctx->options ? ctx->options->fonts : NULL, k, m, &pen_x, &pen_y, glyphs, &glyph_count);
    k = m;
  }
  if (r != GVEC_OK) {
    return r;
  }
  for (size_t g = 0; g < glyph_count; g++) {
    const size_t c = glyphs[g].ch;
    if (c >= n) {
      continue;
    }
    advance[c] += glyphs[g].cell_x1 - glyphs[g].cell_x0;
    w.trail[c] = glyphs[g].trail > w.trail[c] ? glyphs[g].trail : w.trail[c];
    w.start[c] = true;
  }
  for (size_t i = 0; i < n; i++) {
    w.prefix[i + 1] = w.prefix[i] + advance[i];
  }
  /* A line that ends at a soft hyphen shows a hyphen (U+2010 when the face has it, else U+002D): how wide it is, in the span's own font. */
  for (size_t c = 0; !props.hyphens_none && c < n; c++) {
    if (w.cps[c] != 0xAD) {
      continue;
    }
    const tstyle * st = &t->styles[t->chars[c].style];
    const GFNT_Face * face = NULL;
    bool found = false;
    size_t rank = 0;
    r = gvec_lw_font_face(ctx, st->families, st->family_count, st->weight, st->italic, st->width, 0x2010, st->language, &face, &found, &rank);
    if (r != GVEC_OK) {
      return r;
    }
    w.hyphen_cp[c] = found ? 0x2010u : 0x2Du;
    size_t one = 0;
    double hx = 0, hy = 0;
    t->chars[c].cp = w.hyphen_cp[c];
    r = gvec_lw_text_shape_run(t, ctx->options ? ctx->options->fonts : NULL, c, c + 1, &hx, &hy, glyphs, &one);
    t->chars[c].cp = 0xAD;
    if (r != GVEC_OK) {
      return r;
    }
    for (size_t g = 0; g < one; g++) {
      w.hyphen[c] += glyphs[g].advance;
    }
  }
  /* The places a line may end: UAX 14 as `line-break` tailors it, and `word-break` and `hyphens` on top of it. */
  if (!props.nowrap) {
    r = boundaries(t, &w, GUNI_BREAK_LINE, props.tailoring, w.allowed);
    if (r != GVEC_OK) {
      return r;
    }
  }
  r = boundaries(t, &w, GUNI_BREAK_GRAPHEME, GUNI_LINE_BREAK_STRICT, w.grapheme);
  if (r != GVEC_OK) {
    return r;
  }
  for (size_t c = 1; c < n; c++) {
    if (props.word_break == 1 && w.grapheme[c] && is_letter_or_number(w.cps[c - 1]) && is_letter_or_number(w.cps[c])) {
      w.allowed[c] = true;
    }
    if (props.word_break == 2 && cjk_letter(w.cps[c - 1]) && cjk_letter(w.cps[c])) {
      w.allowed[c] = false;
    }
    if (props.hyphens_none && w.cps[c - 1] == 0xAD) {
      w.allowed[c] = false;
    }
    if (!w.start[c]) {
      w.allowed[c] = false; /* inside a glyph (a ligature, a cluster): not a place a line can end */
    }
  }
  /* `text-wrap-style: balance`: the narrowest width that takes the lines the full width does. */
  double width = props.size;
  if (props.balance && !props.nowrap && !area) {
    const size_t lines = count_lines(&w, width);
    double lo = 0, hi = width;
    for (int step = 0; step < 24; step++) {
      const double mid = (lo + hi) / 2;
      if (count_lines(&w, mid) <= lines) {
        hi = mid;
      }
      else {
        lo = mid;
      }
    }
    width = hi;
  }
  /* The lines: a band of the area at a time, each span of the band filled in turn (a rectangle has one span, as wide as the `inline-size`, in every band). */
  wrap_line * lines = gvec_arena_array(&ctx->arena, n + 1, sizeof *lines, sizeof(double));
  double * above = gvec_arena_array(&ctx->arena, t->style_count + 1, sizeof *above, sizeof(double));
  double * below = gvec_arena_array(&ctx->arena, t->style_count + 1, sizeof *below, sizeof(double));
  bool * boxed = gvec_arena_array(&ctx->arena, t->style_count + 1, sizeof *boxed, 1);
  if (!lines || !above || !below || !boxed) {
    return GVEC_ERR_OOM;
  }
  /* Each span's line box: as tall as the tallest of what is on it and of the strut of the text element itself (CSS Inline 3, 2.1); the box of a span is
   * its face's own ascent and descent, with the half-leading that `line-height` makes of the rest. */
  for (size_t c = 0; c <= n; c++) {
    const uint32_t style = c == n ? 0 : t->chars[c].style;
    if (!boxed[style]) {
      r = style_box(t, style, &above[style], &below[style]);
      if (r != GVEC_OK) {
        return r;
      }
      if (t->vertical) {
        const double half = (above[style] + below[style]) / 2;
        above[style] = below[style] = half; /* the central baseline is in the middle of the line */
      }
      boxed[style] = true;
    }
  }
  if (!boxed[0]) {
    r = style_box(t, 0, &above[0], &below[0]);
    if (r != GVEC_OK) {
      return r;
    }
    boxed[0] = true;
  }
  double tallest = above[0] + below[0];
  for (size_t s = 1; s < t->style_count; s++) {
    if (boxed[s] && above[s] + below[s] > tallest) {
      tallest = above[s] + below[s];
    }
  }
  /* The frame the lines are laid out in has its second axis up for the layout and down for the page's columns: `ysign` turns the direction the lines go in to
   * +1, whichever it is (they go left in vertical text, right in `vertical-lr`). */
  const double ysign = t->vertical_lr ? -1.0 : 1.0;
  const bool rtl = t->styles[0].rtl;
  const double left = rtl ? x0 - props.size : x0;
  double band_top = 0, area_bottom = 0;
  if (area) {
    gvec_lw_text_area_extent(area, &band_top, &area_bottom);
  }
  const double step = tallest > 0 ? tallest / 4 : 1;
  size_t line_count = 0, at = 0, attempts = 0;
  double baseline = 0;
  bool first_band = true;
  while (at < n) {
    wrap_band band = {&w, t, area, &props, rtl, !area, left, width, tallest, lines};
    if (area && (band_top > area_bottom || ++attempts > (n + 1) * 16 + 4096)) {
      break; /* the text that does not fit in the shape is not drawn */
    }
    size_t placed = 0, cursor = at;
    r = place_band(&band, band_top, at, line_count, &placed, &cursor);
    if (r != GVEC_OK) {
      return r;
    }
    if (placed == 0) {
      /* nothing fits here: the box goes lower, by steps, and then back by halves to the highest place it fits at */
      double lo = band_top, hi = band_top + step;
      bool found = false;
      while (hi <= area_bottom + step && ++attempts <= (n + 1) * 16 + 4096) {
        r = place_band(&band, hi, at, line_count, &placed, &cursor);
        if (r != GVEC_OK) {
          return r;
        }
        if (placed) {
          found = true;
          break;
        }
        lo = hi;
        hi += step;
      }
      if (!found || hi > area_bottom + step) {
        break;
      }
      for (int refine = 0; refine < 14; refine++) {
        const double mid = (lo + hi) / 2;
        size_t mid_placed = 0, mid_cursor = at;
        r = place_band(&band, mid, at, line_count, &mid_placed, &mid_cursor);
        if (r != GVEC_OK) {
          return r;
        }
        if (mid_placed) {
          hi = mid;
        }
        else {
          lo = mid;
        }
      }
      r = place_band(&band, hi, at, line_count, &placed, &cursor);
      if (r != GVEC_OK) {
        return r;
      }
      band_top = hi;
    }
    const size_t band_first = line_count;
    line_count += placed;
    at = cursor;
    double band_above = 0, band_below = 0;
    for (size_t i = band_first; i < line_count; i++) {
      uint32_t previous = UINT32_MAX;
      double la = above[0], lb = below[0];
      for (size_t c = lines[i].from; c < lines[i].to; c++) {
        const uint32_t style = t->chars[c].style;
        if (style != previous) {
          previous = style;
          la = above[style] > la ? above[style] : la;
          lb = below[style] > lb ? below[style] : lb;
        }
      }
      band_above = i == band_first || la > band_above ? la : band_above;
      band_below = i == band_first || lb > band_below ? lb : band_below;
    }
    if (!area && first_band) {
      baseline = ysign * y0; /* the first baseline is where the text was put */
    }
    else {
      baseline = band_top + band_above;
    }
    band_top = baseline + band_below;
    first_band = false;
    for (size_t i = band_first; i < line_count; i++) {
      lines[i].baseline = baseline;
    }
  }
  /* Each line begins a chunk at its place, the anchor moving the chunk back by its own width: a line that starts at the anchor's fraction of the width
   * and is moved by that fraction of its own is aligned in its stretch as the anchor says. The characters that are not on a line are dropped. */
  bool * keep = gvec_arena_array(&ctx->arena, n + 1, sizeof *keep, 1);
  size_t * moved = gvec_arena_array(&ctx->arena, n + 1, sizeof *moved, sizeof *moved);
  if (!keep || !moved) {
    return GVEC_ERR_OOM;
  }
  for (size_t i = 0; i < line_count; i++) {
    const wrap_line * line = &lines[i];
    for (size_t c = line->from; c < line->to; c++) {
      keep[c] = true;
    }
    if (line->to > line->from) {
      tchar * first = &t->chars[line->from];
      double anchor = t->styles[first->style].anchor;
      if (t->styles[first->style].rtl && anchor != 0.5) {
        anchor = 1.0 - anchor;
      }
      first->has_x = first->has_y = true;
      first->x = line->left + anchor * line->width;
      first->y = ysign * line->baseline;
      if (line->next == line->to && line->next < n && !t->chars[line->next - 1].newline && w.cps[line->to - 1] == 0xAD && w.hyphen_cp[line->to - 1]) {
        t->chars[line->to - 1].cp = w.hyphen_cp[line->to - 1]; /* the line ends at a soft hyphen: it is shown */
      }
    }
  }
  size_t out = 0;
  for (size_t c = 0; c < n; c++) {
    moved[c] = out;
    if (keep[c]) {
      t->chars[out++] = t->chars[c];
    }
  }
  moved[n] = out;
  for (size_t d = 0; d < t->decor_count; d++) {
    t->decors[d].start = moved[t->decors[d].start > n ? n : t->decors[d].start];
    t->decors[d].end = moved[t->decors[d].end > n ? n : t->decors[d].end];
  }
  t->count = out;
  *wrapped = true;
  return GVEC_OK;
}
