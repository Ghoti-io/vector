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
 * @file lower_emit_edit.c
 *
 * The edit emitter (P1, P8, AD-9): the drawing tree as keyed nodes of the retained scene.
 *
 * Every node the lowering makes is found again by its key, so lowering a document again sets a node to the properties it has
 * (which changes nothing), adds the nodes that are new, moves the ones that are out of place and removes the ones the document
 * no longer has. All of them are under one group of the library's, so that removing what is stale cannot reach a node of the
 * caller's. A `use` is a hidden group that holds the picture's content and an instance node that draws it.
 */

#include <ghoti.io/vector/macros.h>

#include "lower_internal.h"

static GVEC_Result edit_children(gvec_lw_ctx * ctx, GSHSCENE_Edit * edit, uint64_t group, const gvec_lw_node * first);

static GVEC_Result scene_failure(gvec_lw_ctx * ctx, uint32_t site, GSHSCENE_Result r, bool * skipped) {
  *skipped = false;
  if (r == GSHSCENE_OK) {
    return GVEC_OK;
  }
  if (r == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (r == GSHSCENE_ERR_LIMIT && site != GVEC_LW_NO_SITE) {
    *skipped = true;
    return gvec_lw_refuse(ctx, ctx->sites[site].element, GVEC_LOWER_LIMIT, "a limit of the scene was reached recording the element");
  }
  return GVEC_ERR_INTERNAL;
}

/* The node `key` names, with `props`, as child `position` of `parent`: made, set, and moved if it is somewhere else. */
static GVEC_Result put(gvec_lw_ctx * ctx, GSHSCENE_Edit * edit, uint64_t parent, size_t position, uint64_t key_value, uint32_t site,
    const GSHSCENE_Node * props, uint64_t * out_id, bool * skipped) {
  GSHSCENE_Key key = {GVEC_LOWER_KEY_DOMAIN, key_value};
  uint64_t id = 0;
  GVEC_Result r = scene_failure(ctx, site, gshscene_edit_upsert(edit, &key, parent, position, props, &id), skipped);
  if (r != GVEC_OK || *skipped) {
    return r;
  }
  GSHSCENE_Node_Info info;
  if (gshscene_edit_node(edit, id, &info) != GSHSCENE_OK) {
    return GVEC_ERR_INTERNAL;
  }
  uint64_t at = 0;
  if (info.parent != parent || gshscene_edit_child(edit, parent, position, &at) != GSHSCENE_OK || at != id) {
    r = scene_failure(ctx, site, gshscene_edit_move(edit, id, parent, position), skipped);
    if (r != GVEC_OK || *skipped) {
      return r;
    }
  }
  *out_id = id;
  return GVEC_OK;
}

/* Remove the children of `group` from index `keep` on: what an earlier lowering made and this one did not. */
static GVEC_Result trim(GSHSCENE_Edit * edit, uint64_t group, size_t keep) {
  GSHSCENE_Node_Info info;
  if (gshscene_edit_node(edit, group, &info) != GSHSCENE_OK) {
    return GVEC_ERR_INTERNAL;
  }
  for (size_t count = info.child_count; count > keep; count--) {
    uint64_t stale;
    if (gshscene_edit_child(edit, group, keep, &stale) != GSHSCENE_OK || gshscene_edit_remove(edit, stale) != GSHSCENE_OK) {
      return GVEC_ERR_INTERNAL;
    }
  }
  return GVEC_OK;
}

static GVEC_Result emit_use(gvec_lw_ctx * ctx, GSHSCENE_Edit * edit, uint64_t group, size_t * position, const gvec_lw_node * use) {
  bool skipped;
  GSHSCENE_Node container;
  gshscene_node_init(GSHSCENE_NODE_GROUP, &container);
  container.flags = GSHSCENE_NODE_HIDDEN;
  uint64_t container_id = 0;
  GVEC_Result r = put(ctx, edit, group, *position, use->container_key, use->container_site, &container, &container_id, &skipped);
  if (r != GVEC_OK || skipped) {
    return r;
  }
  r = edit_children(ctx, edit, container_id, use->first);
  if (r != GVEC_OK) {
    return r;
  }
  GSHSCENE_Node instance;
  gshscene_node_init(GSHSCENE_NODE_INSTANCE, &instance);
  instance.map = use->map;
  instance.target = container_id;
  uint64_t instance_id = 0;
  r = put(ctx, edit, group, *position + 1, use->key, use->site, &instance, &instance_id, &skipped);
  if (r != GVEC_OK) {
    return r;
  }
  if (skipped) {
    /* The picture is kept only with the instance that draws it. */
    return gshscene_edit_remove(edit, container_id) == GSHSCENE_OK ? GVEC_OK : GVEC_ERR_INTERNAL;
  }
  *position += 2;
  return GVEC_OK;
}

static GVEC_Result edit_children(gvec_lw_ctx * ctx, GSHSCENE_Edit * edit, uint64_t group, const gvec_lw_node * first) {
  size_t position = 0;
  for (const gvec_lw_node * n = first; n; n = n->next) {
    GVEC_Result r = GVEC_OK;
    bool skipped = false;
    uint64_t id = 0;
    GSHSCENE_Node props;
    switch (n->kind) {
    case GVEC_LW_FILL:
      gshscene_node_init(GSHSCENE_NODE_SHAPE, &props);
      props.map = n->map;
      props.resource = n->resource;
      props.fill_rule = n->rule;
      props.paint = n->paint;
      props.antialias = n->crisp ? GSHSCENE_ANTIALIAS_NONE : GSHSCENE_ANTIALIAS_AREA;
      r = put(ctx, edit, group, position, n->key, n->site, &props, &id, &skipped);
      if (r == GVEC_OK && !skipped) {
        position++;
      }
      break;
    case GVEC_LW_STROKE:
      gshscene_node_init(GSHSCENE_NODE_STROKE, &props);
      props.map = n->map;
      props.resource = n->resource;
      props.style = n->style;
      props.paint = n->paint;
      props.antialias = n->crisp ? GSHSCENE_ANTIALIAS_NONE : GSHSCENE_ANTIALIAS_AREA;
      r = put(ctx, edit, group, position, n->key, n->site, &props, &id, &skipped);
      if (r == GVEC_OK && !skipped) {
        position++;
      }
      break;
    case GVEC_LW_GROUP:
    case GVEC_LW_CLIP:
      gshscene_node_init(GSHSCENE_NODE_GROUP, &props);
      if (n->kind == GVEC_LW_GROUP) {
        props.opacity = n->opacity;
        props.soft_mask = n->mask;
        props.composite = n->composite;
        if (n->isolate) {
          props.blend_space.primaries = GCOL_PRIMARIES_SRGB;
          props.blend_space.transfer = GCOL_TRANSFER_SRGB;
        }
      }
      else {
        props.clip = n->resource;
        props.clip_map = n->map;
        props.clip_rule = n->rule;
      }
      r = put(ctx, edit, group, position, n->key, n->site, &props, &id, &skipped);
      if (r == GVEC_OK && !skipped) {
        r = edit_children(ctx, edit, id, n->first);
        position++;
      }
      break;
    case GVEC_LW_USE:
      r = emit_use(ctx, edit, group, &position, n);
      break;
    }
    if (r != GVEC_OK) {
      return r;
    }
  }
  return trim(edit, group, position);
}

GVEC_Result gvec_lw_emit_edit(gvec_lw_ctx * ctx, GSHSCENE_Edit * edit) {
  const GVEC_Lower_Options * options = ctx->options;
  uint64_t parent = options && options->parent ? options->parent : GSHSCENE_ROOT_ID;
  GSHSCENE_Node_Info info;
  if (gshscene_edit_node(edit, parent, &info) != GSHSCENE_OK || info.node.kind != GSHSCENE_NODE_GROUP) {
    return GVEC_ERR_INVALID;
  }
  bool at_position = options && (options->flags & GVEC_LOWER_AT_POSITION);
  size_t position = at_position ? options->position : GSHSCENE_APPEND;
  if (at_position && position > info.child_count) {
    return GVEC_ERR_INVALID;
  }
  GSHSCENE_Node drawing;
  gshscene_node_init(GSHSCENE_NODE_GROUP, &drawing);
  uint64_t key = gvec_lw_key(ctx->document->root ? ctx->document->root->key : 0, 0, GVEC_LW_ROLE_DRAWING);
  GSHSCENE_Key k = {GVEC_LOWER_KEY_DOMAIN, key};
  uint64_t id = 0;
  GSHSCENE_Result g = gshscene_edit_upsert(edit, &k, parent, position, &drawing, &id);
  if (g == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (g == GSHSCENE_ERR_LIMIT) {
    return GVEC_ERR_LIMIT;
  }
  if (g != GSHSCENE_OK) {
    return GVEC_ERR_INTERNAL;
  }
  if (at_position) {
    uint64_t at;
    GSHSCENE_Node_Info now;
    if (gshscene_edit_node(edit, id, &now) != GSHSCENE_OK) {
      return GVEC_ERR_INTERNAL;
    }
    if (now.parent != parent || gshscene_edit_child(edit, parent, options->position, &at) != GSHSCENE_OK || at != id) {
      if (gshscene_edit_move(edit, id, parent, options->position) != GSHSCENE_OK) {
        return GVEC_ERR_INVALID;
      }
    }
  }
  return edit_children(ctx, edit, id, ctx->root.first);
}
