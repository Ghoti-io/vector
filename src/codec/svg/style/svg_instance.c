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
 * @file svg_instance.c
 *
 * The style context and its instances (C3), the element interface over the typed document, feeding the cascade (C4)
 * and the viewports the percentages are resolved against (C6).
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include "svg_style.h"
#include "svg_used.h"

//-----------------------------------------------------------------------------
// The element interface
//-----------------------------------------------------------------------------

static const GVEC_Svg_Element * element_of(GTEXT_CSS_Element_Handle handle) {
  return ((const GVEC_Svg_Instance *) handle)->element;
}

static bool cb_local_name(void * user, GTEXT_CSS_Element_Handle handle, GTEXT_CSS_String * out) {
  (void) user;
  const GVEC_Svg_Element * e = element_of(handle);
  out->data = e->name;
  out->length = strlen(e->name);
  return true;
}

static bool cb_namespace_uri(void * user, GTEXT_CSS_Element_Handle handle, GTEXT_CSS_String * out) {
  (void) user;
  const GVEC_Svg_Element * e = element_of(handle);
  if (!e->ns) {
    return false;
  }
  out->data = e->ns;
  out->length = strlen(e->ns);
  return true;
}

static bool cb_attribute(void * user, GTEXT_CSS_Element_Handle handle, const GTEXT_CSS_String * ns,
    const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
  (void) user;
  const GVEC_Svg_Element * e = element_of(handle);
  for (size_t i = 0; i < e->attribute_count; i++) {
    const GVEC_Svg_Attribute * a = &e->attributes[i];
    if (strlen(a->name) != name->length || memcmp(a->name, name->data, name->length) != 0) {
      continue;
    }
    /* An attribute with no namespace is the one asked for with none; a foreign attribute of the same local name is not. */
    if (ns && ns->length) {
      if (!a->ns || strlen(a->ns) != ns->length || memcmp(a->ns, ns->data, ns->length) != 0) {
        continue;
      }
    }
    else if (a->ns) {
      continue;
    }
    out->data = a->raw;
    out->length = a->raw_length;
    return true;
  }
  return false;
}

void gvec_svg_element_interface(GTEXT_CSS_Element_Interface * table, void * user) {
  memset(table, 0, sizeof *table);
  table->size = sizeof *table;
  table->user = user;
  table->local_name = cb_local_name;
  table->namespace_uri = cb_namespace_uri;
  table->attribute = cb_attribute;
}

//-----------------------------------------------------------------------------
// The environment
//-----------------------------------------------------------------------------

/* A document has no fonts, so the metrics a font would give are the fallbacks CSS Values 4 and CSS Fonts 4 name: the x-height
 * and the advance of "0" are half the font size when the font has none (Values 4, 6.1.1), the cap height is 0.7 em, the advance
 * of U+6C34 is 1 em, and the `normal` line height is 1.2 em. A renderer that has the fonts supplies its own through text layout
 * (a later milestone); these keep a style a function of the document alone. */
static bool env_font_metric(void * user, const GTEXT_CSS_Font_Query * query, GTEXT_CSS_Font_Metric metric, double * px) {
  (void) user;
  switch (metric) {
  case GTEXT_CSS_METRIC_EX:
  case GTEXT_CSS_METRIC_CH:
    *px = query->size_px * 0.5;
    return true;
  case GTEXT_CSS_METRIC_CAP:
    *px = query->size_px * 0.7;
    return true;
  case GTEXT_CSS_METRIC_IC:
    *px = query->size_px;
    return true;
  }
  return false;
}

static bool env_normal_line_height(void * user, const GTEXT_CSS_Font_Query * query, double * px) {
  (void) user;
  *px = query->size_px * 1.2;
  return true;
}

void gvec_svg_environment(GTEXT_CSS_Environment * env, double width, double height, GVEC_Color_Scheme scheme, void * user) {
  *env = gtext_css_environment_default();
  env->small_viewport.width = env->large_viewport.width = env->dynamic_viewport.width = width;
  env->small_viewport.height = env->large_viewport.height = env->dynamic_viewport.height = height;
  env->color_scheme = scheme == GVEC_COLOR_SCHEME_DARK ? GTEXT_CSS_SCHEME_DARK : GTEXT_CSS_SCHEME_LIGHT;
  env->user = user;
  env->font_metric = env_font_metric;
  env->normal_line_height = env_normal_line_height;
}

//-----------------------------------------------------------------------------
// The context
//-----------------------------------------------------------------------------

GVEC_Result gvec_svg_style_context_new(const GVEC_Svg_Document * document, GVEC_Diagnostics * diagnostics,
    const GVEC_Allocator * allocator, GVEC_Svg_Style_Context ** out_context) {
  if (!document || !out_context) {
    return GVEC_ERR_INVALID;
  }
  GVEC_Svg_Style_Context * context = gvec_alloc(allocator, sizeof *context);
  if (!context) {
    return GVEC_ERR_OOM;
  }
  memset(context, 0, sizeof *context);
  context->document = document;
  context->allocator = allocator;
  context->diagnostics = diagnostics;
  context->limits = document->limits;
  context->viewport_width = document->viewport_width;
  context->viewport_height = document->viewport_height;
  context->color_scheme = document->color_scheme;
  gvec_arena_init(&context->arena, allocator);

  GVEC_Result r = gvec_svg_engine_acquire(allocator, context->viewport_width, context->viewport_height, context->color_scheme,
      context->limits.max_custom_expansion, &context->engine);
  if (r != GVEC_OK) {
    gvec_svg_style_context_free(context);
    return r;
  }
  for (size_t k = 0; k < GVEC_ONCE_COUNT; k++) {
    context->once[k] = gvec_arena_alloc(&context->arena, (document->element_count + 7) / 8 + 1, 1);
    if (!context->once[k]) {
      gvec_svg_style_context_free(context);
      return GVEC_ERR_OOM;
    }
  }
  context->style_options = gtext_css_style_options_default();
  context->style_options.presentation.table = &context->engine->reg.table;
  context->style_options.parse.allocator = allocator;
  context->style_options.candidates.allocator = allocator;
  context->style_options.parse.max_input_bytes = context->limits.max_style_bytes;
  context->style_options.parse.max_declarations = context->limits.max_declarations;
  /* A shorthand counts its longhands among the candidates, and `font` has dozens. */
  size_t candidates = 0;
  if (!gcu_safe_mul_size(context->limits.max_declarations, 64, &candidates) || candidates > ((size_t) 1 << 28)) {
    candidates = (size_t) 1 << 28;
  }
  context->style_options.candidates.max_candidates = candidates;
  *out_context = context;
  return GVEC_OK;
}

void gvec_svg_style_context_free(GVEC_Svg_Style_Context * context) {
  if (!context) {
    return;
  }
  for (size_t i = 0; i < context->style_count; i++) {
    gtext_css_computed_free(context->styles[i]);
  }
  gvec_free(context->allocator, context->styles);
  gvec_svg_engine_release(context->engine);
  gvec_arena_destroy(&context->arena);
  gvec_free(context->allocator, context);
}

size_t gvec_svg_style_context_override_count(const GVEC_Svg_Style_Context * context) {
  (void) context;
  size_t n = 0;
  for (size_t i = 0; i < GVEC_SVG_PROP_COUNT; i++) {
    n += gvec_svg_prop_rows[i].source == GVEC_SVG_SOURCE_OVERRIDDEN;
  }
  return n;
}

bool gvec_svg_style_context_override_at(
    const GVEC_Svg_Style_Context * context, size_t index, GVEC_Svg_Property * out_property, const char ** out_note) {
  (void) context;
  for (size_t i = 0; i < GVEC_SVG_PROP_COUNT; i++) {
    if (gvec_svg_prop_rows[i].source != GVEC_SVG_SOURCE_OVERRIDDEN) {
      continue;
    }
    if (index-- == 0) {
      if (out_property) {
        *out_property = (GVEC_Svg_Property) i;
      }
      if (out_note) {
        *out_note = gvec_svg_prop_rows[i].note;
      }
      return true;
    }
  }
  return false;
}

//-----------------------------------------------------------------------------
// Instances
//-----------------------------------------------------------------------------

static GVEC_Result make_instance(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * element,
    const GVEC_Svg_Instance * parent, const GVEC_Svg_Instance * use, double vw, double vh, GVEC_Svg_Instance ** out) {
  size_t bound = 0;
  if (!gcu_safe_add_size(context->limits.max_elements, context->limits.max_reference_expansion, &bound) ||
      context->instances >= bound) {
    gvec_diag_add(context->diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, GVEC_LIMIT_REFERENCE_EXPANSION, element->node.line,
        element->node.column, element->node.offset, element->name,
        "more instances of elements than the document and its references allow");
    return GVEC_ERR_LIMIT;
  }
  GVEC_Svg_Instance * instance = gvec_arena_alloc(&context->arena, sizeof *instance, 8);
  if (!instance) {
    return GVEC_ERR_OOM;
  }
  instance->element = element;
  instance->parent = parent;
  instance->use = use;
  instance->viewport_width = vw;
  instance->viewport_height = vh;
  instance->depth = parent ? parent->depth + 1 : 0;
  context->instances++;
  *out = instance;
  return GVEC_OK;
}

const GVEC_Svg_Instance * gvec_svg_instance_root(GVEC_Svg_Style_Context * context) {
  if (!context || !context->document->root) {
    return NULL;
  }
  GVEC_Svg_Instance * root = NULL;
  if (make_instance(context, context->document->root, NULL, NULL, context->viewport_width, context->viewport_height, &root) != GVEC_OK) {
    return NULL;
  }
  return root;
}

/* Whether an element of this kind establishes a viewport for its children. */
static bool establishes_viewport(const GVEC_Svg_Element * e) {
  switch (e->kind) {
  case GVEC_SVG_KIND_SVG:
  case GVEC_SVG_KIND_SYMBOL:
  case GVEC_SVG_KIND_MARKER:
  case GVEC_SVG_KIND_PATTERN:
    return true;
  default:
    return false;
  }
}

/* The usable `viewBox` of an element: positive size, or none. */
static bool usable_view_box(const GVEC_Svg_Element * e, GVEC_Svg_View_Box * out) {
  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, "viewBox");
  if (!a || gvec_svg_attribute_class(a) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_view_box(a, out)) {
    return false;
  }
  return out->width > 0 && out->height > 0;
}

/* A marker's `markerWidth` or `markerHeight`: a length in user units, default 3 (SVG 2, 11.6.2). */
static double marker_extent(const GVEC_Svg_Element * e, const char * name) {
  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, name);
  GVEC_Svg_Length length;
  if (a && gvec_svg_attribute_class(a) == GVEC_SVG_ATTR_TYPED && gvec_svg_attribute_length(a, &length) &&
      length.unit == GVEC_SVG_UNIT_NONE && length.value >= 0) {
    return length.value;
  }
  return 3;
}

/*
 * The viewport the children of `instance` are laid out in, for the basis of their percentages (C6): the viewBox size when the
 * element has a usable one, else the size of the viewport itself. The `width` and `height` a `use` gives its referent override
 * its own.
 */
static GVEC_Result children_viewport(
    GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, double * out_w, double * out_h) {
  const GVEC_Svg_Element * e = instance->element;
  GVEC_Svg_View_Box box;
  if (!establishes_viewport(e)) {
    *out_w = instance->viewport_width;
    *out_h = instance->viewport_height;
    return GVEC_OK;
  }
  double w = instance->viewport_width, h = instance->viewport_height;
  if (e->kind == GVEC_SVG_KIND_MARKER) {
    w = marker_extent(e, "markerWidth");
    h = marker_extent(e, "markerHeight");
  }
  else if (e->kind != GVEC_SVG_KIND_PATTERN) {
    /* `svg` and `symbol`: the used `width` and `height`, `auto` being 100% of the containing viewport. */
    double used[2];
    bool is_auto[2];
    GVEC_Result r = gvec_svg_used_length(context, instance, GVEC_SVG_PROP_WIDTH, &used[0], &is_auto[0]);
    if (r == GVEC_OK) {
      r = gvec_svg_used_length(context, instance, GVEC_SVG_PROP_HEIGHT, &used[1], &is_auto[1]);
    }
    if (r == GVEC_ERR_UNSUPPORTED) {
      /* The element has no usable style and is refused when it is read; what is inside it is still made, in the viewport around it. */
      used[0] = used[1] = 0;
      is_auto[0] = is_auto[1] = true;
      r = GVEC_OK;
    }
    if (r != GVEC_OK) {
      return r;
    }
    if (!is_auto[0]) {
      w = used[0];
    }
    if (!is_auto[1]) {
      h = used[1];
    }
    if (instance->has_override_width) {
      w = instance->override_width;
    }
    if (instance->has_override_height) {
      h = instance->override_height;
    }
  }
  if (usable_view_box(e, &box)) {
    w = box.width;
    h = box.height;
  }
  *out_w = w;
  *out_h = h;
  return GVEC_OK;
}

GVEC_Result gvec_svg_instance_child(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * parent,
    const GVEC_Svg_Element * element, const GVEC_Svg_Instance ** out_instance) {
  if (!context || !parent || !element || !out_instance || gvec_svg_element_parent(element) != parent->element) {
    return GVEC_ERR_INVALID;
  }
  double w, h;
  GVEC_Result r = children_viewport(context, parent, &w, &h);
  if (r != GVEC_OK) {
    return r;
  }
  GVEC_Svg_Instance * made = NULL;
  r = make_instance(context, element, parent, parent->use, w, h, &made);
  if (r == GVEC_OK) {
    *out_instance = made;
  }
  return r;
}

GVEC_Result gvec_svg_instance_use(
    GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * use_instance, const GVEC_Svg_Instance ** out_instance) {
  if (!context || !use_instance || !out_instance || use_instance->element->kind != GVEC_SVG_KIND_USE) {
    return GVEC_ERR_INVALID;
  }
  const GVEC_Svg_Element * target = gvec_svg_element_href_target(use_instance->element);
  if (!target) {
    return GVEC_ERR_INVALID;
  }
  /* The referent is drawn in the use's viewport; a symbol or svg establishes its own from the width and height of the use
   * when it gives them, and from its own otherwise (SVG 2, 5.6). The referent's own percentages (of its width and height)
   * are resolved against the use's viewport. */
  GVEC_Svg_Instance * clone = NULL;
  GVEC_Result r = make_instance(
      context, target, use_instance, use_instance, use_instance->viewport_width, use_instance->viewport_height, &clone);
  if (r != GVEC_OK) {
    return r;
  }
  if (target->kind == GVEC_SVG_KIND_SVG || target->kind == GVEC_SVG_KIND_SYMBOL) {
    double used[2];
    bool is_auto[2];
    r = gvec_svg_used_length(context, use_instance, GVEC_SVG_PROP_WIDTH, &used[0], &is_auto[0]);
    if (r == GVEC_OK) {
      r = gvec_svg_used_length(context, use_instance, GVEC_SVG_PROP_HEIGHT, &used[1], &is_auto[1]);
    }
    if (r == GVEC_ERR_UNSUPPORTED) {
      used[0] = used[1] = 0;
      is_auto[0] = is_auto[1] = true;
      r = GVEC_OK;
    }
    if (r != GVEC_OK) {
      return r;
    }
    clone->has_override_width = !is_auto[0];
    clone->override_width = used[0];
    clone->has_override_height = !is_auto[1];
    clone->override_height = used[1];
  }
  *out_instance = clone;
  return GVEC_OK;
}

const GVEC_Svg_Element * gvec_svg_instance_element(const GVEC_Svg_Instance * instance) {
  return instance ? instance->element : NULL;
}

const GVEC_Svg_Instance * gvec_svg_instance_parent(const GVEC_Svg_Instance * instance) {
  return instance ? instance->parent : NULL;
}

const GVEC_Svg_Instance * gvec_svg_instance_use_context(const GVEC_Svg_Instance * instance) {
  return instance ? instance->use : NULL;
}

void gvec_svg_instance_viewport(const GVEC_Svg_Instance * instance, double * out_width, double * out_height) {
  if (!instance) {
    return;
  }
  if (out_width) {
    *out_width = instance->viewport_width;
  }
  if (out_height) {
    *out_height = instance->viewport_height;
  }
}

//-----------------------------------------------------------------------------
// Styles
//-----------------------------------------------------------------------------

static GVEC_Result keep_style(GVEC_Svg_Style_Context * context, GTEXT_CSS_Computed_Style * style) {
  if (context->style_count == context->style_capacity) {
    size_t capacity = context->style_capacity ? context->style_capacity * 2 : 64;
    GTEXT_CSS_Computed_Style ** grown = gvec_realloc(context->allocator, context->styles, capacity * sizeof *grown);
    if (!grown) {
      return GVEC_ERR_OOM;
    }
    context->styles = grown;
    context->style_capacity = capacity;
  }
  context->styles[context->style_count++] = style;
  return GVEC_OK;
}

GVEC_Result gvec_svg_style_diag(GVEC_Svg_Style_Context * context, GVEC_Diag_Severity severity, GVEC_Diag_Code code, uint32_t detail,
    const GVEC_Svg_Element * e, const GVEC_Svg_Attribute * a, const char * message) {
  return gvec_diag_add(context->diagnostics, severity, code, detail, a ? a->line : e->node.line, a ? a->column : e->node.column,
      a ? a->offset : e->node.offset, e->name, message);
}

/* What was dropped, once for the element (it is the same whichever instance is drawn). */
static GVEC_Result report_dropped(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * e, const GTEXT_CSS_Computed_Style * style) {
  size_t total = gtext_css_computed_diagnostic_total(style);
  for (size_t i = 0; i < total; i++) {
    GTEXT_CSS_Style_Diagnostic d;
    if (!gtext_css_computed_diagnostic(style, i, &d)) {
      break;
    }
    const GVEC_Svg_Attribute * a = NULL;
    const char * name = d.source == GTEXT_CSS_STYLE_SOURCE_STYLE_ATTRIBUTE ? "style" : NULL;
    char buffer[96];
    if (!name && d.name.length < sizeof buffer) {
      memcpy(buffer, d.name.data, d.name.length);
      buffer[d.name.length] = 0;
      name = buffer;
    }
    if (name) {
      a = gvec_svg_element_find_attribute(e, name);
    }
    uint32_t detail = d.offset > UINT32_MAX ? UINT32_MAX : (uint32_t) d.offset;
    GVEC_Result r = gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_STYLE_INVALID, d.source == GTEXT_CSS_STYLE_SOURCE_STYLE_ATTRIBUTE ? detail : 0,
        e, a,
        d.kind == GTEXT_CSS_STYLE_DIAG_UNKNOWN_PROPERTY ? "a declaration names no property of the registry and was dropped"
        : d.kind == GTEXT_CSS_STYLE_DIAG_UNJUDGEABLE   ? "a declaration's value cannot be judged by the engine and was dropped"
                                                       : "a declaration or presentation attribute is not a value of its property and was dropped");
    if (r == GVEC_ERR_OOM) {
      return r;
    }
  }
  return GVEC_OK;
}

bool gvec_svg_once_first(GVEC_Svg_Style_Context * context, gvec_svg_once kind, const GVEC_Svg_Element * e) {
  size_t bit = e->order;
  uint8_t mask = (uint8_t) (1u << (bit & 7));
  bool was = (context->once[kind][bit >> 3] & mask) != 0;
  context->once[kind][bit >> 3] |= mask;
  return !was;
}

/* One element's style from its parent's. A limit of the `style` attribute drops the attribute and reports it; the element is styled
 * from its presentation attributes. */
static GVEC_Result compute_one(GVEC_Svg_Style_Context * context, GVEC_Svg_Instance * instance, const GTEXT_CSS_Computed_Style * parent) {
  const GVEC_Svg_Element * e = instance->element;
  GTEXT_CSS_Style_Options options = context->style_options;
  GTEXT_CSS_Computed_Style * style = NULL;
  GTEXT_CSS_Compute_Result result;
  memset(&result, 0, sizeof result);
  /* The user agent's style sheet, as the declarations of the rules that match this element. */
  const char * rules[GVEC_SVG_UA_MAX];
  GTEXT_CSS_Style_Entry entries[GVEC_SVG_UA_MAX];
  size_t count = gvec_svg_ua_declarations(instance, rules);
  for (size_t i = 0; i < count; i++) {
    memset(&entries[i], 0, sizeof entries[i]);
    entries[i].keys.origin = GTEXT_CSS_ORIGIN_USER_AGENT;
    entries[i].keys.rule = (uint32_t) i;
    entries[i].text.data = rules[i];
    entries[i].text.length = strlen(rules[i]);
  }
  /* What is wrong with an element's style is the same whichever instance of it is drawn: said once. */
  bool first = gvec_svg_once_first(context, GVEC_ONCE_STYLE, e);
  GTEXT_CSS_Status status = gtext_css_style_compute(
      context->engine->compute, instance, parent, count ? entries : NULL, count, &options, &style, &result);
  if (status == GTEXT_CSS_E_LIMIT_INPUT || status == GTEXT_CSS_E_LIMIT_DECLARATIONS || status == GTEXT_CSS_E_LIMIT_CANDIDATES) {
    bool bytes = status == GTEXT_CSS_E_LIMIT_INPUT;
    if (first) {
      gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_LIMIT, bytes ? GVEC_LIMIT_STYLE_BYTES : GVEC_LIMIT_DECLARATIONS, e,
          gvec_svg_element_find_attribute(e, "style"),
          bytes ? "a style attribute is longer than max_style_bytes and was dropped"
                : "a style attribute has more declarations than max_declarations and was dropped");
    }
    /* The element is styled without its style attribute. */
    options.sources = GTEXT_CSS_STYLE_PRESENTATION_ATTRIBUTES;
    memset(&result, 0, sizeof result);
    status = gtext_css_style_compute(
        context->engine->compute, instance, parent, count ? entries : NULL, count, &options, &style, &result);
  }
  if (status == GTEXT_CSS_OK) {
    if (first) {
      GVEC_Result r = report_dropped(context, e, style);
      if (r == GVEC_OK && result.substitution_limited > 0) {
        r = gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_LIMIT, GVEC_LIMIT_CUSTOM_EXPANSION, e, NULL,
            "a value expands past max_custom_expansion and is invalid at computed-value time");
      }
      if (r == GVEC_ERR_OOM) {
        gtext_css_computed_free(style);
        return r;
      }
    }
  }
  else {
    if (status == GTEXT_CSS_E_OOM) {
      return GVEC_ERR_OOM;
    }
    if (status == GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES || status == GTEXT_CSS_E_LIMIT_CUSTOM_VALUE || status == GTEXT_CSS_E_LIMIT_LIST ||
        status == GTEXT_CSS_E_LIMIT_DEPTH) {
      if (first) {
        gvec_svg_style_diag(context, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, GVEC_LIMIT_CUSTOM_EXPANSION, e, NULL,
            "custom properties or values are larger than the engine's bounds allow; the element has no style");
      }
      return GVEC_ERR_LIMIT;
    }
    if (first) {
      gvec_svg_style_diag(context, GVEC_DIAG_ERROR, GVEC_DIAG_STYLE_INVALID, 0, e, NULL,
          status == GTEXT_CSS_E_UNSUPPORTED ? "the engine cannot compute a property declared here; the element has no style"
                                            : "the style of the element could not be computed");
    }
    return GVEC_ERR_UNSUPPORTED;
  }
  GVEC_Result r = keep_style(context, style);
  if (r != GVEC_OK) {
    gtext_css_computed_free(style);
    return r;
  }
  instance->style = style;
  return GVEC_OK;
}

GVEC_Result gvec_svg_instance_style(
    GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, const GTEXT_CSS_Computed_Style ** out) {
  GVEC_Svg_Instance * top = (GVEC_Svg_Instance *) instance;
  if (top->style) {
    *out = top->style;
    return GVEC_OK;
  }
  if (top->style_failed) {
    return top->style_result;
  }
  /* The inheritance chain up to the first instance with a style, computed from the top down: no recursion, so a deep chain of
   * `use` is bounded by memory and not the stack. */
  size_t length = 0;
  for (GVEC_Svg_Instance * i = top; i && !i->style; i = (GVEC_Svg_Instance *) i->parent) {
    if (i->style_failed) {
      top->style_failed = true;
      top->style_result = i->style_result;
      return top->style_result;
    }
    length++;
  }
  GVEC_Svg_Instance ** chain = gvec_alloc(context->allocator, length * sizeof *chain);
  if (!chain) {
    return GVEC_ERR_OOM;
  }
  size_t n = 0;
  for (GVEC_Svg_Instance * i = top; i && !i->style; i = (GVEC_Svg_Instance *) i->parent) {
    chain[n++] = i;
  }
  GVEC_Result r = GVEC_OK;
  while (n > 0 && r == GVEC_OK) {
    GVEC_Svg_Instance * i = chain[--n];
    const GTEXT_CSS_Computed_Style * parent = i->parent ? i->parent->style : NULL;
    r = compute_one(context, i, parent);
    if (r != GVEC_OK && r != GVEC_ERR_OOM) {
      i->style_failed = true;
      i->style_result = r;
    }
  }
  gvec_free(context->allocator, chain);
  if (r != GVEC_OK) {
    if (r != GVEC_ERR_OOM) {
      top->style_failed = true;
      top->style_result = r;
    }
    return r;
  }
  *out = top->style;
  return GVEC_OK;
}

GVEC_Result gvec_svg_document_instances(GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * const ** out) {
  if (context->document_instances) {
    *out = context->document_instances;
    return GVEC_OK;
  }
  const GVEC_Svg_Document * doc = context->document;
  size_t n = doc->element_count;
  const GVEC_Svg_Instance ** instances = gvec_arena_array(&context->arena, n ? n : 1, sizeof *instances, 8);
  if (!instances) {
    return GVEC_ERR_OOM;
  }
  for (size_t i = 0; i < n; i++) {
    const GVEC_Svg_Element * e = doc->elements[i];
    GVEC_Result r;
    if (i == 0) {
      instances[0] = gvec_svg_instance_root(context);
      r = instances[0] ? GVEC_OK : GVEC_ERR_OOM;
    }
    else {
      r = gvec_svg_instance_child(context, instances[e->node.parent->order], e, &instances[i]);
    }
    if (r != GVEC_OK) {
      return r;
    }
  }
  context->document_instances = instances;
  *out = instances;
  return GVEC_OK;
}
