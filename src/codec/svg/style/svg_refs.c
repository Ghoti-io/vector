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
 * @file svg_refs.c
 *
 * The references of properties (C9), added to the document's reference graph: `fill` and `stroke` to a paint server, `clip-path` to a
 * `clipPath`, `mask` to a `mask`, `marker-*` to a `marker` and `filter` to a `filter`. A clip path that clips itself, a pattern painted
 * with itself, a marker whose content uses it: each is a cycle, found with the `use` and `href` edges of the typed document and the
 * containment of elements by the one pass (the strongly connected components of the graph, Tarjan's algorithm, without recursion), and
 * the reference of a property that closes one is cut: it reads as no reference, and is reported.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "svg_style.h"
#include "svg_used.h"

/* One edge of a property reference. */
typedef struct prop_edge {
  uint32_t from, to;
} prop_edge;

typedef struct edge_list {
  prop_edge * items;
  size_t count, capacity;
} edge_list;

static GVEC_Result push_edge(GVEC_Svg_Style_Context * context, edge_list * list, uint32_t from, uint32_t to) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 64;
    prop_edge * grown = gvec_realloc(context->allocator, list->items, capacity * sizeof *grown);
    if (!grown) {
      return GVEC_ERR_OOM;
    }
    list->items = grown;
    list->capacity = capacity;
  }
  list->items[list->count].from = from;
  list->items[list->count].to = to;
  list->count++;
  return GVEC_OK;
}

static bool is_server(GVEC_Svg_Kind kind) {
  return kind == GVEC_SVG_KIND_LINEAR_GRADIENT || kind == GVEC_SVG_KIND_RADIAL_GRADIENT || kind == GVEC_SVG_KIND_PATTERN;
}

/* The element a url names, if it is one of `kind`; paint servers when `kind` is UNKNOWN. */
static GVEC_Result url_target(GVEC_Svg_Style_Context * context, const GTEXT_CSS_Value * v, GVEC_Svg_Kind kind,
    const GVEC_Svg_Element ** out) {
  *out = NULL;
  if (v->kind != GTEXT_CSS_VAL_URL) {
    return GVEC_OK;
  }
  bool local;
  const char * id;
  const GVEC_Svg_Element * target;
  GVEC_Result r = gvec_svg_resolve_url(context, &v->text, &local, &id, &target);
  if (r != GVEC_OK || !target) {
    return r;
  }
  if (kind == GVEC_SVG_KIND_UNKNOWN ? is_server(target->kind) : target->kind == kind) {
    *out = target;
  }
  return GVEC_OK;
}

/* The edges one property of one element has: every url of its value. */
static GVEC_Result property_edges(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Svg_Property property,
    GVEC_Svg_Kind kind, edge_list * list) {
  const GTEXT_CSS_Value * v = NULL;
  GVEC_Result r = gvec_svg_computed(context, instance, property, NULL, &v);
  if (r != GVEC_OK) {
    return r == GVEC_ERR_OOM ? r : GVEC_OK; /* A style that cannot be computed has no references to follow. */
  }
  if (!v) {
    return GVEC_OK;
  }
  const GTEXT_CSS_Value * items = v;
  size_t n = 1;
  if (v->kind == GTEXT_CSS_VAL_LIST) {
    items = v->children;
    n = v->child_count;
  }
  for (size_t i = 0; i < n; i++) {
    const GVEC_Svg_Element * target = NULL;
    r = url_target(context, &items[i], kind, &target);
    if (r == GVEC_OK && target) {
      r = push_edge(context, list, instance->element->order, target->order);
    }
    if (r != GVEC_OK) {
      return r;
    }
  }
  return GVEC_OK;
}

/* A frame of the iterative walk. */
typedef struct frame {
  uint32_t node;
  const GVEC_Svg_Element * child; ///< The next child to visit, or NULL when the children are done.
  uint8_t stage;                  ///< 0 children, 1 the href edge, 2 the property edges.
  size_t next_edge;
} frame;

GVEC_Result gvec_svg_style_resolve_references(GVEC_Svg_Style_Context * context) {
  if (!context) {
    return GVEC_ERR_INVALID;
  }
  if (context->references_resolved) {
    return GVEC_OK;
  }
  const GVEC_Svg_Document * doc = context->document;
  size_t n = doc->element_count;
  const GVEC_Svg_Instance * const * instances = NULL;
  edge_list edges = {NULL, 0, 0};
  size_t * start = NULL;
  prop_edge * sorted = NULL;
  uint32_t *index = NULL, *low = NULL, *component = NULL, *stack = NULL;
  uint8_t * on_stack = NULL;
  frame * frames = NULL;
  GVEC_Result r = gvec_svg_document_instances(context, &instances);
  if (r != GVEC_OK) {
    return r;
  }
  for (size_t i = 0; r == GVEC_OK && i < n; i++) {
    static const struct {
      GVEC_Svg_Property property;
      GVEC_Svg_Kind kind;
    } k_properties[] = {
        {GVEC_SVG_PROP_FILL, GVEC_SVG_KIND_UNKNOWN},
        {GVEC_SVG_PROP_STROKE, GVEC_SVG_KIND_UNKNOWN},
        {GVEC_SVG_PROP_CLIP_PATH, GVEC_SVG_KIND_CLIP_PATH},
        {GVEC_SVG_PROP_MASK, GVEC_SVG_KIND_MASK},
        {GVEC_SVG_PROP_MARKER_START, GVEC_SVG_KIND_MARKER},
        {GVEC_SVG_PROP_MARKER_MID, GVEC_SVG_KIND_MARKER},
        {GVEC_SVG_PROP_MARKER_END, GVEC_SVG_KIND_MARKER},
        {GVEC_SVG_PROP_FILTER, GVEC_SVG_KIND_FILTER},
    };
    for (size_t k = 0; r == GVEC_OK && k < sizeof k_properties / sizeof k_properties[0]; k++) {
      r = property_edges(context, instances[i], k_properties[k].property, k_properties[k].kind, &edges);
    }
  }
  if (r != GVEC_OK) {
    goto done;
  }
  /* The property edges by source (a counting sort). */
  start = gvec_alloc(context->allocator, (n + 2) * sizeof *start);
  sorted = gvec_alloc(context->allocator, (edges.count ? edges.count : 1) * sizeof *sorted);
  index = gvec_alloc(context->allocator, (n ? n : 1) * sizeof *index);
  low = gvec_alloc(context->allocator, (n ? n : 1) * sizeof *low);
  component = gvec_arena_array(&context->arena, n ? n : 1, sizeof *component, 4);
  stack = gvec_alloc(context->allocator, (n ? n : 1) * sizeof *stack);
  on_stack = gvec_alloc(context->allocator, (n ? n : 1));
  frames = gvec_alloc(context->allocator, (n ? n : 1) * sizeof *frames);
  if (!start || !sorted || !index || !low || !component || !stack || !on_stack || !frames) {
    r = GVEC_ERR_OOM;
    goto done;
  }
  memset(start, 0, (n + 2) * sizeof *start);
  for (size_t i = 0; i < edges.count; i++) {
    start[edges.items[i].from + 1]++;
  }
  for (size_t i = 0; i < n; i++) {
    start[i + 1] += start[i];
  }
  {
    size_t * fill = gvec_alloc(context->allocator, (n + 1) * sizeof *fill);
    if (!fill) {
      r = GVEC_ERR_OOM;
      goto done;
    }
    memcpy(fill, start, (n + 1) * sizeof *fill);
    for (size_t i = 0; i < edges.count; i++) {
      sorted[fill[edges.items[i].from]++] = edges.items[i];
    }
    gvec_free(context->allocator, fill);
  }
  memset(index, 0xff, (n ? n : 1) * sizeof *index); /* UINT32_MAX: unvisited */
  memset(on_stack, 0, n ? n : 1);
  uint32_t counter = 0, components = 0;
  size_t stack_top = 0;
  for (size_t root = 0; root < n; root++) {
    if (index[root] != UINT32_MAX) {
      continue;
    }
    size_t depth = 0;
    frames[0] = (frame){(uint32_t) root, gvec_svg_element_first_child(doc->elements[root]), 0, start[root]};
    index[root] = low[root] = counter++;
    stack[stack_top++] = (uint32_t) root;
    on_stack[root] = 1;
    depth = 1;
    while (depth > 0) {
      frame * f = &frames[depth - 1];
      uint32_t v = f->node;
      const GVEC_Svg_Element * e = doc->elements[v];
      uint32_t next = UINT32_MAX;
      bool have = false;
      if (f->stage == 0) {
        if (f->child) {
          next = f->child->order;
          f->child = gvec_svg_element_next_sibling(f->child);
          have = true;
        }
        else {
          f->stage = 1;
        }
      }
      if (!have && f->stage == 1) {
        f->stage = 2;
        if (e->href_target) {
          next = e->href_target->order;
          have = true;
        }
      }
      if (!have && f->stage == 2) {
        if (f->next_edge < start[v + 1]) {
          next = sorted[f->next_edge++].to;
          have = true;
        }
      }
      if (have) {
        if (index[next] == UINT32_MAX) {
          index[next] = low[next] = counter++;
          stack[stack_top++] = next;
          on_stack[next] = 1;
          frames[depth++] = (frame){next, gvec_svg_element_first_child(doc->elements[next]), 0, start[next]};
        }
        else if (on_stack[next] && index[next] < low[v]) {
          low[v] = index[next];
        }
        continue;
      }
      /* All successors of v are done. */
      if (low[v] == index[v]) {
        size_t size = 0;
        uint32_t w;
        size_t first = stack_top;
        do {
          w = stack[--stack_top];
          on_stack[w] = 0;
          size++;
        } while (w != v);
        /* A component of one element is a cycle only if the element refers to itself. */
        bool self = false;
        for (size_t k = start[v]; k < start[v + 1]; k++) {
          self = self || sorted[k].to == v;
        }
        if (size > 1 || self) {
          components++;
          for (size_t k = first; k > stack_top; k--) {
            component[stack[k - 1]] = components;
          }
        }
      }
      depth--;
      if (depth > 0) {
        uint32_t parent = frames[depth - 1].node;
        if (low[v] < low[parent]) {
          low[parent] = low[v];
        }
      }
    }
  }
  /* A component is a cycle through a property when one of the property edges lies inside it; a cycle of `use` and `href` alone was
   * reported when the document was loaded. */
  uint32_t * has_property = gvec_alloc(context->allocator, (components + 1) * sizeof *has_property);
  if (!has_property) {
    r = GVEC_ERR_OOM;
    goto done;
  }
  memset(has_property, 0, (components + 1) * sizeof *has_property);
  for (size_t i = 0; i < edges.count; i++) {
    uint32_t c = component[edges.items[i].from];
    if (c && c == component[edges.items[i].to]) {
      has_property[c] = 1;
    }
  }
  for (size_t i = 0; i < n; i++) {
    uint32_t c = component[i];
    if (c && !has_property[c]) {
      component[i] = 0;
    }
  }
  gvec_free(context->allocator, has_property);
  for (size_t i = 0; i < n && r == GVEC_OK; i++) {
    if (component[i] && gvec_svg_once_first(context, GVEC_ONCE_CYCLE, doc->elements[i])) {
      r = gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_REFERENCE_CYCLE, 0, doc->elements[i], NULL,
          "the element is on a cycle through the references of properties; the reference that closes it is cut");
      if (r == GVEC_ERR_OOM) {
        break;
      }
      r = GVEC_OK;
    }
  }
  context->component = component;
  context->references_resolved = true;
done:
  gvec_free(context->allocator, frames);
  gvec_free(context->allocator, on_stack);
  gvec_free(context->allocator, stack);
  gvec_free(context->allocator, low);
  gvec_free(context->allocator, index);
  gvec_free(context->allocator, sorted);
  gvec_free(context->allocator, start);
  gvec_free(context->allocator, edges.items);
  return r;
}

bool gvec_svg_style_on_reference_cycle(const GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * element) {
  return context && element && context->component && context->component[element->order] != 0;
}

bool gvec_svg_reference_cut(const GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * from, const GVEC_Svg_Element * to) {
  if (!context || !context->component || !from || !to) {
    return false;
  }
  uint32_t c = context->component[from->order];
  return c != 0 && c == context->component[to->order];
}
