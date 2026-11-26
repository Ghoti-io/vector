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
 * @file
 *
 * The animated document (design.md A1 to A12). Every animation element of the document as written is read (its timing, target,
 * attribute and values; one in error is reported and applies nothing), the timing model is run to the time, and the animations
 * that apply are put in SMIL's sandwich order. Then:
 *
 *  - an animation of a property becomes a keyframe effect for the style layer, which hands it to text's sampler;
 *  - an animation of `d` is interpolated here (text interpolates `path()` as a string) and becomes one effect of its result;
 *  - an animation of any other attribute is computed here, on the attribute's typed values, and its result is written into the
 *    document built again from the markup, as if the file had said it;
 *  - `animateMotion` becomes a matrix the lowering applies outside the target's transform;
 *  - `discard` leaves its target out of the document built again.
 */

#include <ghoti.io/vector/macros.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/math/elementary.h>
#include <ghoti.io/text/markup.h>

#include "../../../core/core_internal.h"
#include "../svg_arena.h"
#include "../svg_doc.h"
#include "../svg_grammar.h"
#include "../svg_tables.h"
#include "anim_internal.h"
#include "anim_motion.h"
#include "anim_value.h"

/* ---- the state an animated document carries ------------------------------------------------------------------------ */

struct gvec_anim_state {
  double time_ms;
  size_t element_count;
  size_t * effect_start;     ///< Per element of the animated document, the first of its effects; one more at the end.
  gvec_anim_effect * effects;
  size_t effect_count;
  const GVEC_Svg_Element ** written; ///< Per element: the element of the document as written it was built for.
  double * motion;           ///< Six numbers per element.
  uint8_t * has_motion;
  gvec_arena arena;          ///< The frames' text.
};

void gvec_anim_state_free(struct gvec_anim_state * s, const GVEC_Allocator * allocator) {
  if (!s) {
    return;
  }
  gvec_free(allocator, s->effect_start);
  gvec_free(allocator, s->effects);
  gvec_free(allocator, (void *) s->written);
  gvec_free(allocator, s->motion);
  gvec_free(allocator, s->has_motion);
  gvec_arena_destroy(&s->arena);
  gvec_free(allocator, s);
}

const gvec_anim_effect * gvec_anim_effects_of(const GVEC_Svg_Document * d, const GVEC_Svg_Element * e, size_t * count) {
  *count = 0;
  if (!d || !d->anim || !e || e->order >= d->anim->element_count) {
    return NULL;
  }
  const struct gvec_anim_state * s = d->anim;
  size_t a = s->effect_start[e->order], b = s->effect_start[e->order + 1];
  *count = b - a;
  return b > a ? s->effects + a : NULL;
}

bool gvec_anim_animates(const GVEC_Svg_Document * d, const GVEC_Svg_Element * e, const char * attribute) {
  size_t n = 0;
  const gvec_anim_effect * list = gvec_anim_effects_of(d, e, &n);
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i].attribute, attribute) == 0) {
      return true;
    }
  }
  return false;
}

const GVEC_Svg_Element * gvec_anim_written_element(const GVEC_Svg_Document * d, const GVEC_Svg_Element * e) {
  if (!d || !d->anim || !e || e->order >= d->anim->element_count || !d->anim->written[e->order]) {
    return e;
  }
  return d->anim->written[e->order];
}

bool gvec_anim_motion_of(const GVEC_Svg_Document * d, const GVEC_Svg_Element * e, double m[6]) {
  if (!d || !d->anim || !e || e->order >= d->anim->element_count || !d->anim->has_motion[e->order]) {
    return false;
  }
  memcpy(m, d->anim->motion + 6 * e->order, 6 * sizeof *m);
  return true;
}

/* ---- reading an animation element ---------------------------------------------------------------------------------- */

typedef enum { MODE_VALUES, MODE_FROM_TO, MODE_FROM_BY, MODE_BY, MODE_TO } anim_mode;
typedef enum { CALC_DISCRETE, CALC_LINEAR, CALC_PACED, CALC_SPLINE } anim_calc;
typedef enum { CLS_NONE, CLS_PROPERTY, CLS_D, CLS_ATTRIBUTE, CLS_MOTION, CLS_DISCARD } anim_cls;
typedef enum { ROT_ANGLE, ROT_AUTO, ROT_AUTO_REVERSE } anim_rotate;

typedef struct text_span {
  const char * s;
  size_t n;
} text_span;

typedef struct anim {
  const GVEC_Svg_Element * e;
  const GVEC_Svg_Element * target;
  uint32_t index;
  anim_cls cls;
  const char * attr;
  bool valid;
  anim_mode mode;
  anim_calc calc;
  bool additive, accumulate, set;
  text_span * vals;      ///< The values (`from`, `to`, `by` among them, in that order), for an animation of a property as CSS-to-be text.
  size_t nvals;
  double * kt;           ///< keyTimes, NULL for none.
  size_t nkt;
  double * splines;      ///< Four per interval.
  size_t nsplines;
  /* animateMotion */
  gvec_anim_motion path;
  bool has_path;
  double * kp;
  size_t nkp;
  anim_rotate rotate;
  double rotate_angle;
  double * points;       ///< values/from/to/by as points (pairs).
  size_t npoints;
  /* the timing */
  gvec_anim_spec * specs;
  gvec_anim_timing timing;
  gvec_anim_sample sample;
} anim;

typedef struct actx {
  const GVEC_Svg_Document * doc;
  const GVEC_Allocator * allocator;
  GVEC_Diagnostics * diag;
  gvec_arena scratch;
  gvec_svg_parse_context parse;
  uint32_t * timed_of;   ///< Per element: its timed index, or GVEC_ANIM_NONE.
  anim * anims;
  size_t count;
  GVEC_Result error;
} actx;

static bool wsp(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static text_span trim(const char * s, size_t n) {
  while (n && wsp(s[0])) {
    s++;
    n--;
  }
  while (n && wsp(s[n - 1])) {
    n--;
  }
  text_span t = {s, n};
  return t;
}

static bool is(text_span t, const char * word) {
  size_t n = strlen(word);
  return t.n == n && memcmp(t.s, word, n) == 0;
}

static const GVEC_Svg_Attribute * attr_of(const GVEC_Svg_Element * e, const char * name) {
  for (size_t i = 0; i < e->attribute_count; i++) {
    if (!e->attributes[i].ns && strcmp(e->attributes[i].name, name) == 0) {
      return &e->attributes[i];
    }
  }
  return NULL;
}

static bool raw(const GVEC_Svg_Element * e, const char * name, text_span * out) {
  const GVEC_Svg_Attribute * a = attr_of(e, name);
  if (!a) {
    return false;
  }
  *out = trim(a->raw, a->raw_length);
  return true;
}

static void report(actx * c, const anim * a, const char * message) {
  const GVEC_Svg_Element * e = a->e;
  if (gvec_diag_add(c->diag, GVEC_DIAG_WARNING, GVEC_DIAG_ANIMATION_INVALID, 0, e->node.line, e->node.column, e->node.offset, e->name,
          message) == GVEC_ERR_OOM) {
    c->error = GVEC_ERR_OOM;
  }
}

/* Split `t` at `;` into trimmed items. A `;` at the end ends the list (SMIL's lists end without one, the browsers accept it). An empty
 * item in the middle is kept, empty, for the caller to refuse. */
static text_span * split(actx * c, text_span t, size_t * count) {
  size_t n = 1;
  for (size_t i = 0; i < t.n; i++) {
    n += t.s[i] == ';';
  }
  text_span * items = gvec_arena_array(&c->scratch, n, sizeof *items, 8);
  if (!items) {
    c->error = GVEC_ERR_OOM;
    *count = 0;
    return NULL;
  }
  size_t k = 0, start = 0;
  for (size_t i = 0; i <= t.n; i++) {
    if (i == t.n || t.s[i] == ';') {
      items[k++] = trim(t.s + start, i - start);
      start = i + 1;
    }
  }
  if (k > 1 && items[k - 1].n == 0) {
    k--;
  }
  *count = k;
  return items;
}

static bool number_of(text_span t, double * out) {
  gvec_svg_parse_error err;
  return t.n && gvec_svg_parse_number(t.s, t.n, out, &err) == GVEC_SVG_PARSE_OK;
}

/* Numbers separated by comma-wsp. */
static double * numbers_of(actx * c, text_span t, size_t * count) {
  gvec_svg_numbers list;
  gvec_svg_parse_error err;
  *count = 0;
  if (gvec_svg_parse_number_list(t.s, t.n, &c->parse, &list, &err) != GVEC_SVG_PARSE_OK) {
    return NULL;
  }
  *count = list.count;
  return (double *) list.items;
}

/* ---- timing values ------------------------------------------------------------------------------------------------- */

static bool name_char(char ch, bool first) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || (!first && ((ch >= '0' && ch <= '9') || ch == '-' || ch == ':'));
}

/* S? ("+" | "-") S? Clock-value, or nothing; after a syncbase or an event the sign is required. */
static bool offset_of(text_span t, bool required_sign, double * out) {
  t = trim(t.s, t.n);
  *out = 0;
  if (t.n == 0) {
    return true;
  }
  double sign = 1;
  if (t.s[0] == '+' || t.s[0] == '-') {
    sign = t.s[0] == '-' ? -1 : 1;
    t = trim(t.s + 1, t.n - 1);
  } else if (required_sign) {
    return false;
  }
  double v;
  if (!gvec_anim_clock(t.s, t.n, &v)) {
    return false;
  }
  *out = sign * v;
  return true;
}

typedef enum { TV_INVALID, TV_NEVER, TV_SPEC } tv_kind;

/* One begin or end value: a spec that resolves without events, a valid value that never does here, or one that is not in the grammar
 * (or names no timed element). */
static tv_kind time_value(actx * c, text_span t, gvec_anim_spec * spec) {
  memset(spec, 0, sizeof *spec);
  spec->source = GVEC_ANIM_NONE;
  if (t.n == 0) {
    return TV_INVALID;
  }
  if (is(t, "indefinite")) {
    return TV_NEVER;
  }
  if (t.s[0] == '+' || t.s[0] == '-' || (t.s[0] >= '0' && t.s[0] <= '9') || t.s[0] == '.') {
    double v;
    if (!offset_of(t, false, &v)) {
      return TV_INVALID;
    }
    spec->sync = GVEC_ANIM_SYNC_OFFSET;
    spec->offset = v;
    return TV_SPEC;
  }
  if ((t.n > 9 && memcmp(t.s, "wallclock", 9) == 0) || (t.n > 9 && memcmp(t.s, "accessKey", 9) == 0)) {
    const char * open = memchr(t.s, '(', t.n);
    const char * close = open ? memchr(open, ')', t.n - (size_t) (open - t.s)) : NULL;
    if (!open || !close) {
      return TV_INVALID;
    }
    if (memcmp(t.s, "wallclock", 9) == 0) {
      return close == t.s + t.n - 1 ? TV_NEVER : TV_INVALID;
    }
    double v;
    return offset_of((text_span){close + 1, t.n - (size_t) (close + 1 - t.s)}, true, &v) ? TV_NEVER : TV_INVALID;
  }
  /* (Id-value ".")? ( "begin" | "end" | event-ref | "repeat(" n ")" ) offset? ; an Id-value escapes a "." as "\." */
  char id[256];
  size_t idn = 0, i = 0;
  bool dotted = false;
  for (; i < t.n; i++) {
    char ch = t.s[i];
    if (ch == '\\' && i + 1 < t.n) {
      if (idn + 1 >= sizeof id) {
        return TV_INVALID;
      }
      id[idn++] = t.s[++i];
      continue;
    }
    if (ch == '.') {
      dotted = true;
      break;
    }
    if (ch == '+' || ch == '-' || wsp(ch)) {
      break;
    }
    if (idn + 1 >= sizeof id) {
      return TV_INVALID;
    }
    id[idn++] = ch;
  }
  id[idn] = '\0';
  size_t name_at = dotted ? i + 1 : 0;
  size_t j = name_at;
  while (j < t.n && (name_char(t.s[j], j == name_at) || t.s[j] == '(' || t.s[j] == ')' || (t.s[j] >= '0' && t.s[j] <= '9'))) {
    j++;
  }
  text_span name = {t.s + name_at, j - name_at};
  double off;
  if (name.n == 0 || !name_char(name.s[0], true) || !offset_of((text_span){t.s + j, t.n - j}, true, &off)) {
    return TV_INVALID;
  }
  bool sync = dotted && (is(name, "begin") || is(name, "end"));
  if (!sync) {
    /* An event, or a repeat value (the repeat event a seek does not fire): valid, and never resolved here. */
    if (name.n > 7 && memcmp(name.s, "repeat(", 7) == 0) {
      if (name.s[name.n - 1] != ')') {
        return TV_INVALID;
      }
    }
    return TV_NEVER;
  }
  if (idn == 0) {
    return TV_INVALID;
  }
  const GVEC_Svg_Element * source = gvec_svg_find_by_id(c->doc, id);
  if (!source || c->timed_of[source->order] == GVEC_ANIM_NONE) {
    return TV_INVALID;
  }
  spec->sync = is(name, "begin") ? GVEC_ANIM_SYNC_BEGIN : GVEC_ANIM_SYNC_END;
  spec->source = c->timed_of[source->order];
  spec->offset = off;
  return TV_SPEC;
}

static void read_list(actx * c, anim * a, const char * name, bool * has, bool * events, const gvec_anim_spec ** out, size_t * out_count) {
  text_span t;
  *out = NULL;
  *out_count = 0;
  *has = raw(a->e, name, &t);
  if (!*has) {
    return;
  }
  size_t n = 0;
  text_span * items = split(c, t, &n);
  gvec_anim_spec * specs = n ? gvec_arena_array(&c->scratch, n, sizeof *specs, 8) : NULL;
  if (n && !specs) {
    c->error = GVEC_ERR_OOM;
    return;
  }
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    tv_kind kind = time_value(c, items[i], &specs[k]);
    if (kind == TV_SPEC) {
      k++;
    } else if (kind == TV_NEVER && events) {
      *events = true;
    }
  }
  *out = specs;
  *out_count = k;
}

static double clock_or(text_span t, double indefinite, bool * ok) {
  double v;
  *ok = true;
  if (is(t, "indefinite")) {
    return indefinite;
  }
  if (gvec_anim_clock(t.s, t.n, &v)) {
    return v;
  }
  *ok = false;
  return NAN;
}

static void read_timing(actx * c, anim * a) {
  gvec_anim_timing * t = &a->timing;
  memset(t, 0, sizeof *t);
  bool has_begin = false;
  read_list(c, a, "begin", &has_begin, NULL, &t->begin, &t->begin_count);
  if (!has_begin) {
    static const gvec_anim_spec zero = {GVEC_ANIM_SYNC_OFFSET, GVEC_ANIM_NONE, 0};
    t->begin = &zero;
    t->begin_count = 1;
  }
  read_list(c, a, "end", &t->has_end, &t->end_events, &t->end, &t->end_count);
  text_span v;
  bool ok;
  t->dur = INFINITY;
  t->dur_given = false;
  if (raw(a->e, "dur", &v)) {
    if (is(v, "media") || is(v, "indefinite")) {
      t->dur_given = true;
    } else {
      double d = clock_or(v, INFINITY, &ok);
      if (ok && d > 0) {
        t->dur = d;
        t->dur_given = true;
      }
    }
  }
  t->repeat_count = NAN;
  if (raw(a->e, "repeatCount", &v)) {
    double n;
    if (is(v, "indefinite")) {
      t->repeat_count = INFINITY;
    } else if (number_of(v, &n) && n > 0) {
      t->repeat_count = n;
    }
  }
  t->repeat_dur = NAN;
  if (raw(a->e, "repeatDur", &v)) {
    double d = clock_or(v, INFINITY, &ok);
    if (ok && d > 0) {
      t->repeat_dur = d;
    }
  }
  t->min = 0;
  if (raw(a->e, "min", &v)) {
    double d = is(v, "media") ? 0 : clock_or(v, 0, &ok);
    if (is(v, "media") || (ok && d >= 0)) {
      t->min = is(v, "media") ? 0 : d;
    }
  }
  t->max = INFINITY;
  if (raw(a->e, "max", &v)) {
    double d = is(v, "media") ? INFINITY : clock_or(v, INFINITY, &ok);
    if (is(v, "media") || (ok && d > 0)) {
      t->max = d;
    }
  }
  t->restart = GVEC_ANIM_RESTART_ALWAYS;
  if (raw(a->e, "restart", &v)) {
    t->restart = is(v, "never") ? GVEC_ANIM_RESTART_NEVER : is(v, "whenNotActive") ? GVEC_ANIM_RESTART_WHEN_NOT_ACTIVE : GVEC_ANIM_RESTART_ALWAYS;
  }
  t->freeze = raw(a->e, "fill", &v) && is(v, "freeze");
  if (a->cls == CLS_DISCARD) {
    /* `discard` acts at its begin and for good: an interval that never ends. */
    t->has_end = false;
    t->end_count = 0;
    t->dur = INFINITY;
    t->dur_given = false;
    t->repeat_count = NAN;
    t->repeat_dur = NAN;
    t->min = 0;
    t->max = INFINITY;
    t->freeze = true;
  }
}

/* ---- what is animated ---------------------------------------------------------------------------------------------- */

/* SVG 2's "Animatable: no" (design A4), with the attributes of the animation elements. */
static bool not_animatable(const char * name) {
  static const char * const k[] = {"id", "lang", "xml:lang", "xml:space", "style", "tabindex", "role", "systemLanguage", "requiredExtensions",
      "requiredFeatures", "zoomAndPan", "download", "ping", "rel", "hreflang", "referrerPolicy", "xlink:title", "xml:base"};
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
    if (strcmp(name, k[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool is_animation_kind(GVEC_Svg_Kind k) {
  return k == GVEC_SVG_KIND_ANIMATE || k == GVEC_SVG_KIND_SET || k == GVEC_SVG_KIND_ANIMATE_TRANSFORM || k == GVEC_SVG_KIND_ANIMATE_MOTION ||
         k == GVEC_SVG_KIND_DISCARD;
}

static const GVEC_Svg_Element * target_of(actx * c, const GVEC_Svg_Element * e, bool * named) {
  text_span t;
  *named = false;
  if (raw(e, "href", &t) || raw(e, "xlink:href", &t)) {
    *named = true;
    if (t.n < 2 || t.s[0] != '#') {
      return NULL;
    }
    char id[256];
    if (t.n - 1 >= sizeof id) {
      return NULL;
    }
    memcpy(id, t.s + 1, t.n - 1);
    id[t.n - 1] = '\0';
    return gvec_svg_find_by_id(c->doc, id);
  }
  return e->node.parent;
}

/* The attribute name, with a prefix of the XLink namespace written as the document names it. */
static const char * attribute_name(actx * c, text_span t) {
  char * s = gvec_arena_strndup(&c->scratch, t.s, t.n);
  if (!s) {
    c->error = GVEC_ERR_OOM;
  }
  return s;
}

/* What an animated value of attribute `name` on `target` is typed as, by the build's own rules. */
static gvec_svg_parse_status type_value(actx * c, const GVEC_Svg_Element * target, const char * name, text_span v, GVEC_Svg_Attribute * out) {
  gvec_svg_parse_error err;
  gvec_svg_parse_status st = gvec_svg_type_value(&c->parse, target->kind, name, v.s, v.n, out, &err);
  if (st == GVEC_SVG_PARSE_OOM) {
    c->error = GVEC_ERR_OOM;
  }
  return st;
}

static bool read_key_times(actx * c, anim * a, size_t n) {
  text_span t;
  if (!raw(a->e, "keyTimes", &t)) {
    return true;
  }
  size_t k = 0;
  text_span * items = split(c, t, &k);
  if (!items || k != n) {
    return false;
  }
  a->kt = gvec_arena_array(&c->scratch, k, sizeof *a->kt, 8);
  if (!a->kt) {
    c->error = GVEC_ERR_OOM;
    return false;
  }
  for (size_t i = 0; i < k; i++) {
    if (!number_of(items[i], &a->kt[i]) || a->kt[i] < 0 || a->kt[i] > 1 || (i && a->kt[i] < a->kt[i - 1])) {
      return false;
    }
  }
  if (a->kt[0] != 0) {
    return false;
  }
  if (a->calc != CALC_DISCRETE && a->kt[k - 1] != 1) {
    return false;
  }
  a->nkt = k;
  return true;
}

static bool read_splines(actx * c, anim * a, size_t intervals) {
  if (a->calc != CALC_SPLINE) {
    return true;
  }
  text_span t;
  if (!raw(a->e, "keySplines", &t)) {
    return false;
  }
  size_t k = 0;
  text_span * items = split(c, t, &k);
  if (!items || k != intervals) {
    return false;
  }
  a->splines = gvec_arena_array(&c->scratch, 4 * (k ? k : 1), sizeof *a->splines, 8);
  if (!a->splines) {
    c->error = GVEC_ERR_OOM;
    return false;
  }
  for (size_t i = 0; i < k; i++) {
    size_t m = 0;
    double * v = numbers_of(c, items[i], &m);
    if (!v || m != 4) {
      return false;
    }
    for (size_t j = 0; j < 4; j++) {
      if (v[j] < 0 || v[j] > 1) {
        return false;
      }
      a->splines[4 * i + j] = v[j];
    }
  }
  a->nsplines = k;
  return true;
}

/* `animateTransform`'s value of its type as the transform function the attribute would say (A6). */
static bool transform_text(actx * c, const char * type, text_span v, text_span * out, double nums[3], size_t * count) {
  size_t m = 0;
  double * x = numbers_of(c, v, &m);
  if (!x) {
    return false;
  }
  double a0 = m > 0 ? x[0] : 0, a1 = m > 1 ? x[1] : 0, a2 = m > 2 ? x[2] : 0;
  gvec_anim_buf b = {c->allocator, NULL, 0, 0, false};
  if (strcmp(type, "translate") == 0 && (m == 1 || m == 2)) {
    gvec_anim_buf_put(&b, "translate(", 10);
    gvec_anim_buf_number(&b, a0);
    gvec_anim_buf_put(&b, " ", 1);
    gvec_anim_buf_number(&b, m > 1 ? a1 : 0);
    nums[0] = a0;
    nums[1] = m > 1 ? a1 : 0;
    *count = 2;
  } else if (strcmp(type, "scale") == 0 && (m == 1 || m == 2)) {
    gvec_anim_buf_put(&b, "scale(", 6);
    gvec_anim_buf_number(&b, a0);
    gvec_anim_buf_put(&b, " ", 1);
    gvec_anim_buf_number(&b, m > 1 ? a1 : a0);
    nums[0] = a0;
    nums[1] = m > 1 ? a1 : a0;
    *count = 2;
  } else if (strcmp(type, "rotate") == 0 && (m == 1 || m == 3)) {
    /* Always with its center, so that every value of the animation is the same three functions (A6). */
    gvec_anim_buf_put(&b, "rotate(", 7);
    gvec_anim_buf_number(&b, a0);
    gvec_anim_buf_put(&b, " ", 1);
    gvec_anim_buf_number(&b, a1);
    gvec_anim_buf_put(&b, " ", 1);
    gvec_anim_buf_number(&b, a2);
    nums[0] = a0;
    *count = 1;
  } else if ((strcmp(type, "skewX") == 0 || strcmp(type, "skewY") == 0) && m == 1) {
    gvec_anim_buf_put(&b, type, 5);
    gvec_anim_buf_put(&b, "(", 1);
    gvec_anim_buf_number(&b, a0);
    nums[0] = a0;
    *count = 1;
  } else {
    gvec_anim_buf_free(&b);
    return false;
  }
  gvec_anim_buf_put(&b, ")", 1);
  if (b.failed) {
    gvec_anim_buf_free(&b);
    c->error = GVEC_ERR_OOM;
    return false;
  }
  out->s = gvec_arena_strndup(&c->scratch, b.data, b.length);
  out->n = b.length;
  gvec_anim_buf_free(&b);
  if (!out->s) {
    c->error = GVEC_ERR_OOM;
    return false;
  }
  return true;
}

/* A color in the forms paced reads (#rgb, #rrggbb, rgb()), as three channels. */
static bool color_of(text_span t, double rgb[3]) {
  if (t.n == 4 || t.n == 7) {
    if (t.s[0] != '#') {
      return false;
    }
    unsigned v[6];
    for (size_t i = 1; i < t.n; i++) {
      char ch = t.s[i];
      int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
      if (d < 0) {
        return false;
      }
      v[i - 1] = (unsigned) d;
    }
    for (int k = 0; k < 3; k++) {
      rgb[k] = t.n == 4 ? v[k] * 17.0 : v[2 * k] * 16.0 + v[2 * k + 1];
    }
    return true;
  }
  if (t.n > 5 && memcmp(t.s, "rgb(", 4) == 0 && t.s[t.n - 1] == ')') {
    size_t k = 0, start = 4;
    for (size_t i = 4; i < t.n && k < 3; i++) {
      if (t.s[i] == ',' || t.s[i] == ')') {
        text_span part = trim(t.s + start, i - start);
        double v;
        bool pct = part.n && part.s[part.n - 1] == '%';
        if (!number_of((text_span){part.s, pct ? part.n - 1 : part.n}, &v)) {
          return false;
        }
        rgb[k++] = pct ? v * 2.55 : v;
        start = i + 1;
      }
    }
    return k == 3;
  }
  return false;
}

/* The point of each motion value (`x,y` or `x y`). */
static bool point_of(actx * c, text_span t, double * xy) {
  size_t m = 0;
  double * v = numbers_of(c, t, &m);
  if (!v || m != 2) {
    return false;
  }
  xy[0] = v[0];
  xy[1] = v[1];
  return true;
}

/* The paced key times: the share of the total distance each value is at. False when the values have no distance (then linear). */
static bool paced_times(actx * c, anim * a, const double * dist, size_t n) {
  double total = 0;
  for (size_t i = 0; i + 1 < n; i++) {
    total += dist[i];
  }
  a->kt = gvec_arena_array(&c->scratch, n, sizeof *a->kt, 8);
  if (!a->kt) {
    c->error = GVEC_ERR_OOM;
    return false;
  }
  a->nkt = n;
  double at = 0;
  for (size_t i = 0; i < n; i++) {
    a->kt[i] = total > 0 ? at / total : (n > 1 ? (double) i / (double) (n - 1) : 0);
    if (i + 1 < n) {
      at += dist[i];
    }
  }
  a->kt[n - 1] = 1;
  return true;
}

static void read_animation(actx * c, anim * a) {
  const GVEC_Svg_Element * e = a->e;
  a->valid = false;
  bool named = false;
  a->target = target_of(c, e, &named);
  if (e->kind == GVEC_SVG_KIND_DISCARD) {
    a->cls = CLS_DISCARD;
    read_timing(c, a);
    a->valid = a->target != NULL;
    if (!a->target) {
      report(c, a, "the discard names no element of the document; it is ignored");
    }
    return;
  }
  a->cls = e->kind == GVEC_SVG_KIND_ANIMATE_MOTION ? CLS_MOTION : CLS_NONE;
  read_timing(c, a);
  if (!a->target) {
    report(c, a, named ? "the animation's href names no element of the document; it is ignored" : "the animation has no target; it is ignored");
    return;
  }
  text_span v;
  /* calcMode, additive, accumulate. */
  a->calc = e->kind == GVEC_SVG_KIND_ANIMATE_MOTION ? CALC_PACED : CALC_LINEAR;
  if (raw(e, "calcMode", &v)) {
    a->calc = is(v, "discrete") ? CALC_DISCRETE : is(v, "linear") ? CALC_LINEAR : is(v, "paced") ? CALC_PACED : is(v, "spline") ? CALC_SPLINE : a->calc;
  }
  a->additive = raw(e, "additive", &v) && is(v, "sum");
  a->accumulate = raw(e, "accumulate", &v) && is(v, "sum");
  a->set = e->kind == GVEC_SVG_KIND_SET;
  if (a->set) {
    a->calc = CALC_DISCRETE;
    a->additive = false;
    a->accumulate = false;
  }
  if (a->cls != CLS_MOTION) {
    if (!raw(e, "attributeName", &v) || v.n == 0) {
      report(c, a, "the animation has no attributeName; it is ignored");
      return;
    }
    a->attr = attribute_name(c, v);
    if (!a->attr) {
      return;
    }
    size_t nl = strlen(a->attr);
    if (is_animation_kind(a->target->kind) || a->target->kind == GVEC_SVG_KIND_SCRIPT || a->target->kind == GVEC_SVG_KIND_STYLE ||
        not_animatable(a->attr)) {
      report(c, a, "the attribute the animation names cannot be animated; it is ignored");
      return;
    }
    bool presentation = gvec_svg_is_presentation(a->target->kind, a->attr, nl);
    GVEC_Svg_Grammar g = gvec_svg_grammar_for(a->target->kind, a->attr, nl);
    if (e->kind == GVEC_SVG_KIND_ANIMATE_TRANSFORM) {
      bool transform = (presentation || g == GVEC_SVG_GRAMMAR_TRANSFORM) &&
                       (strcmp(a->attr, "transform") == 0 || strcmp(a->attr, "gradientTransform") == 0 || strcmp(a->attr, "patternTransform") == 0);
      if (!transform) {
        report(c, a, "animateTransform names an attribute that is not a transform; it is ignored");
        return;
      }
      a->cls = CLS_PROPERTY;
    } else if (presentation && strcmp(a->attr, "d") == 0) {
      a->cls = CLS_D;
    } else if (presentation || g == GVEC_SVG_GRAMMAR_TRANSFORM) {
      a->cls = CLS_PROPERTY;
    } else if (g != GVEC_SVG_GRAMMAR_NONE && g != GVEC_SVG_GRAMMAR_EVENT && g != GVEC_SVG_GRAMMAR_RAW_ARIA && g != GVEC_SVG_GRAMMAR_RAW_ANIM) {
      a->cls = CLS_ATTRIBUTE;
    } else {
      report(c, a, "the animation names an attribute its target does not have (a CSS property with no presentation attribute is not animated); it is ignored");
      return;
    }
  }
  /* The values: `values`, else from/to/by. */
  text_span from, to, by;
  bool has_from = raw(e, "from", &from), has_to = raw(e, "to", &to), has_by = raw(e, "by", &by);
  text_span list;
  if (a->set) {
    if (!has_to) {
      report(c, a, "the set has no to; it is ignored");
      return;
    }
    a->mode = MODE_VALUES;
    a->vals = gvec_arena_array(&c->scratch, 1, sizeof *a->vals, 8);
    if (!a->vals) {
      c->error = GVEC_ERR_OOM;
      return;
    }
    a->vals[0] = to;
    a->nvals = 1;
  } else if (raw(e, "values", &list)) {
    a->mode = MODE_VALUES;
    a->vals = split(c, list, &a->nvals);
  } else if (has_from && has_to) {
    a->mode = MODE_FROM_TO;
  } else if (has_from && has_by) {
    a->mode = MODE_FROM_BY;
  } else if (has_by) {
    a->mode = MODE_BY;
  } else if (has_to) {
    a->mode = MODE_TO;
  } else if (a->cls != CLS_MOTION) {
    report(c, a, "the animation has no values, from, to or by; it is ignored");
    return;
  }
  if (a->mode != MODE_VALUES || (a->cls == CLS_MOTION && !a->vals)) {
    text_span pair[2];
    size_t n = 0;
    if (a->mode == MODE_FROM_TO) {
      pair[n++] = from;
      pair[n++] = to;
    } else if (a->mode == MODE_FROM_BY) {
      pair[n++] = from;
      pair[n++] = by;
    } else if (a->mode == MODE_BY) {
      pair[n++] = by;
    } else if (a->mode == MODE_TO) {
      pair[n++] = to;
    }
    a->vals = n ? gvec_arena_array(&c->scratch, n, sizeof *a->vals, 8) : NULL;
    if (n && !a->vals) {
      c->error = GVEC_ERR_OOM;
      return;
    }
    if (n) {
      memcpy(a->vals, pair, n * sizeof *pair);
    }
    a->nvals = n;
  }
  if (c->error != GVEC_OK) {
    return;
  }
  /* to-animation is neither additive nor cumulative; by-animation is additive (SMIL 3.0 Animation). */
  if (a->mode == MODE_TO) {
    a->additive = false;
    a->accumulate = false;
  }
  if (a->mode == MODE_BY) {
    a->additive = true;
  }
  for (size_t i = 0; i < a->nvals; i++) {
    if (a->vals[i].n == 0) {
      report(c, a, "a value of the animation is empty; it is ignored");
      return;
    }
  }
  /* The distance of each pair of values, for paced. */
  double * dist = a->nvals > 1 ? gvec_arena_array(&c->scratch, a->nvals - 1, sizeof *dist, 8) : NULL;
  bool has_distance = a->nvals > 1 && dist != NULL;
  if (a->cls == CLS_MOTION) {
    /* The motion path: mpath, path, then the values as points. */
    const GVEC_Svg_Element * mpath = NULL;
    for (const GVEC_Svg_Node * n = e->first_node; n; n = n->next) {
      if (n->is_element && n->element->kind == GVEC_SVG_KIND_MPATH) {
        mpath = n->element;
        break;
      }
    }
    const GVEC_Svg_Attribute * path_attr = NULL;
    if (mpath) {
      bool mn;
      const GVEC_Svg_Element * p = target_of(c, mpath, &mn);
      if (p && p != mpath->node.parent && p->kind == GVEC_SVG_KIND_PATH) {
        path_attr = attr_of(p, "d");
      }
    }
    GVEC_Svg_Attribute typed;
    if (!path_attr && raw(e, "path", &v)) {
      gvec_svg_parse_error err;
      gvec_svg_parse_status st = gvec_svg_type_value(&c->parse, GVEC_SVG_KIND_PATH, "d", v.s, v.n, &typed, &err);
      if (st == GVEC_SVG_PARSE_OOM) {
        c->error = GVEC_ERR_OOM;
        return;
      }
      if (st == GVEC_SVG_PARSE_OK && typed.value_kind == GVEC_SVG_VALUE_PATH) {
        path_attr = &typed;
      } else {
        report(c, a, "the path of the animateMotion is not path data; it is ignored");
        return;
      }
    }
    if (path_attr) {
      if (path_attr->cls != GVEC_SVG_ATTR_TYPED || path_attr->value_kind != GVEC_SVG_VALUE_PATH) {
        report(c, a, "the path of the animateMotion is not path data; it is ignored");
        return;
      }
      GVEC_Result r = gvec_anim_motion_path(path_attr->value.path.segments, path_attr->value.path.segment_count, c->allocator, &a->path);
      if (r != GVEC_OK) {
        if (r == GVEC_ERR_OOM) {
          c->error = r;
        }
        report(c, a, "the path of the animateMotion cannot be measured; it is ignored");
        return;
      }
      a->has_path = true;
    } else {
      if (a->nvals == 0) {
        report(c, a, "the animateMotion has no path, values, from, to or by; it is ignored");
        return;
      }
      size_t n = a->nvals + (a->mode == MODE_BY || a->mode == MODE_TO ? 1 : 0);
      a->points = gvec_arena_array(&c->scratch, 2 * n, sizeof *a->points, 8);
      if (!a->points) {
        c->error = GVEC_ERR_OOM;
        return;
      }
      size_t k = 0;
      if (a->mode == MODE_BY || a->mode == MODE_TO) {
        /* From the motion beneath, which is no motion: the origin. */
        a->points[k++] = 0;
        a->points[k++] = 0;
      }
      for (size_t i = 0; i < a->nvals; i++, k += 2) {
        if (!point_of(c, a->vals[i], a->points + k)) {
          report(c, a, "a value of the animateMotion is not a point; it is ignored");
          return;
        }
      }
      if (a->mode == MODE_FROM_BY) {
        a->points[2] += a->points[0];
        a->points[3] += a->points[1];
      }
      a->npoints = n;
      GVEC_Result r = gvec_anim_motion_points(a->points, n, c->allocator, &a->path);
      if (r != GVEC_OK) {
        if (r == GVEC_ERR_OOM) {
          c->error = r;
        }
        report(c, a, "the points of the animateMotion cannot be measured; it is ignored");
        return;
      }
      a->has_path = true;
    }
    /* keyPoints go with keyTimes, and are fractions of the length (not for paced). */
    if (raw(e, "keyPoints", &v) && a->calc != CALC_PACED) {
      size_t k = 0;
      text_span * items = split(c, v, &k);
      a->kp = k ? gvec_arena_array(&c->scratch, k, sizeof *a->kp, 8) : NULL;
      if (!items || !a->kp) {
        report(c, a, "the keyPoints of the animateMotion are invalid; it is ignored");
        return;
      }
      for (size_t i = 0; i < k; i++) {
        if (!number_of(items[i], &a->kp[i]) || a->kp[i] < 0 || a->kp[i] > 1) {
          report(c, a, "the keyPoints of the animateMotion are invalid; it is ignored");
          return;
        }
      }
      a->nkp = k;
    }
    a->rotate = ROT_ANGLE;
    a->rotate_angle = 0;
    if (raw(e, "rotate", &v)) {
      if (is(v, "auto")) {
        a->rotate = ROT_AUTO;
      } else if (is(v, "auto-reverse")) {
        a->rotate = ROT_AUTO_REVERSE;
      } else {
        GVEC_Svg_Length angle;
        gvec_svg_parse_error err;
        if (gvec_svg_parse_angle(v.s, v.n, &angle, &err) == GVEC_SVG_PARSE_OK) {
          a->rotate_angle = gvec_svg_angle_degrees(&angle);
        }
      }
    }
    size_t keys = a->nkp ? a->nkp : a->path.vertex_count;
    if (a->calc != CALC_PACED) {
      if (!read_key_times(c, a, keys)) {
        report(c, a, "the keyTimes of the animateMotion do not match its keyPoints or path; it is ignored");
        return;
      }
      if (a->nkp && !a->kt) {
        report(c, a, "keyPoints need keyTimes; the animateMotion is ignored");
        return;
      }
      if (!read_splines(c, a, keys > 1 ? keys - 1 : 1)) {
        report(c, a, "the keySplines of the animateMotion are invalid; it is ignored");
        return;
      }
    }
    a->valid = true;
    return;
  }
  /* Values of a property or an attribute: each must be a value of it. */
  if (e->kind == GVEC_SVG_KIND_ANIMATE_TRANSFORM) {
    char type[16] = "translate";
    if (raw(e, "type", &v)) {
      if (v.n >= sizeof type) {
        report(c, a, "the type of the animateTransform is not a transform type; it is ignored");
        return;
      }
      memcpy(type, v.s, v.n);
      type[v.n] = '\0';
    }
    double prev[3] = {0, 0, 0}, cur[3];
    size_t pn = 0;
    for (size_t i = 0; i < a->nvals; i++) {
      size_t cn = 0;
      if (!transform_text(c, type, a->vals[i], &a->vals[i], cur, &cn)) {
        report(c, a, "a value of the animateTransform is not a value of its type; it is ignored");
        return;
      }
      if (i > 0 && dist) {
        double s = 0;
        for (size_t j = 0; j < cn && j < pn; j++) {
          s += (cur[j] - prev[j]) * (cur[j] - prev[j]);
        }
        dist[i - 1] = gmath_sqrt(s);
      }
      memcpy(prev, cur, sizeof prev);
      pn = cn;
    }
  } else if (a->cls == CLS_ATTRIBUTE || a->cls == CLS_D) {
    gvec_anim_context vc;
    memset(&vc, 0, sizeof vc);
    vc.allocator = c->allocator;
    vc.font_size = 16;
    vc.viewport_width = c->doc->viewport_width;
    vc.viewport_height = c->doc->viewport_height;
    vc.percent_basis = c->doc->viewport_width;
    gvec_anim_value prev;
    memset(&prev, 0, sizeof prev);
    bool numeric = true;
    for (size_t i = 0; i < a->nvals; i++) {
      GVEC_Svg_Attribute typed;
      gvec_svg_parse_status st = type_value(c, a->target, a->attr, a->vals[i], &typed);
      if (st != GVEC_SVG_PARSE_OK && typed.cls != GVEC_SVG_ATTR_RAW) {
        gvec_anim_value_free(&prev, c->allocator);
        report(c, a, "a value of the animation is not a value of the attribute; it is ignored");
        return;
      }
      gvec_anim_value cur;
      if (numeric && gvec_anim_value_of(&typed, &vc, &cur)) {
        if (i > 0 && dist) {
          if (gvec_anim_value_same_shape(&prev, &cur)) {
            gvec_anim_value_unify(&prev, &cur, &vc);
            dist[i - 1] = gvec_anim_value_distance(&prev, &cur);
          } else {
            numeric = false;
          }
        }
        gvec_anim_value_free(&prev, c->allocator);
        prev = cur;
      } else {
        if (cur.oom) {
          c->error = GVEC_ERR_OOM;
        }
        numeric = false;
      }
    }
    gvec_anim_value_free(&prev, c->allocator);
    has_distance = has_distance && numeric;
  } else {
    /* A property: its values are checked by the style layer, against the property's own grammar. Paced needs a distance, which the
     * forms of number, length and color give. */
    double prev[3] = {0, 0, 0};
    int prev_unit = -1;
    for (size_t i = 0; i < a->nvals && has_distance; i++) {
      double cur[3] = {0, 0, 0};
      int unit = 0;
      GVEC_Svg_Length len;
      gvec_svg_parse_error err;
      if (gvec_svg_parse_length(a->vals[i].s, a->vals[i].n, &len, &err) == GVEC_SVG_PARSE_OK) {
        cur[0] = len.value;
        unit = 1 + (int) len.unit;
      } else if (color_of(a->vals[i], cur)) {
        unit = 100;
      } else {
        has_distance = false;
        break;
      }
      if (i > 0) {
        if (unit != prev_unit) {
          has_distance = false;
          break;
        }
        dist[i - 1] = gmath_sqrt((cur[0] - prev[0]) * (cur[0] - prev[0]) + (cur[1] - prev[1]) * (cur[1] - prev[1]) + (cur[2] - prev[2]) * (cur[2] - prev[2]));
      }
      memcpy(prev, cur, sizeof prev);
      prev_unit = unit;
    }
  }
  if (c->error != GVEC_OK) {
    return;
  }
  /* The key times and splines. The number of values the keys index: a to- or by-animation has two (the value beneath is the first). */
  size_t keyed = a->mode == MODE_VALUES ? a->nvals : 2;
  if (a->calc == CALC_PACED && a->mode == MODE_VALUES && keyed > 1) {
    if (!has_distance || !paced_times(c, a, dist, keyed)) {
      a->calc = CALC_LINEAR;
    }
  } else if (a->calc == CALC_PACED) {
    a->calc = CALC_LINEAR;
  }
  if (!a->kt && !read_key_times(c, a, keyed)) {
    report(c, a, "the keyTimes of the animation are invalid or do not match its values; it is ignored");
    return;
  }
  if (!read_splines(c, a, keyed > 1 ? keyed - 1 : 1)) {
    report(c, a, "the keySplines of the animation are invalid; it is ignored");
    return;
  }
  a->valid = c->error == GVEC_OK;
}

/* ---- where in the values ------------------------------------------------------------------------------------------- */

/* A cubic Bézier timing function at x (SMIL's keySplines): its y, by bisection on x. */
static double spline(const double * k, double x) {
  double lo = 0, hi = 1, t = x;
  for (int i = 0; i < 64; i++) {
    t = (lo + hi) / 2;
    double u = 1 - t;
    double bx = 3 * u * u * t * k[0] + 3 * u * t * t * k[2] + t * t * t;
    if (bx < x) {
      lo = t;
    } else {
      hi = t;
    }
  }
  double u = 1 - t;
  return 3 * u * u * t * k[1] + 3 * u * t * t * k[3] + t * t * t;
}

/* The key time of value i of n (keyTimes, else even spacing: i/(n-1), or i/n for discrete). */
static double key_time(const anim * a, size_t i, size_t n) {
  if (a->kt) {
    return a->kt[i];
  }
  if (a->calc == CALC_DISCRETE) {
    return (double) i / (double) n;
  }
  return n > 1 ? (double) i / (double) (n - 1) : 0;
}

/* The interval of the values the progress is in and how far into it, eased: for discrete, the value that holds and 0. */
static void locate(const anim * a, size_t n, double p, size_t * index, double * local) {
  *index = 0;
  *local = 0;
  if (n <= 1) {
    return;
  }
  if (a->calc == CALC_DISCRETE) {
    size_t i = 0;
    while (i + 1 < n && key_time(a, i + 1, n) <= p) {
      i++;
    }
    *index = i;
    return;
  }
  size_t i = 0;
  while (i + 2 < n && key_time(a, i + 1, n) <= p) {
    i++;
  }
  double k0 = key_time(a, i, n), k1 = key_time(a, i + 1, n);
  double l = k1 > k0 ? (p - k0) / (k1 - k0) : 1;
  if (l < 0) {
    l = 0;
  }
  if (l > 1) {
    l = 1;
  }
  if (a->calc == CALC_SPLINE && a->splines && i < a->nsplines) {
    l = spline(a->splines + 4 * i, l);
  }
  *index = i;
  *local = l;
}

/* ---- the effects of a property ------------------------------------------------------------------------------------ */

typedef struct effects {
  gvec_anim_effect * v;
  uint32_t * target;     ///< The element of the document as written each effect is for.
  size_t n, cap;
} effects;

static bool push_effect(actx * c, effects * fx, uint32_t target, const gvec_anim_effect * e) {
  if (fx->n == fx->cap) {
    size_t cap = fx->cap ? fx->cap * 2 : 16;
    gvec_anim_effect * v = gvec_realloc(c->allocator, fx->v, cap * sizeof *v);
    if (!v) {
      c->error = GVEC_ERR_OOM;
      return false;
    }
    fx->v = v;
    uint32_t * t = gvec_realloc(c->allocator, fx->target, cap * sizeof *t);
    if (!t) {
      c->error = GVEC_ERR_OOM;
      return false;
    }
    fx->target = t;
    fx->cap = cap;
  }
  fx->v[fx->n] = *e;
  fx->target[fx->n] = target;
  fx->n++;
  return true;
}

static const char * keep(struct gvec_anim_state * s, actx * c, const char * text, size_t n) {
  char * k = gvec_arena_strndup(&s->arena, text, n);
  if (!k) {
    c->error = GVEC_ERR_OOM;
  }
  return k;
}

/* The keyframes of one animation of a property (A5): every value at its key time, discrete as steps, the progress where the eased time is. */
static void property_effects(actx * c, struct gvec_anim_state * s, effects * fx, const anim * a) {
  const gvec_anim_sample * smp = &a->sample;
  double p = smp->indefinite ? 0 : smp->progress;
  gvec_anim_effect ef;
  memset(&ef, 0, sizeof ef);
  ef.attribute = keep(s, c, a->attr, strlen(a->attr));
  ef.animation = a->e;
  ef.add = a->additive;
  ef.accumulate = a->accumulate;
  ef.iteration = smp->iteration;
  if (c->error != GVEC_OK) {
    return;
  }
  size_t n = a->nvals;
  if (a->mode == MODE_VALUES || a->mode == MODE_FROM_TO) {
    size_t frames_n = a->calc == CALC_DISCRETE ? 2 * n : (n == 1 ? 2 : n);
    gvec_anim_frame * f = gvec_arena_array(&s->arena, frames_n, sizeof *f, 8);
    if (!f) {
      c->error = GVEC_ERR_OOM;
      return;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      const char * text = keep(s, c, a->vals[i].s, a->vals[i].n);
      if (!text) {
        return;
      }
      if (a->calc == CALC_DISCRETE) {
        f[k++] = (gvec_anim_frame){key_time(a, i, n), text, a->vals[i].n, -1};
        f[k++] = (gvec_anim_frame){i + 1 < n ? key_time(a, i + 1, n) : 1, text, a->vals[i].n, -1};
      } else if (n == 1) {
        f[k++] = (gvec_anim_frame){0, text, a->vals[i].n, -1};
        f[k++] = (gvec_anim_frame){1, text, a->vals[i].n, -1};
      } else {
        f[k++] = (gvec_anim_frame){key_time(a, i, n), text, a->vals[i].n, -1};
      }
    }
    ef.frames = f;
    ef.frame_count = k;
    if (a->calc == CALC_DISCRETE || n <= 1) {
      ef.progress = p;
    } else {
      size_t i;
      double l;
      locate(a, n, p, &i, &l);
      double k0 = key_time(a, i, n), k1 = key_time(a, i + 1, n);
      ef.progress = k0 + l * (k1 - k0);
    }
    push_effect(c, fx, a->target->order, &ef);
    return;
  }
  /* to, by and from-by: the value beneath is the first value, at key time 0 (an implicit keyframe for text's sampler). */
  size_t i;
  double l;
  locate(a, 2, p, &i, &l);
  double q = a->calc == CALC_DISCRETE ? (i == 1 ? 1 : 0) : l;
  if (a->mode == MODE_FROM_BY) {
    /* From `from` to `from` + `by`: a constant `from` and, above it, `by` added as it goes. */
    const char * from = keep(s, c, a->vals[0].s, a->vals[0].n);
    const char * by = keep(s, c, a->vals[1].s, a->vals[1].n);
    gvec_anim_frame * f = gvec_arena_array(&s->arena, 3, sizeof *f, 8);
    if (!from || !by || !f) {
      c->error = GVEC_ERR_OOM;
      return;
    }
    f[0] = (gvec_anim_frame){0, from, a->vals[0].n, -1};
    f[1] = (gvec_anim_frame){1, from, a->vals[0].n, -1};
    f[2] = (gvec_anim_frame){1, by, a->vals[1].n, -1};
    gvec_anim_effect lower = ef;
    lower.frames = f;
    lower.frame_count = 2;
    lower.progress = q;
    if (!push_effect(c, fx, a->target->order, &lower)) {
      return;
    }
    gvec_anim_effect upper = ef;
    upper.frames = f + 2;
    upper.frame_count = 1;
    upper.add = true;
    upper.progress = q;
    push_effect(c, fx, a->target->order, &upper);
    return;
  }
  const char * text = keep(s, c, a->vals[0].s, a->vals[0].n);
  gvec_anim_frame * f = gvec_arena_array(&s->arena, 1, sizeof *f, 8);
  if (!text || !f) {
    c->error = GVEC_ERR_OOM;
    return;
  }
  f[0] = (gvec_anim_frame){1, text, a->vals[0].n, -1};
  ef.frames = f;
  ef.frame_count = 1;
  ef.progress = q;
  push_effect(c, fx, a->target->order, &ef);
}

/* ---- attributes and `d` --------------------------------------------------------------------------------------------- */

typedef struct override_list {
  gvec_svg_override * v;
  size_t n, cap;
} override_list;

static bool push_override(actx * c, override_list * o, uint32_t element, const char * name, const char * value, size_t length) {
  if (o->n == o->cap) {
    size_t cap = o->cap ? o->cap * 2 : 16;
    gvec_svg_override * v = gvec_realloc(c->allocator, o->v, cap * sizeof *v);
    if (!v) {
      c->error = GVEC_ERR_OOM;
      return false;
    }
    o->v = v;
    o->cap = cap;
  }
  o->v[o->n++] = (gvec_svg_override){element, name, value, length};
  return true;
}

/* The units an attribute of a paint server, a pattern, a mask or a filter is in, when it is a length of the box. */
static bool bounding_box_units(const GVEC_Svg_Element * t, const char * name) {
  const char * units = NULL;
  bool box_default = true;
  switch (t->kind) {
  case GVEC_SVG_KIND_LINEAR_GRADIENT:
  case GVEC_SVG_KIND_RADIAL_GRADIENT:
    units = "gradientUnits";
    break;
  case GVEC_SVG_KIND_PATTERN:
    units = "patternUnits";
    break;
  case GVEC_SVG_KIND_MASK:
    units = "maskUnits";
    break;
  case GVEC_SVG_KIND_FILTER:
    units = "filterUnits";
    break;
  default:
    return false;
  }
  (void) name;
  const GVEC_Svg_Attribute * u = attr_of(t, units);
  if (u && u->cls == GVEC_SVG_ATTR_TYPED && u->value_kind == GVEC_SVG_VALUE_KEYWORD) {
    return strcmp(u->value.keyword.word, "objectBoundingBox") == 0;
  }
  return box_default;
}

/* The axis a percentage of an attribute is of: x, y, or the normalized diagonal. */
static double percent_basis(const GVEC_Svg_Document * d, const char * name) {
  char first = name[0];
  bool horizontal = first == 'x' || first == 'd' ? name[1] != 'y' : strcmp(name, "width") == 0 || strcmp(name, "fx") == 0 || strcmp(name, "cx") == 0;
  bool vertical = first == 'y' || strcmp(name, "height") == 0 || strcmp(name, "fy") == 0 || strcmp(name, "cy") == 0 || strcmp(name, "dy") == 0;
  if (horizontal) {
    return d->viewport_width;
  }
  if (vertical) {
    return d->viewport_height;
  }
  return gmath_sqrt((d->viewport_width * d->viewport_width + d->viewport_height * d->viewport_height) / 2);
}

typedef struct running {
  gvec_anim_value value;  ///< The value so far, when it is numeric.
  bool numeric;
  text_span text;         ///< The value so far as text (the last discrete one), when it is not.
  bool have;              ///< There is a value beneath (the attribute is written, or an animation below gave one).
} running;

/* One animation of an attribute over the value beneath it (A5, A10). */
static void apply_attribute(actx * c, const anim * a, running * r, const gvec_anim_context * vc) {
  const gvec_anim_sample * smp = &a->sample;
  double p = smp->indefinite ? 0 : smp->progress;
  size_t n = a->nvals;
  /* The values in play, typed. */
  size_t keyed = a->mode == MODE_VALUES ? n : 2;
  size_t i;
  double l;
  locate(a, keyed, p, &i, &l);
  text_span vt[2];
  size_t vn = 0;
  bool beneath_first = a->mode == MODE_TO || a->mode == MODE_BY;
  if (a->mode == MODE_VALUES || a->mode == MODE_FROM_TO || a->mode == MODE_FROM_BY) {
    vt[vn++] = a->vals[i < n ? i : n - 1];
    if (a->calc != CALC_DISCRETE && i + 1 < n) {
      vt[vn++] = a->vals[i + 1];
    }
  } else {
    vt[vn++] = a->vals[0];
  }
  gvec_anim_value tv[2];
  memset(tv, 0, sizeof tv);
  bool numeric = true;
  for (size_t k = 0; k < vn; k++) {
    GVEC_Svg_Attribute typed;
    type_value(c, a->target, a->attr, vt[k], &typed);
    if (!numeric || !gvec_anim_value_of(&typed, vc, &tv[k])) {
      if (tv[k].oom) {
        c->error = GVEC_ERR_OOM;
      }
      numeric = false;
    }
  }
  if (numeric && vn == 2 && !gvec_anim_value_same_shape(&tv[0], &tv[1])) {
    numeric = false;
  }
  if (numeric && (beneath_first || a->additive || a->accumulate) && r->have && r->numeric && !gvec_anim_value_same_shape(&r->value, &tv[0])) {
    numeric = false;
  }
  if (!numeric) {
    /* Discrete: the first value until half way (A5), or the value that holds. */
    text_span chosen;
    if (beneath_first) {
      bool second = a->calc == CALC_DISCRETE ? i == 1 : l >= 0.5;
      if (!second && r->have) {
        goto done;
      }
      chosen = a->vals[0];
    } else if (a->calc == CALC_DISCRETE || vn == 1) {
      chosen = vt[0];
    } else {
      chosen = l < 0.5 ? vt[0] : vt[1];
    }
    if (r->numeric) {
      gvec_anim_value_free(&r->value, c->allocator);
      r->numeric = false;
    }
    r->text = chosen;
    r->have = true;
    goto done;
  }
  {
    gvec_anim_value out;
    if (!gvec_anim_value_copy(&tv[0], &out, c->allocator)) {
      c->error = GVEC_ERR_OOM;
      goto done;
    }
    if (a->mode == MODE_TO) {
      if (r->have && r->numeric) {
        gvec_anim_value_unify(&r->value, &tv[0], vc);
        gvec_anim_value_unify(&r->value, &out, vc);
        gvec_anim_value_mix(&out, &r->value, &tv[0], a->calc == CALC_DISCRETE ? (i == 1 ? 1 : 0) : l);
      }
    } else if (a->mode == MODE_BY || a->mode == MODE_FROM_BY) {
      double q = a->calc == CALC_DISCRETE ? (i == 1 ? 1 : 0) : l;
      if (a->mode == MODE_FROM_BY) {
        GVEC_Svg_Attribute typed;
        gvec_anim_value by;
        type_value(c, a->target, a->attr, a->vals[1], &typed);
        if (gvec_anim_value_of(&typed, vc, &by) && gvec_anim_value_same_shape(&by, &out)) {
          gvec_anim_value_unify(&out, &by, vc);
          gvec_anim_value_add(&out, &by, q);
          if (a->accumulate) {
            gvec_anim_value end;
            if (gvec_anim_value_copy(&tv[0], &end, c->allocator)) {
              gvec_anim_value_unify(&end, &by, vc);
              gvec_anim_value_add(&end, &by, 1);
              gvec_anim_value_add(&out, &end, smp->iteration);
              gvec_anim_value_free(&end, c->allocator);
            }
          }
        }
        gvec_anim_value_free(&by, c->allocator);
      } else {
        for (size_t k = 0; k < out.count; k++) {
          out.numbers[k] *= q;
        }
        if (a->accumulate) {
          gvec_anim_value_add(&out, &tv[0], smp->iteration);
        }
      }
    } else {
      if (vn == 2) {
        gvec_anim_value_unify(&tv[0], &tv[1], vc);
        gvec_anim_value_unify(&out, &tv[1], vc);
        gvec_anim_value_mix(&out, &tv[0], &tv[1], l);
      }
      if (a->accumulate && smp->iteration > 0) {
        GVEC_Svg_Attribute typed;
        gvec_anim_value last;
        type_value(c, a->target, a->attr, a->vals[n - 1], &typed);
        if (gvec_anim_value_of(&typed, vc, &last) && gvec_anim_value_same_shape(&last, &out)) {
          gvec_anim_value_unify(&out, &last, vc);
          gvec_anim_value_add(&out, &last, smp->iteration);
        }
        gvec_anim_value_free(&last, c->allocator);
      }
    }
    if (a->additive && a->mode != MODE_TO && r->have && r->numeric) {
      gvec_anim_value_unify(&out, &r->value, vc);
      gvec_anim_value_add(&out, &r->value, 1);
    }
    if (r->numeric) {
      gvec_anim_value_free(&r->value, c->allocator);
    }
    r->value = out;
    r->numeric = true;
    r->have = true;
  }
done:
  gvec_anim_value_free(&tv[0], c->allocator);
  gvec_anim_value_free(&tv[1], c->allocator);
}

/* Write the result of an attribute's sandwich: an override of the attribute, or for `d` an effect. */
static void finish_attribute(actx * c, struct gvec_anim_state * s, effects * fx, override_list * ov, const anim * top, running * r) {
  if (!r->have) {
    return;
  }
  gvec_anim_buf b = {c->allocator, NULL, 0, 0, false};
  if (top->cls == CLS_D) {
    gvec_anim_buf_put(&b, "path(\"", 6);
  }
  if (r->numeric) {
    gvec_anim_value_write(&r->value, &b);
  } else {
    gvec_anim_buf_put(&b, r->text.s, r->text.n);
  }
  if (top->cls == CLS_D) {
    gvec_anim_buf_put(&b, "\")", 2);
  }
  if (b.failed) {
    gvec_anim_buf_free(&b);
    c->error = GVEC_ERR_OOM;
    return;
  }
  const char * text = keep(s, c, b.data, b.length);
  size_t length = b.length;
  gvec_anim_buf_free(&b);
  if (!text) {
    return;
  }
  if (top->cls == CLS_D) {
    gvec_anim_frame * f = gvec_arena_array(&s->arena, 2, sizeof *f, 8);
    const char * name = keep(s, c, "d", 1);
    if (!f || !name) {
      c->error = GVEC_ERR_OOM;
      return;
    }
    f[0] = (gvec_anim_frame){0, text, length, -1};
    f[1] = (gvec_anim_frame){1, text, length, -1};
    gvec_anim_effect ef;
    memset(&ef, 0, sizeof ef);
    ef.attribute = name;
    ef.animation = top->e;
    ef.frames = f;
    ef.frame_count = 2;
    ef.css = true;
    ef.progress = 0;
    push_effect(c, fx, top->target->order, &ef);
    return;
  }
  const char * name = keep(s, c, top->attr, strlen(top->attr));
  if (name) {
    push_override(c, ov, top->target->order, name, text, length);
  }
}

/* ---- motion ---------------------------------------------------------------------------------------------------------- */

static void mul(double out[6], const double a[6], const double b[6]) {
  double r[6];
  r[0] = a[0] * b[0] + a[2] * b[1];
  r[1] = a[1] * b[0] + a[3] * b[1];
  r[2] = a[0] * b[2] + a[2] * b[3];
  r[3] = a[1] * b[2] + a[3] * b[3];
  r[4] = a[0] * b[4] + a[2] * b[5] + a[4];
  r[5] = a[1] * b[4] + a[3] * b[5] + a[5];
  memcpy(out, r, sizeof r);
}

/* The motion of one animateMotion at its sample: translate to the point, then turn (A7). */
static bool motion_matrix(const anim * a, double m[6]) {
  const gvec_anim_sample * smp = &a->sample;
  double p = smp->indefinite ? 0 : smp->progress;
  const gvec_anim_motion * path = &a->path;
  double distance;
  if (a->calc == CALC_PACED) {
    distance = p * path->length;
  } else {
    size_t keys = a->nkp ? a->nkp : path->vertex_count;
    size_t i;
    double l;
    locate(a, keys, p, &i, &l);
    double d0 = a->nkp ? a->kp[i] * path->length : path->vertices[i];
    double d1 = i + 1 < keys ? (a->nkp ? a->kp[i + 1] * path->length : path->vertices[i + 1]) : d0;
    distance = a->calc == CALC_DISCRETE ? d0 : d0 + (d1 - d0) * l;
  }
  double x, y, angle;
  if (!gvec_anim_motion_at(path, distance, &x, &y, &angle)) {
    return false;
  }
  if (a->accumulate && smp->iteration > 0) {
    double ex, ey, ea;
    if (gvec_anim_motion_at(path, path->length, &ex, &ey, &ea)) {
      double sx, sy, sa;
      gvec_anim_motion_at(path, 0, &sx, &sy, &sa);
      x += (ex - sx) * smp->iteration;
      y += (ey - sy) * smp->iteration;
    }
  }
  double turn = a->rotate == ROT_AUTO ? angle : a->rotate == ROT_AUTO_REVERSE ? angle + 180 : a->rotate_angle;
  double rad = turn * (3.14159265358979323846 / 180);
  double cs = gmath_cos(rad), sn = gmath_sin(rad);
  m[0] = cs;
  m[1] = sn;
  m[2] = -sn;
  m[3] = cs;
  m[4] = x;
  m[5] = y;
  return true;
}

/* ---- the sandwich -------------------------------------------------------------------------------------------------- */

static int cls_group(anim_cls k) {
  return k == CLS_MOTION ? 3 : k == CLS_PROPERTY ? 0 : 1;
}

/* SMIL's priority: the later begin is higher; at one begin, a syncbase's dependent is higher than the syncbase; then document order. */
static int priority(const anim * x, const anim * y) {
  if (x->sample.begin != y->sample.begin) {
    return x->sample.begin < y->sample.begin ? -1 : 1;
  }
  if (x->sample.begin_source == y->index) {
    return 1;
  }
  if (y->sample.begin_source == x->index) {
    return -1;
  }
  return x->e->order < y->e->order ? -1 : x->e->order > y->e->order ? 1 : 0;
}

static int by_group(const anim * anims, uint32_t pa, uint32_t pb) {
  const anim * x = &anims[pa];
  const anim * y = &anims[pb];
  if (x->target->order != y->target->order) {
    return x->target->order < y->target->order ? -1 : 1;
  }
  int gx = cls_group(x->cls), gy = cls_group(y->cls);
  if (gx != gy) {
    return gx < gy ? -1 : 1;
  }
  if (gx != 3) {
    int s = strcmp(x->attr, y->attr);
    if (s) {
      return s;
    }
  }
  return priority(x, y);
}

static int by_override(const void * pa, const void * pb) {
  const gvec_svg_override * x = pa;
  const gvec_svg_override * y = pb;
  return x->element < y->element ? -1 : x->element > y->element ? 1 : 0;
}

/* A stable merge sort of the indexes (qsort has no context to read the animations through). */
static bool sort_anims(const anim * anims, uint32_t * order, size_t n, const GVEC_Allocator * allocator) {
  if (n < 2) {
    return true;
  }
  uint32_t * tmp = gvec_alloc(allocator, n * sizeof *tmp);
  if (!tmp) {
    return false;
  }
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n;
      size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        tmp[k++] = by_group(anims, order[j], order[i]) < 0 ? order[j++] : order[i++];
      }
      while (i < mid) {
        tmp[k++] = order[i++];
      }
      while (j < hi) {
        tmp[k++] = order[j++];
      }
    }
    memcpy(order, tmp, n * sizeof *order);
  }
  gvec_free(allocator, tmp);
  return true;
}

/* ---- the document --------------------------------------------------------------------------------------------------- */

static void free_anims(actx * c) {
  for (size_t i = 0; i < c->count; i++) {
    gvec_anim_motion_free(&c->anims[i].path);
  }
  gvec_free(c->allocator, c->anims);
  gvec_free(c->allocator, c->timed_of);
  gvec_arena_destroy(&c->scratch);
}

GVEC_Result gvec_svg_animate(const GVEC_Svg_Document * base, double time_ms, GVEC_Diagnostics * diagnostics, GVEC_Svg_Document ** out) {
  *out = NULL;
  if (!base->has_animation || !base->markup) {
    return GVEC_OK;
  }
  actx c;
  memset(&c, 0, sizeof c);
  c.doc = base;
  c.allocator = base->allocator;
  c.diag = diagnostics;
  c.error = GVEC_OK;
  gvec_arena_init(&c.scratch, c.allocator);
  c.parse.arena = &c.scratch;
  c.parse.allocator = c.allocator;
  c.parse.max_list_items = base->limits.max_list_items;
  c.parse.max_path_segments = base->limits.max_path_segments;
  size_t n = 0;
  for (size_t i = 0; i < base->element_count; i++) {
    n += is_animation_kind(base->elements[i]->kind);
  }
  if (n == 0) {
    gvec_arena_destroy(&c.scratch);
    return GVEC_OK;
  }
  c.timed_of = gvec_alloc(c.allocator, base->element_count * sizeof *c.timed_of);
  c.anims = gvec_alloc(c.allocator, n * sizeof *c.anims);
  gvec_anim_timing * timing = gvec_alloc(c.allocator, n * sizeof *timing);
  gvec_anim_sample * samples = gvec_alloc(c.allocator, n * sizeof *samples);
  uint32_t * order = gvec_alloc(c.allocator, n * sizeof *order);
  struct gvec_anim_state * s = gvec_alloc(c.allocator, sizeof *s);
  if (!c.timed_of || !c.anims || !timing || !samples || !order || !s) {
    gvec_free(c.allocator, timing);
    gvec_free(c.allocator, samples);
    gvec_free(c.allocator, order);
    gvec_free(c.allocator, s);
    free_anims(&c);
    return GVEC_ERR_OOM;
  }
  memset(c.anims, 0, n * sizeof *c.anims);
  memset(s, 0, sizeof *s);
  gvec_arena_init(&s->arena, c.allocator);
  s->time_ms = time_ms;
  for (size_t i = 0; i < base->element_count; i++) {
    c.timed_of[i] = GVEC_ANIM_NONE;
    if (is_animation_kind(base->elements[i]->kind)) {
      c.anims[c.count].e = base->elements[i];
      c.anims[c.count].index = (uint32_t) c.count;
      c.timed_of[i] = (uint32_t) c.count++;
    }
  }
  for (size_t i = 0; i < c.count && c.error == GVEC_OK; i++) {
    read_animation(&c, &c.anims[i]);
    timing[i] = c.anims[i].timing;
  }
  GVEC_Result r = c.error;
  size_t events = 0;
  if (r == GVEC_OK) {
    r = gvec_anim_time_run(timing, c.count, time_ms / 1000, base->limits.max_animation_intervals, c.allocator, samples, &events);
    if (r == GVEC_ERR_LIMIT) {
      gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, GVEC_LIMIT_ANIMATION_INTERVALS, 0, 0, 0, NULL,
          "the timing of the animations passes more interval boundaries than max_animation_intervals; the document is drawn without its animation");
    }
  }
  effects fx;
  memset(&fx, 0, sizeof fx);
  override_list ov;
  memset(&ov, 0, sizeof ov);
  uint8_t * discarded = NULL;
  uint32_t * subtree = NULL;
  GVEC_Svg_Element ** from_base = NULL;
  double * motion = NULL;
  uint8_t * has_motion = NULL;
  if (r == GVEC_OK) {
    size_t k = 0;
    for (size_t i = 0; i < c.count; i++) {
      c.anims[i].sample = samples[i];
      if (c.anims[i].valid && samples[i].applies) {
        order[k++] = (uint32_t) i;
      }
    }
    if (!sort_anims(c.anims, order, k, c.allocator)) {
      r = GVEC_ERR_OOM;
    }
    discarded = gvec_alloc(c.allocator, base->element_count);
    subtree = gvec_alloc(c.allocator, base->element_count * sizeof *subtree);
    from_base = gvec_alloc(c.allocator, base->element_count * sizeof *from_base);
    motion = gvec_alloc(c.allocator, base->element_count * 6 * sizeof *motion);
    has_motion = gvec_alloc(c.allocator, base->element_count);
    if (!discarded || !subtree || !from_base || !motion || !has_motion) {
      r = GVEC_ERR_OOM;
    } else {
      memset(discarded, 0, base->element_count);
      memset(has_motion, 0, base->element_count);
      memset(from_base, 0, base->element_count * sizeof *from_base);
      for (size_t i = 0; i < base->element_count; i++) {
        subtree[i] = base->elements[i]->subtree_size;
      }
    }
    for (size_t g = 0; g < k && r == GVEC_OK && c.error == GVEC_OK;) {
      const anim * a = &c.anims[order[g]];
      size_t h = g + 1;
      while (h < k) {
        const anim * b = &c.anims[order[h]];
        if (b->target != a->target || cls_group(b->cls) != cls_group(a->cls) || (cls_group(a->cls) != 3 && strcmp(b->attr, a->attr) != 0)) {
          break;
        }
        h++;
      }
      if (a->cls == CLS_DISCARD) {
        /* The root is never discarded: the document would have nothing to draw and no root to say so. */
        if (a->target->order != 0) {
          discarded[a->target->order] = 1;
        }
      } else if (a->cls == CLS_PROPERTY) {
        for (size_t j = g; j < h; j++) {
          property_effects(&c, s, &fx, &c.anims[order[j]]);
        }
      } else if (a->cls == CLS_MOTION) {
        double m[6] = {1, 0, 0, 1, 0, 0};
        bool any = false;
        for (size_t j = g; j < h; j++) {
          const anim * x = &c.anims[order[j]];
          double mm[6];
          if (!motion_matrix(x, mm)) {
            continue;
          }
          if (x->additive && any) {
            mul(m, m, mm);
          } else {
            memcpy(m, mm, sizeof m);
          }
          any = true;
        }
        if (any) {
          memcpy(motion + 6 * a->target->order, m, sizeof m);
          has_motion[a->target->order] = 1;
        }
      } else {
        /* An attribute (or `d`): from the value written, up the sandwich. */
        gvec_anim_context vc;
        memset(&vc, 0, sizeof vc);
        vc.allocator = c.allocator;
        vc.bounding_box = a->cls == CLS_ATTRIBUTE && bounding_box_units(a->target, a->attr);
        vc.percent_basis = percent_basis(base, a->attr);
        vc.font_size = 16;
        vc.viewport_width = base->viewport_width;
        vc.viewport_height = base->viewport_height;
        running run;
        memset(&run, 0, sizeof run);
        const GVEC_Svg_Attribute * written = attr_of(a->target, a->attr);
        if (written) {
          GVEC_Svg_Attribute typed = *written;
          if (a->cls == CLS_D || written->cls == GVEC_SVG_ATTR_RAW) {
            type_value(&c, a->target, a->attr, trim(written->raw, written->raw_length), &typed);
          }
          run.have = true;
          run.numeric = gvec_anim_value_of(&typed, &vc, &run.value);
          if (!run.numeric) {
            run.text = trim(written->raw, written->raw_length);
          }
        }
        for (size_t j = g; j < h && c.error == GVEC_OK; j++) {
          apply_attribute(&c, &c.anims[order[j]], &run, &vc);
        }
        finish_attribute(&c, s, &fx, &ov, &c.anims[order[h - 1]], &run);
        if (run.numeric) {
          gvec_anim_value_free(&run.value, c.allocator);
        }
      }
      g = h;
    }
    if (r == GVEC_OK) {
      r = c.error;
    }
  }
  /* The document built again, with the results in it. */
  GVEC_Svg_Document * animated = NULL;
  if (r == GVEC_OK) {
    qsort(ov.v, ov.n, sizeof *ov.v, by_override);
    gvec_svg_rebuild rb;
    rb.overrides = ov.v;
    rb.override_count = ov.n;
    rb.discarded = discarded;
    rb.subtree = subtree;
    rb.from_base = from_base;
    gvec_svg_build_options build;
    memset(&build, 0, sizeof build);
    build.limits = &base->limits;
    build.languages = base->languages;
    build.language_count = base->language_count;
    build.viewport_width = base->viewport_width;
    build.viewport_height = base->viewport_height;
    build.color_scheme = base->color_scheme;
    build.rebuild = &rb;
    const GTEXT_Markup_Node * root = gtext_markup_document_element((GTEXT_Markup_Document *) base->markup);
    r = gvec_svg_build(root, base->markup_elements, &build, c.allocator, NULL, &animated);
    if (r == GVEC_OK && animated) {
      r = gvec_svg_resolve(animated, &base->limits, NULL);
    }
    if (r == GVEC_OK && animated) {
      r = gvec_svg_conditional(animated, base->languages, base->language_count);
    }
  }
  if (r == GVEC_OK && animated) {
    /* The effects and motions, from the elements as written to the elements built. */
    s->element_count = animated->element_count;
    s->effect_start = gvec_alloc(c.allocator, (animated->element_count + 1) * sizeof *s->effect_start);
    s->effects = fx.n ? gvec_alloc(c.allocator, fx.n * sizeof *s->effects) : NULL;
    s->motion = gvec_alloc(c.allocator, (animated->element_count ? animated->element_count : 1) * 6 * sizeof *s->motion);
    s->has_motion = gvec_alloc(c.allocator, animated->element_count ? animated->element_count : 1);
    s->written = gvec_alloc(c.allocator, (animated->element_count ? animated->element_count : 1) * sizeof *s->written);
    if (!s->effect_start || (fx.n && !s->effects) || !s->motion || !s->has_motion || !s->written) {
      r = GVEC_ERR_OOM;
    } else {
      memset((void *) s->written, 0, (animated->element_count ? animated->element_count : 1) * sizeof *s->written);
      for (size_t i = 0; i < base->element_count; i++) {
        if (from_base[i]) {
          s->written[from_base[i]->order] = base->elements[i];
        }
      }
      memset(s->has_motion, 0, animated->element_count ? animated->element_count : 1);
      size_t * count = s->effect_start;
      memset(count, 0, (animated->element_count + 1) * sizeof *count);
      for (size_t i = 0; i < fx.n; i++) {
        const GVEC_Svg_Element * e = from_base[fx.target[i]];
        if (e) {
          count[e->order + 1]++;
        }
      }
      for (size_t i = 0; i < animated->element_count; i++) {
        count[i + 1] += count[i];
      }
      size_t * fill = gvec_alloc(c.allocator, (animated->element_count + 1) * sizeof *fill);
      if (!fill) {
        r = GVEC_ERR_OOM;
      } else {
        memcpy(fill, count, (animated->element_count + 1) * sizeof *fill);
        for (size_t i = 0; i < fx.n; i++) {
          const GVEC_Svg_Element * e = from_base[fx.target[i]];
          if (e) {
            s->effects[fill[e->order]++] = fx.v[i];
          }
        }
        s->effect_count = fx.n;
        gvec_free(c.allocator, fill);
      }
      for (size_t i = 0; i < base->element_count; i++) {
        if (has_motion[i] && from_base[i]) {
          memcpy(s->motion + 6 * from_base[i]->order, motion + 6 * i, 6 * sizeof *motion);
          s->has_motion[from_base[i]->order] = 1;
        }
      }
    }
  }
  gvec_free(c.allocator, fx.v);
  gvec_free(c.allocator, fx.target);
  gvec_free(c.allocator, ov.v);
  gvec_free(c.allocator, discarded);
  gvec_free(c.allocator, subtree);
  gvec_free(c.allocator, from_base);
  gvec_free(c.allocator, motion);
  gvec_free(c.allocator, has_motion);
  gvec_free(c.allocator, timing);
  gvec_free(c.allocator, samples);
  gvec_free(c.allocator, order);
  /* The overrides' strings live in the state's arena; the scratch arena holds only what was read. */
  free_anims(&c);
  if (r != GVEC_OK) {
    gvec_anim_state_free(s, base->allocator);
    gvec_svg_document_free(animated);
    return r;
  }
  animated->anim = s;
  animated->base = base;
  *out = animated;
  return GVEC_OK;
}
