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
 * @file svg_load.c
 *
 * Loading SVG (L8): the XML parse, the root check, the limits the parser does
 * not count, and the report of what was kept unresolved.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include "../../core/core_internal.h"
#include "svg_doc.h"
#include "svg_internal.h"

/* A status of the XML parser that is one of our limits, as that field. */
static bool gvec_svg_limit_field(GTEXT_XML_Status status, GVEC_Transport transport, GVEC_Limit_Field * out) {
  switch (status) {
  case GTEXT_XML_E_LIMIT_INPUT:
    *out = transport == GVEC_TRANSPORT_GZIP ? GVEC_LIMIT_INFLATED_BYTES : GVEC_LIMIT_INPUT_BYTES;
    return true;
  case GTEXT_XML_E_LIMIT_DEPTH:
    *out = GVEC_LIMIT_DEPTH;
    return true;
  case GTEXT_XML_E_LIMIT_ATTRIBUTES:
    *out = GVEC_LIMIT_ATTRIBUTES;
    return true;
  case GTEXT_XML_E_LIMIT_EXPANSION:
    *out = GVEC_LIMIT_ENTITY_EXPANDED_BYTES;
    return true;
  case GTEXT_XML_E_LIMIT_ENTITY_DEPTH:
    *out = GVEC_LIMIT_ENTITY_DEPTH;
    return true;
  case GTEXT_XML_E_LIMIT_REFERENCES:
    *out = GVEC_LIMIT_ENTITY_REFERENCES;
    return true;
  case GTEXT_XML_E_LIMIT_AMPLIFICATION:
    *out = GVEC_LIMIT_ENTITY_AMPLIFICATION;
    return true;
  default:
    return false;
  }
}

/* The parser's own fixed caps, which GVEC_Limits does not expose. */
static bool gvec_svg_is_fixed_limit(GTEXT_XML_Status status) {
  switch (status) {
  case GTEXT_XML_E_LIMIT_VALUE:
  case GTEXT_XML_E_LIMIT_NAME:
  case GTEXT_XML_E_LIMIT_NODES:
  case GTEXT_XML_E_LIMIT_NAMESPACES:
  case GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE:
  case GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL:
  case GTEXT_XML_E_LIMIT_DECLARATIONS:
  case GTEXT_XML_E_LIMIT_WORK:
    return true;
  default:
    return false;
  }
}

GVEC_Result gvec_svg_parse_failed(
    const GTEXT_XML_Error * error, GVEC_Transport transport, GVEC_Diagnostics * diagnostics) {
  GVEC_Limit_Field field;
  const char * message = error->message ? error->message : "the XML parser refused the document";
  if (error->code == GTEXT_XML_E_OOM) {
    gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_XML, (uint32_t) error->code, error->line, error->column,
        error->byte_offset, NULL, message);
    return GVEC_ERR_OOM;
  }
  if (gvec_svg_limit_field(error->code, transport, &field)) {
    gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, (uint32_t) field, error->line, error->column,
        error->byte_offset, NULL, message);
    return GVEC_ERR_LIMIT;
  }
  gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_XML, (uint32_t) error->code, error->line, error->column,
      error->byte_offset, NULL, message);
  if (gvec_svg_is_fixed_limit(error->code)) {
    return GVEC_ERR_LIMIT;
  }
  if (error->code == GTEXT_XML_E_INVALID) {
    /* The options are ours; the parser refusing them is our defect. */
    return GVEC_ERR_INTERNAL;
  }
  return GVEC_ERR_FORMAT;
}

/* The local name of a node's nearest element ancestor, or NULL. */
static const char * gvec_svg_parent_element(const GTEXT_Markup_Node * node) {
  for (const GTEXT_Markup_Node * p = gtext_markup_node_parent(node); p; p = gtext_markup_node_parent(p)) {
    if (gtext_markup_node_kind(p) == GTEXT_MARKUP_KIND_ELEMENT) {
      return gtext_markup_element_local_name(p);
    }
  }
  return NULL;
}

GVEC_Result gvec_svg_load(GVEC_Doc * doc, const unsigned char * data, size_t size, const GVEC_Limits * limits,
    const GVEC_Load_Options * load_options, GVEC_Diagnostics * diagnostics) {
  GTEXT_XML_Parse_Options options;
  GTEXT_XML_Error error;
  GTEXT_Markup_Document * tree = NULL;
  GTEXT_Markup_Node * root;
  GTEXT_Markup_Location location;
  GTEXT_XML_Status status;
  gvec_svg_doc * svg;
  size_t elements = 0;
  size_t input_cap =
      doc->transport == GVEC_TRANSPORT_GZIP ? limits->max_inflated_bytes : limits->max_input_bytes;

  memset(&error, 0, sizeof(error));
  gvec_svg_xml_options(limits, input_cap, doc->allocator, &options);
  status = gtext_xml_parse(size ? (const void *) data : (const void *) "", size, &options, &tree, &error);
  if (status != GTEXT_XML_OK) {
    GVEC_Result r = gvec_svg_parse_failed(&error, doc->transport, diagnostics);
    gtext_xml_error_free(&error);
    return r;
  }
  gtext_xml_error_free(&error);

  root = gtext_markup_document_element(tree);
  memset(&location, 0, sizeof(location));
  if (root) {
    gtext_markup_node_location(root, &location);
  }
  if (!root || !gtext_markup_element_is(root, GTEXT_MARKUP_NS_SVG, "svg")) {
    gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_NOT_SVG, 0, location.line, location.column,
        location.offset, root ? gtext_markup_element_local_name(root) : NULL,
        "the root element is not svg in the SVG namespace");
    gtext_markup_document_free(tree);
    return GVEC_ERR_FORMAT;
  }

  /* The elements cap, which the parser does not count (its own cap is on
   * nodes of every kind); and the external references it kept unresolved. */
  for (GTEXT_Markup_Node * node = root; node; node = gtext_markup_node_next_in_order(root, node, false)) {
    GTEXT_Markup_Kind kind = gtext_markup_node_kind(node);
    if (kind == GTEXT_MARKUP_KIND_ELEMENT) {
      if (++elements > limits->max_elements) {
        GTEXT_Markup_Location at;
        memset(&at, 0, sizeof(at));
        gtext_markup_node_location(node, &at);
        gvec_diag_add(diagnostics, GVEC_DIAG_ERROR, GVEC_DIAG_LIMIT, GVEC_LIMIT_ELEMENTS, at.line, at.column,
            at.offset, gtext_markup_element_local_name(node), "the document has more than max_elements elements");
        gtext_markup_document_free(tree);
        return GVEC_ERR_LIMIT;
      }
    }
    else if (kind == GTEXT_MARKUP_KIND_ENTITY_REFERENCE) {
      GTEXT_Markup_Location at;
      memset(&at, 0, sizeof(at));
      gtext_markup_node_location(node, &at);
      if (gvec_diag_add(diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_ENTITY_SKIPPED, 0, at.line, at.column, at.offset,
              gvec_svg_parent_element(node), "an external entity was kept unresolved; nothing was fetched") ==
          GVEC_ERR_OOM) {
        gtext_markup_document_free(tree);
        return GVEC_ERR_OOM;
      }
    }
  }
  /* An unread external subset or parameter entity is DTD knowledge not had; a
   * skipped general entity is reported above, where it is referenced. */
  bool unread_dtd = false;
  for (size_t i = 0; i < gtext_markup_document_dtd_unread_resource_count(tree); i++) {
    GTEXT_Markup_DTD_Unread_Resource resource;
    if (gtext_markup_document_dtd_unread_resource(tree, i, &resource) &&
        resource.kind != GTEXT_MARKUP_DTD_RESOURCE_GENERAL_ENTITY) {
      unread_dtd = true;
    }
  }
  if (unread_dtd) {
    if (gvec_diag_add(diagnostics, GVEC_DIAG_NOTICE, GVEC_DIAG_DTD_INCOMPLETE, 0, 0, 0, 0, NULL,
            "an external DTD subset or parameter entity was not read; nothing was fetched") == GVEC_ERR_OOM) {
      gtext_markup_document_free(tree);
      return GVEC_ERR_OOM;
    }
  }

  svg = gvec_alloc(doc->allocator, sizeof(*svg));
  if (!svg) {
    gtext_markup_document_free(tree);
    return GVEC_ERR_OOM;
  }
  memset(svg, 0, sizeof(*svg));
  svg->root_line = location.line;
  svg->root_column = location.column;
  svg->root_offset = location.offset;
  /* D-B: the typed document is built from the tree here, and the tree is freed. */
  gvec_svg_build_options build;
  build.limits = limits;
  build.languages = load_options ? load_options->languages : NULL;
  build.language_count = load_options ? load_options->language_count : 0;
  if (!build.language_count) {
    build.languages = NULL;
  }
  GVEC_Result built = gvec_svg_build(root, elements, &build, doc->allocator, diagnostics, &svg->document);
  gtext_markup_document_free(tree);
  if (built == GVEC_OK) {
    built = gvec_svg_resolve(svg->document, limits, diagnostics);
  }
  if (built == GVEC_OK) {
    built = gvec_svg_conditional(svg->document, build.languages, build.language_count);
  }
  if (built != GVEC_OK) {
    gvec_svg_document_free(svg->document);
    gvec_free(doc->allocator, svg);
    return built;
  }
  doc->priv = svg;
  return GVEC_OK;
}

void gvec_svg_free(GVEC_Doc * doc) {
  gvec_svg_doc * svg = doc->priv;
  if (svg) {
    gvec_svg_document_free(svg->document);
  }
  gvec_free(doc->allocator, svg);
  doc->priv = NULL;
}
