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
 * @file svg.h
 *
 * The typed SVG document: a tree of elements whose attribute values have been
 * parsed once, with their errors located, their references resolved and their
 * conditional processing evaluated. It is built from the markup tree, which is
 * then freed (D-B), and it is read-only afterwards.
 *
 * ### Ownership
 *
 * Everything returned here belongs to the GVEC_Doc it was read from and is valid
 * until gvec_doc_free(). Strings are NUL-terminated and, unless said, kept
 * exactly as written.
 *
 * ### Elements and attributes
 *
 * An element is one of the SVG 2 element kinds (svg_kind.h), or GVEC_SVG_KIND_UNKNOWN
 * (in the SVG namespace but not an element of SVG 2) or GVEC_SVG_KIND_FOREIGN (in
 * another namespace). The last two are kept in place, with their children and
 * attributes, and are never rendered.
 *
 * Every attribute of an element is kept, in source order, with its raw text and
 * where it came from, in one of these classes:
 *
 *  - TYPED: parsed by its grammar; read the value through the typed getter named by
 *    gvec_svg_attribute_value_kind().
 *  - INVALID: its grammar failed. The raw text and the located diagnostic are kept and
 *    there is no value (SVG 2's error rule). Path data and point lists keep the valid
 *    prefix and say so (gvec_svg_attribute_is_partial()).
 *  - PROPERTY: a CSS property or a presentation attribute. Held as written; the cascade
 *    types it.
 *  - RAW: defined by SVG 2 but not read in this version (animation, filters, ARIA, legacy
 *    attributes). Held as written.
 *  - UNKNOWN: not an attribute SVG 2 defines for that element, or in a foreign namespace.
 *  - SCRIPT: an event handler. Kept, never evaluated.
 */

#ifndef GHOTI_IO_GVEC_SVG_H
#define GHOTI_IO_GVEC_SVG_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/svg_kind.h>
#include <ghoti.io/vector/svg_value.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The typed document. */
typedef struct GVEC_Svg_Document GVEC_Svg_Document;
/** An element of the document. */
typedef struct GVEC_Svg_Element GVEC_Svg_Element;
/** An attribute of an element. */
typedef struct GVEC_Svg_Attribute GVEC_Svg_Attribute;
/** A child of an element: an element or character data. */
typedef struct GVEC_Svg_Node GVEC_Svg_Node;

/** How an attribute was read. The numbers are stable. */
typedef enum GVEC_Svg_Attribute_Class {
  GVEC_SVG_ATTR_TYPED = 0,
  GVEC_SVG_ATTR_INVALID,
  GVEC_SVG_ATTR_PROPERTY,
  GVEC_SVG_ATTR_RAW,
  GVEC_SVG_ATTR_UNKNOWN,
  GVEC_SVG_ATTR_SCRIPT,
  GVEC_SVG_ATTR_CLASS_COUNT
} GVEC_Svg_Attribute_Class;

/** Which typed getter reads a TYPED attribute. The numbers are stable. */
typedef enum GVEC_Svg_Value_Kind {
  GVEC_SVG_VALUE_NONE = 0,
  GVEC_SVG_VALUE_NUMBER,
  GVEC_SVG_VALUE_INTEGER,
  GVEC_SVG_VALUE_LENGTH,
  GVEC_SVG_VALUE_NUMBER_LIST,
  GVEC_SVG_VALUE_LENGTH_LIST,
  GVEC_SVG_VALUE_POINTS,
  GVEC_SVG_VALUE_PATH,
  GVEC_SVG_VALUE_VIEW_BOX,
  GVEC_SVG_VALUE_PRESERVE_ASPECT_RATIO,
  GVEC_SVG_VALUE_ORIENT,
  GVEC_SVG_VALUE_REFERENCE,
  GVEC_SVG_VALUE_KEYWORD,
  GVEC_SVG_VALUE_STRING,
  GVEC_SVG_VALUE_LANGUAGES,
  GVEC_SVG_VALUE_EXTENSIONS,
  GVEC_SVG_VALUE_OFFSET,
  GVEC_SVG_VALUE_COUNT
} GVEC_Svg_Value_Kind;

/** The typed document of an SVG GVEC_Doc, or NULL for any other document. */
GVEC_API const GVEC_Svg_Document * gvec_svg_document(const GVEC_Doc * doc);

/** The root element (always `svg`). */
GVEC_API const GVEC_Svg_Element * gvec_svg_root(const GVEC_Svg_Document * document);

/** How many elements the document has. */
GVEC_API size_t gvec_svg_element_count(const GVEC_Svg_Document * document);

/** The first element with this `id` in document order, or NULL. */
GVEC_API const GVEC_Svg_Element * gvec_svg_find_by_id(const GVEC_Svg_Document * document, const char * id);

//-----------------------------------------------------------------------------
// Elements
//-----------------------------------------------------------------------------

GVEC_API GVEC_Svg_Kind gvec_svg_element_kind(const GVEC_Svg_Element * element);
/** The local name as written (for every kind). */
GVEC_API const char * gvec_svg_element_name(const GVEC_Svg_Element * element);
/** The namespace URI, or NULL for none. */
GVEC_API const char * gvec_svg_element_namespace(const GVEC_Svg_Element * element);
/** The element's key (AD-9): stable across loads of the same bytes, and across edits for elements with an `id`. */
GVEC_API uint64_t gvec_svg_element_key(const GVEC_Svg_Element * element);
GVEC_API size_t gvec_svg_element_line(const GVEC_Svg_Element * element);
GVEC_API size_t gvec_svg_element_column(const GVEC_Svg_Element * element);
GVEC_API size_t gvec_svg_element_offset(const GVEC_Svg_Element * element);
/** The parent element, or NULL for the root. */
GVEC_API const GVEC_Svg_Element * gvec_svg_element_parent(const GVEC_Svg_Element * element);
/** The element's `id` attribute value, or NULL. */
GVEC_API const char * gvec_svg_element_id(const GVEC_Svg_Element * element);

/** The first child (an element or text), or NULL. */
GVEC_API const GVEC_Svg_Node * gvec_svg_element_first_node(const GVEC_Svg_Element * element);
GVEC_API const GVEC_Svg_Node * gvec_svg_node_next(const GVEC_Svg_Node * node);
/** The node as an element, or NULL if it is character data. */
GVEC_API const GVEC_Svg_Element * gvec_svg_node_element(const GVEC_Svg_Node * node);
/** The character data of a text node (NUL-terminated), or NULL for an element. */
GVEC_API const char * gvec_svg_node_text(const GVEC_Svg_Node * node, size_t * length);
/** The first child that is an element, or NULL. */
GVEC_API const GVEC_Svg_Element * gvec_svg_element_first_child(const GVEC_Svg_Element * element);
/** The next sibling that is an element, or NULL. */
GVEC_API const GVEC_Svg_Element * gvec_svg_element_next_sibling(const GVEC_Svg_Element * element);

/** The conditional processing attributes evaluate to false (the element is treated as `display: none`). */
GVEC_API bool gvec_svg_element_conditional_false(const GVEC_Svg_Element * element);
/** For a `switch`: the child it selects, or NULL if none is true. */
GVEC_API const GVEC_Svg_Element * gvec_svg_element_switch_selected(const GVEC_Svg_Element * element);
/** The element is on a reference cycle (diagnosed); its referring edge was cut. */
GVEC_API bool gvec_svg_element_on_cycle(const GVEC_Svg_Element * element);
/** A `use` whose expansion exceeds the limit (diagnosed); it is not instantiated. */
GVEC_API bool gvec_svg_element_expansion_limited(const GVEC_Svg_Element * element);
/** The elements a `use` instantiates (its referent's subtree, counting nested `use`), saturated. */
GVEC_API uint64_t gvec_svg_element_expansion(const GVEC_Svg_Element * element);
/**
 * The element an `href` resolves to (an internal reference to an element of this document), or NULL when there is
 * none, it is missing, or the edge was cut by a cycle or the expansion limit.
 */
GVEC_API const GVEC_Svg_Element * gvec_svg_element_href_target(const GVEC_Svg_Element * element);

//-----------------------------------------------------------------------------
// Attributes
//-----------------------------------------------------------------------------

GVEC_API size_t gvec_svg_element_attribute_count(const GVEC_Svg_Element * element);
GVEC_API const GVEC_Svg_Attribute * gvec_svg_element_attribute_at(const GVEC_Svg_Element * element, size_t index);
/** The attribute with this name (`xlink:href` and `xml:space` spelled with their prefix), or NULL. */
GVEC_API const GVEC_Svg_Attribute * gvec_svg_element_find_attribute(const GVEC_Svg_Element * element, const char * name);

/** The name as the tables know it: local, or `xlink:...` / `xml:...`; foreign attributes keep their local name. */
GVEC_API const char * gvec_svg_attribute_name(const GVEC_Svg_Attribute * attribute);
/** The namespace URI of a foreign attribute, or NULL. */
GVEC_API const char * gvec_svg_attribute_namespace(const GVEC_Svg_Attribute * attribute);
/** The value as written. */
GVEC_API const char * gvec_svg_attribute_raw(const GVEC_Svg_Attribute * attribute, size_t * length);
GVEC_API GVEC_Svg_Attribute_Class gvec_svg_attribute_class(const GVEC_Svg_Attribute * attribute);
/** The grammar's name, for example "length"; "none" when it has none. */
GVEC_API const char * gvec_svg_attribute_grammar(const GVEC_Svg_Attribute * attribute);
/** Whether the attribute is a presentation attribute of its element (SVG 2, 6.6). */
GVEC_API bool gvec_svg_attribute_is_presentation(const GVEC_Svg_Attribute * attribute);
/** The part of a value that was kept when its grammar failed part way (path data, points). */
GVEC_API bool gvec_svg_attribute_is_partial(const GVEC_Svg_Attribute * attribute);
/** For INVALID and partial attributes: the offset in the value of the first bad byte, and why. */
GVEC_API size_t gvec_svg_attribute_error_offset(const GVEC_Svg_Attribute * attribute);
GVEC_API const char * gvec_svg_attribute_error_reason(const GVEC_Svg_Attribute * attribute);
GVEC_API size_t gvec_svg_attribute_line(const GVEC_Svg_Attribute * attribute);
GVEC_API size_t gvec_svg_attribute_column(const GVEC_Svg_Attribute * attribute);
GVEC_API size_t gvec_svg_attribute_offset(const GVEC_Svg_Attribute * attribute);

/** Which getter reads this attribute's value; GVEC_SVG_VALUE_NONE unless it is TYPED (or partial). */
GVEC_API GVEC_Svg_Value_Kind gvec_svg_attribute_value_kind(const GVEC_Svg_Attribute * attribute);
GVEC_API bool gvec_svg_attribute_number(const GVEC_Svg_Attribute * attribute, double * out);
GVEC_API bool gvec_svg_attribute_integer(const GVEC_Svg_Attribute * attribute, long * out);
GVEC_API bool gvec_svg_attribute_length(const GVEC_Svg_Attribute * attribute, GVEC_Svg_Length * out);
/** A number list or `points` (flat x, y pairs). */
GVEC_API bool gvec_svg_attribute_numbers(const GVEC_Svg_Attribute * attribute, const double ** items, size_t * count);
GVEC_API bool gvec_svg_attribute_lengths(const GVEC_Svg_Attribute * attribute, const GVEC_Svg_Length ** items, size_t * count);
GVEC_API bool gvec_svg_attribute_path(const GVEC_Svg_Attribute * attribute, const GVEC_Svg_Path_Command ** commands,
    size_t * command_count, const GVEC_Svg_Path_Segment ** segments, size_t * segment_count);
GVEC_API bool gvec_svg_attribute_view_box(const GVEC_Svg_Attribute * attribute, GVEC_Svg_View_Box * out);
GVEC_API bool gvec_svg_attribute_preserve_aspect_ratio(const GVEC_Svg_Attribute * attribute, GVEC_Svg_Preserve_Aspect_Ratio * out);
GVEC_API bool gvec_svg_attribute_orient(const GVEC_Svg_Attribute * attribute, GVEC_Svg_Orient * out);
GVEC_API bool gvec_svg_attribute_reference(const GVEC_Svg_Attribute * attribute, GVEC_Svg_Reference * out);
/** An enumeration: the index of the keyword in the attribute's list, and the keyword. */
GVEC_API bool gvec_svg_attribute_keyword(const GVEC_Svg_Attribute * attribute, size_t * index, const char ** keyword);
GVEC_API bool gvec_svg_attribute_string(const GVEC_Svg_Attribute * attribute, const char ** out);

/** The text of `data:` URI payload decoded to bytes (percent-decoding, or base64 through cutil). */
GVEC_API GVEC_Result gvec_svg_data_uri_decode(const GVEC_Svg_Data_Uri * uri, void * out, size_t capacity, size_t * written);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_H
