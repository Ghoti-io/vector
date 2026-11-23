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
 * @file lower_text_area.c
 *
 * The wrapping area of a text that flows into shapes (CSS Shapes 2's `shape-inside`, `shape-subtract`, `shape-padding` and `shape-margin`, which SVG 2's text
 * wrapping names): the shapes are flattened to polygons once, and then asked, for a line box between two heights, where along the line the box can be put:
 * the stretches of it that lie inside the shape in every height of the box and outside every subtracted shape in every height of it.
 *
 * The padding insets the shape and the margin outsets each subtracted one by the distance, as a disc is moved along the line box: the box rounded by that
 * distance lies inside the shape (or clear of the subtracted one). What is measured is the cross section of the polygons at the heights between each two
 * vertices (where the edges are straight, so that the narrowest and the widest section of a range are at its ends) and, in the rounded part of the box, at
 * the heights of a regular sampling that rounds the way up (a stretch is left out, never added).
 *
 * The polygons are in the frame the text is laid out in: the shapes of vertical text are turned a quarter (and mirrored for `vertical-lr`), so that the
 * bands of the layout, which are horizontal in the frame, are the columns of the page.
 */

#include <ghoti.io/vector/macros.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/math/basic.h>
#include <ghoti.io/shape-geometry/flatten.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/resource.h>

#include "../svg_grammar.h"
#include "lower_internal.h"
#include "lower_text_types.h"

#define AREA_POINTS_MAX ((size_t) 1 << 16) ///< How many vertices the shapes of one text have between them.
#define AREA_SHAPES_MAX 16u                ///< How many shapes of `shape-subtract` a text keeps.
#define AREA_ZONE_STEPS 8                  ///< The steps of the rounded part of a padded box on each side.
#define PRECISION 16.0                     ///< The flattener works in 1/64 of a unit: the shapes are scaled up by this first, so that its steps are 1/1024.

typedef struct tpoly {
  const double * pts; ///< x, y pairs; the polygon is closed.
  size_t count;       ///< The points.
} tpoly;

typedef struct tregion {
  tpoly * polys;
  size_t poly_count;
  bool even_odd;
} tregion;

struct tarea {
  tregion inside;
  tregion subtract[AREA_SHAPES_MAX];
  size_t subtract_count;
  double padding, margin;
  double top, bottom;   ///< The extent of the shape the text flows into.
  double * ys;          ///< The heights of all vertices, sorted: where a section's topology can change.
  size_t y_count;
  struct cross {
    double x;
    int dir;
  } * crossings;
  size_t crossing_capacity;
  double * sample_ys, * sample_w; ///< Room for the heights a band is sampled at.
  size_t sample_capacity;
};

static int compare_double(const void * a, const void * b) {
  const double x = *(const double *) a, y = *(const double *) b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int compare_cross(const void * a, const void * b) {
  const double x = ((const struct cross *) a)->x, y = ((const struct cross *) b)->x;
  return x < y ? -1 : x > y ? 1 : 0;
}

//-----------------------------------------------------------------------------
// Loading the shapes
//-----------------------------------------------------------------------------

/* The polygons of `path` in the frame of the text: the path under `user` (the element's own transform, when it has one) and then moved by (ox, oy). */
static GVEC_Result region_of_path(tctx * t, const GSHGEOM_Path * path, const GSHGEOM_Map * user, double ox, double oy, bool even_odd, size_t * budget, tregion * out) {
  GSHGEOM_Map shift, scale, moved, whole;
  if (gshgeom_map_translate(2, ox, oy, 0, &shift) != GSHGEOM_OK || gshgeom_map_scale(2, PRECISION, PRECISION, 1, &scale) != GSHGEOM_OK) {
    return gvec_lw_refuse(t->lw, gvec_svg_instance_element(t->styles[0].inst), GVEC_LOWER_LIMIT, "a shape of a wrapped text is past the range the waist can carry");
  }
  if (user) {
    if (!gvec_lw_compose(&shift, user, &moved) || !gvec_lw_compose(&scale, &moved, &whole)) {
      return gvec_lw_refuse(t->lw, gvec_svg_instance_element(t->styles[0].inst), GVEC_LOWER_LIMIT, "a shape of a wrapped text is past the range the waist can carry");
    }
  }
  else if (!gvec_lw_compose(&scale, &shift, &whole)) {
    return gvec_lw_refuse(t->lw, gvec_svg_instance_element(t->styles[0].inst), GVEC_LOWER_LIMIT, "a shape of a wrapped text is past the range the waist can carry");
  }
  GSHGEOM_Flat_Path * flat = NULL;
  GSHGEOM_Result fr = gshgeom_path_flatten(path, &whole, NULL, NULL, &flat);
  if (fr != GSHGEOM_OK) {
    return fr == GSHGEOM_ERR_OOM ? GVEC_ERR_OOM : gvec_lw_refuse(t->lw, gvec_svg_instance_element(t->styles[0].inst), GVEC_LOWER_LIMIT, "a shape of a wrapped text has more points than the lowering keeps");
  }
  const size_t subpaths = gshgeom_flat_path_subpath_count(flat);
  const size_t points = gshgeom_flat_path_point_count(flat);
  const GSHGEOM_Device_Point * pts = gshgeom_flat_path_points(flat);
  GVEC_Result r = GVEC_OK;
  if (points > *budget) {
    gshgeom_flat_path_destroy(flat);
    return gvec_lw_refuse(t->lw, gvec_svg_instance_element(t->styles[0].inst), GVEC_LOWER_LIMIT, "the shapes of a wrapped text have more points than the lowering keeps");
  }
  *budget -= points;
  out->even_odd = even_odd;
  out->polys = gvec_arena_array(&t->lw->arena, subpaths ? subpaths : 1, sizeof *out->polys, sizeof(void *));
  double * numbers = gvec_arena_array(&t->lw->arena, points * 2 + 2, sizeof *numbers, sizeof *numbers);
  if (!out->polys || !numbers) {
    r = GVEC_ERR_OOM;
  }
  for (size_t s = 0; r == GVEC_OK && s < subpaths; s++) {
    GSHGEOM_Flat_Subpath sub;
    if (gshgeom_flat_path_subpath(flat, s, &sub) != GSHGEOM_OK || sub.count < 3) {
      continue; /* a line or a point has no inside */
    }
    double * at = numbers + 2 * sub.first;
    for (size_t k = 0; k < sub.count; k++) {
      const double x = (double) pts[sub.first + k].x / (64.0 * PRECISION);
      const double y = (double) pts[sub.first + k].y / (64.0 * PRECISION);
      /* The frame of the layout: vertical text is turned a quarter, so that the page's columns are the layout's lines; `vertical-lr` is mirrored along them. */
      double fx = x, fy = y;
      if (t->vertical) {
        fx = y;
        fy = -x;
      }
      if (t->vertical_lr) {
        fy = -fy;
      }
      at[2 * k] = fx;
      at[2 * k + 1] = fy;
    }
    out->polys[out->poly_count].pts = at;
    out->polys[out->poly_count].count = sub.count;
    out->poly_count++;
  }
  gshgeom_flat_path_destroy(flat);
  return r;
}

/* The region a reference of `shape-inside` or `shape-subtract` names: an element of this document that is a shape, or a basic shape. `*kind`: 0 a region, 1 nothing
 * (a value that is not one: it is as if not given), 2 a region with no area. */
static GVEC_Result region_of_item(tctx * t, const GVEC_Svg_Instance * inst, const GVEC_Svg_Style_Reference * item, size_t * budget, tregion * out, int * kind) {
  gvec_lw_ctx * ctx = t->lw;
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  *kind = 1;
  memset(out, 0, sizeof *out);
  if (!item->text) {
    return GVEC_OK;
  }
  if (item->is_url) {
    const GVEC_Svg_Element * target = item->target;
    if (!target || item->cut) {
      return GVEC_OK; /* a reference to nothing is no shape */
    }
    const GVEC_Svg_Instance * target_inst = NULL;
    GVEC_Result r = gvec_lw_instance_of(ctx, target, &target_inst);
    if (r != GVEC_OK) {
      return r;
    }
    gvec_lw_geometry geometry;
    const GSHSCENE_Resource * resource = NULL;
    const GVEC_Svg_Kind kind_of = gvec_svg_element_kind(target);
    if (kind_of == GVEC_SVG_KIND_RECT || kind_of == GVEC_SVG_KIND_CIRCLE || kind_of == GVEC_SVG_KIND_ELLIPSE) {
      bool usable = true;
      r = gvec_lw_primitive_of(ctx, target_inst, &geometry, &resource, &usable);
      if (r == GVEC_OK && !usable) {
        t->refused = true;
        return GVEC_OK; /* the shape was refused (and reported) */
      }
    }
    else if (kind_of == GVEC_SVG_KIND_PATH || kind_of == GVEC_SVG_KIND_POLYGON) {
      r = gvec_lw_path_of(ctx, target, NULL, false, &geometry, &resource);
    }
    else {
      return GVEC_OK; /* what is not a shape is no area */
    }
    if (r != GVEC_OK) {
      return r;
    }
    if (geometry == GVEC_LW_GEOMETRY_LIMIT) {
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the shape a wrapped text flows into is past what the waist can carry");
    }
    if (geometry == GVEC_LW_GEOMETRY_EMPTY || !resource) {
      *kind = 2;
      return GVEC_OK;
    }
    /* The shape is under its own transform (the text's is not part of it: the shape's space is the text's). */
    GVEC_Svg_Style_Value tv;
    bool usable = false;
    r = gvec_lw_read_property(ctx, target_inst, GVEC_SVG_PROP_TRANSFORM, GVEC_LOWER_STYLE, &tv, &usable);
    if (r != GVEC_OK || !usable) {
      t->refused = true;
      return r;
    }
    GSHGEOM_Map own = tv.transform.map;
    if (tv.transform.depends_on_box) {
      bool known;
      r = gvec_lw_box_transform(ctx, target_inst, &tv.transform, &own, &known);
      if (r != GVEC_OK) {
        return r;
      }
      if (!known) {
        t->refused = true;
        return gvec_lw_refuse(ctx, e, GVEC_LOWER_TRANSFORM_BOX, "a wrapped text flows into a shape whose transform has percentages of a bounding box that the lowering cannot make");
      }
    }
    GVEC_Svg_Style_Value rule;
    r = gvec_lw_read_property(ctx, target_inst, GVEC_SVG_PROP_FILL_RULE, GVEC_LOWER_STYLE, &rule, &usable);
    if (r != GVEC_OK || !usable) {
      t->refused = true;
      return r;
    }
    r = region_of_path(t, gshscene_resource_path(resource), &own, 0, 0, gvec_lw_keyword_is(&rule, "evenodd"), budget, out);
    *kind = r == GVEC_OK ? 0 : 1;
    return r;
  }
  /* A basic shape, against the nearest viewport unless it names a box of its own (the text has no box before it is laid out). */
  const char * text = item->text;
  if (strstr(text, "-box") == NULL) {
    const size_t length = strlen(text);
    char * boxed = gvec_arena_array(&ctx->arena, length + 10, 1, 1);
    if (!boxed) {
      return GVEC_ERR_OOM;
    }
    memcpy(boxed, text, length);
    memcpy(boxed + length, " view-box", 10);
    text = boxed;
  }
  gvec_lw_shape shape;
  GVEC_Result r = gvec_lw_basic_shape(ctx, inst, text, &shape);
  if (r != GVEC_OK) {
    return r;
  }
  switch (shape.kind) {
  case GVEC_LW_SHAPE_PATH:
    r = region_of_path(t, gshscene_resource_path(shape.path), NULL, shape.ox, shape.oy, shape.rule == GSHGEOM_FILL_EVEN_ODD, budget, out);
    *kind = r == GVEC_OK ? 0 : 1;
    return r;
  case GVEC_LW_SHAPE_ALL:
    *kind = 2;
    return GVEC_OK;
  case GVEC_LW_SHAPE_REFUSED:
    t->refused = true;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_TEXT, shape.why ? shape.why : "a shape of a wrapped text that this does not read");
  case GVEC_LW_SHAPE_ERROR:
    t->refused = true;
    return GVEC_OK; /* reported */
  default:
    return GVEC_OK; /* not a shape: as if it were not given */
  }
}

static void bound_region(const tregion * region, double * lo, double * hi, bool * any) {
  for (size_t p = 0; p < region->poly_count; p++) {
    for (size_t k = 0; k < region->polys[p].count; k++) {
      const double y = region->polys[p].pts[2 * k + 1];
      if (!*any || y < *lo) {
        *lo = y;
      }
      if (!*any || y > *hi) {
        *hi = y;
      }
      *any = true;
    }
  }
}

GVEC_Result gvec_lw_text_area_make(tctx * t, const GVEC_Svg_Instance * inst, tarea ** out) {
  gvec_lw_ctx * ctx = t->lw;
  *out = NULL;
  GVEC_Svg_Style_Value v;
  bool ok;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_SHAPE_INSIDE, GVEC_LOWER_STYLE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  /* `auto` and `none` are no shape; the others are the reference of one (one only: the text flows into a single shape). */
  const GVEC_Svg_Style_Reference * inside_item = NULL;
  for (size_t i = 0; i < v.reference_count; i++) {
    const char * text = v.references[i].text;
    if (!text || strcmp(text, "auto") == 0 || strcmp(text, "none") == 0) {
      continue;
    }
    if (inside_item) {
      t->refused = true;
      return gvec_lw_refuse(ctx, gvec_svg_instance_element(inst), GVEC_LOWER_TEXT, "a shape-inside of more than one shape (the text would flow from one into the next)");
    }
    inside_item = &v.references[i];
  }
  if (!inside_item) {
    return GVEC_OK;
  }
  if (!inside_item->is_url && (strstr(inside_item->text, "(") == NULL)) {
    t->refused = true;
    return gvec_lw_refuse(ctx, gvec_svg_instance_element(inst), GVEC_LOWER_TEXT, "a shape-inside of a value this does not read (outside-shape, an image, display)");
  }
  tarea * area = gvec_arena_array(&ctx->arena, 1, sizeof *area, sizeof(void *));
  if (!area) {
    return GVEC_ERR_OOM;
  }
  size_t budget = AREA_POINTS_MAX;
  int kind;
  r = region_of_item(t, inst, inside_item, &budget, &area->inside, &kind);
  if (r != GVEC_OK || t->refused) {
    return r;
  }
  if (kind == 1) {
    return GVEC_OK; /* a reference to nothing, or to what is not a shape: as if not given (CSS: the property is invalid at computed-value time) */
  }
  /* `shape-subtract`: the shapes the text does not go into. */
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_SHAPE_SUBTRACT, GVEC_LOWER_STYLE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  for (size_t i = 0; i < v.reference_count; i++) {
    const char * text = v.references[i].text;
    if (!text || strcmp(text, "none") == 0) {
      continue;
    }
    if (area->subtract_count == AREA_SHAPES_MAX) {
      t->refused = true;
      return gvec_lw_refuse(ctx, gvec_svg_instance_element(inst), GVEC_LOWER_LIMIT, "a wrapped text subtracts more shapes than the lowering keeps");
    }
    int sub_kind;
    r = region_of_item(t, inst, &v.references[i], &budget, &area->subtract[area->subtract_count], &sub_kind);
    if (r != GVEC_OK || t->refused) {
      return r;
    }
    if (sub_kind == 0) {
      area->subtract_count++;
    }
  }
  /* `shape-padding` and `shape-margin`: lengths (a percentage is of the viewport's width). */
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_SHAPE_PADDING, GVEC_LOWER_STYLE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  if (!gvec_lw_text_spacing_of(inst, &v, t->styles[0].size, false, &area->padding) || !(area->padding > 0)) {
    area->padding = 0;
  }
  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_SHAPE_MARGIN, GVEC_LOWER_STYLE, &v, &ok);
  if (r != GVEC_OK || !ok) {
    t->refused = true;
    return r;
  }
  if (!gvec_lw_text_spacing_of(inst, &v, t->styles[0].size, false, &area->margin) || !(area->margin > 0)) {
    area->margin = 0;
  }
  /* The extent, the heights where a section can change, and room for the crossings of a scan line. */
  bool any = false;
  double lo = 0, hi = 0;
  bound_region(&area->inside, &lo, &hi, &any);
  area->top = lo;
  area->bottom = hi;
  size_t points = 0, edges = 0;
  const tregion * all[1 + AREA_SHAPES_MAX];
  size_t region_count = 0;
  all[region_count++] = &area->inside;
  for (size_t s = 0; s < area->subtract_count; s++) {
    all[region_count++] = &area->subtract[s];
  }
  for (size_t g = 0; g < region_count; g++) {
    for (size_t p = 0; p < all[g]->poly_count; p++) {
      points += all[g]->polys[p].count;
      edges += all[g]->polys[p].count;
    }
  }
  area->ys = gvec_arena_array(&ctx->arena, points + 1, sizeof *area->ys, sizeof(double));
  area->crossings = gvec_arena_array(&ctx->arena, edges + 1, sizeof *area->crossings, sizeof(double));
  if (!area->ys || !area->crossings) {
    return GVEC_ERR_OOM;
  }
  area->crossing_capacity = edges + 1;
  area->sample_capacity = points + 2 * AREA_ZONE_STEPS + 4;
  area->sample_ys = gvec_arena_array(&ctx->arena, area->sample_capacity, sizeof *area->sample_ys, sizeof(double));
  area->sample_w = gvec_arena_array(&ctx->arena, area->sample_capacity, sizeof *area->sample_w, sizeof(double));
  if (!area->sample_ys || !area->sample_w) {
    return GVEC_ERR_OOM;
  }
  for (size_t g = 0; g < region_count; g++) {
    for (size_t p = 0; p < all[g]->poly_count; p++) {
      for (size_t k = 0; k < all[g]->polys[p].count; k++) {
        area->ys[area->y_count++] = all[g]->polys[p].pts[2 * k + 1];
      }
    }
  }
  qsort(area->ys, area->y_count, sizeof *area->ys, compare_double);
  *out = area;
  return GVEC_OK;
}

void gvec_lw_text_area_extent(const tarea * area, double * top, double * bottom) {
  *top = area->top + area->padding;
  *bottom = area->bottom - area->padding;
}

//-----------------------------------------------------------------------------
// Stretches of a line
//-----------------------------------------------------------------------------

#define SPANS_MAX 128u

typedef struct spans {
  tspan s[SPANS_MAX];
  size_t n;
} spans;

/* The stretches of the line `y` that are inside the region. */
static void section(tarea * area, const tregion * region, double y, spans * out) {
  size_t count = 0;
  struct cross * c = area->crossings;
  for (size_t p = 0; p < region->poly_count; p++) {
    const double * pts = region->polys[p].pts;
    const size_t n = region->polys[p].count;
    for (size_t k = 0; k < n; k++) {
      const double x0 = pts[2 * k], y0 = pts[2 * k + 1];
      const size_t next = k + 1 == n ? 0 : k + 1;
      const double x1 = pts[2 * next], y1 = pts[2 * next + 1];
      if ((y0 <= y && y < y1) || (y1 <= y && y < y0)) {
        if (count < area->crossing_capacity) {
          c[count].x = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
          c[count].dir = y1 > y0 ? 1 : -1;
          count++;
        }
      }
    }
  }
  qsort(c, count, sizeof *c, compare_cross);
  out->n = 0;
  int winding = 0;
  for (size_t i = 0; i < count; i++) {
    const bool was = region->even_odd ? (winding & 1) != 0 : winding != 0;
    winding += region->even_odd ? 1 : c[i].dir;
    const bool now = region->even_odd ? (winding & 1) != 0 : winding != 0;
    if (!was && now) {
      if (out->n < SPANS_MAX) {
        out->s[out->n].from = c[i].x;
        out->s[out->n].to = c[i].x;
        out->n++;
      }
    }
    else if (was && !now && out->n > 0) {
      out->s[out->n - 1].to = c[i].x;
    }
  }
}

/* The stretches that `a` and `b` have in common. */
static void intersect(const spans * a, const spans * b, spans * out) {
  out->n = 0;
  size_t i = 0, j = 0;
  while (i < a->n && j < b->n) {
    const double from = a->s[i].from > b->s[j].from ? a->s[i].from : b->s[j].from;
    const double to = a->s[i].to < b->s[j].to ? a->s[i].to : b->s[j].to;
    if (from < to && out->n < SPANS_MAX) {
      out->s[out->n].from = from;
      out->s[out->n].to = to;
      out->n++;
    }
    if (a->s[i].to < b->s[j].to) {
      i++;
    }
    else {
      j++;
    }
  }
}

/* The stretches narrowed by `w` at each end (the ones that nothing is left of are gone). */
static void narrow(spans * a, double w) {
  size_t k = 0;
  for (size_t i = 0; i < a->n; i++) {
    const double from = a->s[i].from + w, to = a->s[i].to - w;
    if (from < to) {
      a->s[k].from = from;
      a->s[k].to = to;
      k++;
    }
  }
  a->n = k;
}

/* `a` and `b` as one set of stretches: the union, sorted and merged. */
static void unite(spans * a, const spans * b) {
  spans all;
  all.n = 0;
  for (size_t i = 0; i < a->n && all.n < SPANS_MAX; i++) {
    all.s[all.n++] = a->s[i];
  }
  for (size_t i = 0; i < b->n && all.n < SPANS_MAX; i++) {
    all.s[all.n++] = b->s[i];
  }
  for (size_t i = 1; i < all.n; i++) { /* insertion sort: the lists are short */
    const tspan key = all.s[i];
    size_t j = i;
    while (j > 0 && all.s[j - 1].from > key.from) {
      all.s[j] = all.s[j - 1];
      j--;
    }
    all.s[j] = key;
  }
  a->n = 0;
  for (size_t i = 0; i < all.n; i++) {
    if (a->n > 0 && all.s[i].from <= a->s[a->n - 1].to) {
      if (all.s[i].to > a->s[a->n - 1].to) {
        a->s[a->n - 1].to = all.s[i].to;
      }
    }
    else {
      a->s[a->n++] = all.s[i];
    }
  }
}

/* What is left of `a` once `b` is taken out of it. */
static void subtract(const spans * a, const spans * b, spans * out) {
  out->n = 0;
  for (size_t i = 0; i < a->n; i++) {
    double from = a->s[i].from;
    const double to = a->s[i].to;
    for (size_t j = 0; j < b->n && from < to; j++) {
      if (b->s[j].to <= from || b->s[j].from >= to) {
        continue;
      }
      if (b->s[j].from > from && out->n < SPANS_MAX) {
        out->s[out->n].from = from;
        out->s[out->n].to = b->s[j].from;
        out->n++;
      }
      from = b->s[j].to > from ? b->s[j].to : from;
    }
    if (from < to && out->n < SPANS_MAX) {
      out->s[out->n].from = from;
      out->s[out->n].to = to;
      out->n++;
    }
  }
}

/* The heights at which a region of `reach` (padding or margin) is sampled for a box from `top` to `bottom`: the ends of the box, the vertices between them, and the
 * rounded part above and below (the box moved by a disc of radius `reach`); each with the distance the disc leaves at it. The count. */
static size_t sample_heights(const tarea * area, double top, double bottom, double reach, double * ys, double * w, size_t cap) {
  size_t n = 0;
#define PUSH(Y, W) \
  do { \
    if (n < cap) { \
      ys[n] = (Y); \
      w[n] = (W); \
      n++; \
    } \
  } while (0)
  if (reach > 0) {
    for (int k = 0; k < AREA_ZONE_STEPS; k++) {
      const double d = reach * (double) (AREA_ZONE_STEPS - k) / (double) AREA_ZONE_STEPS; /* how far above the box */
      PUSH(top - d, gmath_sqrt(reach * reach - d * d));
    }
  }
  PUSH(top, reach);
  /* the vertices between the ends: the first of them past `top`, by a search of the sorted heights */
  size_t lo = 0, hi = area->y_count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (area->ys[mid] <= top) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }
  for (size_t i = lo; i < area->y_count && area->ys[i] < bottom; i++) {
    if (n == 0 || area->ys[i] > ys[n - 1]) {
      PUSH(area->ys[i], reach);
    }
  }
  if (bottom > top) {
    PUSH(bottom, reach);
  }
  if (reach > 0) {
    for (int k = 1; k <= AREA_ZONE_STEPS; k++) {
      const double d = reach * (double) k / (double) AREA_ZONE_STEPS;
      PUSH(bottom + d, gmath_sqrt(reach * reach - d * d));
    }
  }
#undef PUSH
  return n;
}

/* Where along the lines from `top` to `bottom` a box of that height can go, for a region: `inside` takes the stretches that are in the region at every height of the
 * box, narrowed by `reach` (and the rounding of the box); otherwise the ones the region (grown by `reach`) takes at any height. */
static void region_band(tarea * area, const tregion * region, bool inside, double top, double bottom, double reach, spans * out) {
  double * ys = area->sample_ys, * w = area->sample_w;
  const size_t n = sample_heights(area, top, bottom, reach, ys, w, area->sample_capacity);
  const size_t pairs = n > 1 ? n - 1 : n;
  out->n = 0;
  bool first = true;
  for (size_t i = 0; i < pairs; i++) {
    spans a, b, c;
    const bool single = n == 1;
    const double ya = ys[i], yb = single ? ys[i] : ys[i + 1];
    const double delta = single ? 0 : (yb - ya) * 1e-7;
    if (!single && !(ya < yb)) {
      continue;
    }
    const double reach_here = single ? w[i] : (w[i] > w[i + 1] ? w[i] : w[i + 1]);
    section(area, region, ya + delta, &a);
    section(area, region, yb - delta, &b);
    if (inside) {
      intersect(&a, &b, &c);
      narrow(&c, reach_here);
      if (first) {
        *out = c;
        first = false;
      }
      else {
        spans merged;
        intersect(out, &c, &merged);
        *out = merged;
      }
    }
    else {
      /* The stretch the region takes over the range is the hull of its ends where they match, else both of them. */
      spans hull;
      hull.n = 0;
      if (a.n == b.n) {
        for (size_t k = 0; k < a.n; k++) {
          hull.s[hull.n].from = (a.s[k].from < b.s[k].from ? a.s[k].from : b.s[k].from) - reach_here;
          hull.s[hull.n].to = (a.s[k].to > b.s[k].to ? a.s[k].to : b.s[k].to) + reach_here;
          hull.n++;
        }
      }
      else {
        hull = a;
        unite(&hull, &b);
        for (size_t k = 0; k < hull.n; k++) {
          hull.s[k].from -= reach_here;
          hull.s[k].to += reach_here;
        }
      }
      if (first) {
        *out = hull;
        first = false;
      }
      else {
        unite(out, &hull);
      }
    }
  }
}

GVEC_Result gvec_lw_text_area_band(tctx * t, tarea * area, double top, double bottom, tspan * out, size_t cap, size_t * count) {
  (void) t;
  spans allowed;
  region_band(area, &area->inside, true, top, bottom, area->padding, &allowed);
  for (size_t s = 0; s < area->subtract_count && allowed.n > 0; s++) {
    spans forbidden, left;
    region_band(area, &area->subtract[s], false, top, bottom, area->margin, &forbidden);
    subtract(&allowed, &forbidden, &left);
    allowed = left;
  }
  *count = 0;
  for (size_t i = 0; i < allowed.n && *count < cap; i++) {
    if (allowed.s[i].to > allowed.s[i].from) {
      out[(*count)++] = allowed.s[i];
    }
  }
  return GVEC_OK;
}
