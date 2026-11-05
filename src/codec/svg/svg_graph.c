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
 * References, cycles and expansion (svg_doc.h).
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "../../core/core_internal.h"
#include "svg_doc.h"

static uint64_t sat_add(uint64_t a, uint64_t b) {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static const GVEC_Svg_Element * find_id(const GVEC_Svg_Document * doc, const char * id) {
  size_t lo = 0, hi = doc->id_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = strcmp(doc->ids[mid].id, id);
    if (c == 0) {
      return doc->ids[mid].element;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return NULL;
}

const GVEC_Svg_Element * gvec_svg_find_by_id(const GVEC_Svg_Document * document, const char * id) {
  return document && id ? find_id(document, id) : NULL;
}

static bool reports_missing(GVEC_Svg_Kind kind) {
  return kind == GVEC_SVG_KIND_USE || kind == GVEC_SVG_KIND_LINEAR_GRADIENT || kind == GVEC_SVG_KIND_RADIAL_GRADIENT ||
      kind == GVEC_SVG_KIND_PATTERN || kind == GVEC_SVG_KIND_TEXT_PATH || kind == GVEC_SVG_KIND_MPATH ||
      kind == GVEC_SVG_KIND_FE_IMAGE;
}

static bool is_paint_server_chain(GVEC_Svg_Kind kind) {
  return kind == GVEC_SVG_KIND_LINEAR_GRADIENT || kind == GVEC_SVG_KIND_RADIAL_GRADIENT || kind == GVEC_SVG_KIND_PATTERN;
}

static void diag_cycle(GVEC_Diagnostics * diag, GVEC_Svg_Element * e) {
  gvec_diag_add(diag, GVEC_DIAG_WARNING, GVEC_DIAG_REFERENCE_CYCLE, 0, e->node.line, e->node.column, e->node.offset, e->name,
      "this element is on a reference cycle; its reference is ignored");
}

static void mark_cycle(GVEC_Diagnostics * diag, GVEC_Svg_Element * e) {
  if (!(e->flags & GVEC_SVG_ELEMENT_ON_CYCLE)) {
    e->flags |= GVEC_SVG_ELEMENT_ON_CYCLE;
    diag_cycle(diag, e);
  }
  if (e->href_target) {
    e->cut_target = e->href_target;
  }
  e->href_target = NULL;
}

/* Paint servers: each has at most one outgoing href, so a walk with colours finds every cycle. */
static GVEC_Result resolve_chains(GVEC_Svg_Document * doc, GVEC_Diagnostics * diag) {
  size_t n = doc->element_count;
  uint8_t * state = gvec_arena_array(&doc->arena, n ? n : 1, 1, 1);
  GVEC_Svg_Element ** path = gvec_arena_array(&doc->arena, n ? n : 1, sizeof(GVEC_Svg_Element *), 8);
  if (!state || !path) {
    return GVEC_ERR_OOM;
  }
  for (size_t i = 0; i < n; i++) {
    GVEC_Svg_Element * s = doc->elements[i];
    if (!is_paint_server_chain(s->kind) || state[s->order]) {
      continue;
    }
    size_t depth = 0;
    GVEC_Svg_Element * cur = s;
    while (cur && state[cur->order] == 0) {
      state[cur->order] = 1;
      path[depth++] = cur;
      const GVEC_Svg_Element * next = cur->href_target;
      cur = next && is_paint_server_chain(next->kind) ? (GVEC_Svg_Element *) next : NULL;
    }
    if (cur && state[cur->order] == 1) {
      size_t start = 0;
      while (path[start] != cur) {
        start++;
      }
      for (size_t k = start; k < depth; k++) {
        mark_cycle(diag, path[k]);
      }
    }
    for (size_t k = 0; k < depth; k++) {
      state[path[k]->order] = 2;
    }
  }
  return GVEC_OK;
}

/* A segment tree over the uses, either a minimum (the depth-first index of the on-stack uses) or a saturating sum
 * (the expansions finished so far), so that "every use in a range" is O(log n) however large the range. */
typedef struct segtree {
  size_t leaves;
  uint64_t * node;
  bool minimum;
} segtree;

static bool segtree_init(segtree * t, gvec_arena * arena, size_t n, bool minimum) {
  t->leaves = 1;
  while (t->leaves < n) {
    t->leaves <<= 1;
  }
  t->minimum = minimum;
  t->node = gvec_arena_array(arena, 2 * t->leaves, sizeof(uint64_t), 8);
  if (!t->node) {
    return false;
  }
  if (minimum) {
    for (size_t i = 0; i < 2 * t->leaves; i++) {
      t->node[i] = UINT64_MAX;
    }
  }
  return true;
}

static uint64_t combine(const segtree * t, uint64_t a, uint64_t b) {
  return t->minimum ? (a < b ? a : b) : sat_add(a, b);
}

static void segtree_set(segtree * t, size_t i, uint64_t value) {
  size_t k = t->leaves + i;
  t->node[k] = value;
  for (k >>= 1; k >= 1; k >>= 1) {
    t->node[k] = combine(t, t->node[2 * k], t->node[2 * k + 1]);
  }
}

static uint64_t segtree_query(const segtree * t, size_t lo, size_t hi) {
  uint64_t acc = t->minimum ? UINT64_MAX : 0;
  for (size_t l = lo + t->leaves, r = hi + t->leaves; l < r; l >>= 1, r >>= 1) {
    if (l & 1) {
      acc = combine(t, acc, t->node[l++]);
    }
    if (r & 1) {
      acc = combine(t, acc, t->node[--r]);
    }
  }
  return acc;
}

/* The index of the first use whose order is at least `order`. */
static size_t lower_bound(GVEC_Svg_Element ** uses, size_t count, uint32_t order) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (uses[mid]->order < order) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

typedef struct frame {
  size_t use;
  size_t cursor;  /* the next index of the range to look at for an unvisited use */
} frame;

/* The next unvisited use at or after i (path-compressed), or `count` when there is none. */
static size_t next_unvisited(size_t * skip, size_t i) {
  size_t root = i;
  while (skip[root] != root) {
    root = skip[root];
  }
  while (skip[i] != root) {
    size_t up = skip[i];
    skip[i] = root;
    i = up;
  }
  return root;
}

/*
 * The uses form a graph in which a use points at every use inside its referent's subtree (a contiguous range of
 * the uses, in document order). The members of a cycle are the uses in a strongly connected component of more than
 * one use, and a use that is inside its own referent. Tarjan's algorithm finds them in O(n log n) however large
 * the ranges are (the discovery skips visited uses, and a range minimum over the uses on the stack finds back
 * edges), and emits a component only after everything its uses instantiate, which is the order in which the
 * expansion counts can be summed (a range sum over those finished).
 */
static GVEC_Result resolve_uses(GVEC_Svg_Document * doc, const GVEC_Limits * limits, GVEC_Diagnostics * diag) {
  size_t n = doc->element_count, count = 0;
  for (size_t i = 0; i < n; i++) {
    if (doc->elements[i]->kind == GVEC_SVG_KIND_USE) {
      count++;
    }
  }
  if (count == 0) {
    return GVEC_OK;
  }
  GVEC_Svg_Element ** uses = gvec_arena_array(&doc->arena, count, sizeof(GVEC_Svg_Element *), 8);
  size_t * lo = gvec_arena_array(&doc->arena, count, sizeof(size_t), 8);
  size_t * hi = gvec_arena_array(&doc->arena, count, sizeof(size_t), 8);
  size_t * skip = gvec_arena_array(&doc->arena, count + 1, sizeof(size_t), 8);
  uint64_t * index = gvec_arena_array(&doc->arena, count, sizeof(uint64_t), 8);
  uint64_t * low = gvec_arena_array(&doc->arena, count, sizeof(uint64_t), 8);
  uint8_t * state = gvec_arena_array(&doc->arena, count, 1, 1);   /* 0 unvisited, 1 on the stack, 2 done */
  uint8_t * cyclic = gvec_arena_array(&doc->arena, count, 1, 1);
  frame * call = gvec_arena_array(&doc->arena, count, sizeof(frame), 8);
  size_t * scc = gvec_arena_array(&doc->arena, count, sizeof(size_t), 8);
  uint32_t * cover = gvec_arena_array(&doc->arena, n + 1, sizeof(uint32_t), 4);
  segtree stack_min, expansion_sum;
  if (!uses || !lo || !hi || !skip || !index || !low || !state || !cyclic || !call || !scc || !cover ||
      !segtree_init(&stack_min, &doc->arena, count, true) || !segtree_init(&expansion_sum, &doc->arena, count, false)) {
    return GVEC_ERR_OOM;
  }
  size_t at = 0;
  for (size_t i = 0; i < n; i++) {
    if (doc->elements[i]->kind == GVEC_SVG_KIND_USE) {
      uses[at++] = doc->elements[i];
    }
  }
  for (size_t i = 0; i < count; i++) {
    const GVEC_Svg_Element * t = uses[i]->href_target;
    skip[i] = i;
    lo[i] = hi[i] = 0;
    if (t) {
      lo[i] = lower_bound(uses, count, t->order);
      hi[i] = lower_bound(uses, count, t->order + t->subtree_size);
      if (uses[i]->order >= t->order && uses[i]->order < t->order + t->subtree_size) {
        cyclic[i] = 1; /* a use of itself, or of an ancestor */
      }
    }
  }
  skip[count] = count;
  uint64_t counter = 0;
  size_t scc_top = 0;
  for (size_t start = 0; start < count; start++) {
    if (state[start]) {
      continue;
    }
    size_t depth = 0;
#define VISIT(idx)                                                                                    \
  do {                                                                                                \
    size_t v_ = (idx);                                                                                \
    state[v_] = 1;                                                                                    \
    skip[v_] = v_ + 1;                                                                                \
    index[v_] = low[v_] = counter++;                                                                  \
    segtree_set(&stack_min, v_, index[v_]);                                                           \
    scc[scc_top++] = v_;                                                                              \
    call[depth].use = v_;                                                                             \
    call[depth].cursor = lo[v_];                                                                      \
    depth++;                                                                                          \
  } while (0)
    VISIT(start);
    while (depth > 0) {
      frame * f = &call[depth - 1];
      size_t u = f->use;
      size_t w = f->cursor < hi[u] ? next_unvisited(skip, f->cursor) : count;
      if (w < hi[u]) {
        f->cursor = w + 1;
        VISIT(w);
        continue;
      }
      /* Every use of the range is visited now. Those on the stack are back edges; the rest are finished. */
      if (hi[u] > lo[u]) {
        uint64_t m = segtree_query(&stack_min, lo[u], hi[u]);
        if (m < low[u]) {
          low[u] = m;
        }
      }
      depth--;
      if (depth > 0 && low[u] < low[call[depth - 1].use]) {
        low[call[depth - 1].use] = low[u];
      }
      if (low[u] == index[u]) {
        /* u is the root of a component: pop it. */
        size_t members = 0;
        while (scc[scc_top - 1 - members] != u) {
          members++;
        }
        members++;
        bool loop = members > 1 || cyclic[u];
        for (size_t k = 0; k < members; k++) {
          size_t m = scc[--scc_top];
          state[m] = 2;
          segtree_set(&stack_min, m, UINT64_MAX);
          if (loop) {
            cyclic[m] = 1;
          }
        }
        if (!loop) {
          GVEC_Svg_Element * ue = uses[u];
          const GVEC_Svg_Element * t = ue->href_target;
          uint64_t total = 0;
          if (t) {
            total = sat_add(t->subtree_size, segtree_query(&expansion_sum, lo[u], hi[u]));
          }
          ue->expansion = total;
          segtree_set(&expansion_sum, u, total);
        }
      }
    }
#undef VISIT
  }
  /* Cut and report every member of a cycle, in document order. */
  for (size_t i = 0; i < count; i++) {
    if (cyclic[i]) {
      mark_cycle(diag, uses[i]);
      uses[i]->expansion = 0;
    }
  }
  /* The bound per use. */
  for (size_t i = 0; i < count; i++) {
    GVEC_Svg_Element * u = uses[i];
    if (u->href_target && u->expansion > limits->max_reference_expansion) {
      u->flags |= GVEC_SVG_ELEMENT_EXPANSION_LIMITED;
      gvec_diag_add(diag, GVEC_DIAG_WARNING, GVEC_DIAG_EXPANSION_LIMIT, GVEC_LIMIT_REFERENCE_EXPANSION, u->node.line,
          u->node.column, u->node.offset, u->name,
          "this use instantiates more elements than max_reference_expansion; it is not instantiated");
    }
  }
  /* The bound over the document: the uses that no other use's referent contains, in document order. A difference
   * array over element order marks the elements inside some referent. */
  for (size_t i = 0; i < count; i++) {
    const GVEC_Svg_Element * t = uses[i]->href_target;
    if (t) {
      cover[t->order]++;
      cover[t->order + t->subtree_size]--;
    }
  }
  int64_t inside = 0;
  uint64_t total = 0;
  for (size_t i = 0; i < n; i++) {
    inside += (int32_t) cover[i];
    GVEC_Svg_Element * e = doc->elements[i];
    if (e->kind != GVEC_SVG_KIND_USE || !e->href_target || (e->flags & GVEC_SVG_ELEMENT_EXPANSION_LIMITED) || inside > 0) {
      continue;
    }
    total = sat_add(total, e->expansion);
    if (total > limits->max_reference_expansion) {
      e->flags |= GVEC_SVG_ELEMENT_EXPANSION_LIMITED;
      gvec_diag_add(diag, GVEC_DIAG_WARNING, GVEC_DIAG_EXPANSION_LIMIT, GVEC_LIMIT_REFERENCE_EXPANSION, e->node.line,
          e->node.column, e->node.offset, e->name,
          "the document's uses instantiate more elements than max_reference_expansion; this use is not instantiated");
    }
  }
  return GVEC_OK;
}

GVEC_Result gvec_svg_resolve(GVEC_Svg_Document * doc, const GVEC_Limits * limits, GVEC_Diagnostics * diag) {
  for (size_t i = 0; i < doc->element_count; i++) {
    GVEC_Svg_Element * e = doc->elements[i];
    if (!e->href || e->href->value.reference.kind != GVEC_SVG_REF_INTERNAL) {
      continue;
    }
    const GVEC_Svg_Element * target = find_id(doc, e->href->value.reference.fragment);
    if (!target) {
      if (reports_missing(e->kind)) {
        gvec_diag_add(diag, GVEC_DIAG_WARNING, GVEC_DIAG_REFERENCE_MISSING, 0, e->href->line, e->href->column,
            e->href->offset, e->name, "the reference names an element the document does not have");
      }
      continue;
    }
    e->href_target = target;
  }
  GVEC_Result r = resolve_chains(doc, diag);
  if (r != GVEC_OK) {
    return r;
  }
  return resolve_uses(doc, limits, diag);
}
