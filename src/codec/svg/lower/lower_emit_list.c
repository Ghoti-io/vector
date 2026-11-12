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
 * @file lower_emit_list.c
 *
 * The list emitter (P1): the drawing tree recorded in an immediate-mode list. A `use` is a picture, recorded in a list of its
 * own, interned by content, and one instance item.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "lower_internal.h"

static GVEC_Result emit_nodes(gvec_lw_ctx * ctx, GSHSCENE_List * list, size_t table, const gvec_lw_node * first);

/*
 * What a failed call of the scene means. Out of memory ends the lowering. A limit of the scene refuses the element the node
 * came from (and the list stays as it was, because a call that fails records nothing); anything else is a defect.
 *
 * @param skipped Receives whether the node was refused.
 */
static GVEC_Result scene_failure(gvec_lw_ctx * ctx, const gvec_lw_node * node, GSHSCENE_Result r, bool * skipped) {
  *skipped = false;
  if (r == GSHSCENE_OK) {
    return GVEC_OK;
  }
  if (r == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (r == GSHSCENE_ERR_LIMIT && node->site != GVEC_LW_NO_SITE) {
    *skipped = true;
    return gvec_lw_refuse(ctx, ctx->sites[node->site].element, GVEC_LOWER_LIMIT, "a limit of the scene was reached recording the element");
  }
  return GVEC_ERR_INTERNAL;
}

static GVEC_Result emit_use(gvec_lw_ctx * ctx, GSHSCENE_List * list, size_t table, const gvec_lw_node * use) {
  GSHSCENE_List * picture_list = NULL;
  GSHSCENE_Result g = gshscene_list_create(ctx->scene_allocator, ctx->scene_limits, ctx->store, &picture_list);
  bool skipped;
  GVEC_Result r = scene_failure(ctx, use, g, &skipped);
  if (r != GVEC_OK || skipped) {
    return r;
  }
  size_t picture_table = 0;
  r = gvec_lw_table_new(ctx, &picture_table);
  if (r == GVEC_OK) {
    r = emit_nodes(ctx, picture_list, picture_table, use->first);
  }
  const GSHSCENE_Resource * picture = NULL;
  if (r == GVEC_OK) {
    g = gshscene_list_close(picture_list);
    if (g == GSHSCENE_OK) {
      g = gshscene_store_intern_picture(ctx->store, picture_list, &picture);
    }
    r = scene_failure(ctx, use, g, &skipped);
  }
  gshscene_list_destroy(picture_list);
  if (r != GVEC_OK || skipped) {
    return r;
  }
  g = gshscene_list_instance(list, picture, &use->map, 0, 0.0);
  /* The list holds its own reference to the picture. */
  gshscene_resource_release(picture);
  r = scene_failure(ctx, use, g, &skipped);
  if (r != GVEC_OK || skipped) {
    return r;
  }
  return gvec_lw_table_add(ctx, table, use->site, ctx->tracing ? (uint32_t) (picture_table + 1) : 0);
}

static GVEC_Result emit_nodes(gvec_lw_ctx * ctx, GSHSCENE_List * list, size_t table, const gvec_lw_node * first) {
  for (const gvec_lw_node * n = first; n; n = n->next) {
    GVEC_Result r = GVEC_OK;
    bool skipped = false;
    switch (n->kind) {
    case GVEC_LW_FILL:
      r = scene_failure(ctx, n, gshscene_list_fill_antialias(list, n->resource, &n->map, n->rule, &n->paint, n->crisp ? GSHSCENE_ANTIALIAS_NONE : GSHSCENE_ANTIALIAS_AREA, 0, 0.0), &skipped);
      if (r == GVEC_OK && !skipped) {
        r = gvec_lw_table_add(ctx, table, n->site, 0);
      }
      break;
    case GVEC_LW_STROKE:
      r = scene_failure(ctx, n, gshscene_list_stroke_antialias(list, n->resource, n->style, &n->map, &n->paint, n->crisp ? GSHSCENE_ANTIALIAS_NONE : GSHSCENE_ANTIALIAS_AREA, 0, 0.0), &skipped);
      if (r == GVEC_OK && !skipped) {
        r = gvec_lw_table_add(ctx, table, n->site, 0);
      }
      break;
    case GVEC_LW_GROUP: {
      GSHSCENE_Group group;
      memset(&group, 0, sizeof group);
      group.opacity = n->opacity;
      group.soft_mask = n->mask;
      group.composite = n->composite;
      if (n->isolate) {
        group.blend_space.primaries = GCOL_PRIMARIES_SRGB;
        group.blend_space.transfer = GCOL_TRANSFER_SRGB;
      }
      r = scene_failure(ctx, n, gshscene_list_group_push(list, &group, 0, 0.0), &skipped);
      if (r == GVEC_OK && !skipped) {
        r = gvec_lw_table_add(ctx, table, n->site, 0);
        if (r == GVEC_OK) {
          r = emit_nodes(ctx, list, table, n->first);
        }
        if (r == GVEC_OK) {
          r = scene_failure(ctx, n, gshscene_list_group_pop(list), &skipped);
        }
        if (r == GVEC_OK) {
          r = gvec_lw_table_add(ctx, table, n->site, 0);
        }
      }
      break;
    }
    case GVEC_LW_CLIP:
      r = scene_failure(ctx, n, gshscene_list_clip_push(list, n->resource, &n->map, n->rule, 0, 0.0), &skipped);
      if (r == GVEC_OK && !skipped) {
        r = gvec_lw_table_add(ctx, table, n->site, 0);
        if (r == GVEC_OK) {
          r = emit_nodes(ctx, list, table, n->first);
        }
        if (r == GVEC_OK) {
          r = scene_failure(ctx, n, gshscene_list_clip_pop(list), &skipped);
        }
        if (r == GVEC_OK) {
          r = gvec_lw_table_add(ctx, table, n->site, 0);
        }
      }
      break;
    case GVEC_LW_USE:
      r = emit_use(ctx, list, table, n);
      break;
    }
    if (r != GVEC_OK) {
      return r;
    }
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_emit_picture(gvec_lw_ctx * ctx, const gvec_lw_node * holder, const GSHSCENE_Resource ** out) {
  *out = NULL;
  GSHSCENE_List * picture_list = NULL;
  GSHSCENE_Result g = gshscene_list_create(ctx->scene_allocator, ctx->scene_limits, ctx->store, &picture_list);
  bool skipped;
  GVEC_Result r = scene_failure(ctx, holder, g, &skipped);
  if (r != GVEC_OK || skipped) {
    return r;
  }
  size_t table = 0;
  r = gvec_lw_table_new(ctx, &table);
  if (r == GVEC_OK) {
    r = emit_nodes(ctx, picture_list, table, holder->first);
  }
  const GSHSCENE_Resource * picture = NULL;
  if (r == GVEC_OK) {
    g = gshscene_list_close(picture_list);
    if (g == GSHSCENE_OK) {
      g = gshscene_store_intern_picture(ctx->store, picture_list, &picture);
    }
    r = scene_failure(ctx, holder, g, &skipped);
  }
  gshscene_list_destroy(picture_list);
  if (r == GVEC_OK && !skipped) {
    *out = picture;
  }
  return r;
}

GVEC_Result gvec_lw_emit_list(gvec_lw_ctx * ctx, GSHSCENE_List * list) {
  size_t table = 0;
  GVEC_Result r = gvec_lw_table_new(ctx, &table);
  if (r != GVEC_OK) {
    return r;
  }
  /* A list may already hold items of the caller's: they come from no element, and what is recorded follows them. */
  for (size_t i = gshscene_list_count(list); i > 0 && ctx->tracing; i--) {
    r = gvec_lw_table_add(ctx, table, GVEC_LW_NO_SITE, 0);
    if (r != GVEC_OK) {
      return r;
    }
  }
  return emit_nodes(ctx, list, table, ctx->root.first);
}
