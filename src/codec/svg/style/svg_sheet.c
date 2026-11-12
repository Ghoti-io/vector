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
 * @file svg_sheet.c
 *
 * The document's `style` elements as the rules of the cascade: each sheet parsed and compiled by `text`, all of them in one frozen index
 * in document order, and the match context that reads the document's own tree. The cascade is `text`'s: this file hands it the rules and
 * it orders them (origin, importance, specificity, order of appearance) with the presentation attributes and the `style` attribute.
 */

#include <ghoti.io/vector/macros.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ghoti.io/cutil/safemath.h>

#include "svg_style.h"

/* A `style` element's type decides whether it is a CSS sheet: `text/css` or none (SVG 2, 6.? / HTML), compared without regard to case. */
static bool is_css(const GVEC_Svg_Element * e) {
  for (size_t i = 0; i < e->attribute_count; i++) {
    const GVEC_Svg_Attribute * a = &e->attributes[i];
    if (!a->ns && strcmp(a->name, "type") == 0) {
      return a->raw_length == 0 || (a->raw_length == 8 && strncasecmp(a->raw, "text/css", 8) == 0);
    }
  }
  return true;
}

/* The character data of a `style` element, its text and CDATA nodes joined, in memory the caller frees. */
static GVEC_Result sheet_text(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * e, char ** out, size_t * length) {
  size_t total = 0;
  for (const GVEC_Svg_Node * n = e->first_node; n; n = n->next) {
    if (!n->is_element && !gcu_safe_add_size(total, n->text_length, &total)) {
      return GVEC_ERR_LIMIT;
    }
  }
  char * buffer = gvec_alloc(context->allocator, total ? total : 1);
  if (!buffer) {
    return GVEC_ERR_OOM;
  }
  size_t at = 0;
  for (const GVEC_Svg_Node * n = e->first_node; n; n = n->next) {
    if (!n->is_element && n->text_length) {
      memcpy(buffer + at, n->text, n->text_length);
      at += n->text_length;
    }
  }
  *out = buffer;
  *length = total;
  return GVEC_OK;
}

static void refuse(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * e, const char * why) {
  if (!context->sheet_refused) {
    context->sheet_refused = e;
    context->sheet_refused_why = why;
  }
}

static GVEC_Result status_result(GTEXT_CSS_Status status) {
  return status == GTEXT_CSS_E_OOM ? GVEC_ERR_OOM : GVEC_ERR_LIMIT;
}

/* An `@import` is never fetched: it contributes nothing, and the sheet is reported as one that cannot be applied whole. */
static GTEXT_CSS_Sheet * import_none(void * user, const GTEXT_CSS_Import_Request * request) {
  (void) request;
  *(bool *) user = true;
  return NULL;
}

/* One sheet into the index. A sheet that cannot be applied is named and left out; a failure of memory or a limit ends the walk. */
static GVEC_Result add_sheet(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * e, GTEXT_CSS_Rule_Index * index) {
  char * text = NULL;
  size_t length = 0;
  GVEC_Result r = sheet_text(context, e, &text, &length);
  if (r != GVEC_OK) {
    return r;
  }
  bool blank = true;
  for (size_t i = 0; blank && i < length; i++) {
    blank = text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == '\f';
  }
  if (blank) {
    gvec_free(context->allocator, text);
    return GVEC_OK;
  }
  GTEXT_CSS_Parse_Options parse = context->style_options.parse;
  parse.max_input_bytes = context->limits.max_style_bytes;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status status = gtext_css_parse_stylesheet(text, length, &parse, &tree, NULL);
  gvec_free(context->allocator, text);
  if (status != GTEXT_CSS_OK) {
    if (status == GTEXT_CSS_E_OOM) {
      return GVEC_ERR_OOM;
    }
    /* A sheet longer than a limit is not applied, and said so: its elements would be drawn without it. */
    refuse(context, e, "a style sheet is larger than the limits allow");
    return gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_LIMIT, GVEC_LIMIT_STYLE_BYTES, e, NULL,
        "a style sheet is longer than max_style_bytes and is not applied");
  }
  GTEXT_CSS_Sheet_Options options = gtext_css_sheet_options_default();
  options.allocator = context->style_options.parse.allocator;
  bool imported = false;
  options.import = import_none;
  options.import_user = &imported;
  GTEXT_CSS_Sheet * sheet = NULL;
  GTEXT_CSS_Sheet_Result result;
  memset(&result, 0, sizeof result);
  status = gtext_css_sheet_compile(tree, context->engine->reg.registry, &options, &sheet, &result);
  gtext_css_tree_free(tree);
  if (status != GTEXT_CSS_OK) {
    if (status == GTEXT_CSS_E_OOM) {
      return GVEC_ERR_OOM;
    }
    refuse(context, e, "a style sheet is larger than the limits allow");
    return gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_LIMIT, 0, e, NULL,
        "a style sheet has more rules or a deeper nesting than the limits allow and is not applied");
  }
  if (imported) {
    refuse(context, e, "an @import is not fetched");
  }
  size_t total = gtext_css_sheet_diagnostic_total(sheet);
  for (size_t i = 0; i < total; i++) {
    GTEXT_CSS_Sheet_Diagnostic d;
    if (!gtext_css_sheet_diagnostic(sheet, i, &d)) {
      continue;
    }
    if (d.kind == GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED) {
      refuse(context, e, "an @import is not fetched");
    }
    r = gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_STYLE_INVALID, 0, e, NULL,
        d.kind == GTEXT_CSS_SHEET_DIAG_INVALID_SELECTOR  ? "a rule of a style sheet has a selector that is not valid and was dropped"
        : d.kind == GTEXT_CSS_SHEET_DIAG_UNKNOWN_PROPERTY ? "a declaration of a style sheet names a property that is not known and was dropped"
        : d.kind == GTEXT_CSS_SHEET_DIAG_UNJUDGEABLE      ? "a declaration of a style sheet cannot be judged by the engine and was dropped"
        : d.kind == GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED    ? "an @import of a style sheet is not fetched"
        : d.kind == GTEXT_CSS_SHEET_DIAG_INVALID_RULE     ? "a rule of a style sheet is not valid where it is and was dropped"
                                                          : "a declaration of a style sheet is not a value of its property and was dropped");
    if (r == GVEC_ERR_OOM) {
      gtext_css_sheet_free(sheet);
      return r;
    }
  }
  /* A conditional rule (`@media`, `@supports`, `@container`, `@when`) needs an evaluator for its environment; this layer has none, so a sheet
   * that holds one is not applied: guessing its conditions would be drawing a picture the document does not describe. */
  if (gtext_css_sheet_condition_count(sheet) > 0) {
    gtext_css_sheet_free(sheet);
    refuse(context, e, "a style sheet has conditional rules, which are not applied");
    return GVEC_OK;
  }
  GTEXT_CSS_Sheet_Registration registration;
  memset(&registration, 0, sizeof registration);
  registration.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  GTEXT_CSS_Rule_Index_Result added;
  memset(&added, 0, sizeof added);
  status = gtext_css_rule_index_add_sheet(index, sheet, &registration, &added);
  gtext_css_sheet_free(sheet);
  if (status == GTEXT_CSS_E_UNSUPPORTED) {
    refuse(context, e, "a style sheet has conditional rules, which are not applied");
    return GVEC_OK;
  }
  if (status != GTEXT_CSS_OK) {
    return status_result(status);
  }
  return GVEC_OK;
}

GVEC_Result gvec_svg_style_sheets(GVEC_Svg_Style_Context * context) {
  if (context->sheets_ready) {
    return context->sheets_result;
  }
  context->sheets_ready = true;
  GVEC_Result r = GVEC_OK;
  GTEXT_CSS_Rule_Index * index = NULL;
  const GVEC_Svg_Document * doc = context->document;
  for (size_t i = 0; r == GVEC_OK && i < doc->element_count; i++) {
    const GVEC_Svg_Element * e = doc->elements[i];
    if (e->kind != GVEC_SVG_KIND_STYLE || !is_css(e)) {
      continue;
    }
    if (!index) {
      GTEXT_CSS_Rule_Index_Options options = gtext_css_rule_index_options_default();
      options.allocator = context->style_options.parse.allocator;
      GTEXT_CSS_Status status = gtext_css_rule_index_new(context->engine->reg.registry, &options, &index);
      if (status != GTEXT_CSS_OK) {
        r = status_result(status);
        break;
      }
    }
    r = add_sheet(context, e, index);
  }
  if (r == GVEC_OK && index) {
    GTEXT_CSS_Rule_Index_Result frozen;
    memset(&frozen, 0, sizeof frozen);
    GTEXT_CSS_Status status = gtext_css_rule_index_freeze(index, &frozen);
    if (status != GTEXT_CSS_OK) {
      r = status_result(status);
    }
  }
  if (r == GVEC_OK && index) {
    GTEXT_CSS_Selector_Match_Options match = gtext_css_selector_match_options_default();
    GTEXT_CSS_Status status = gtext_css_selector_match_context_new(&context->engine->table, &match, &context->match);
    if (status != GTEXT_CSS_OK) {
      r = status_result(status);
    }
  }
  if (r != GVEC_OK) {
    gtext_css_rule_index_free(index);
    context->match = NULL;
    context->sheets_result = r;
    return r;
  }
  context->rules = index;
  context->style_options.rules = index;
  context->style_options.match = context->match;
  return GVEC_OK;
}

const GVEC_Svg_Element * gvec_svg_style_sheet_refused(const GVEC_Svg_Style_Context * context, const char ** why) {
  if (why) {
    *why = context->sheet_refused_why;
  }
  return context->sheet_refused;
}
