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
 * The public face of the typed document (svg.h).
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/base64.h>

#include "../../core/core_internal.h"
#include "svg_doc.h"
#include "svg_internal.h"

const GVEC_Svg_Document * gvec_svg_document_of(const GVEC_Doc * doc) {
  if (!doc || !doc->codec || !doc->priv || strcmp(doc->codec->name, "svg") != 0) {
    return NULL;
  }
  return ((const gvec_svg_doc *) doc->priv)->document;
}

const GVEC_Svg_Document * gvec_svg_document(const GVEC_Doc * doc) {
  return gvec_svg_document_of(doc);
}

const GVEC_Svg_Element * gvec_svg_root(const GVEC_Svg_Document * document) {
  return document ? document->root : NULL;
}

size_t gvec_svg_element_count(const GVEC_Svg_Document * document) {
  return document ? document->element_count : 0;
}

GVEC_Svg_Kind gvec_svg_element_kind(const GVEC_Svg_Element * e) {
  return e ? e->kind : GVEC_SVG_KIND_UNKNOWN;
}

const char * gvec_svg_element_name(const GVEC_Svg_Element * e) {
  return e ? e->name : NULL;
}

const char * gvec_svg_element_namespace(const GVEC_Svg_Element * e) {
  return e ? e->ns : NULL;
}

uint64_t gvec_svg_element_key(const GVEC_Svg_Element * e) {
  return e ? e->key : 0;
}

size_t gvec_svg_element_line(const GVEC_Svg_Element * e) {
  return e ? e->node.line : 0;
}

size_t gvec_svg_element_column(const GVEC_Svg_Element * e) {
  return e ? e->node.column : 0;
}

size_t gvec_svg_element_offset(const GVEC_Svg_Element * e) {
  return e ? e->node.offset : 0;
}

const GVEC_Svg_Element * gvec_svg_element_parent(const GVEC_Svg_Element * e) {
  return e ? e->node.parent : NULL;
}

const char * gvec_svg_element_id(const GVEC_Svg_Element * e) {
  return e ? e->id : NULL;
}

const GVEC_Svg_Node * gvec_svg_element_first_node(const GVEC_Svg_Element * e) {
  return e ? e->first_node : NULL;
}

const GVEC_Svg_Node * gvec_svg_node_next(const GVEC_Svg_Node * n) {
  return n ? n->next : NULL;
}

const GVEC_Svg_Element * gvec_svg_node_element(const GVEC_Svg_Node * n) {
  return n ? n->element : NULL;
}

const char * gvec_svg_node_text(const GVEC_Svg_Node * n, size_t * length) {
  if (!n || n->is_element) {
    return NULL;
  }
  if (length) {
    *length = n->text_length;
  }
  return n->text;
}

static const GVEC_Svg_Element * next_element(const GVEC_Svg_Node * n) {
  while (n && !n->is_element) {
    n = n->next;
  }
  return n ? n->element : NULL;
}

const GVEC_Svg_Element * gvec_svg_element_first_child(const GVEC_Svg_Element * e) {
  return e ? next_element(e->first_node) : NULL;
}

const GVEC_Svg_Element * gvec_svg_element_next_sibling(const GVEC_Svg_Element * e) {
  return e ? next_element(e->node.next) : NULL;
}

bool gvec_svg_element_conditional_false(const GVEC_Svg_Element * e) {
  return e && (e->flags & GVEC_SVG_ELEMENT_COND_FALSE);
}

const GVEC_Svg_Element * gvec_svg_element_switch_selected(const GVEC_Svg_Element * e) {
  return e ? e->switch_selected : NULL;
}

bool gvec_svg_element_on_cycle(const GVEC_Svg_Element * e) {
  return e && (e->flags & GVEC_SVG_ELEMENT_ON_CYCLE);
}

bool gvec_svg_element_expansion_limited(const GVEC_Svg_Element * e) {
  return e && (e->flags & GVEC_SVG_ELEMENT_EXPANSION_LIMITED);
}

uint64_t gvec_svg_element_expansion(const GVEC_Svg_Element * e) {
  return e ? e->expansion : 0;
}

const GVEC_Svg_Element * gvec_svg_element_href_target(const GVEC_Svg_Element * e) {
  if (!e || (e->flags & (GVEC_SVG_ELEMENT_ON_CYCLE | GVEC_SVG_ELEMENT_EXPANSION_LIMITED))) {
    return NULL;
  }
  return e->href_target;
}

size_t gvec_svg_element_attribute_count(const GVEC_Svg_Element * e) {
  return e ? e->attribute_count : 0;
}

const GVEC_Svg_Attribute * gvec_svg_element_attribute_at(const GVEC_Svg_Element * e, size_t index) {
  return e && index < e->attribute_count ? &e->attributes[index] : NULL;
}

const GVEC_Svg_Attribute * gvec_svg_element_find_attribute(const GVEC_Svg_Element * e, const char * name) {
  if (!e || !name) {
    return NULL;
  }
  for (size_t i = 0; i < e->attribute_count; i++) {
    if (!e->attributes[i].ns && strcmp(e->attributes[i].name, name) == 0) {
      return &e->attributes[i];
    }
  }
  return NULL;
}

const char * gvec_svg_attribute_name(const GVEC_Svg_Attribute * a) {
  return a ? a->name : NULL;
}

const char * gvec_svg_attribute_namespace(const GVEC_Svg_Attribute * a) {
  return a ? a->ns : NULL;
}

const char * gvec_svg_attribute_raw(const GVEC_Svg_Attribute * a, size_t * length) {
  if (!a) {
    return NULL;
  }
  if (length) {
    *length = a->raw_length;
  }
  return a->raw;
}

GVEC_Svg_Attribute_Class gvec_svg_attribute_class(const GVEC_Svg_Attribute * a) {
  return a ? a->cls : GVEC_SVG_ATTR_UNKNOWN;
}

const char * gvec_svg_attribute_grammar(const GVEC_Svg_Attribute * a) {
  return a ? gvec_svg_grammar_name((GVEC_Svg_Grammar) a->grammar) : "none";
}

bool gvec_svg_attribute_is_presentation(const GVEC_Svg_Attribute * a) {
  return a && a->presentation;
}

bool gvec_svg_attribute_is_partial(const GVEC_Svg_Attribute * a) {
  return a && a->partial;
}

size_t gvec_svg_attribute_error_offset(const GVEC_Svg_Attribute * a) {
  return a ? a->error_offset : 0;
}

const char * gvec_svg_attribute_error_reason(const GVEC_Svg_Attribute * a) {
  return a ? a->error_reason : NULL;
}

size_t gvec_svg_attribute_line(const GVEC_Svg_Attribute * a) {
  return a ? a->line : 0;
}

size_t gvec_svg_attribute_column(const GVEC_Svg_Attribute * a) {
  return a ? a->column : 0;
}

size_t gvec_svg_attribute_offset(const GVEC_Svg_Attribute * a) {
  return a ? a->offset : 0;
}

GVEC_Svg_Value_Kind gvec_svg_attribute_value_kind(const GVEC_Svg_Attribute * a) {
  return a && a->cls == GVEC_SVG_ATTR_TYPED ? (GVEC_Svg_Value_Kind) a->value_kind : GVEC_SVG_VALUE_NONE;
}

#define ASK(a, kind) ((a) && (a)->cls == GVEC_SVG_ATTR_TYPED && (a)->value_kind == (kind))

bool gvec_svg_attribute_number(const GVEC_Svg_Attribute * a, double * out) {
  if (!ASK(a, GVEC_SVG_VALUE_NUMBER) && !ASK(a, GVEC_SVG_VALUE_OFFSET)) {
    return false;
  }
  *out = a->value.number;
  return true;
}

bool gvec_svg_attribute_integer(const GVEC_Svg_Attribute * a, long * out) {
  if (!ASK(a, GVEC_SVG_VALUE_INTEGER)) {
    return false;
  }
  *out = a->value.integer;
  return true;
}

bool gvec_svg_attribute_length(const GVEC_Svg_Attribute * a, GVEC_Svg_Length * out) {
  if (!ASK(a, GVEC_SVG_VALUE_LENGTH)) {
    return false;
  }
  *out = a->value.length;
  return true;
}

bool gvec_svg_attribute_numbers(const GVEC_Svg_Attribute * a, const double ** items, size_t * count) {
  if (!ASK(a, GVEC_SVG_VALUE_NUMBER_LIST) && !ASK(a, GVEC_SVG_VALUE_POINTS)) {
    return false;
  }
  *items = a->value.numbers.items;
  *count = a->value.numbers.count;
  return true;
}

bool gvec_svg_attribute_lengths(const GVEC_Svg_Attribute * a, const GVEC_Svg_Length ** items, size_t * count) {
  if (!ASK(a, GVEC_SVG_VALUE_LENGTH_LIST)) {
    return false;
  }
  *items = a->value.lengths.items;
  *count = a->value.lengths.count;
  return true;
}

bool gvec_svg_attribute_path(const GVEC_Svg_Attribute * a, const GVEC_Svg_Path_Command ** commands, size_t * command_count,
    const GVEC_Svg_Path_Segment ** segments, size_t * segment_count) {
  if (!ASK(a, GVEC_SVG_VALUE_PATH)) {
    return false;
  }
  *commands = a->value.path.commands;
  *command_count = a->value.path.command_count;
  *segments = a->value.path.segments;
  *segment_count = a->value.path.segment_count;
  return true;
}

bool gvec_svg_attribute_view_box(const GVEC_Svg_Attribute * a, GVEC_Svg_View_Box * out) {
  if (!ASK(a, GVEC_SVG_VALUE_VIEW_BOX)) {
    return false;
  }
  *out = a->value.view_box;
  return true;
}

bool gvec_svg_attribute_preserve_aspect_ratio(const GVEC_Svg_Attribute * a, GVEC_Svg_Preserve_Aspect_Ratio * out) {
  if (!ASK(a, GVEC_SVG_VALUE_PRESERVE_ASPECT_RATIO)) {
    return false;
  }
  *out = a->value.par;
  return true;
}

bool gvec_svg_attribute_orient(const GVEC_Svg_Attribute * a, GVEC_Svg_Orient * out) {
  if (!ASK(a, GVEC_SVG_VALUE_ORIENT)) {
    return false;
  }
  *out = a->value.orient;
  return true;
}

bool gvec_svg_attribute_reference(const GVEC_Svg_Attribute * a, GVEC_Svg_Reference * out) {
  if (!ASK(a, GVEC_SVG_VALUE_REFERENCE)) {
    return false;
  }
  *out = a->value.reference;
  return true;
}

bool gvec_svg_attribute_keyword(const GVEC_Svg_Attribute * a, size_t * index, const char ** keyword) {
  if (!ASK(a, GVEC_SVG_VALUE_KEYWORD)) {
    return false;
  }
  *index = a->value.keyword.index;
  *keyword = a->value.keyword.word;
  return true;
}

bool gvec_svg_attribute_string(const GVEC_Svg_Attribute * a, const char ** out) {
  if (!ASK(a, GVEC_SVG_VALUE_STRING)) {
    return false;
  }
  *out = a->value.string;
  return true;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

GVEC_Result gvec_svg_data_uri_decode(const GVEC_Svg_Data_Uri * uri, void * out, size_t capacity, size_t * written) {
  if (!uri || !written || (capacity && !out)) {
    return GVEC_ERR_INVALID;
  }
  if (uri->base64) {
    /* data: URIs and MIME: white space between characters is skipped and padding may be left out. */
    GCU_Base64_Decode_Options o;
    memset(&o, 0, sizeof o);
    o.padding = GCU_BASE64_PADDING_OPTIONAL;
    o.whitespace = GCU_BASE64_WHITESPACE_SKIP_ASCII;
    GCU_Base64_Status s = gcu_base64_decode(uri->payload, uri->payload_length, out, capacity, &o, written, NULL);
    if (s == GCU_BASE64_OK) {
      return GVEC_OK;
    }
    return s == GCU_BASE64_ERR_CAPACITY ? GVEC_ERR_LIMIT : s == GCU_BASE64_ERR_INVALID ? GVEC_ERR_INVALID : GVEC_ERR_FORMAT;
  }
  /* Percent-encoded bytes. */
  size_t needed = 0;
  for (size_t i = 0; i < uri->payload_length; i++) {
    if (uri->payload[i] == '%') {
      if (i + 2 >= uri->payload_length || hex_value(uri->payload[i + 1]) < 0 || hex_value(uri->payload[i + 2]) < 0) {
        return GVEC_ERR_FORMAT;
      }
      i += 2;
    }
    needed++;
  }
  *written = needed;
  if (needed > capacity) {
    return GVEC_ERR_LIMIT;
  }
  unsigned char * p = out;
  size_t o = 0;
  for (size_t i = 0; i < uri->payload_length; i++) {
    if (uri->payload[i] == '%') {
      p[o++] = (unsigned char) (hex_value(uri->payload[i + 1]) * 16 + hex_value(uri->payload[i + 2]));
      i += 2;
    } else {
      p[o++] = (unsigned char) uri->payload[i];
    }
  }
  return GVEC_OK;
}
