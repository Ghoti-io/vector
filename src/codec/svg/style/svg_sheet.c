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

/* Whether a `style` element's `media` attribute admits it: the list is answered by the conditions of the context (a screen of the viewport's size, in the
 * document's color scheme). A list that cannot be read is `not all`, as the engine reads it, and no `media` is every media. */
static bool media_admits(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * e) {
  for (size_t i = 0; i < e->attribute_count; i++) {
    const GVEC_Svg_Attribute * a = &e->attributes[i];
    if (a->ns || strcmp(a->name, "media") != 0) {
      continue;
    }
    GTEXT_CSS_Media_List * list = NULL;
    if (gtext_css_media_list_parse(a->raw, a->raw_length, NULL, &list, NULL) != GTEXT_CSS_OK || !list) {
      return false;
    }
    bool matches = false;
    GTEXT_CSS_Status status = gtext_css_conditions_matches_list(context->conditions, list, NULL, &matches);
    gtext_css_media_list_free(list);
    return status == GTEXT_CSS_OK && matches;
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

/* One level of the sheets being compiled: the inline sheet of a `style` element, or one an `@import` loaded. */
typedef struct sheet_frame {
  struct sheet_frame * up;
  GVEC_Svg_Style_Context * context;
  const GVEC_Svg_Element * element;  /* the `style` element the whole chain is under */
  const char * url;                  /* the resolved url of the sheet, NULL for the inline one */
  size_t depth;                      /* 0 for the inline one */
  bool failed;                       /* an import could not be applied: the chain is refused (the reason is already given) */
} sheet_frame;

static GVEC_Result compile_text(sheet_frame * frame, const char * text, size_t length, GTEXT_CSS_Sheet ** out);

/* The sheet of one `@import`: asked of the resolver, parsed and compiled like the sheet that names it (so its own imports, at a depth one more). A
 * sheet that cannot be had, or cannot be applied whole, is not applied and the style element is refused (a missing sheet would be drawing the wrong picture). */
static GTEXT_CSS_Sheet * import_sheet(void * user, const GTEXT_CSS_Import_Request * request) {
  sheet_frame * frame = user;
  GVEC_Svg_Style_Context * context = frame->context;
  if (request->has_scope) {
    frame->failed = true;
    refuse(context, frame->element, "an @import has a scope condition, which is not applied");
    return NULL;
  }
  if (!context->res || !gvec_res_enabled(context->res)) {
    frame->failed = true;
    refuse(context, frame->element, "an @import is not fetched");
    return NULL;
  }
  char * reference = gvec_alloc(context->allocator, request->url.length + 1);
  if (!reference) {
    frame->failed = true;
    context->sheets_result = GVEC_ERR_OOM;
    return NULL;
  }
  memcpy(reference, request->url.data, request->url.length);
  reference[request->url.length] = 0;
  gvec_res_outcome outcome;
  gvec_res_entry * entry = NULL;
  const char * base = frame->url ? frame->url : context->base;
  GVEC_Result r = gvec_res_fetch(context->res, GVEC_RESOURCE_STYLE, base, reference, context->limits.max_style_bytes,
      context->res_depth + frame->depth + 1, &outcome, &entry);
  gvec_free(context->allocator, reference);
  if (r != GVEC_OK) {
    frame->failed = true;
    context->sheets_result = r;
    return NULL;
  }
  const char * why = NULL;
  switch (outcome) {
  case GVEC_RES_OK:
    break;
  case GVEC_RES_NO_RESOLVER:
  case GVEC_RES_REFUSED:
    why = "an @import names a style sheet that the resolver refuses";
    break;
  case GVEC_RES_NOT_FOUND:
    why = "an @import names a style sheet that the resolver does not find";
    break;
  case GVEC_RES_FAILED:
    why = "an @import names a style sheet that the resolver failed to give";
    break;
  case GVEC_RES_TOO_BIG:
    why = "an imported style sheet is longer than the limit allows";
    break;
  case GVEC_RES_LIMIT_COUNT:
    why = "an @import would pass max_resources";
    break;
  case GVEC_RES_LIMIT_DEPTH:
    why = "@imports are nested deeper than max_resource_depth";
    break;
  }
  if (why) {
    frame->failed = true;
    refuse(context, frame->element, why);
    return NULL;
  }
  /* A sheet that imports itself, directly or through others, is a cycle: it contributes nothing and the chain is not applied. */
  for (const sheet_frame * f = frame; f; f = f->up) {
    if (f->url && strcmp(f->url, entry->url) == 0) {
      frame->failed = true;
      refuse(context, frame->element, "an @import closes a cycle of style sheets");
      return NULL;
    }
  }
  sheet_frame inner = {frame, context, frame->element, entry->url, frame->depth + 1, false};
  GTEXT_CSS_Sheet * sheet = NULL;
  r = compile_text(&inner, (const char *) entry->bytes, entry->size, &sheet);
  if (r != GVEC_OK) {
    frame->failed = true;
    context->sheets_result = r;
    return NULL;
  }
  if (inner.failed || !sheet) {
    frame->failed = true;
    gtext_css_sheet_free(sheet);
    return NULL;
  }
  return sheet;
}

static GVEC_Result report_sheet(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * e, GTEXT_CSS_Sheet * sheet) {
  size_t total = gtext_css_sheet_diagnostic_total(sheet);
  for (size_t i = 0; i < total; i++) {
    GTEXT_CSS_Sheet_Diagnostic d;
    if (!gtext_css_sheet_diagnostic(sheet, i, &d)) {
      continue;
    }
    if (d.kind == GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED) {
      refuse(context, e, "an @import is not fetched");
    }
    GVEC_Result r = gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_STYLE_INVALID, 0, e, NULL,
        d.kind == GTEXT_CSS_SHEET_DIAG_INVALID_SELECTOR  ? "a rule of a style sheet has a selector that is not valid and was dropped"
        : d.kind == GTEXT_CSS_SHEET_DIAG_UNKNOWN_PROPERTY ? "a declaration of a style sheet names a property that is not known and was dropped"
        : d.kind == GTEXT_CSS_SHEET_DIAG_UNJUDGEABLE      ? "a declaration of a style sheet cannot be judged by the engine and was dropped"
        : d.kind == GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED    ? "an @import of a style sheet was not applied (the refusal of its style element says why)"
        : d.kind == GTEXT_CSS_SHEET_DIAG_INVALID_RULE     ? "a rule of a style sheet is not valid where it is and was dropped"
                                                          : "a declaration of a style sheet is not a value of its property and was dropped");
    if (r == GVEC_ERR_OOM) {
      return r;
    }
  }
  return GVEC_OK;
}

/* Parse and compile the text of a sheet. `*out` is NULL when it cannot be applied (reported, and `frame->failed` where it is a refusal). */
static GVEC_Result compile_text(sheet_frame * frame, const char * text, size_t length, GTEXT_CSS_Sheet ** out) {
  GVEC_Svg_Style_Context * context = frame->context;
  const GVEC_Svg_Element * e = frame->element;
  *out = NULL;
  GTEXT_CSS_Parse_Options parse = context->style_options.parse;
  parse.max_input_bytes = context->limits.max_style_bytes;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status status = gtext_css_parse_stylesheet(text, length, &parse, &tree, NULL);
  if (status != GTEXT_CSS_OK) {
    if (status == GTEXT_CSS_E_OOM) {
      return GVEC_ERR_OOM;
    }
    /* A sheet longer than a limit is not applied, and said so: its elements would be drawn without it. */
    frame->failed = true;
    refuse(context, e, "a style sheet is larger than the limits allow");
    return gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_LIMIT, GVEC_LIMIT_STYLE_BYTES, e, NULL,
        "a style sheet is longer than max_style_bytes and is not applied");
  }
  GTEXT_CSS_Sheet_Options options = gtext_css_sheet_options_default();
  options.allocator = context->style_options.parse.allocator;
  options.import = import_sheet;
  options.import_user = frame;
  GTEXT_CSS_Sheet * sheet = NULL;
  GTEXT_CSS_Sheet_Result result;
  memset(&result, 0, sizeof result);
  status = gtext_css_sheet_compile(tree, context->engine->reg.registry, &options, &sheet, &result);
  if (context->atrules) {
    /* The at-rules the style layer reads (`@font-feature-values`) are collected from the tree, after the sheets it imports (their trees were added while it compiled). */
    GTEXT_CSS_Status collected = gtext_css_atrule_set_add_tree(context->atrules, tree);
    if (collected == GTEXT_CSS_E_OOM) {
      gtext_css_tree_free(tree);
      gtext_css_sheet_free(sheet);
      return GVEC_ERR_OOM;
    }
  }
  gtext_css_tree_free(tree);
  if (context->sheets_result != GVEC_OK) {
    gtext_css_sheet_free(sheet);
    return context->sheets_result;
  }
  if (status != GTEXT_CSS_OK) {
    if (status == GTEXT_CSS_E_OOM) {
      return GVEC_ERR_OOM;
    }
    frame->failed = true;
    refuse(context, e, "a style sheet is larger than the limits allow");
    return gvec_svg_style_diag(context, GVEC_DIAG_WARNING, GVEC_DIAG_LIMIT, 0, e, NULL,
        "a style sheet has more rules or a deeper nesting than the limits allow and is not applied");
  }
  GVEC_Result r = report_sheet(context, e, sheet);
  if (r != GVEC_OK) {
    gtext_css_sheet_free(sheet);
    return r;
  }
  *out = sheet;
  return GVEC_OK;
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
  sheet_frame frame = {NULL, context, e, NULL, 0, false};
  GTEXT_CSS_Sheet * sheet = NULL;
  r = compile_text(&frame, text, length, &sheet);
  gvec_free(context->allocator, text);
  if (r != GVEC_OK) {
    return r;
  }
  if (!sheet) {
    return GVEC_OK;
  }
  if (frame.failed) {
    /* An import could not be applied (the reason is given): the sheet is not applied whole. */
    gtext_css_sheet_free(sheet);
    return GVEC_OK;
  }
  /* A conditional rule (`@media`, `@supports`, `@container`, `@when`) is answered by the conditions of the context (see gvec_svg_style_sheets()). */
  GTEXT_CSS_Sheet_Registration registration;
  memset(&registration, 0, sizeof registration);
  registration.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  GTEXT_CSS_Rule_Index_Result added;
  memset(&added, 0, sizeof added);
  GTEXT_CSS_Status status = gtext_css_rule_index_add_sheet(index, sheet, &registration, &added);
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
      if (!context->conditions) {
        /* The environment of the rules' conditions is a screen of the viewport's size (the engine's own environment, which the lengths in vw
         * and vh are read against), in the document's color scheme; no element is a container, so a container query is false. */
        GTEXT_CSS_Media_Environment media = gtext_css_media_environment_default(&context->engine->env);
        media.prefers_color_scheme = context->color_scheme == GVEC_COLOR_SCHEME_DARK ? GTEXT_CSS_MQ_SCHEME_DARK : GTEXT_CSS_MQ_SCHEME_LIGHT;
        GTEXT_CSS_Conditions_Options conditions_options = gtext_css_conditions_options_default();
        conditions_options.allocator = context->style_options.parse.allocator;
        conditions_options.media = &media;
        GTEXT_CSS_Status made = gtext_css_conditions_new(context->engine->reg.registry, &conditions_options, &context->conditions);
        if (made != GTEXT_CSS_OK) {
          r = status_result(made);
          break;
        }
      }
      options.conditions = context->conditions;
      GTEXT_CSS_Atrule_Set_Options atrule_options = gtext_css_atrule_set_options_default();
      atrule_options.conditions = context->conditions;
      GTEXT_CSS_Status set_made = gtext_css_atrule_set_new(context->engine->reg.registry, &atrule_options, &context->atrules);
      if (set_made != GTEXT_CSS_OK) {
        r = status_result(set_made);
        break;
      }
      GTEXT_CSS_Status status = gtext_css_rule_index_new(context->engine->reg.registry, &options, &index);
      if (status != GTEXT_CSS_OK) {
        r = status_result(status);
        break;
      }
    }
    if (!media_admits(context, e)) {
      continue;
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

const GTEXT_CSS_Feature_Values * gvec_svg_style_feature_values(GVEC_Svg_Style_Context * context, const char * family, size_t length) {
  if (!context->atrules) {
    return NULL;
  }
  if (!context->atrules_frozen) {
    context->atrules_frozen = true;
    if (gtext_css_atrule_set_freeze(context->atrules) != GTEXT_CSS_OK) {
      return NULL;
    }
  }
  char key[256];
  size_t n = gtext_css_atrule_set_family_key(family, length, key, sizeof key);
  return n ? gtext_css_atrule_set_feature_values(context->atrules, key, n) : NULL;
}

const GTEXT_CSS_Color_Profile * gvec_svg_style_color_profile(GVEC_Svg_Style_Context * context, const char * name, size_t length) {
  if (!context->atrules) {
    return NULL;
  }
  if (!context->atrules_frozen) {
    context->atrules_frozen = true;
    if (gtext_css_atrule_set_freeze(context->atrules) != GTEXT_CSS_OK) {
      return NULL;
    }
  }
  return gtext_css_atrule_set_color_profile(context->atrules, name, length);
}
