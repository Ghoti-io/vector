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
 * The planted lowerings (stubs.h).
 */

#include "stubs.h"

#include <string.h>

#include <ghoti.io/shape-geometry/shape-geometry.h>

#include "../../src/core/core_internal.h"

static GSHSCENE_Store * g_store;

void conf_stub_set_store(GSHSCENE_Store * store) {
  g_store = store;
}

static GVEC_Result draw_rect(GSHSCENE_List * list, double bottom, double red_level) {
  GSHGEOM_Path * path = NULL;
  const GSHSCENE_Resource * resource = NULL;
  if (gshgeom_path_create(NULL, NULL, 2, &path) != GSHGEOM_OK) {
    return GVEC_ERR_OOM;
  }
  const GSHGEOM_Point corners[4] = {{2.0, 2.0, 0.0}, {10.0, 2.0, 0.0}, {10.0, bottom, 0.0}, {2.0, bottom, 0.0}};
  gshgeom_path_move_to(path, corners[0]);
  for (int i = 1; i < 4; i++) {
    gshgeom_path_line_to(path, corners[i]);
  }
  gshgeom_path_close(path);
  GSHSCENE_Result r = gshscene_store_intern_path(g_store, path, &resource);
  gshgeom_path_destroy(path);
  if (r != GSHSCENE_OK) {
    return GVEC_ERR_INTERNAL;
  }
  GSHSCENE_Paint red;
  memset(&red, 0, sizeof red);
  red.kind = GSHSCENE_PAINT_SOLID;
  red.rgba[0] = red_level;
  red.rgba[3] = 1.0;
  r = gshscene_list_fill(list, resource, NULL, GSHGEOM_FILL_NONZERO, &red, 0, 0.0);
  gshscene_resource_release(resource);
  return r == GSHSCENE_OK ? GVEC_OK : GVEC_ERR_INTERNAL;
}

static GVEC_Result lower_exact(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics) {
  (void) doc;
  (void) options;
  (void) diagnostics;
  return draw_rect(list, 10.0, 1.0);
}

static GVEC_Result lower_exact_refused(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_List * list, GVEC_Diagnostics * diagnostics) {
  GVEC_Result r = lower_exact(doc, options, list, diagnostics);
  if (r == GVEC_OK && diagnostics) {
    r = gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LOWER_REFUSED, GVEC_LOWER_GRADIENT, 1, 1, 0,
        "rect", "refused by the planted lowering");
  }
  return r;
}

static GVEC_Result lower_off_by_one(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics) {
  (void) doc;
  (void) options;
  (void) diagnostics;
  return draw_rect(list, 11.0, 1.0);
}

static GVEC_Result lower_exact_dropped(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_List * list, GVEC_Diagnostics * diagnostics) {
  GVEC_Result r = lower_exact(doc, options, list, diagnostics);
  if (r == GVEC_OK && diagnostics) {
    diagnostics->dropped++;
  }
  return r;
}

static GVEC_Result lower_minus1(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics) {
  (void) doc;
  (void) options;
  (void) diagnostics;
  return draw_rect(list, 10.0, 254.0 / 255.0);
}

static GVEC_Result lower_minus2(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics) {
  (void) doc;
  (void) options;
  (void) diagnostics;
  return draw_rect(list, 10.0, 253.0 / 255.0);
}

/* The rectangle after a placeholder, which the sink reports as refused: a drawing the sink reports as not drawn, though the pixels
 * agree. (A knockout group was this stub's refusal until the sink composited them.) */
static GVEC_Result lower_sink_refused(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics) {
  (void) doc;
  (void) options;
  (void) diagnostics;
  if (gshscene_list_placeholder(list, GSHSCENE_PLACEHOLDER_SOURCE_COLR, GSHSCENE_PLACEHOLDER_COLR_UNBOUNDED, 0, NULL, NULL, 0, 0.0) !=
      GSHSCENE_OK) {
    return GVEC_ERR_INTERNAL;
  }
  return draw_rect(list, 10.0, 1.0);
}

static const struct {
  const char * name;
  Conf_Lower_Fn fn;
} k_stubs[] = {
  {"exact", lower_exact},
  {"exact-refused", lower_exact_refused},
  {"off-by-one", lower_off_by_one},
  {"exact-dropped", lower_exact_dropped},
  {"minus1", lower_minus1},
  {"minus2", lower_minus2},
  {"sink-refused", lower_sink_refused},
};

Conf_Lower_Fn conf_stub_find(const char * name) {
  for (size_t i = 0; i < sizeof k_stubs / sizeof k_stubs[0]; i++) {
    if (strcmp(k_stubs[i].name, name) == 0) {
      return k_stubs[i].fn;
    }
  }
  return NULL;
}
