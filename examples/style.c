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
 * @file style.c
 *
 * Read the style of an SVG document: the used values of the properties of its elements, and of an element drawn by a `use`.
 *
 *     style [FILE]
 *
 * With no FILE it reads a document built in below. An element is drawn in a place, and its style depends on the place: the rect inside
 * the `use` below is blue where it is written and red where the `use` draws it, because a clone inherits from the `use`. The program
 * checks what it shows and exits 1 when that does not hold.
 */

#include <stdio.h>
#include <string.h>

#include <ghoti.io/vector/vector.h>

static const char builtin[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" viewBox=\"0 0 100 50\">\n"
    "  <defs><g fill=\"blue\"><rect id=\"r\" width=\"50%\" height=\"20\" stroke-width=\"1in\"/></g></defs>\n"
    "  <g fill=\"red\"><use id=\"u\" href=\"#r\"/></g>\n"
    "</svg>\n";

static int g_failures;

static void expect(int condition, const char * what) {
  if (!condition) {
    fprintf(stderr, "style: expected %s\n", what);
    g_failures++;
  }
}

static void show_fill(const char * where, GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance) {
  GVEC_Svg_Style_Value fill, width, stroke_width;
  if (gvec_svg_get(context, instance, GVEC_SVG_PROP_FILL, &fill) != GVEC_OK ||
      gvec_svg_get(context, instance, GVEC_SVG_PROP_WIDTH, &width) != GVEC_OK ||
      gvec_svg_get(context, instance, GVEC_SVG_PROP_STROKE_WIDTH, &stroke_width) != GVEC_OK) {
    g_failures++;
    return;
  }
  const double * rgba = fill.paint.color.color.rgba;
  printf("%s: fill rgb(%.0f, %.0f, %.0f), width %g, stroke-width %g\n", where, rgba[0] * 255, rgba[1] * 255, rgba[2] * 255, width.number,
      stroke_width.number);
  expect(fill.paint.kind == GVEC_SVG_PAINT_COLOR, "the fill to be a color");
  expect(width.number == 50.0, "50% of the 100 unit wide viewBox to be 50");
  expect(stroke_width.number == 96.0, "an inch to be 96 user units");
}

int main(int argc, char ** argv) {
  GVEC_Stream * stream = NULL;
  GVEC_Doc * doc = NULL;
  GVEC_Diagnostics diagnostics;
  gvec_diagnostics_init(&diagnostics, NULL);
  GVEC_Result r = argc > 1 ? gvec_stream_create_file(argv[1], &stream) : gvec_stream_create_memory(builtin, strlen(builtin), &stream);
  if (r != GVEC_OK) {
    fprintf(stderr, "style: cannot open the input\n");
    return 1;
  }
  r = gvec_doc_load(stream, NULL, &diagnostics, &doc);
  gvec_stream_destroy(stream);
  if (r != GVEC_OK) {
    fprintf(stderr, "style: %s\n", gvec_result_string(r));
    gvec_diagnostics_destroy(&diagnostics);
    return 1;
  }
  const GVEC_Svg_Document * svg = gvec_svg_document(doc);
  GVEC_Svg_Style_Context * context = NULL;
  if (gvec_svg_style_context_new(svg, &diagnostics, NULL, &context) != GVEC_OK) {
    fprintf(stderr, "style: no context\n");
    return 1;
  }
  if (argc == 1) {
    const GVEC_Svg_Element * rect = gvec_svg_find_by_id(svg, "r");
    const GVEC_Svg_Element * use = gvec_svg_find_by_id(svg, "u");
    const GVEC_Svg_Instance * root = gvec_svg_instance_root(context);
    /* Where it is written: root, defs, g, rect. */
    const GVEC_Svg_Instance * written = root;
    const GVEC_Svg_Element * path[8];
    size_t n = 0;
    for (const GVEC_Svg_Element * e = rect; e && gvec_svg_element_parent(e); e = gvec_svg_element_parent(e)) {
      path[n++] = e;
    }
    while (n > 0) {
      const GVEC_Svg_Instance * child = NULL;
      expect(gvec_svg_instance_child(context, written, path[--n], &child) == GVEC_OK, "a child instance");
      written = child;
    }
    show_fill("written in defs", context, written);
    expect(gvec_svg_instance_element(written) == rect, "the instance of the rect");
    /* Where the use draws it. */
    const GVEC_Svg_Instance * g = NULL;
    const GVEC_Svg_Instance * use_instance = NULL;
    const GVEC_Svg_Instance * clone = NULL;
    expect(gvec_svg_instance_child(context, root, gvec_svg_element_parent(use), &g) == GVEC_OK, "the group");
    expect(gvec_svg_instance_child(context, g, use, &use_instance) == GVEC_OK, "the use");
    expect(gvec_svg_instance_use(context, use_instance, &clone) == GVEC_OK, "the clone");
    GVEC_Svg_Style_Value fill;
    expect(gvec_svg_get(context, clone, GVEC_SVG_PROP_FILL, &fill) == GVEC_OK, "the clone's fill");
    printf("drawn by the use: fill rgb(%.0f, %.0f, %.0f)\n", fill.paint.color.color.rgba[0] * 255, fill.paint.color.color.rgba[1] * 255,
        fill.paint.color.color.rgba[2] * 255);
    expect(fill.paint.color.color.rgba[0] == 1.0 && fill.paint.color.color.rgba[2] == 0.0, "the clone to inherit red from the use");
    GVEC_Svg_Style_Value written_fill;
    expect(gvec_svg_get(context, written, GVEC_SVG_PROP_FILL, &written_fill) == GVEC_OK, "the written fill");
    expect(written_fill.paint.color.color.rgba[2] == 1.0, "the rect written in the blue group to be blue");
  }
  printf("%zu diagnostics\n", diagnostics.count);
  gvec_svg_style_context_free(context);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&diagnostics);
  return g_failures ? 1 : 0;
}
