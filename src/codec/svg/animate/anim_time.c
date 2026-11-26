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
 * The timing model of SMIL 3.0 (design.md A3): clock values, the active duration, and the intervals of the timed elements of
 * the root time container, run forward from the document's begin to a time.
 *
 * Each element has two lists of instance times, its begin and its end. An offset puts one time in a list for good; a syncbase
 * value puts one there for each interval of its syncbase, and moves it when that interval moves ("Propagating changes to times").
 * An element has at most one current interval: a pending one is worked out again whenever its lists change (getFirstInterval,
 * getNextInterval); an active one keeps its begin and has its end worked out again (and cut short by a later begin when
 * `restart="always"`). The model passes the boundaries (begins and ends) in time order up to the time sampled, ends before begins
 * at one time, and counts them against the caller's bound, which a ring of zero-length intervals would otherwise never leave.
 */

#include <ghoti.io/vector/macros.h>

#include <math.h>
#include <string.h>

#include <ghoti.io/math/basic.h>

#include "../../../core/core_internal.h"
#include "anim_internal.h"

static bool is_nan(double x) {
  return x != x;
}

static bool is_wsp(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ---- clock values --------------------------------------------------------------------------------------------------- */

/* Digits as a number; *n set to how many were read. */
static double digits(const char * s, size_t length, size_t * n) {
  double v = 0;
  size_t i = 0;
  while (i < length && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i] - '0');
    i++;
  }
  *n = i;
  return v;
}

/* "." Fraction, if present: its value, *n the characters read (0 when there is none). A "." with no digit is not one. */
static bool fraction(const char * s, size_t length, double * value, size_t * n) {
  *value = 0;
  *n = 0;
  if (length == 0 || s[0] != '.') {
    return true;
  }
  double scale = 0.1, v = 0;
  size_t i = 1;
  while (i < length && s[i] >= '0' && s[i] <= '9') {
    v += (s[i] - '0') * scale;
    scale /= 10;
    i++;
  }
  if (i == 1) {
    return false;
  }
  *value = v;
  *n = i;
  return true;
}

bool gvec_anim_clock(const char * text, size_t length, double * seconds) {
  while (length && is_wsp(text[0])) {
    text++;
    length--;
  }
  while (length && is_wsp(text[length - 1])) {
    length--;
  }
  size_t n = 0;
  double first = digits(text, length, &n);
  if (n == 0) {
    return false;
  }
  if (n < length && text[n] == ':') {
    /* Full-clock-value (Hours ":" Minutes ":" Seconds) or Partial-clock-value (Minutes ":" Seconds); Minutes and Seconds are two
     * digits, 00 to 59. */
    double parts[3];
    size_t count = 0;
    parts[count++] = first;
    size_t i = n;
    size_t first_digits = n;
    while (i < length && text[i] == ':' && count < 3) {
      size_t m = 0;
      double v = digits(text + i + 1, length - i - 1, &m);
      if (m != 2 || v > 59) {
        return false;
      }
      parts[count++] = v;
      i += 1 + m;
    }
    if (count == 2 && (first_digits != 2 || first > 59)) {
      return false;
    }
    double frac = 0;
    size_t f = 0;
    if (!fraction(text + i, length - i, &frac, &f) || i + f != length) {
      return false;
    }
    double s = count == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : parts[0] * 60 + parts[1];
    *seconds = s + frac;
    return true;
  }
  /* Timecount-value: Timecount ("." Fraction)? Metric? */
  double frac = 0;
  size_t f = 0;
  if (!fraction(text + n, length - n, &frac, &f)) {
    return false;
  }
  double v = first + frac;
  size_t i = n + f;
  const char * metric = text + i;
  size_t m = length - i;
  double scale;
  if (m == 0 || (m == 1 && metric[0] == 's')) {
    scale = 1;
  } else if (m == 2 && metric[0] == 'm' && metric[1] == 's') {
    scale = 0.001;
  } else if (m == 3 && memcmp(metric, "min", 3) == 0) {
    scale = 60;
  } else if (m == 1 && metric[0] == 'h') {
    scale = 3600;
  } else {
    return false;
  }
  *seconds = v * scale;
  return true;
}

/* ---- the active duration ---------------------------------------------------------------------------------------------- */

/* SMIL 3.0, "Computing the active duration": the intermediate active duration. */
static double intermediate(const gvec_anim_timing * t) {
  double d = t->dur;
  if (d == 0) {
    return 0;
  }
  bool rc = !is_nan(t->repeat_count), rd = !is_nan(t->repeat_dur);
  if (!rc && !rd) {
    return d;
  }
  if (rc && !rd) {
    return d * t->repeat_count;
  }
  if (!rc) {
    return t->repeat_dur;
  }
  double a = d * t->repeat_count;
  return a < t->repeat_dur ? a : t->repeat_dur;
}

double gvec_anim_active_duration(const gvec_anim_timing * t, double begin, double end) {
  double iad = intermediate(t);
  double pad;
  if (end < INFINITY) {
    bool none = !t->dur_given && is_nan(t->repeat_count) && is_nan(t->repeat_dur);
    double span = end - begin;
    pad = none ? span : (iad < span ? iad : span);
  } else {
    pad = iad;
  }
  double lo = t->min, hi = t->max;
  if (lo > hi) {
    lo = 0;
    hi = INFINITY;
  }
  if (pad < lo) {
    pad = lo;
  }
  if (pad > hi) {
    pad = hi;
  }
  return pad;
}

/* ---- the model -------------------------------------------------------------------------------------------------------- */

typedef struct inst {
  double t;
  uint32_t src;    ///< The element whose interval made it, or GVEC_ANIM_NONE for an offset.
  uint32_t serial; ///< That interval's serial.
  uint32_t spec;   ///< The value of the list it is for.
} inst;

typedef struct ilist {
  inst * v;
  size_t n, cap;
} ilist;

typedef struct ival {
  double b, e;
  uint32_t serial;
  uint32_t src; ///< Where its begin came from.
} ival;

/* A syncbase value of another element that reads this one. */
typedef struct dep {
  uint32_t element;
  uint32_t spec;
  bool end_list;
} dep;

typedef struct tl {
  ilist begin, end;
  ival cur;
  bool has_cur, begun;
  ival last;
  bool has_last;
  size_t intervals;
  uint32_t next_serial;
  dep * deps;
  size_t dep_count, dep_cap;
  bool queued;
  uint32_t stamp;      ///< The version of its entry in the heap of boundaries.
  double scheduled;    ///< The boundary that entry is for (INFINITY for none).
} tl;

/* A boundary in the heap: valid while its stamp is the element's. */
typedef struct due {
  double t;
  uint32_t element;
  uint32_t stamp;
} due;

typedef struct model {
  const gvec_anim_timing * timing;
  tl * el;
  size_t count;
  uint32_t * queue;
  size_t queue_n;
  const GVEC_Allocator * allocator;
  size_t work, max_work;
  GVEC_Result error;
  due * heap;          ///< The next boundary of each element, earliest first (stale entries are skipped as they surface).
  size_t heap_n, heap_cap;
} model;

/* A new capacity for `need` items, or 0 when it cannot be had. */
static size_t capacity_for(size_t cap, size_t need) {
  size_t c = cap ? cap : 8;
  while (c < need) {
    if (c > SIZE_MAX / 2) {
      return 0;
    }
    c *= 2;
  }
  return c;
}

static bool grow_list(model * m, ilist * l, size_t need) {
  if (need <= l->cap) {
    return true;
  }
  size_t c = capacity_for(l->cap, need);
  inst * fresh = c && c <= SIZE_MAX / sizeof *fresh ? gvec_realloc(m->allocator, l->v, c * sizeof *fresh) : NULL;
  if (!fresh) {
    m->error = GVEC_ERR_OOM;
    return false;
  }
  l->v = fresh;
  l->cap = c;
  return true;
}

static bool grow_deps(model * m, tl * s, size_t need) {
  if (need <= s->dep_cap) {
    return true;
  }
  size_t c = capacity_for(s->dep_cap, need);
  dep * fresh = c && c <= SIZE_MAX / sizeof *fresh ? gvec_realloc(m->allocator, s->deps, c * sizeof *fresh) : NULL;
  if (!fresh) {
    m->error = GVEC_ERR_OOM;
    return false;
  }
  s->deps = fresh;
  s->dep_cap = c;
  return true;
}

/* Insert keeping the list sorted by time (stable: an equal time goes after). */
static bool ilist_add(model * m, ilist * l, inst x) {
  if (!grow_list(m, l, l->n + 1)) {
    return false;
  }
  size_t i = l->n;
  while (i > 0 && l->v[i - 1].t > x.t) {
    l->v[i] = l->v[i - 1];
    i--;
  }
  l->v[i] = x;
  l->n++;
  return true;
}

static void ilist_resort(ilist * l) {
  for (size_t i = 1; i < l->n; i++) {
    inst x = l->v[i];
    size_t j = i;
    while (j > 0 && l->v[j - 1].t > x.t) {
      l->v[j] = l->v[j - 1];
      j--;
    }
    l->v[j] = x;
  }
}

/* The first instance at or after `t` (strictly after with `after`): the lists are sorted, and a document can hold many. */
static size_t lower_bound(const ilist * l, double t, bool after) {
  size_t lo = 0, hi = l->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (after ? l->v[mid].t <= t : l->v[mid].t < t) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* Drop the instances before `t`: no interval of the element can use them again. Without this a long run keeps every time it was given and
 * each search walks them all. A syncbase's time that moves later is added again by notify(). */
static void prune(ilist * l, double t) {
  size_t k = lower_bound(l, t, false);
  if (k > 0) {
    memmove(l->v, l->v + k, (l->n - k) * sizeof *l->v);
    l->n -= k;
  }
}

static void heap_push(model * m, due d) {
  if (m->heap_n == m->heap_cap) {
    size_t c = capacity_for(m->heap_cap, m->heap_n + 1);
    due * fresh = c && c <= SIZE_MAX / sizeof *fresh ? gvec_realloc(m->allocator, m->heap, c * sizeof *fresh) : NULL;
    if (!fresh) {
      m->error = GVEC_ERR_OOM;
      return;
    }
    m->heap = fresh;
    m->heap_cap = c;
  }
  size_t i = m->heap_n++;
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (m->heap[parent].t <= d.t) {
      break;
    }
    m->heap[i] = m->heap[parent];
    i = parent;
  }
  m->heap[i] = d;
}

static due heap_pop(model * m) {
  due top = m->heap[0];
  due last = m->heap[--m->heap_n];
  size_t i = 0;
  for (;;) {
    size_t c = 2 * i + 1;
    if (c >= m->heap_n) {
      break;
    }
    if (c + 1 < m->heap_n && m->heap[c + 1].t < m->heap[c].t) {
      c++;
    }
    if (m->heap[c].t >= last.t) {
      break;
    }
    m->heap[i] = m->heap[c];
    i = c;
  }
  if (m->heap_n > 0) {
    m->heap[i] = last;
  }
  return top;
}

static double milestone(const tl * s);

/* Put the element's next boundary in the heap, if it changed. */
static void schedule(model * m, uint32_t e) {
  tl * s = &m->el[e];
  double ms = milestone(s);
  if (ms == s->scheduled) {
    return;
  }
  s->stamp++;
  s->scheduled = ms;
  if (ms < INFINITY) {
    due d = {ms, e, s->stamp};
    heap_push(m, d);
  }
}

static void enqueue(model * m, uint32_t e) {
  if (!m->el[e].queued) {
    m->el[e].queued = true;
    m->queue[m->queue_n++] = e;
  }
}

/* The interval `serial` of `src` is new (add), moved (update) or gone (remove): the times it gave its dependents follow it. */
typedef enum { NOTIFY_ADD, NOTIFY_UPDATE, NOTIFY_REMOVE } notify_kind;

static void notify(model * m, uint32_t src, const ival * iv, notify_kind kind) {
  tl * s = &m->el[src];
  for (size_t k = 0; k < s->dep_count && m->error == GVEC_OK; k++) {
    const dep * d = &s->deps[k];
    const gvec_anim_timing * dt = &m->timing[d->element];
    const gvec_anim_spec * spec = d->end_list ? &dt->end[d->spec] : &dt->begin[d->spec];
    ilist * l = d->end_list ? &m->el[d->element].end : &m->el[d->element].begin;
    double t = (spec->sync == GVEC_ANIM_SYNC_BEGIN ? iv->b : iv->e) + spec->offset;
    if (kind == NOTIFY_ADD) {
      if (spec->sync == GVEC_ANIM_SYNC_END && iv->e == INFINITY) {
        /* An unresolved end gives no time yet; it is added when the end resolves (an update finds none and adds it). */
        continue;
      }
      inst x = {t, src, iv->serial, d->spec};
      ilist_add(m, l, x);
    } else {
      bool found = false;
      for (size_t i = 0; i < l->n; i++) {
        if (l->v[i].src == src && l->v[i].serial == iv->serial && l->v[i].spec == d->spec) {
          found = true;
          if (kind == NOTIFY_REMOVE || t == INFINITY) {
            memmove(&l->v[i], &l->v[i + 1], (l->n - i - 1) * sizeof *l->v);
            l->n--;
          } else {
            l->v[i].t = t;
            ilist_resort(l);
          }
          break;
        }
      }
      if (!found && kind == NOTIFY_UPDATE && t < INFINITY) {
        inst x = {t, src, iv->serial, d->spec};
        ilist_add(m, l, x);
      }
    }
    enqueue(m, d->element);
  }
}

/* The first end instance >= begin, with the zero-duration rule: when it equals `skip_equal`, the next one > it. INFINITY for an
 * unresolved end; NAN when there is none and the interval fails. */
static double end_for(const model * m, uint32_t e, double begin, double skip_equal) {
  const ilist * l = &m->el[e].end;
  const gvec_anim_timing * t = &m->timing[e];
  for (size_t i = lower_bound(l, begin, false); i < l->n; i++) {
    double v = l->v[i].t;
    if (v == begin && v == skip_equal) {
      continue;
    }
    return v;
  }
  if (t->end_events || l->n == 0) {
    return INFINITY;
  }
  return NAN;
}

/* getFirstInterval (first) or getNextInterval: the interval the lists now give, or false. */
static bool find_interval(const model * m, uint32_t e, ival * out) {
  const tl * s = &m->el[e];
  const gvec_anim_timing * t = &m->timing[e];
  if (s->intervals > 0 && t->restart == GVEC_ANIM_RESTART_NEVER) {
    return false;
  }
  const ilist * bl = &s->begin;
  if (!s->has_last) {
    double begin_after = -INFINITY;
    double used_end = NAN;
    for (;;) {
      size_t i = lower_bound(bl, begin_after, false);
      /* After a zero-length interval that failed to reach 0, the next begin must be later (SMIL's "> beginAfter"). */
      if (used_end == begin_after) {
        while (i < bl->n && bl->v[i].t == begin_after) {
          i++;
        }
      }
      if (i == bl->n) {
        return false;
      }
      double b = bl->v[i].t;
      double end = INFINITY;
      if (t->has_end) {
        end = end_for(m, e, b, used_end);
        if (is_nan(end)) {
          return false;
        }
      }
      double ad = gvec_anim_active_duration(t, b, end);
      double ae = b + ad;
      if (ae > 0 || (b == 0 && ae == 0)) {
        out->b = b;
        out->e = ae;
        out->src = bl->v[i].src;
        return true;
      }
      begin_after = ae;
      used_end = ae;
    }
  }
  double begin_after = s->last.e;
  bool zero = s->last.b == s->last.e;
  size_t i = lower_bound(bl, begin_after, zero);
  if (i == bl->n) {
    return false;
  }
  double b = bl->v[i].t;
  double end = INFINITY;
  if (t->has_end) {
    end = end_for(m, e, b, s->last.e);
    if (is_nan(end)) {
      return false;
    }
  }
  out->b = b;
  out->e = b + gvec_anim_active_duration(t, b, end);
  out->src = bl->v[i].src;
  return true;
}

/* The end of the active interval as the lists now give it: the end value, and an earlier restart. */
static double active_end(const model * m, uint32_t e) {
  const tl * s = &m->el[e];
  const gvec_anim_timing * t = &m->timing[e];
  double end = INFINITY;
  if (t->has_end) {
    end = end_for(m, e, s->cur.b, s->has_last ? s->last.e : NAN);
    if (is_nan(end)) {
      /* No end at or after the begin: the interval keeps the end it had. */
      return s->cur.e;
    }
  }
  double ae = s->cur.b + gvec_anim_active_duration(t, s->cur.b, end);
  if (t->restart == GVEC_ANIM_RESTART_ALWAYS) {
    const ilist * bl = &s->begin;
    size_t i = lower_bound(bl, s->cur.b, true);
    if (i < bl->n && bl->v[i].t > s->cur.b && bl->v[i].t < ae) {
      ae = bl->v[i].t;
    }
  }
  return ae;
}

/* Work an element's current interval out again after its lists changed. */
static void recompute(model * m, uint32_t e) {
  tl * s = &m->el[e];
  m->work++;
  if (s->has_cur && s->begun) {
    double ae = active_end(m, e);
    if (ae != s->cur.e) {
      s->cur.e = ae;
      notify(m, e, &s->cur, NOTIFY_UPDATE);
    }
    return;
  }
  ival fresh;
  bool found = find_interval(m, e, &fresh);
  if (s->has_cur) {
    if (found && fresh.b == s->cur.b && fresh.e == s->cur.e) {
      return;
    }
    if (found) {
      fresh.serial = s->cur.serial;
      bool moved = fresh.b != s->cur.b || fresh.e != s->cur.e;
      s->cur = fresh;
      if (moved) {
        notify(m, e, &s->cur, NOTIFY_UPDATE);
      }
      return;
    }
    ival gone = s->cur;
    s->has_cur = false;
    notify(m, e, &gone, NOTIFY_REMOVE);
    return;
  }
  if (found) {
    fresh.serial = s->next_serial++;
    s->cur = fresh;
    s->has_cur = true;
    s->begun = false;
    notify(m, e, &s->cur, NOTIFY_ADD);
  }
}

static void drain(model * m) {
  while (m->queue_n > 0 && m->error == GVEC_OK) {
    if (m->work > m->max_work) {
      m->error = GVEC_ERR_LIMIT;
      return;
    }
    /* Last in, first out: the order does not change the fixed point, only how fast it is reached. */
    uint32_t e = m->queue[--m->queue_n];
    m->el[e].queued = false;
    recompute(m, e);
    schedule(m, e);
  }
}

static double milestone(const tl * s) {
  if (!s->has_cur) {
    return INFINITY;
  }
  return s->begun ? s->cur.e : s->cur.b;
}

static void sample_one(const model * m, uint32_t e, double time, gvec_anim_sample * out) {
  const tl * s = &m->el[e];
  const gvec_anim_timing * t = &m->timing[e];
  memset(out, 0, sizeof *out);
  out->begin_source = GVEC_ANIM_NONE;
  const ival * iv = NULL;
  double at = 0;
  if (s->has_cur && s->begun && time >= s->cur.b && time < s->cur.e) {
    iv = &s->cur;
    at = time - s->cur.b;
  } else if (s->has_last && t->freeze) {
    iv = &s->last;
    at = s->last.e - s->last.b;
    out->frozen = true;
  }
  if (!iv) {
    return;
  }
  double d = t->dur;
  double iad = intermediate(t);
  if (at >= iad && !out->frozen) {
    /* A `min` holds the element active past the end of its repeats: it shows the value it ends on, if it freezes. */
    if (!t->freeze) {
      return;
    }
    at = iad;
    out->frozen = true;
  } else if (out->frozen && at > iad) {
    at = iad;
  }
  out->applies = true;
  out->begin = iv->b;
  out->begin_source = iv->src;
  if (!(d < INFINITY)) {
    out->indefinite = true;
    return;
  }
  if (d <= 0) {
    out->progress = 1;
    return;
  }
  double n = gmath_floor(at / d);
  double rest = at - n * d;
  if (out->frozen && rest == 0 && at > 0) {
    /* The end of the last simple duration (SMIL: a frozen value on a boundary is the end of the iteration before it). */
    out->iteration = n - 1;
    out->progress = 1;
    return;
  }
  out->iteration = n;
  out->progress = rest / d;
  if (out->progress > 1) {
    out->progress = 1;
  }
}

GVEC_Result gvec_anim_time_run(const gvec_anim_timing * timing, size_t count, double time, size_t max_events,
    const GVEC_Allocator * allocator, gvec_anim_sample * out, size_t * events) {
  if (events) {
    *events = 0;
  }
  if (count == 0) {
    return GVEC_OK;
  }
  model m;
  memset(&m, 0, sizeof m);
  m.timing = timing;
  m.count = count;
  m.allocator = allocator;
  m.max_work = max_events * 4 + count * 4;
  m.error = GVEC_OK;
  m.el = gvec_alloc(allocator, count * sizeof *m.el);
  m.queue = gvec_alloc(allocator, count * sizeof *m.queue);
  if (!m.el || !m.queue) {
    gvec_free(allocator, m.el);
    gvec_free(allocator, m.queue);
    return GVEC_ERR_OOM;
  }
  memset(m.el, 0, count * sizeof *m.el);
  /* The dependents of each syncbase, and the offsets, which never move. */
  for (uint32_t e = 0; e < count && m.error == GVEC_OK; e++) {
    const gvec_anim_timing * t = &timing[e];
    for (int which = 0; which < 2 && m.error == GVEC_OK; which++) {
      const gvec_anim_spec * list = which ? t->end : t->begin;
      size_t n = which ? t->end_count : t->begin_count;
      for (uint32_t k = 0; k < n && m.error == GVEC_OK; k++) {
        if (list[k].sync == GVEC_ANIM_SYNC_OFFSET) {
          inst x = {list[k].offset, GVEC_ANIM_NONE, 0, k};
          ilist_add(&m, which ? &m.el[e].end : &m.el[e].begin, x);
        } else if (list[k].source < count) {
          tl * s = &m.el[list[k].source];
          if (grow_deps(&m, s, s->dep_count + 1)) {
            dep d = {e, k, which != 0};
            s->deps[s->dep_count++] = d;
          }
        }
      }
    }
    enqueue(&m, e);
  }
  for (uint32_t e = 0; e < count; e++) {
    m.el[e].scheduled = INFINITY;
  }
  drain(&m);
  size_t passed = 0;
  uint32_t * batch = gvec_alloc(allocator, count * sizeof *batch);
  if (!batch) {
    m.error = GVEC_ERR_OOM;
  }
  while (m.error == GVEC_OK) {
    /* The earliest boundary that is still due. */
    while (m.heap_n > 0 && (m.heap[0].stamp != m.el[m.heap[0].element].stamp)) {
      heap_pop(&m);
    }
    if (m.heap_n == 0 || !(m.heap[0].t <= time)) {
      break;
    }
    double next = m.heap[0].t;
    size_t nb = 0;
    while (m.heap_n > 0 && m.heap[0].t == next) {
      due d = heap_pop(&m);
      if (d.stamp == m.el[d.element].stamp) {
        m.el[d.element].stamp++; /* taken: its entry is consumed */
        m.el[d.element].scheduled = INFINITY;
        batch[nb++] = d.element;
      }
    }
    /* Ends first, then begins, at one time. */
    for (int phase = 0; phase < 2 && m.error == GVEC_OK; phase++) {
      for (size_t k = 0; k < nb && m.error == GVEC_OK; k++) {
        uint32_t e = batch[k];
        tl * s = &m.el[e];
        if (!s->has_cur || milestone(s) != next) {
          continue;
        }
        if (phase == 0 && s->begun) {
          if (++passed > max_events) {
            m.error = GVEC_ERR_LIMIT;
            break;
          }
          s->last = s->cur;
          s->has_last = true;
          s->has_cur = false;
          s->begun = false;
          s->intervals++;
          prune(&s->begin, s->last.e);
          prune(&s->end, s->last.e);
          enqueue(&m, e);
        } else if (phase == 1 && !s->begun) {
          if (++passed > max_events) {
            m.error = GVEC_ERR_LIMIT;
            break;
          }
          s->begun = true;
          /* Its end may already be due (a restart, or an end value that arrived while it was pending). */
          enqueue(&m, e);
        }
      }
      drain(&m);
    }
    /* What the batch did not take (a pending interval whose begin is now, made by an end at this time) is in the heap again. */
    for (size_t k = 0; k < nb; k++) {
      schedule(&m, batch[k]);
    }
  }
  gvec_free(allocator, batch);
  GVEC_Result r = m.error;
  if (r == GVEC_OK) {
    for (uint32_t e = 0; e < count; e++) {
      sample_one(&m, e, time, &out[e]);
    }
    if (events) {
      *events = passed;
    }
  }
  for (size_t e = 0; e < count; e++) {
    gvec_free(allocator, m.el[e].begin.v);
    gvec_free(allocator, m.el[e].end.v);
    gvec_free(allocator, m.el[e].deps);
  }
  gvec_free(allocator, m.el);
  gvec_free(allocator, m.queue);
  gvec_free(allocator, m.heap);
  return r;
}
