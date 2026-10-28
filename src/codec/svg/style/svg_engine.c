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
 * @file svg_engine.c
 *
 * The engines of style (see svg_style.h) and the cache of them.
 *
 * Making an engine builds the registry of properties (the pin's grammars of every property are compiled when one is overridden) and
 * computes the initial value of every property: milliseconds, about forty times what loading an icon costs. It depends on nothing in the
 * document but the environment (the viewport, the color scheme, the bound of `var()` expansion), so an engine made with the default
 * allocator is kept, by environment, and shared by every context made for the same environment on any thread. An engine is immutable
 * once made (the registry compiles a grammar the first time a property is read, under its own lock).
 *
 * The cache holds at most eight environments. A document with another allocator makes an engine of its own through that allocator and
 * frees it with the context, so that nothing is allocated behind an allocator's back.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/mutex.h>
#include <ghoti.io/cutil/once.h>

#include "svg_style.h"

#define CACHE_CAPACITY 8

static GCU_Once g_once = GCU_ONCE_INIT;
static GCU_MUTEX_T g_mutex;
static bool g_ready;
static gvec_svg_engine * g_head;
static size_t g_count;

static void init_cache(void) {
  g_ready = GCU_MUTEX_CREATE(g_mutex) == 0;
}

static GVEC_Result build(const GVEC_Allocator * allocator, double w, double h, GVEC_Color_Scheme scheme, size_t expansion,
    gvec_svg_engine ** out) {
  gvec_svg_engine * engine = gvec_alloc(allocator, sizeof *engine);
  if (!engine) {
    return GVEC_ERR_OOM;
  }
  memset(engine, 0, sizeof *engine);
  engine->allocator = allocator;
  engine->viewport_width = w;
  engine->viewport_height = h;
  engine->color_scheme = scheme;
  engine->max_custom_expansion = expansion;
  engine->references = 1;
  GVEC_Result r = gvec_svg_registry_new(allocator, &engine->reg);
  if (r != GVEC_OK) {
    gvec_free(allocator, engine);
    return r;
  }
  /* The callbacks ignore their `user`: an engine is shared, and knows nothing of any document. */
  gvec_svg_element_interface(&engine->table, NULL);
  gvec_svg_environment(&engine->env, w, h, scheme, NULL);
  GTEXT_CSS_Compute_Options options = gtext_css_compute_options_default();
  options.allocator = allocator;
  options.elements = &engine->table;
  options.substitute.max_substituted_tokens = expansion;
  GTEXT_CSS_Compute_Result result;
  GTEXT_CSS_Status status = gtext_css_compute_context_new(engine->reg.registry, &engine->env, &options, &engine->compute, &result);
  if (status != GTEXT_CSS_OK) {
    gvec_svg_registry_free(&engine->reg);
    gvec_free(allocator, engine);
    return status == GTEXT_CSS_E_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
  }
  *out = engine;
  return GVEC_OK;
}

static void destroy(gvec_svg_engine * engine) {
  gtext_css_compute_context_free(engine->compute);
  gvec_svg_registry_free(&engine->reg);
  gvec_free(engine->allocator, engine);
}

/* The cached engine for an environment; the caller holds the lock. */
static gvec_svg_engine * find(double w, double h, GVEC_Color_Scheme scheme, size_t expansion) {
  for (gvec_svg_engine * e = g_head; e; e = e->next) {
    if (e->viewport_width == w && e->viewport_height == h && e->color_scheme == scheme && e->max_custom_expansion == expansion) {
      return e;
    }
  }
  return NULL;
}

static bool is_default(const GVEC_Allocator * allocator) {
  return allocator == NULL || allocator == gvec_allocator_default();
}

GVEC_Result gvec_svg_engine_acquire(const GVEC_Allocator * allocator, double w, double h, GVEC_Color_Scheme scheme, size_t expansion,
    gvec_svg_engine ** out) {
  if (!is_default(allocator)) {
    return build(allocator, w, h, scheme, expansion, out);
  }
  gcu_once(&g_once, init_cache);
  if (!g_ready) {
    return build(NULL, w, h, scheme, expansion, out);
  }
  GCU_MUTEX_LOCK(g_mutex);
  gvec_svg_engine * found = find(w, h, scheme, expansion);
  if (found) {
    found->references++;
    GCU_MUTEX_UNLOCK(g_mutex);
    *out = found;
    return GVEC_OK;
  }
  GCU_MUTEX_UNLOCK(g_mutex);
  /* Built outside the lock: another thread may build the same one, and one of them is then dropped below. */
  gvec_svg_engine * made = NULL;
  GVEC_Result r = build(NULL, w, h, scheme, expansion, &made);
  if (r != GVEC_OK) {
    return r;
  }
  GCU_MUTEX_LOCK(g_mutex);
  found = find(w, h, scheme, expansion);
  if (found) {
    found->references++;
    GCU_MUTEX_UNLOCK(g_mutex);
    destroy(made);
    *out = found;
    return GVEC_OK;
  }
  /* Make room: the oldest engine nobody uses goes. When all are in use, the new one is the caller's alone. */
  if (g_count >= CACHE_CAPACITY) {
    gvec_svg_engine **link = &g_head, **victim = NULL;
    for (; *link; link = &(*link)->next) {
      if ((*link)->references == 1) {
        victim = link;
      }
    }
    if (victim) {
      gvec_svg_engine * gone = *victim;
      *victim = gone->next;
      g_count--;
      GCU_MUTEX_UNLOCK(g_mutex);
      destroy(gone);
      GCU_MUTEX_LOCK(g_mutex);
    }
  }
  if (g_count < CACHE_CAPACITY) {
    made->shared = true;
    made->references = 2; /* the cache's and the caller's */
    made->next = g_head;
    g_head = made;
    g_count++;
  }
  GCU_MUTEX_UNLOCK(g_mutex);
  *out = made;
  return GVEC_OK;
}

void gvec_svg_engine_release(gvec_svg_engine * engine) {
  if (!engine) {
    return;
  }
  if (!engine->shared) {
    destroy(engine);
    return;
  }
  GCU_MUTEX_LOCK(g_mutex);
  engine->references--;
  bool last = engine->references == 0;
  GCU_MUTEX_UNLOCK(g_mutex);
  if (last) {
    destroy(engine);
  }
}

void gvec_svg_style_cache_clear(void) {
  gcu_once(&g_once, init_cache);
  if (!g_ready) {
    return;
  }
  GCU_MUTEX_LOCK(g_mutex);
  gvec_svg_engine * list = g_head;
  g_head = NULL;
  g_count = 0;
  GCU_MUTEX_UNLOCK(g_mutex);
  while (list) {
    gvec_svg_engine * next = list->next;
    gvec_svg_engine_release(list); /* the cache's reference; a context still using it keeps it alive */
    list = next;
  }
}
