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
 * Conditional processing and `switch` (svg_doc.h), after SVG 2 section 5.7.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "svg_doc.h"

static unsigned char lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char) (c + 32) : c;
}

/* One user tag matches one attribute tag: equal, or the user tag is a prefix followed by '-' (ASCII case-insensitive). */
static bool tag_matches(const char * user, const char * tag) {
  size_t ul = strlen(user), tl = strlen(tag);
  if (ul > tl) {
    return false;
  }
  for (size_t i = 0; i < ul; i++) {
    if (lower((unsigned char) user[i]) != lower((unsigned char) tag[i])) {
      return false;
    }
  }
  return ul == tl || tag[ul] == '-';
}

static bool language_true(const GVEC_Svg_Attribute * a, const char * const * languages, size_t language_count) {
  for (size_t t = 0; t < a->value.languages.count; t++) {
    for (size_t u = 0; u < language_count; u++) {
      if (tag_matches(languages[u], a->value.languages.tags[t])) {
        return true;
      }
    }
  }
  return false; /* includes the empty list: a null or empty value evaluates to false */
}

static bool is_switch_candidate(GVEC_Svg_Kind kind) {
  switch (kind) {
  case GVEC_SVG_KIND_CIRCLE: case GVEC_SVG_KIND_ELLIPSE: case GVEC_SVG_KIND_LINE: case GVEC_SVG_KIND_PATH:
  case GVEC_SVG_KIND_POLYGON: case GVEC_SVG_KIND_POLYLINE: case GVEC_SVG_KIND_RECT: case GVEC_SVG_KIND_A:
  case GVEC_SVG_KIND_AUDIO: case GVEC_SVG_KIND_CANVAS: case GVEC_SVG_KIND_FOREIGN_OBJECT: case GVEC_SVG_KIND_G:
  case GVEC_SVG_KIND_IFRAME: case GVEC_SVG_KIND_IMAGE: case GVEC_SVG_KIND_SVG: case GVEC_SVG_KIND_SWITCH:
  case GVEC_SVG_KIND_TEXT: case GVEC_SVG_KIND_USE: case GVEC_SVG_KIND_VIDEO:
    return true;
  default:
    return false;
  }
}

GVEC_Result gvec_svg_conditional(GVEC_Svg_Document * doc, const char * const * languages, size_t language_count) {
  static const char * const english[] = {"en"};
  if (!languages) {
    languages = english;
    language_count = 1;
  }
  for (size_t i = 0; i < doc->element_count; i++) {
    GVEC_Svg_Element * e = doc->elements[i];
    if (e->kind == GVEC_SVG_KIND_FOREIGN) {
      continue;
    }
    bool ok = true;
    for (size_t k = 0; k < e->attribute_count; k++) {
      const GVEC_Svg_Attribute * a = &e->attributes[k];
      if (a->cls != GVEC_SVG_ATTR_TYPED) {
        continue;
      }
      if (a->grammar == GVEC_SVG_GRAMMAR_SYSTEM_LANGUAGE && !language_true(a, languages, language_count)) {
        ok = false;
      } else if (a->grammar == GVEC_SVG_GRAMMAR_REQUIRED_EXTENSIONS) {
        ok = false; /* no extension is supported, and an empty list is false */
      }
    }
    if (!ok) {
      e->flags |= GVEC_SVG_ELEMENT_COND_FALSE;
    }
  }
  /* Children are evaluated before the switch that selects among them. */
  for (size_t i = 0; i < doc->element_count; i++) {
    GVEC_Svg_Element * e = doc->elements[i];
    if (e->kind != GVEC_SVG_KIND_SWITCH) {
      continue;
    }
    for (GVEC_Svg_Node * n = e->first_node; n; n = n->next) {
      if (!n->is_element) {
        continue;
      }
      GVEC_Svg_Element * c = n->element;
      if (is_switch_candidate(c->kind) && !(c->flags & GVEC_SVG_ELEMENT_COND_FALSE)) {
        e->switch_selected = c;
        break;
      }
    }
  }
  return GVEC_OK;
}
