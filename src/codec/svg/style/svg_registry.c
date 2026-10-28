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
 * @file svg_registry.c
 *
 * The property view (C1, C2): the registry of `text`, with the properties the SVG 2 CR has and the pin lacks
 * registered, the ones the pin defines differently overridden (each reported), and the presentation attribute table
 * extended with the two attributes of the CR's list that the served drafts dropped.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "svg_style.h"

/* The presentation attributes of the CR's list (chapter 6.6) that the table of `text`, which follows the served draft, does not
 * have. Each sets the property of the same name on every element in the SVG namespace. */
static const char * const k_added_attributes[] = {"color-rendering", "glyph-orientation-horizontal"};
#define ADDED_COUNT (sizeof k_added_attributes / sizeof k_added_attributes[0])

static GVEC_Result translate(GTEXT_CSS_Status status) {
  return status == GTEXT_CSS_E_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
}

GVEC_Svg_Property_Source gvec_svg_property_source(GVEC_Svg_Property property) {
  if ((unsigned) property >= GVEC_SVG_PROP_COUNT) {
    return GVEC_SVG_PROPERTY_GENERATED;
  }
  return (GVEC_Svg_Property_Source) gvec_svg_prop_rows[property].source;
}

static GVEC_Result build_table(const GVEC_Allocator * allocator, gvec_svg_registry * out) {
  const GTEXT_CSS_Presentation_Table * base = gtext_css_presentation_table_svg2();
  size_t count = base->count + ADDED_COUNT;
  GTEXT_CSS_Presentation_Row * rows = gvec_alloc(allocator, count * sizeof *rows);
  if (!rows) {
    return GVEC_ERR_OOM;
  }
  memcpy(rows, base->rows, base->count * sizeof *rows);
  for (size_t i = 0; i < ADDED_COUNT; i++) {
    GTEXT_CSS_Presentation_Row * row = &rows[base->count + i];
    memset(row, 0, sizeof *row);
    row->attribute = k_added_attributes[i];
    row->property = k_added_attributes[i];
    row->svg_namespace_only = true;
    row->elements = NULL;
    row->syntax = GTEXT_CSS_PRESENTATION_CSS;
  }
  out->rows = rows;
  out->row_count = count;
  out->table.rows = rows;
  out->table.count = count;
  return GVEC_OK;
}

GVEC_Result gvec_svg_registry_new(const GVEC_Allocator * allocator, gvec_svg_registry * out) {
  GTEXT_CSS_Registry_Options options;
  GTEXT_CSS_Registry_Builder * builder = NULL;
  GTEXT_CSS_Registry_Result result;
  GTEXT_CSS_Status status;
  GVEC_Result r;
  memset(out, 0, sizeof *out);
  out->allocator = allocator;
  memset(&options, 0, sizeof options);
  options.allocator = allocator;
  status = gtext_css_registry_builder_new(&options, &builder);
  if (status != GTEXT_CSS_OK) {
    return translate(status);
  }
  for (size_t i = 0; i < GVEC_SVG_PROP_COUNT; i++) {
    const gvec_svg_prop_row * row = &gvec_svg_prop_rows[i];
    if (row->source == GVEC_SVG_SOURCE_GENERATED) {
      continue;
    }
    GTEXT_CSS_Property_Def def;
    memset(&def, 0, sizeof def);
    def.name = row->name;
    def.inherited = row->inherits;
    def.initial = row->initial;
    def.grammar = row->grammar;
    /* A grammar of keywords alone is its own computed value; anything else is computed by the types it is made of. */
    def.computed = strchr(row->grammar, '<') ? GTEXT_CSS_COMPUTED_UNSPECIFIED : GTEXT_CSS_COMPUTED_AS_SPECIFIED;
    def.override = row->source == GVEC_SVG_SOURCE_OVERRIDDEN;
    memset(&result, 0, sizeof result);
    status = gtext_css_registry_register_property(builder, &def, &result);
    if (status != GTEXT_CSS_OK) {
      gtext_css_registry_builder_free(builder);
      return translate(status);
    }
  }
  status = gtext_css_registry_freeze(builder, &out->registry, &result);
  if (status != GTEXT_CSS_OK) {
    gtext_css_registry_builder_free(builder);
    return translate(status);
  }
  for (size_t i = 0; i < GVEC_SVG_PROP_COUNT; i++) {
    const gvec_svg_prop_row * row = &gvec_svg_prop_rows[i];
    GTEXT_CSS_Property_Info info;
    GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(out->registry, row->name, strlen(row->name));
    GTEXT_CSS_Property_Id reads = gtext_css_registry_find_property(out->registry, row->reads, strlen(row->reads));
    if (id == GTEXT_CSS_PROPERTY_NONE || reads == GTEXT_CSS_PROPERTY_NONE || !gtext_css_registry_property_info(out->registry, id, &info)) {
      /* The pin no longer has a property this library names: its generated layer changed under it. */
      gvec_svg_registry_free(out);
      return GVEC_ERR_INTERNAL;
    }
    /* The registered layer holds what the pin lacks and overrides what it has differently; the generated bit says which. */
    bool generated = (info.flags & GTEXT_CSS_PROP_GENERATED) != 0;
    bool overrides = (info.flags & GTEXT_CSS_PROP_OVERRIDE) != 0;
    bool ok = row->source == GVEC_SVG_SOURCE_GENERATED ? generated && !overrides
              : row->source == GVEC_SVG_SOURCE_REGISTERED ? !generated && !overrides
                                                           : overrides;
    if (!ok) {
      gvec_svg_registry_free(out);
      return GVEC_ERR_INTERNAL;
    }
    out->id[i] = gtext_css_registry_resolve_alias(out->registry, id);
    out->reads[i] = gtext_css_registry_resolve_alias(out->registry, reads);
  }
  out->color_scheme = gtext_css_registry_find_property(out->registry, "color-scheme", 12);
  if (out->color_scheme == GTEXT_CSS_PROPERTY_NONE) {
    gvec_svg_registry_free(out);
    return GVEC_ERR_INTERNAL;
  }
  r = build_table(allocator, out);
  if (r != GVEC_OK) {
    gvec_svg_registry_free(out);
    return r;
  }
  return GVEC_OK;
}

void gvec_svg_registry_free(gvec_svg_registry * registry) {
  if (!registry) {
    return;
  }
  gtext_css_registry_free(registry->registry);
  gvec_free(registry->allocator, registry->rows);
  memset(registry, 0, sizeof *registry);
}
