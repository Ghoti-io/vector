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
 * The build pass: the XML parser's tree into the typed document (svg_doc.h).
 */

#include <ghoti.io/vector/macros.h>

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/text/markup.h>

#include "../../core/core_internal.h"
#include "svg_doc.h"

#define MAX_NAMES_NOTED 256

typedef struct build_ctx {
  GVEC_Svg_Document * doc;
  const gvec_svg_build_options * options;
  const GVEC_Limits * limits;
  GVEC_Diagnostics * diag;
  gvec_svg_parse_context parse;
  uint32_t next_order;
  GVEC_Result error;
  const char * noted[MAX_NAMES_NOTED];
  size_t noted_count;
  bool noted_overflow;
  bool script_noted;
  bool animation_noted;
} build_ctx;

static const char * const k_units[] = {"userSpaceOnUse", "objectBoundingBox"};
static const char * const k_xml_space[] = {"default", "preserve"};
static const char * const k_spread[] = {"pad", "reflect", "repeat"};
static const char * const k_marker_units[] = {"userSpaceOnUse", "strokeWidth"};
static const char * const k_length_adjust[] = {"spacing", "spacingAndGlyphs"};
static const char * const k_method[] = {"align", "stretch"};
static const char * const k_side[] = {"left", "right"};
static const char * const k_spacing[] = {"auto", "exact"};
static const char * const k_zoom_pan[] = {"disable", "magnify"};

static void note(build_ctx * ctx, GVEC_Diag_Severity severity, GVEC_Diag_Code code, uint32_t detail, size_t line,
    size_t column, size_t offset, const char * element, const char * message) {
  gvec_diag_add(ctx->diag, severity, code, detail, line, column, offset, element, message);
}

/* True the first time a name is seen (up to a bound), so a notice is issued once per name. */
static bool first_time(build_ctx * ctx, const char * prefix, const char * name) {
  char key[160];
  size_t pl = strlen(prefix), nl = strlen(name);
  if (pl + nl + 1 > sizeof key) {
    return false;
  }
  memcpy(key, prefix, pl);
  memcpy(key + pl, name, nl + 1);
  for (size_t i = 0; i < ctx->noted_count; i++) {
    if (strcmp(ctx->noted[i], key) == 0) {
      return false;
    }
  }
  if (ctx->noted_count == MAX_NAMES_NOTED) {
    ctx->noted_overflow = true;
    return false;
  }
  char * copy = gvec_arena_strndup(&ctx->doc->arena, key, pl + nl);
  if (!copy) {
    ctx->error = GVEC_ERR_OOM;
    return false;
  }
  ctx->noted[ctx->noted_count++] = copy;
  return true;
}

static bool is_text_reader(GVEC_Svg_Kind kind) {
  switch (kind) {
  case GVEC_SVG_KIND_TITLE: case GVEC_SVG_KIND_DESC: case GVEC_SVG_KIND_STYLE: case GVEC_SVG_KIND_SCRIPT:
  case GVEC_SVG_KIND_TEXT: case GVEC_SVG_KIND_TSPAN: case GVEC_SVG_KIND_TEXT_PATH: case GVEC_SVG_KIND_A:
  case GVEC_SVG_KIND_FOREIGN_OBJECT: case GVEC_SVG_KIND_METADATA: case GVEC_SVG_KIND_FOREIGN:
  case GVEC_SVG_KIND_UNKNOWN:
    return true;
  default:
    return false;
  }
}

static bool is_animation(GVEC_Svg_Kind kind) {
  return kind == GVEC_SVG_KIND_ANIMATE || kind == GVEC_SVG_KIND_ANIMATE_MOTION ||
      kind == GVEC_SVG_KIND_ANIMATE_TRANSFORM || kind == GVEC_SVG_KIND_SET || kind == GVEC_SVG_KIND_DISCARD ||
      kind == GVEC_SVG_KIND_MPATH;
}

static const char * const * keywords_for(GVEC_Svg_Grammar g, size_t * count) {
#define K(table) (*count = sizeof table / sizeof table[0], table)
  switch (g) {
  case GVEC_SVG_GRAMMAR_ENUM_UNITS: return K(k_units);
  case GVEC_SVG_GRAMMAR_ENUM_XML_SPACE: return K(k_xml_space);
  case GVEC_SVG_GRAMMAR_ENUM_SPREAD: return K(k_spread);
  case GVEC_SVG_GRAMMAR_ENUM_MARKER_UNITS: return K(k_marker_units);
  case GVEC_SVG_GRAMMAR_ENUM_LENGTH_ADJUST: return K(k_length_adjust);
  case GVEC_SVG_GRAMMAR_ENUM_METHOD: return K(k_method);
  case GVEC_SVG_GRAMMAR_ENUM_SIDE: return K(k_side);
  case GVEC_SVG_GRAMMAR_ENUM_SPACING: return K(k_spacing);
  case GVEC_SVG_GRAMMAR_ENUM_ZOOM_PAN: return K(k_zoom_pan);
  default: return NULL;
  }
#undef K
}

/** Parse a TYPED attribute by its grammar into `a`; returns the parse status. */
static gvec_svg_parse_status parse_typed(build_ctx * ctx, GVEC_Svg_Kind kind, GVEC_Svg_Attribute * a,
    GVEC_Svg_Grammar g, gvec_svg_parse_error * err) {
  const char * s = a->raw;
  size_t n = a->raw_length;
  gvec_svg_value * v = &a->value;
  (void) kind;
  size_t kcount = 0;
  const char * const * words = keywords_for(g, &kcount);
  if (words) {
    size_t index;
    gvec_svg_parse_status st = gvec_svg_parse_keyword(s, n, words, kcount, &index, err);
    if (st == GVEC_SVG_PARSE_OK) {
      v->keyword.index = index;
      v->keyword.word = words[index];
      a->value_kind = GVEC_SVG_VALUE_KEYWORD;
    }
    return st;
  }
  switch (g) {
  case GVEC_SVG_GRAMMAR_ID: {
    size_t i = 0;
    if (n == 0) {
      err->offset = 0;
      err->reason = "an id may not be empty";
      return GVEC_SVG_PARSE_INVALID;
    }
    for (; i < n; i++) {
      if (gvec_svg_is_wsp((unsigned char) s[i])) {
        err->offset = i;
        err->reason = "an id may not contain white space";
        return GVEC_SVG_PARSE_INVALID;
      }
    }
    v->string = a->raw;
    a->value_kind = GVEC_SVG_VALUE_STRING;
    return GVEC_SVG_PARSE_OK;
  }
  case GVEC_SVG_GRAMMAR_STRING:
    v->string = a->raw;
    a->value_kind = GVEC_SVG_VALUE_STRING;
    return GVEC_SVG_PARSE_OK;
  case GVEC_SVG_GRAMMAR_TABINDEX:
    a->value_kind = GVEC_SVG_VALUE_INTEGER;
    return gvec_svg_parse_integer(s, n, &v->integer, err);
  case GVEC_SVG_GRAMMAR_HREF: {
    gvec_svg_parse_status st = gvec_svg_parse_reference(s, n, &ctx->parse, &v->reference, err);
    a->value_kind = GVEC_SVG_VALUE_REFERENCE;
    return st;
  }
  case GVEC_SVG_GRAMMAR_SYSTEM_LANGUAGE:
    a->value_kind = GVEC_SVG_VALUE_LANGUAGES;
    return gvec_svg_parse_language_list(s, n, &ctx->parse, (const char ***) &v->languages.tags, &v->languages.count, err);
  case GVEC_SVG_GRAMMAR_REQUIRED_EXTENSIONS:
    a->value_kind = GVEC_SVG_VALUE_EXTENSIONS;
    return gvec_svg_parse_extension_list(s, n, &v->count);
  case GVEC_SVG_GRAMMAR_VIEWBOX:
    a->value_kind = GVEC_SVG_VALUE_VIEW_BOX;
    return gvec_svg_parse_view_box(s, n, &v->view_box, err);
  case GVEC_SVG_GRAMMAR_PAR:
    a->value_kind = GVEC_SVG_VALUE_PRESERVE_ASPECT_RATIO;
    return gvec_svg_parse_par(s, n, &v->par, err);
  case GVEC_SVG_GRAMMAR_NUMBER: {
    a->value_kind = GVEC_SVG_VALUE_NUMBER;
    gvec_svg_parse_status st = gvec_svg_parse_number(s, n, &v->number, err);
    if (st == GVEC_SVG_PARSE_OK && v->number < 0) {
      err->offset = 0;
      err->reason = "the number may not be negative";
      return GVEC_SVG_PARSE_INVALID;
    }
    return st;
  }
  case GVEC_SVG_GRAMMAR_LENGTH:
    a->value_kind = GVEC_SVG_VALUE_LENGTH;
    return gvec_svg_parse_length(s, n, &v->length, err);
  case GVEC_SVG_GRAMMAR_COORD_LIST:
    a->value_kind = GVEC_SVG_VALUE_LENGTH_LIST;
    return gvec_svg_parse_length_list(s, n, &ctx->parse, &v->lengths, err);
  case GVEC_SVG_GRAMMAR_NUMBER_LIST:
    a->value_kind = GVEC_SVG_VALUE_NUMBER_LIST;
    return gvec_svg_parse_number_list(s, n, &ctx->parse, &v->numbers, err);
  case GVEC_SVG_GRAMMAR_POINTS:
    a->value_kind = GVEC_SVG_VALUE_POINTS;
    return gvec_svg_parse_points(s, n, &ctx->parse, &v->numbers, err);
  case GVEC_SVG_GRAMMAR_PATH_DATA:
    a->value_kind = GVEC_SVG_VALUE_PATH;
    return gvec_svg_parse_path(s, n, &ctx->parse, &v->path, err);
  case GVEC_SVG_GRAMMAR_ORIENT:
    a->value_kind = GVEC_SVG_VALUE_ORIENT;
    return gvec_svg_parse_orient(s, n, &v->orient, err);
  case GVEC_SVG_GRAMMAR_OFFSET:
    a->value_kind = GVEC_SVG_VALUE_OFFSET;
    return gvec_svg_parse_offset(s, n, &v->number, err);
  case GVEC_SVG_GRAMMAR_REF_COORD:
    a->value_kind = GVEC_SVG_VALUE_LENGTH;
    return gvec_svg_parse_ref_coord(s, n, a->name[3] == 'X', &v->length, err);
  default:
    err->offset = 0;
    err->reason = "no grammar";
    return GVEC_SVG_PARSE_INVALID;
  }
}

/** Fill `a` from the markup attribute; classify, parse, diagnose. */
static void build_attribute(build_ctx * ctx, GVEC_Svg_Element * e, const GTEXT_Markup_Attribute * view,
    const GTEXT_Markup_Location * where, GVEC_Svg_Attribute * a) {
  gvec_arena * arena = &ctx->doc->arena;
  const char * local = view->local;
  a->line = where ? (uint32_t) where->line : 0;
  a->column = where ? (uint32_t) where->column : 0;
  a->offset = where ? where->offset : 0;
  a->raw = gvec_arena_strndup(arena, view->value ? view->value : "", view->value_length);
  a->raw_length = view->value_length;
  if (!a->raw) {
    ctx->error = GVEC_ERR_OOM;
    return;
  }
  const char * name = local;
  bool foreign = false;
  if (view->ns && strcmp(view->ns, GVEC_XLINK_NS) == 0) {
    size_t l = strlen(local);
    char * joined = gvec_arena_alloc(arena, l + 7, 1);
    if (!joined) {
      ctx->error = GVEC_ERR_OOM;
      return;
    }
    memcpy(joined, "xlink:", 6);
    memcpy(joined + 6, local, l);
    name = joined;
  } else if (view->ns && strcmp(view->ns, GVEC_XML_NS) == 0) {
    size_t l = strlen(local);
    char * joined = gvec_arena_alloc(arena, l + 5, 1);
    if (!joined) {
      ctx->error = GVEC_ERR_OOM;
      return;
    }
    memcpy(joined, "xml:", 4);
    memcpy(joined + 4, local, l);
    name = joined;
  } else if (view->ns) {
    foreign = true;
    a->ns = gvec_arena_strndup(arena, view->ns, strlen(view->ns));
    if (!a->ns) {
      ctx->error = GVEC_ERR_OOM;
      return;
    }
  }
  a->name = gvec_arena_strndup(arena, name, strlen(name));
  if (!a->name) {
    ctx->error = GVEC_ERR_OOM;
    return;
  }
  a->cls = GVEC_SVG_ATTR_UNKNOWN;
  a->grammar = GVEC_SVG_GRAMMAR_NONE;
  size_t nl = strlen(a->name);
  if (foreign || e->kind == GVEC_SVG_KIND_FOREIGN) {
    return; /* Editor annotations and foreign content: kept, never interpreted, never diagnosed. */
  }
  GVEC_Svg_Grammar g = gvec_svg_grammar_for(e->kind, a->name, nl);
  bool presentation = gvec_svg_is_presentation(e->kind, a->name, nl);
  a->presentation = presentation;
  a->grammar = (uint8_t) g;
  if (g == GVEC_SVG_GRAMMAR_EVENT) {
    a->cls = GVEC_SVG_ATTR_SCRIPT;
    if (!ctx->script_noted) {
      ctx->script_noted = true;
      note(ctx, GVEC_DIAG_NOTICE, GVEC_DIAG_SCRIPT_IGNORED, 0, a->line, a->column, a->offset, e->name,
          "script is kept and never evaluated");
    }
    return;
  }
  if (strcmp(a->name, "style") == 0) {
    a->cls = GVEC_SVG_ATTR_PROPERTY;
    return;
  }
  if (presentation && strcmp(a->name, "d") != 0) {
    a->cls = GVEC_SVG_ATTR_PROPERTY;
    return;
  }
  if (presentation && strcmp(a->name, "d") == 0) {
    g = GVEC_SVG_GRAMMAR_PATH_DATA;
    a->grammar = (uint8_t) g;
  }
  if (g == GVEC_SVG_GRAMMAR_NONE) {
    if (strncmp(a->name, "data-", 5) == 0 || gvec_svg_is_legacy(a->name, nl)) {
      a->cls = GVEC_SVG_ATTR_RAW;
      return;
    }
    if (first_time(ctx, "a:", a->name)) {
      note(ctx, GVEC_DIAG_NOTICE, GVEC_DIAG_UNKNOWN_ATTRIBUTE, 0, a->line, a->column, a->offset, e->name,
          "SVG 2 defines no such attribute for this element; kept and ignored");
    }
    return;
  }
  if (g == GVEC_SVG_GRAMMAR_TRANSFORM) {
    a->cls = GVEC_SVG_ATTR_PROPERTY;
    return;
  }
  if (g == GVEC_SVG_GRAMMAR_RAW_ARIA || g == GVEC_SVG_GRAMMAR_RAW_ANIM || g == GVEC_SVG_GRAMMAR_RAW_FILTER) {
    a->cls = GVEC_SVG_ATTR_RAW;
    return;
  }
  gvec_svg_parse_error err = {0, NULL};
  gvec_svg_parse_status st = parse_typed(ctx, e->kind, a, g, &err);
  switch (st) {
  case GVEC_SVG_PARSE_OK:
    a->cls = GVEC_SVG_ATTR_TYPED;
    break;
  case GVEC_SVG_PARSE_PARTIAL:
  case GVEC_SVG_PARSE_LIMIT:
    a->cls = GVEC_SVG_ATTR_TYPED;
    a->partial = 1;
    a->error_offset = err.offset;
    a->error_reason = err.reason;
    note(ctx, GVEC_DIAG_WARNING, GVEC_DIAG_ATTRIBUTE_INVALID, (uint32_t) g, a->line, a->column, a->offset, e->name,
        err.reason ? err.reason : "the attribute is partly invalid");
    break;
  case GVEC_SVG_PARSE_OOM:
    ctx->error = GVEC_ERR_OOM;
    break;
  case GVEC_SVG_PARSE_INVALID:
  default:
    a->cls = GVEC_SVG_ATTR_INVALID;
    a->value_kind = GVEC_SVG_VALUE_NONE;
    a->error_offset = err.offset;
    a->error_reason = err.reason;
    note(ctx, GVEC_DIAG_WARNING, GVEC_DIAG_ATTRIBUTE_INVALID, (uint32_t) g, a->line, a->column, a->offset, e->name,
        err.reason ? err.reason : "the attribute is invalid");
    break;
  }
}

static void append_node(GVEC_Svg_Element * parent, GVEC_Svg_Node * node) {
  node->parent = parent;
  if (parent->last_node) {
    parent->last_node->next = node;
  } else {
    parent->first_node = node;
  }
  parent->last_node = node;
}

static bool all_wsp(const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!gvec_svg_is_wsp((unsigned char) s[i])) {
      return false;
    }
  }
  return true;
}

static GVEC_Svg_Element * build_element(build_ctx * ctx, const GTEXT_Markup_Node * mnode, GVEC_Svg_Element * parent,
    uint32_t ordinal);

static void build_children(build_ctx * ctx, const GTEXT_Markup_Node * mnode, GVEC_Svg_Element * e) {
  uint32_t ordinals[GVEC_SVG_KIND_COUNT];
  memset(ordinals, 0, sizeof ordinals);
  for (const GTEXT_Markup_Node * child = gtext_markup_node_first_child(mnode); child && !ctx->error;
       child = gtext_markup_node_next_sibling(child)) {
    GTEXT_Markup_Kind k = gtext_markup_node_kind(child);
    if (k == GTEXT_MARKUP_KIND_ELEMENT) {
      /* The ordinal is among the siblings of the same kind, which needs the kind first. */
      const char * ns = gtext_markup_element_namespace(child);
      const char * local = gtext_markup_element_local_name(child);
      GVEC_Svg_Kind kind = GVEC_SVG_KIND_FOREIGN;
      if (ns && strcmp(ns, GVEC_SVG_NS) == 0) {
        kind = gvec_svg_kind_find(local, strlen(local));
      }
      build_element(ctx, child, e, ordinals[kind]++);
    } else if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA) {
      size_t length = 0;
      const char * data = gtext_markup_node_data(child, &length);
      if (!data || length == 0) {
        continue;
      }
      if (!is_text_reader(e->kind) && all_wsp(data, length)) {
        continue;
      }
      size_t total;
      if (!gcu_safe_add_size(ctx->doc->text_bytes, length, &total) || total > ctx->limits->max_text_bytes) {
        GTEXT_Markup_Location where;
        memset(&where, 0, sizeof where);
        gtext_markup_node_location(child, &where);
        note(ctx, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, GVEC_LIMIT_TEXT_BYTES, where.line, where.column, where.offset,
            e->name, "the document has more character data than max_text_bytes");
        ctx->error = GVEC_ERR_LIMIT;
        return;
      }
      ctx->doc->text_bytes = total;
      GVEC_Svg_Node * t = gvec_arena_alloc(&ctx->doc->arena, sizeof *t, 8);
      char * copy = gvec_arena_strndup(&ctx->doc->arena, data, length);
      if (!t || !copy) {
        ctx->error = GVEC_ERR_OOM;
        return;
      }
      GTEXT_Markup_Location where;
      memset(&where, 0, sizeof where);
      gtext_markup_node_location(child, &where);
      t->is_element = 0;
      t->text = copy;
      t->text_length = length;
      t->line = (uint32_t) where.line;
      t->column = (uint32_t) where.column;
      t->offset = where.offset;
      append_node(e, t);
    }
  }
}

static GVEC_Svg_Element * build_element(build_ctx * ctx, const GTEXT_Markup_Node * mnode, GVEC_Svg_Element * parent,
    uint32_t ordinal) {
  gvec_arena * arena = &ctx->doc->arena;
  GVEC_Svg_Element * e = gvec_arena_alloc(arena, sizeof *e, 8);
  if (!e) {
    ctx->error = GVEC_ERR_OOM;
    return NULL;
  }
  const char * ns = gtext_markup_element_namespace(mnode);
  const char * local = gtext_markup_element_local_name(mnode);
  GTEXT_Markup_Location where;
  memset(&where, 0, sizeof where);
  gtext_markup_node_location(mnode, &where);
  e->node.is_element = 1;
  e->node.element = e;
  e->node.line = (uint32_t) where.line;
  e->node.column = (uint32_t) where.column;
  e->node.offset = where.offset;
  e->kind = GVEC_SVG_KIND_FOREIGN;
  if (ns && strcmp(ns, GVEC_SVG_NS) == 0) {
    e->kind = gvec_svg_kind_find(local, strlen(local));
  }
  e->ns = ns ? gvec_arena_strndup(arena, ns, strlen(ns)) : NULL;
  e->name = gvec_arena_strndup(arena, local, strlen(local));
  if ((ns && !e->ns) || !e->name) {
    ctx->error = GVEC_ERR_OOM;
    return NULL;
  }
  e->order = ctx->next_order++;
  e->handle.element = e;
  ctx->doc->elements[e->order] = e;
  if (parent) {
    append_node(parent, &e->node);
  }
  if (e->kind == GVEC_SVG_KIND_UNKNOWN && first_time(ctx, "e:", e->name)) {
    note(ctx, GVEC_DIAG_NOTICE, GVEC_DIAG_UNKNOWN_ELEMENT, 0, where.line, where.column, where.offset, e->name,
        "SVG 2 defines no such element; kept and not rendered");
  }
  if (is_animation(e->kind) && !ctx->animation_noted) {
    ctx->animation_noted = true;
    ctx->doc->has_animation = true;
    note(ctx, GVEC_DIAG_NOTICE, GVEC_DIAG_ANIMATION_NOT_APPLIED, 0, where.line, where.column, where.offset, e->name,
        "animation is kept and not applied; a still render draws the initial values");
  }
  if (e->kind == GVEC_SVG_KIND_SCRIPT && !ctx->script_noted) {
    ctx->script_noted = true;
    ctx->doc->has_script = true;
    note(ctx, GVEC_DIAG_NOTICE, GVEC_DIAG_SCRIPT_IGNORED, 0, where.line, where.column, where.offset, e->name,
        "script is kept and never evaluated");
  }
  /* Attributes. */
  size_t total = gtext_markup_element_attribute_count(mnode);
  size_t kept = 0;
  GVEC_Svg_Attribute * attributes = total ? gvec_arena_array(arena, total, sizeof(GVEC_Svg_Attribute), 8) : NULL;
  if (total && !attributes) {
    ctx->error = GVEC_ERR_OOM;
    return NULL;
  }
  for (size_t i = 0; i < total && !ctx->error; i++) {
    GTEXT_Markup_Attribute view;
    if (gtext_markup_element_attribute_at(mnode, i, &view) != GTEXT_MARKUP_OK) {
      continue;
    }
    if ((view.ns && strcmp(view.ns, GVEC_XMLNS_NS) == 0) || (!view.ns && strcmp(view.local, "xmlns") == 0)) {
      continue; /* A namespace declaration, not an attribute. */
    }
    GTEXT_Markup_Location at;
    memset(&at, 0, sizeof at);
    bool have = gtext_markup_element_attribute_location(mnode, i, &at);
    GVEC_Svg_Attribute * a = &attributes[kept];
    memset(a, 0, sizeof *a);
    build_attribute(ctx, e, &view, have ? &at : NULL, a);
    kept++;
    if (a->cls == GVEC_SVG_ATTR_TYPED && a->grammar == GVEC_SVG_GRAMMAR_ID && !e->id) {
      e->id = a->raw;
    }
    if (a->cls == GVEC_SVG_ATTR_TYPED && a->grammar == GVEC_SVG_GRAMMAR_HREF && a->value.reference.kind != GVEC_SVG_REF_EMPTY) {
      if (strcmp(a->name, "href") == 0 || !e->href) {
        e->href = a;
      }
    }
  }
  e->attributes = attributes;
  e->attribute_count = kept;
  if (ctx->error) {
    return NULL;
  }
  /* The key, from the parent's. */
  e->key = gvec_svg_key_for(parent ? parent->key : 0, e->id, e->kind, e->ns, e->name, ordinal);
  build_children(ctx, mnode, e);
  e->subtree_size = ctx->next_order - e->order;
  return ctx->error ? NULL : e;
}

static int compare_ids(const void * pa, const void * pb) {
  const gvec_svg_id_entry * a = pa;
  const gvec_svg_id_entry * b = pb;
  int c = strcmp(a->id, b->id);
  if (c != 0) {
    return c;
  }
  return a->element->order < b->element->order ? -1 : a->element->order > b->element->order ? 1 : 0;
}

static GVEC_Result index_ids(build_ctx * ctx) {
  GVEC_Svg_Document * doc = ctx->doc;
  size_t count = 0;
  for (size_t i = 0; i < doc->element_count; i++) {
    if (doc->elements[i]->id) {
      count++;
    }
  }
  if (count == 0) {
    return GVEC_OK;
  }
  gvec_svg_id_entry * all = gvec_arena_array(&doc->arena, count, sizeof *all, 8);
  if (!all) {
    return GVEC_ERR_OOM;
  }
  size_t at = 0;
  for (size_t i = 0; i < doc->element_count; i++) {
    if (doc->elements[i]->id) {
      all[at].id = doc->elements[i]->id;
      all[at].element = doc->elements[i];
      at++;
    }
  }
  qsort(all, count, sizeof *all, compare_ids);
  /* The first in document order wins; later ones are diagnosed and dropped from the index. */
  size_t kept = 0;
  for (size_t i = 0; i < count; i++) {
    if (kept > 0 && strcmp(all[kept - 1].id, all[i].id) == 0) {
      GVEC_Svg_Element * dup = all[i].element;
      note(ctx, GVEC_DIAG_WARNING, GVEC_DIAG_DUPLICATE_ID, 0, dup->node.line, dup->node.column, dup->node.offset, dup->name,
          "this id is already used earlier in the document; the first one wins");
      continue;
    }
    all[kept++] = all[i];
  }
  doc->ids = all;
  doc->id_count = kept;
  return GVEC_OK;
}

/* Make every key distinct: a later element whose key an earlier one holds is re-keyed, deterministically. */
static GVEC_Result unique_keys(GVEC_Svg_Document * doc) {
  size_t slots = 16;
  while (slots < doc->element_count * 2) {
    slots <<= 1;
  }
  uint64_t * table = gvec_arena_array(&doc->arena, slots, sizeof(uint64_t), 8);
  uint8_t * used = gvec_arena_array(&doc->arena, slots, 1, 1);
  if (!table || !used) {
    return GVEC_ERR_OOM;
  }
  for (size_t i = 0; i < doc->element_count; i++) {
    GVEC_Svg_Element * e = doc->elements[i];
    uint32_t attempt = 0;
    for (;;) {
      size_t slot = (size_t) (e->key & (slots - 1));
      bool clash = false;
      while (used[slot]) {
        if (table[slot] == e->key) {
          clash = true;
          break;
        }
        slot = (slot + 1) & (slots - 1);
      }
      if (!clash) {
        used[slot] = 1;
        table[slot] = e->key;
        break;
      }
      e->flags |= GVEC_SVG_ELEMENT_KEY_COLLISION;
      e->key = gvec_svg_key_rehash(e->key, ++attempt);
    }
  }
  return GVEC_OK;
}

GVEC_Result gvec_svg_build(const void * markup_root, size_t element_count, const gvec_svg_build_options * options,
    const GVEC_Allocator * allocator, GVEC_Diagnostics * diagnostics, GVEC_Svg_Document ** out) {
  GVEC_Svg_Document * doc = gvec_alloc(allocator, sizeof *doc);
  if (!doc) {
    return GVEC_ERR_OOM;
  }
  memset(doc, 0, sizeof *doc);
  doc->allocator = allocator;
  doc->viewport_width = options->viewport_width > 0 ? options->viewport_width : GVEC_DEFAULT_VIEWPORT_WIDTH;
  doc->viewport_height = options->viewport_height > 0 ? options->viewport_height : GVEC_DEFAULT_VIEWPORT_HEIGHT;
  doc->color_scheme = options->color_scheme;
  doc->limits = *options->limits;
  gvec_arena_init(&doc->arena, allocator);
  build_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.doc = doc;
  ctx.options = options;
  ctx.limits = options->limits;
  ctx.diag = diagnostics;
  ctx.parse.arena = &doc->arena;
  ctx.parse.allocator = allocator;
  ctx.parse.max_list_items = options->limits->max_list_items;
  ctx.parse.max_path_segments = options->limits->max_path_segments;
  doc->elements = gvec_arena_array(&doc->arena, element_count ? element_count : 1, sizeof(GVEC_Svg_Element *), 8);
  if (!doc->elements) {
    gvec_svg_document_free(doc);
    return GVEC_ERR_OOM;
  }
  doc->element_count = element_count;
  doc->root = build_element(&ctx, (const GTEXT_Markup_Node *) markup_root, NULL, 0);
  GVEC_Result r = ctx.error;
  if (r == GVEC_OK && doc->root) {
    doc->element_count = ctx.next_order;
    r = unique_keys(doc);
    if (r == GVEC_OK) {
      r = index_ids(&ctx);
    }
  } else if (r == GVEC_OK) {
    r = GVEC_ERR_INTERNAL;
  }
  if (ctx.noted_overflow) {
    note(&ctx, GVEC_DIAG_NOTICE, GVEC_DIAG_UNKNOWN_ATTRIBUTE, 0, 0, 0, 0, NULL,
        "more unknown names than are listed individually; the rest are kept without a notice");
  }
  if (r != GVEC_OK) {
    gvec_svg_document_free(doc);
    return r;
  }
  *out = doc;
  return GVEC_OK;
}

void gvec_svg_document_free(GVEC_Svg_Document * document) {
  if (!document) {
    return;
  }
  const GVEC_Allocator * allocator = document->allocator;
  gvec_arena_destroy(&document->arena);
  gvec_free(allocator, document);
}
