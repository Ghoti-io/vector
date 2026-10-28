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
 * Load a file and write the style of its elements to standard output: the program the style oracle
 * (tools/doc-oracle/style_oracle.py) reads.
 *
 *     style_dump FILE [WIDTH HEIGHT]
 *
 * One line for each (element with an id, property): the id, the property, a tag and the value in the tag's form, then the computed
 * value as the engine writes it. The elements are read in the document's own context; the referent of each `use` is read again in the
 * clone context, its elements named `USEID~ID` (and `USEID~INNERUSEID~ID` inside a clone's own uses), the names the oracle's reader of
 * `use` gives the copies it makes.
 *
 *     k TEXT                 a keyword: the computed value as text
 *     n NUMBER               a number
 *     l NUMBER | l auto      a length in user units
 *     c foreground | c srgb R G B A | c wide      a color: R, G and B in 0 to 255, A in 0 to 1
 *     p none | p color C | p url ID none | p url ID C | p context-fill | p context-stroke   a paint, C being a color's fields
 *     d NUMBER...            a dash array, empty for none
 *     t none origin X Y | t matrix A B C D E F origin X Y | t box     a transform, the list about (0, 0) and its origin
 *     r ITEM...              references, `url:ID`, `cut:ID` or `text:TEXT`; empty for none
 *     u                      the value cannot be used (reported)
 *     s                      a shorthand
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/vector/vector.h>

static GVEC_Svg_Style_Context * g_context;
static const GVEC_Svg_Document * g_document;

static void print_color(const GVEC_Svg_Color * c) {
  if (c->kind == GVEC_SVG_COLOR_FOREGROUND) {
    printf("foreground");
    return;
  }
  if (c->color.space.primaries != GCOL_PRIMARIES_SRGB || c->color.space.transfer != GCOL_TRANSFER_SRGB) {
    printf("wide");
    return;
  }
  printf("srgb %.17g %.17g %.17g %.17g", c->color.rgba[0] * 255.0, c->color.rgba[1] * 255.0, c->color.rgba[2] * 255.0, c->color.rgba[3]);
}

static void print_paint_kind(const GVEC_Svg_Paint * p) {
  switch (p->kind) {
  case GVEC_SVG_PAINT_NONE:
    printf("none");
    break;
  case GVEC_SVG_PAINT_COLOR:
    printf("color ");
    print_color(&p->color);
    break;
  case GVEC_SVG_PAINT_SERVER:
    printf("url %s ", p->server_id);
    if (!p->has_fallback) {
      printf("none");  /* the browsers write no fallback; the oracle's reader writes `none` for it */
    }
    else if (p->fallback_kind == GVEC_SVG_PAINT_NONE) {
      printf("none");
    }
    else {
      print_color(&p->fallback_color);
    }
    break;
  case GVEC_SVG_PAINT_CONTEXT_FILL:
    printf("context-fill");
    break;
  case GVEC_SVG_PAINT_CONTEXT_STROKE:
    printf("context-stroke");
    break;
  default:
    printf("?");
  }
}

static void print_element(const char * name, const GVEC_Svg_Instance * instance) {
  for (int p = 0; p < GVEC_SVG_PROP_COUNT; p++) {
    GVEC_Svg_Style_Value v;
    GVEC_Svg_Style_Specified spec;
    GVEC_Result r = gvec_svg_get(g_context, instance, (GVEC_Svg_Property) p, &v);
    printf("%s\t%s\t", name, gvec_svg_property_name((GVEC_Svg_Property) p));
    if (r != GVEC_OK) {
      printf("u\t\t");
    }
    else {
      switch (v.kind) {
      case GVEC_SVG_STYLE_NONE:
        printf("s");
        break;
      case GVEC_SVG_STYLE_KEYWORD:
        printf("k %s", v.keyword);
        break;
      case GVEC_SVG_STYLE_NUMBER:
        printf("n %.17g", v.number);
        break;
      case GVEC_SVG_STYLE_LENGTH:
        if (v.length_auto) {
          printf("l auto");
        }
        else {
          printf("l %.17g", v.number);
        }
        break;
      case GVEC_SVG_STYLE_COLOR:
        printf("c ");
        print_color(&v.color);
        break;
      case GVEC_SVG_STYLE_PAINT:
        printf("p ");
        print_paint_kind(&v.paint);
        break;
      case GVEC_SVG_STYLE_NUMBER_LIST:
        printf("d");
        for (size_t i = 0; i < v.number_count; i++) {
          printf(" %.17g", v.numbers[i]);
        }
        break;
      case GVEC_SVG_STYLE_TRANSFORM:
        if (v.transform.depends_on_box) {
          printf("t box");
        }
        else {
          /* The list about the origin (0, 0), which is what `getComputedStyle().transform` is, and the origin apart. */
          GVEC_Svg_Transform bare = v.transform;
          GSHGEOM_Map map;
          bare.origin[0] = bare.origin[1] = bare.origin_fraction[0] = bare.origin_fraction[1] = 0;
          bare.depends_on_box = true;
          if (v.transform.op_count == 0) {
            printf("t none origin %.17g %.17g", v.transform.origin[0], v.transform.origin[1]);
          }
          else if (gvec_svg_transform_resolve(&bare, 0, 0, 0, 0, &map) == GVEC_OK) {
            printf("t matrix %.17g %.17g %.17g %.17g %.17g %.17g origin %.17g %.17g", map.m[0][0], map.m[1][0], map.m[0][1], map.m[1][1], map.t[0],
                map.t[1], v.transform.origin[0], v.transform.origin[1]);
          }
          else {
            printf("t unresolvable");
          }
        }
        break;
      case GVEC_SVG_STYLE_REFERENCES:
        printf("r");
        for (size_t i = 0; i < v.reference_count; i++) {
          const GVEC_Svg_Style_Reference * ref = &v.references[i];
          if (ref->cut) {
            printf(" cut:%s", ref->id);
          }
          else if (ref->is_url && ref->local && ref->id) {
            printf(" url:%s", ref->id);
          }
          else {
            printf(" text:%s", ref->text);
          }
        }
        break;
      default:
        printf("?");
      }
      printf("\t");
      if (gvec_svg_get_specified(g_context, instance, (GVEC_Svg_Property) p, &spec) == GVEC_OK && spec.computed) {
        printf("%s", spec.computed);
      }
      printf("\t");
    }
    printf("\n");
  }
}

/* The elements of a subtree in a context, and the clones of the uses in it. `prefix` names the clone context ("" for the document's). */
static void walk(const GVEC_Svg_Instance * instance, const char * prefix, unsigned depth);

static void walk_children(const GVEC_Svg_Instance * instance, const char * prefix, unsigned depth) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(instance);
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(e); c; c = gvec_svg_element_next_sibling(c)) {
    const GVEC_Svg_Instance * child = NULL;
    if (gvec_svg_instance_child(g_context, instance, c, &child) == GVEC_OK) {
      walk(child, prefix, depth);
    }
  }
}

static void walk(const GVEC_Svg_Instance * instance, const char * prefix, unsigned depth) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(instance);
  const char * id = gvec_svg_element_id(e);
  char name[512];
  if (id) {
    snprintf(name, sizeof name, "%s%s", prefix, id);
    print_element(name, instance);
  }
  walk_children(instance, prefix, depth);
  /* A use's referent drawn in its clone context (not in the document context, where the referent is walked on its own). */
  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_USE && gvec_svg_element_href_target(e) && depth < 4 && !gvec_svg_element_on_cycle(e) &&
      !gvec_svg_element_expansion_limited(e)) {
    const GVEC_Svg_Instance * clone = NULL;
    if (gvec_svg_instance_use(g_context, instance, &clone) == GVEC_OK) {
      char inner[512];
      snprintf(inner, sizeof inner, "%s%s~", prefix, id ? id : "");
      if (id) {
        walk(clone, inner, depth + 1);
      }
    }
  }
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: style_dump FILE [WIDTH HEIGHT]\n");
    return 2;
  }
  GVEC_Stream * stream = NULL;
  GVEC_Doc * doc = NULL;
  GVEC_Diagnostics diagnostics;
  GVEC_Load_Options options;
  memset(&options, 0, sizeof options);
  if (argc >= 4) {
    options.viewport_width = atof(argv[2]);
    options.viewport_height = atof(argv[3]);
  }
  gvec_diagnostics_init(&diagnostics, NULL);
  if (gvec_stream_create_file(argv[1], &stream) != GVEC_OK) {
    fprintf(stderr, "style_dump: cannot read %s\n", argv[1]);
    return 2;
  }
  GVEC_Result r = gvec_doc_load(stream, &options, &diagnostics, &doc);
  gvec_stream_destroy(stream);
  if (r != GVEC_OK) {
    fprintf(stderr, "style_dump: %s\n", gvec_result_string(r));
    gvec_diagnostics_destroy(&diagnostics);
    return 1;
  }
  g_document = gvec_svg_document(doc);
  if (gvec_svg_style_context_new(g_document, &diagnostics, NULL, &g_context) != GVEC_OK) {
    fprintf(stderr, "style_dump: no context\n");
    return 1;
  }
  gvec_svg_style_resolve_references(g_context);
  walk(gvec_svg_instance_root(g_context), "", 0);
  gvec_svg_style_context_free(g_context);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&diagnostics);
  return 0;
}
