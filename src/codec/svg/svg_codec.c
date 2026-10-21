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
 * @file svg_codec.c
 *
 * The SVG codec object, its XML options, probe and lowering.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "../../core/core_internal.h"
#include "svg_internal.h"

const GVEC_Codec gvec_svg_codec = {
    .name = "svg",
    .capabilities = GVEC_CAP_LOAD | GVEC_CAP_LOWER,
    .probe = gvec_svg_probe,
    .load = gvec_svg_load,
    .dump = NULL,
    .lower_edit = gvec_svg_lower_edit,
    .lower_list = gvec_svg_lower_list,
    .save = NULL,
    .free_private = gvec_svg_free,
};

void gvec_svg_xml_options(const GVEC_Limits * limits, size_t input_bytes, const GVEC_Allocator * allocator,
    GTEXT_XML_Parse_Options * out) {
  *out = gtext_xml_parse_options_default();
  out->allocator = allocator;
  out->max_input_bytes = input_bytes;
  out->max_depth = limits->max_depth;
  out->max_attributes = limits->max_attributes;
  out->max_expanded_bytes = limits->max_entity_expanded_bytes;
  out->max_entity_depth = limits->max_entity_depth;
  out->max_references = limits->max_entity_references;
  /* D7: the internal subset is read, so files that declare their own entities
   * load; nothing outside the file is ever read. With no resolver and access
   * denied the parser has no way to fetch, and SKIP keeps a reference to an
   * external entity in the tree instead of failing the load. */
  out->dtd_mode = GTEXT_XML_DTD_PROCESS;
  out->dtd_external_policy = GTEXT_XML_DTD_EXTERNAL_SKIP;
  out->external_access = GTEXT_XML_EXTERNAL_ACCESS_DENY;
  out->resolver = NULL;
  out->resolver_user_data = NULL;
  out->base_uri = NULL;
}

/* The first element decides, so the reader stops there: what it parses is the
 * prolog (declaration, comments, PIs, doctype and internal subset), by the same
 * grammar that will parse the file. */
int gvec_svg_probe(const unsigned char * data, size_t size, const GVEC_Limits * limits,
    const GVEC_Allocator * allocator, GVEC_Diagnostics * why_not) {
  GTEXT_XML_Parse_Options options;
  GTEXT_XML_Reader * reader;
  GTEXT_XML_Event event;
  GTEXT_XML_Error error;
  GTEXT_XML_Status status;
  int confidence = 0;

  gvec_svg_xml_options(limits, size, allocator, &options);
  reader = gtext_xml_reader_new(&options);
  if (!reader) {
    return -(int) GVEC_ERR_OOM;
  }
  memset(&error, 0, sizeof(error));
  status = gtext_xml_reader_feed(reader, size ? (const void *) data : (const void *) "", size, &error);
  if (status == GTEXT_XML_OK) {
    status = gtext_xml_reader_feed(reader, NULL, 0, &error);
  }
  for (;;) {
    GTEXT_XML_Status next = gtext_xml_reader_next(reader, &event);
    if (next != GTEXT_XML_OK) {
      /* No start tag: the reader stopped at an error (reported where the
       * failing feed said), or the input ended with no element. Out of
       * memory and a limit are not a verdict on the format, so they are
       * passed on; any other error says the bytes are not this format. */
      if (next != GTEXT_XML_E_STATE && next != GTEXT_XML_E_INCOMPLETE) {
        GVEC_Result r;
        error.code = next;
        if (!error.message) {
          error.message = "not well-formed XML before any element";
        }
        r = gvec_svg_parse_failed(&error, GVEC_TRANSPORT_NONE, why_not);
        if (r == GVEC_ERR_OOM || r == GVEC_ERR_LIMIT) {
          confidence = -(int) r;
        }
      }
      break;
    }
    if (event.type == GTEXT_XML_EVENT_ELEMENT_START) {
      if (event.namespace_uri && strcmp(event.namespace_uri, GTEXT_MARKUP_NS_SVG) == 0 && event.local_name &&
          strcmp(event.local_name, "svg") == 0) {
        confidence = 100;
      }
      else {
        gvec_diag_add(why_not, GVEC_DIAG_ERROR, GVEC_DIAG_NOT_SVG, 0, event.location.line, event.location.column,
            event.location.offset, event.local_name, "the root element is not svg in the SVG namespace");
      }
      break;
    }
  }
  gtext_xml_error_free(&error);
  gtext_xml_reader_free(reader);
  return confidence;
}

static GVEC_Result gvec_svg_refuse_root(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GVEC_Diagnostics * diagnostics) {
  const gvec_svg_doc * svg = doc->priv;
  if (options) {
    for (size_t i = 0; i < sizeof(options->_reserved); i++) {
      if (options->_reserved[i] != 0) {
        return GVEC_ERR_INVALID;
      }
    }
  }
  return gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LOWER_REFUSED, GVEC_LOWER_NOT_YET_LOWERED,
      svg->root_line, svg->root_column, svg->root_offset, "svg", "not lowered: this version lowers nothing yet");
}

GVEC_Result gvec_svg_lower_edit(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_Edit * edit,
    GVEC_Diagnostics * diagnostics) {
  (void) edit;
  return gvec_svg_refuse_root(doc, options, diagnostics);
}

GVEC_Result gvec_svg_lower_list(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics) {
  (void) list;
  return gvec_svg_refuse_root(doc, options, diagnostics);
}
