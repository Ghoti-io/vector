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
 * @file svg_doc.h
 *
 * The typed document's layout (svg.h is its public face).
 */

#ifndef GHOTI_IO_GVEC_SVG_DOC_H
#define GHOTI_IO_GVEC_SVG_DOC_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/svg.h>

#include "svg_arena.h"
#include "svg_grammar.h"
#include "svg_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The SVG namespace and the two namespaces the attribute names borrow. */
#define GVEC_SVG_NS "http://www.w3.org/2000/svg"
#define GVEC_XLINK_NS "http://www.w3.org/1999/xlink"
#define GVEC_XML_NS "http://www.w3.org/XML/1998/namespace"
#define GVEC_XMLNS_NS "http://www.w3.org/2000/xmlns/"

/** An attribute's typed value; which member is read is the attribute's value kind. */
typedef union gvec_svg_value {
  double number;
  long integer;
  GVEC_Svg_Length length;
  gvec_svg_numbers numbers;
  gvec_svg_lengths lengths;
  gvec_svg_path path;
  GVEC_Svg_View_Box view_box;
  GVEC_Svg_Preserve_Aspect_Ratio par;
  GVEC_Svg_Orient orient;
  GVEC_Svg_Reference reference;
  struct {
    size_t index;
    const char * word;
  } keyword;
  const char * string;
  struct {
    const char ** tags;
    size_t count;
  } languages;
  size_t count;
} gvec_svg_value;

struct GVEC_Svg_Attribute {
  const char * ns;     ///< The namespace URI of a foreign attribute; NULL otherwise.
  const char * name;
  const char * raw;
  size_t raw_length;
  GVEC_Svg_Attribute_Class cls;
  uint8_t grammar;     ///< A GVEC_Svg_Grammar.
  uint8_t presentation;
  uint8_t partial;
  uint8_t value_kind;  ///< A GVEC_Svg_Value_Kind.
  uint32_t line, column;
  size_t offset;
  size_t error_offset;
  const char * error_reason;
  gvec_svg_value value;
};

/** A child of an element. An element's node is its first member; a text node is a bare node. */
struct GVEC_Svg_Node {
  uint8_t is_element;
  struct GVEC_Svg_Element * element; ///< The element this node is, or NULL for character data.
  struct GVEC_Svg_Node * next;
  struct GVEC_Svg_Element * parent;
  uint32_t line, column;
  size_t offset;
  const char * text;     ///< Text nodes: NUL-terminated.
  size_t text_length;
};

#define GVEC_SVG_ELEMENT_COND_FALSE 1u
#define GVEC_SVG_ELEMENT_ON_CYCLE 2u
#define GVEC_SVG_ELEMENT_EXPANSION_LIMITED 4u
#define GVEC_SVG_ELEMENT_KEY_COLLISION 8u

/**
 * What the style layer's element interface is handed for an element: the element, and nothing the instance adds. A selector is matched
 * against the document's tree (a clone made by `use` matches as the element it clones, as CSS says of the referenced tree), so the
 * interface needs a handle for every element of the document, whichever instances of it are drawn. An instance carries one of its own.
 */
typedef struct gvec_svg_handle {
  const GVEC_Svg_Element * element;
} gvec_svg_handle;

struct GVEC_Svg_Element {
  GVEC_Svg_Node node;
  gvec_svg_handle handle;         ///< The element's own handle for selector matching (its `element` is this element).
  GVEC_Svg_Kind kind;
  const char * ns;
  const char * name;
  uint64_t key;
  uint32_t flags;
  uint32_t order;                 ///< Position in document order, from 0.
  uint32_t subtree_size;          ///< Elements in this element's subtree, itself included.
  GVEC_Svg_Node * first_node;
  GVEC_Svg_Node * last_node;
  GVEC_Svg_Attribute * attributes;
  size_t attribute_count;
  const char * id;
  const GVEC_Svg_Attribute * href;     ///< The `href` (or `xlink:href`) attribute when TYPED, else NULL.
  const GVEC_Svg_Element * href_target;
  const GVEC_Svg_Element * cut_target; ///< The target a reference cycle cut: kept for a paint server's chain, which follows it until it repeats.
  const GVEC_Svg_Element * switch_selected;
  uint64_t expansion;
};

/** One entry of the id index. */
typedef struct gvec_svg_id_entry {
  const char * id;
  GVEC_Svg_Element * element;
} gvec_svg_id_entry;

struct GVEC_Svg_Document {
  gvec_arena arena;
  const GVEC_Allocator * allocator;
  GVEC_Svg_Element * root;
  GVEC_Svg_Element ** elements;   ///< Document order.
  size_t element_count;
  struct gvec_svg_id_entry * ids; ///< Sorted by id; the first element in document order wins.
  size_t id_count;
  bool has_animation;
  bool has_script;
  size_t text_bytes;
  double viewport_width;          ///< The load options' viewport, defaults applied (the style layer's initial containing block).
  double viewport_height;
  GVEC_Color_Scheme color_scheme;
  GVEC_Limits limits;             ///< The limits the document was loaded under, defaults applied (the style layer's bounds are among them).
  /** The XML parser's tree, kept (and owned) when the document has animation: an animated document is built again from it (anim_doc.c). */
  void * markup;
  size_t markup_elements;         ///< The elements of the tree, as the build was told.
  const char * const * languages; ///< The load's languages (copied into the arena), for a build again; NULL for {"en"}.
  size_t language_count;
  /** An animated document: the time it is at and what the style layer and the lowering apply (anim_doc.c); NULL for a document as written. */
  struct gvec_anim_state * anim;
  const struct GVEC_Svg_Document * base; ///< An animated document's document as written.
};

/** An attribute an animated document has in place of the markup's, or in addition to it. */
typedef struct gvec_svg_override {
  uint32_t element;   ///< The element's order in the document as written.
  const char * name;  ///< The attribute's name as the document has it (`xlink:href`, `xml:lang`).
  const char * value;
  size_t length;
} gvec_svg_override;

/** What a build again from the markup changes (an animated document). */
typedef struct gvec_svg_rebuild {
  const gvec_svg_override * overrides; ///< Sorted by element.
  size_t override_count;
  const uint8_t * discarded;           ///< Per element of the document as written: 1 when it and its subtree are not built (`discard`); NULL for none.
  const uint32_t * subtree;            ///< Per element of the document as written: its subtree size.
  GVEC_Svg_Element ** from_base;       ///< Out, per element of the document as written: the element built for it, NULL when discarded.
} gvec_svg_rebuild;

/** What the build needs besides the markup tree. */
typedef struct gvec_svg_build_options {
  const GVEC_Limits * limits;
  const char * const * languages; ///< NULL for {"en"}.
  size_t language_count;
  double viewport_width;          ///< 0 for the default.
  double viewport_height;
  GVEC_Color_Scheme color_scheme;
  const gvec_svg_rebuild * rebuild; ///< NULL for a build of the markup as written.
} gvec_svg_build_options;

/** The document of a GVEC_Doc whose codec is SVG, or NULL. */
GVEC_INTERNAL_API const GVEC_Svg_Document * gvec_svg_document_of(const GVEC_Doc * doc);

/**
 * Build the typed document from the XML parser's tree (the tree is not freed here). Diagnostics are
 * appended for every attribute that failed, every unknown attribute name (once), script, animation,
 * duplicate ids, missing references, cycles and expansion past the limit.
 *
 * @return GVEC_OK, GVEC_ERR_OOM, GVEC_ERR_LIMIT (character data past max_text_bytes).
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_build(const void * markup_root, size_t element_count,
    const gvec_svg_build_options * options, const GVEC_Allocator * allocator, GVEC_Diagnostics * diagnostics,
    GVEC_Svg_Document ** out);

/**
 * Type one value of an attribute of an element of `kind` as the build types it (svg_build.c): grammar, presentation, class and the typed value.
 * A presentation attribute (but `d`), `style` and an attribute with no grammar are RAW: not typed here.
 */
GVEC_INTERNAL_API gvec_svg_parse_status gvec_svg_type_value(gvec_svg_parse_context * parse, GVEC_Svg_Kind kind, const char * name, const char * raw,
    size_t length, GVEC_Svg_Attribute * out, gvec_svg_parse_error * err);

/** Keep the markup tree (owned from now on) and a copy of the languages in `document`, for a build again. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_keep_markup(GVEC_Svg_Document * document, void * tree, size_t elements, const char * const * languages,
    size_t language_count);

/** Free what an animated document's animation holds (anim_doc.c); NULL is ignored. */
GVEC_INTERNAL_API void gvec_anim_state_free(struct gvec_anim_state * state, const GVEC_Allocator * allocator);

/** Free a document built by gvec_svg_build(). */
GVEC_INTERNAL_API void gvec_svg_document_free(GVEC_Svg_Document * document);

/** References, cycles and expansion (svg_graph.c); run after the elements exist. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_resolve(GVEC_Svg_Document * document, const GVEC_Limits * limits,
    GVEC_Diagnostics * diagnostics);

/** Conditional processing and `switch` (svg_conditional.c). */
GVEC_INTERNAL_API GVEC_Result gvec_svg_conditional(GVEC_Svg_Document * document, const char * const * languages,
    size_t language_count);

/** The dump lines of the document (svg_dump.c). */
GVEC_INTERNAL_API GVEC_Result gvec_svg_dump_document(const GVEC_Svg_Document * document, GVEC_Stream * out);

/** The key of an element (svg_key.c), from its parent's key, its kind or name and its place among its siblings. */
GVEC_INTERNAL_API uint64_t gvec_svg_key_for(uint64_t parent_key, const char * id, GVEC_Svg_Kind kind, const char * ns,
    const char * name, uint32_t ordinal);

/** Fold a counter into a key that collided with an earlier element's. */
GVEC_INTERNAL_API uint64_t gvec_svg_key_rehash(uint64_t key, uint32_t attempt);

/** 64-bit hash of bytes (Murmur3 x64-128, low half, seed 0, little-endian bytes). */
GVEC_INTERNAL_API uint64_t gvec_svg_hash64(const void * data, size_t length);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_DOC_H
