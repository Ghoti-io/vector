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
 * @file lower_entry.c
 *
 * The two entry points of the SVG codec's lowering (L11): the walk, then one emitter.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "../svg_internal.h"
#include "lower_internal.h"

static GVEC_Result check_options(const GVEC_Lower_Options * options) {
  if (!options) {
    return GVEC_OK;
  }
  for (size_t i = 0; i < sizeof options->_reserved; i++) {
    if (options->_reserved[i] != 0) {
      return GVEC_ERR_INVALID;
    }
  }
  if (options->flags & ~(GVEC_LOWER_AT_POSITION | GVEC_LOWER_SET_FOREGROUND)) {
    return GVEC_ERR_INVALID;
  }
  if ((options->flags & GVEC_LOWER_SET_FOREGROUND) && gshscene_color_check(&options->foreground) != GSHSCENE_OK) {
    return GVEC_ERR_INVALID;
  }
  return GVEC_OK;
}

GVEC_Result gvec_svg_lower_edit(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_Edit * edit,
    GVEC_Diagnostics * diagnostics) {
  GVEC_Result r = check_options(options);
  if (r != GVEC_OK) {
    return r;
  }
  GSHSCENE_Store * store = gshscene_edit_store(edit);
  if (!store) {
    return GVEC_ERR_INVALID;
  }
  gvec_lw_ctx ctx;
  r = gvec_lw_begin(&ctx, doc, options, store, diagnostics);
  if (r != GVEC_OK) {
    return r;
  }
  if (options && (options->flags & GVEC_LOWER_SET_FOREGROUND) &&
      gshscene_edit_set_foreground(edit, &options->foreground) != GSHSCENE_OK) {
    gvec_lw_end(&ctx);
    return GVEC_ERR_INTERNAL;
  }
  r = gvec_lw_walk(&ctx);
  if (r == GVEC_OK) {
    r = gvec_lw_emit_edit(&ctx, edit);
  }
  if (r == GVEC_OK) {
    r = gvec_lw_trace_fill(&ctx);
  }
  gvec_lw_end(&ctx);
  return r;
}

GVEC_Result gvec_svg_lower_list(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics) {
  GVEC_Result r = check_options(options);
  if (r != GVEC_OK) {
    return r;
  }
  if (!options || !options->store || gshscene_list_is_closed(list)) {
    return GVEC_ERR_INVALID;
  }
  gvec_lw_ctx ctx;
  r = gvec_lw_begin(&ctx, doc, options, options->store, diagnostics);
  if (r != GVEC_OK) {
    return r;
  }
  if ((options->flags & GVEC_LOWER_SET_FOREGROUND) && gshscene_list_set_foreground(list, &options->foreground) != GSHSCENE_OK) {
    gvec_lw_end(&ctx);
    return GVEC_ERR_INVALID;
  }
  r = gvec_lw_walk(&ctx);
  if (r == GVEC_OK) {
    r = gvec_lw_emit_list(&ctx, list);
  }
  if (r == GVEC_OK) {
    r = gvec_lw_trace_fill(&ctx);
  }
  gvec_lw_end(&ctx);
  return r;
}
