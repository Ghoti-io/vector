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
 * @file lower_trace.c
 *
 * The trace (P9) and the names of the reasons (P10): which element each item of a lowering came from, so that an entry of a
 * sink's report can be printed against the document.
 */

#include <ghoti.io/vector/macros.h>

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include "lower_internal.h"
#include "lower_reason_table.h"

const char * gvec_lower_reason_string(GVEC_Lower_Reason reason) {
  if ((unsigned) reason == 0 || (unsigned) reason >= GVEC_LOWER_REASON_COUNT) {
    return "unknown";
  }
  return gvec_lower_reason_rows[reason].name;
}

typedef struct key_entry {
  uint64_t key;
  uint32_t site;
} key_entry;

struct GVEC_Lower_Trace {
  const GVEC_Allocator * allocator;
  gvec_lw_site * sites;
  size_t site_count;
  key_entry * by_key; ///< Sorted by key.
  size_t by_key_count;
  gvec_lw_table * tables;
  size_t table_count;
};

GVEC_Result gvec_lower_trace_create(const GVEC_Allocator * allocator, GVEC_Lower_Trace ** out_trace) {
  if (!out_trace) {
    return GVEC_ERR_INVALID;
  }
  *out_trace = NULL;
  GVEC_Lower_Trace * trace = gvec_alloc(allocator, sizeof *trace);
  if (!trace) {
    return GVEC_ERR_OOM;
  }
  memset(trace, 0, sizeof *trace);
  trace->allocator = allocator;
  *out_trace = trace;
  return GVEC_OK;
}

static void clear(GVEC_Lower_Trace * trace) {
  gvec_free(trace->allocator, trace->sites);
  gvec_free(trace->allocator, trace->by_key);
  for (size_t i = 0; i < trace->table_count; i++) {
    gvec_free(trace->allocator, trace->tables[i].site);
    gvec_free(trace->allocator, trace->tables[i].child);
  }
  gvec_free(trace->allocator, trace->tables);
  trace->sites = NULL;
  trace->by_key = NULL;
  trace->tables = NULL;
  trace->site_count = trace->by_key_count = trace->table_count = 0;
}

void gvec_lower_trace_free(GVEC_Lower_Trace * trace) {
  if (!trace) {
    return;
  }
  clear(trace);
  gvec_free(trace->allocator, trace);
}

//-----------------------------------------------------------------------------
// Filling it
//-----------------------------------------------------------------------------

GVEC_Result gvec_lw_table_new(gvec_lw_ctx * ctx, size_t * out_index) {
  if (!ctx->tracing) {
    *out_index = 0;
    return GVEC_OK;
  }
  if (ctx->table_count == ctx->table_capacity) {
    size_t capacity = ctx->table_capacity ? ctx->table_capacity * 2 : 8, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof *ctx->tables, &bytes)) {
      return GVEC_ERR_OOM;
    }
    void * fresh = gvec_realloc(ctx->allocator, ctx->tables, bytes);
    if (!fresh) {
      return GVEC_ERR_OOM;
    }
    ctx->tables = fresh;
    ctx->table_capacity = capacity;
  }
  memset(&ctx->tables[ctx->table_count], 0, sizeof ctx->tables[0]);
  *out_index = ctx->table_count++;
  return GVEC_OK;
}

GVEC_Result gvec_lw_table_add(gvec_lw_ctx * ctx, size_t table, uint32_t site, uint32_t child) {
  if (!ctx->tracing) {
    return GVEC_OK;
  }
  gvec_lw_table * t = &ctx->tables[table];
  if (t->count == t->capacity) {
    size_t capacity = t->capacity ? t->capacity * 2 : 64, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof(uint32_t), &bytes)) {
      return GVEC_ERR_OOM;
    }
    uint32_t * sites = gvec_realloc(ctx->allocator, t->site, bytes);
    if (!sites) {
      return GVEC_ERR_OOM;
    }
    t->site = sites;
    uint32_t * children = gvec_realloc(ctx->allocator, t->child, bytes);
    if (!children) {
      return GVEC_ERR_OOM;
    }
    t->child = children;
    t->capacity = capacity;
  }
  t->site[t->count] = site;
  t->child[t->count] = child;
  t->count++;
  return GVEC_OK;
}

static int compare_keys(const void * a, const void * b) {
  const key_entry * x = a;
  const key_entry * y = b;
  return x->key < y->key ? -1 : x->key > y->key ? 1 : 0;
}

GVEC_Result gvec_lw_trace_fill(gvec_lw_ctx * ctx) {
  GVEC_Lower_Trace * trace = ctx->options ? ctx->options->trace : NULL;
  if (!trace) {
    return GVEC_OK;
  }
  clear(trace);
  if (ctx->site_count) {
    trace->sites = gvec_alloc(trace->allocator, ctx->site_count * sizeof *trace->sites);
    trace->by_key = gvec_alloc(trace->allocator, ctx->site_count * sizeof *trace->by_key);
    if (!trace->sites || !trace->by_key) {
      clear(trace);
      return GVEC_ERR_OOM;
    }
    memcpy(trace->sites, ctx->sites, ctx->site_count * sizeof *trace->sites);
    trace->site_count = ctx->site_count;
    for (size_t i = 0; i < ctx->site_count; i++) {
      trace->by_key[i].key = ctx->sites[i].key;
      trace->by_key[i].site = (uint32_t) i;
    }
    trace->by_key_count = ctx->site_count;
    qsort(trace->by_key, trace->by_key_count, sizeof *trace->by_key, compare_keys);
  }
  if (ctx->table_count) {
    trace->tables = gvec_alloc(trace->allocator, ctx->table_count * sizeof *trace->tables);
    if (!trace->tables) {
      clear(trace);
      return GVEC_ERR_OOM;
    }
    memset(trace->tables, 0, ctx->table_count * sizeof *trace->tables);
    trace->table_count = ctx->table_count;
    for (size_t i = 0; i < ctx->table_count; i++) {
      const gvec_lw_table * from = &ctx->tables[i];
      gvec_lw_table * to = &trace->tables[i];
      if (from->count) {
        to->site = gvec_alloc(trace->allocator, from->count * sizeof(uint32_t));
        to->child = gvec_alloc(trace->allocator, from->count * sizeof(uint32_t));
        if (!to->site || !to->child) {
          clear(trace);
          return GVEC_ERR_OOM;
        }
        memcpy(to->site, from->site, from->count * sizeof(uint32_t));
        memcpy(to->child, from->child, from->count * sizeof(uint32_t));
      }
      to->count = to->capacity = from->count;
    }
  }
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// Asking it
//-----------------------------------------------------------------------------

static void place(const GVEC_Lower_Trace * trace, uint32_t site, GVEC_Lower_Site * out) {
  if (site == GVEC_LW_NO_SITE || site >= trace->site_count) {
    return;
  }
  const gvec_lw_site * s = &trace->sites[site];
  out->placed = 1;
  out->element = s->element->name;
  out->line = s->element->node.line;
  out->column = s->element->node.column;
  out->element_key = s->element->key;
  out->use_depth = s->use_depth;
}

GVEC_Result gvec_lower_trace_element(const GVEC_Lower_Trace * trace, const GSHSCENE_Version * version,
    const GSHSCENE_Report * report, size_t index, GVEC_Lower_Site * out_site) {
  if (!trace || !report || !out_site) {
    return GVEC_ERR_INVALID;
  }
  memset(out_site, 0, sizeof *out_site);
  GSHSCENE_Report_Entry entry;
  if (gshscene_report_entry(report, index, &entry) != GSHSCENE_OK) {
    return GVEC_ERR_INVALID;
  }
  if (entry.chain_count == 0) {
    return GVEC_OK; /* A render-level entry is about no item. */
  }
  if (!version) {
    size_t table = 0;
    for (size_t i = 0; i < entry.chain_count; i++) {
      if (table >= trace->table_count || entry.chain[i] >= trace->tables[table].count) {
        return GVEC_OK;
      }
      const gvec_lw_table * t = &trace->tables[table];
      if (i + 1 == entry.chain_count) {
        place(trace, t->site[entry.chain[i]], out_site);
        return GVEC_OK;
      }
      uint32_t child = t->child[entry.chain[i]];
      if (child == 0) {
        return GVEC_OK;
      }
      table = child - 1;
    }
    return GVEC_OK;
  }
  uint64_t stack[16];
  uint64_t * path = stack;
  if (entry.chain_count > sizeof stack / sizeof stack[0]) {
    path = gvec_alloc(trace->allocator, entry.chain_count * sizeof *path);
    if (!path) {
      return GVEC_ERR_OOM;
    }
  }
  uint64_t node = 0;
  size_t count = 0;
  GVEC_Result result = GVEC_OK;
  if (gshscene_report_resolve(report, version, index, &node, path, entry.chain_count, &count) == GSHSCENE_OK) {
    GSHSCENE_Node_Info info;
    if (gshscene_version_node(version, node, &info) == GSHSCENE_OK && info.has_key && info.key.domain == GVEC_LOWER_KEY_DOMAIN) {
      key_entry probe = {info.key.value, 0};
      const key_entry * found = bsearch(&probe, trace->by_key, trace->by_key_count, sizeof probe, compare_keys);
      if (found) {
        place(trace, found->site, out_site);
      }
    }
  }
  if (path != stack) {
    gvec_free(trace->allocator, path);
  }
  return result;
}
