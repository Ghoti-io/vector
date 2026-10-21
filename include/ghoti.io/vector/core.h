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
 * @file core.h
 *
 * Results, limits and diagnostics: what every entry point of this library
 * returns, is bounded by, and reports into.
 *
 * **Results.** Every fallible function returns a GVEC_Result. Output
 * parameters come last, are named `out_*`, and are written only on success;
 * a failure leaves nothing allocated for the caller to free.
 *
 * **Limits.** Every quantity a hostile file controls has a cap in
 * GVEC_Limits. Each field has a finite default, and zero in a field selects
 * that default: no field means "unlimited". Each field also has a ceiling,
 * the limit of the XML parser it feeds, so no cap here is larger than the one
 * it is enforced by. A value above its ceiling is GVEC_ERR_INVALID.
 *
 * **Diagnostics.** A load or a lowering appends what it found to a
 * caller-owned GVEC_Diagnostics, on success and on failure, each entry with a
 * stable code and the source location it concerns. Past
 * GVEC_Limits.max_diagnostics entries the list keeps the first ones and counts
 * the rest in `dropped`, so nothing is lost silently.
 */

#ifndef GHOTI_IO_GVEC_CORE_H
#define GHOTI_IO_GVEC_CORE_H

#include <ghoti.io/vector/macros.h>

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// Results
//-----------------------------------------------------------------------------

/** The outcome of a fallible call. The numbers are stable. */
typedef enum GVEC_Result {
  GVEC_OK = 0,          ///< Success.
  GVEC_ERR_IO,          ///< A stream read or write failed, or a file could not be opened.
  GVEC_ERR_FORMAT,      ///< The input is malformed for its format (for example, not well-formed XML).
  GVEC_ERR_UNSUPPORTED, ///< The input is no format this library reads, or uses a feature it does not.
  GVEC_ERR_LIMIT,       ///< A GVEC_Limits cap, or a fixed cap of the XML parser, was reached.
  GVEC_ERR_CORRUPT,     ///< A compressed transport is damaged (bad checksum, truncated member).
  GVEC_ERR_OOM,         ///< An allocation failed.
  GVEC_ERR_INVALID,     ///< An argument was unusable: NULL where a value is required, or a limit above its ceiling.
  GVEC_ERR_INTERNAL,    ///< A defect in this library.
  GVEC_RESULT_COUNT     ///< The number of results; not a result.
} GVEC_Result;

/**
 * A short, static description of a result.
 *
 * @param result Any value.
 * @return The result's name, or "unknown" for a value at or past
 *   GVEC_RESULT_COUNT. Never NULL.
 */
GVEC_API const char * gvec_result_string(GVEC_Result result);

//-----------------------------------------------------------------------------
// Limits
//-----------------------------------------------------------------------------

/**
 * Caps on what one load may consume. Zero in a field selects its default.
 *
 * | Field | Default and ceiling |
 * | --- | --- |
 * | max_input_bytes | 64 MiB |
 * | max_inflated_bytes | 64 MiB |
 * | max_elements | 10,000,000 |
 * | max_depth | 256 |
 * | max_attributes | 1024 per element, namespace declarations included |
 * | max_entity_expanded_bytes | 64 MiB, the cumulative replacement text of every entity reference |
 * | max_entity_depth | 32 |
 * | max_entity_references | 1,000,000 |
 * | max_diagnostics | default 4096, ceiling 2^20 |
 *
 * The default of every field but max_diagnostics is also its ceiling: it is
 * the XML parser's own limit for the same quantity.
 */
typedef struct GVEC_Limits {
  size_t max_input_bytes;           ///< Bytes read from the input stream.
  size_t max_inflated_bytes;        ///< Bytes a gzip transport (`.svgz`) may inflate to.
  size_t max_elements;              ///< Elements in the document.
  size_t max_depth;                 ///< Element nesting depth.
  size_t max_attributes;            ///< Attributes on one element.
  size_t max_entity_expanded_bytes; ///< Total replacement text of entity references.
  size_t max_entity_depth;          ///< Entity references nested inside entity replacement text.
  size_t max_entity_references;     ///< Entity and character references in the document.
  size_t max_diagnostics;           ///< Diagnostics kept; the rest are counted.
  uint8_t _reserved[8];             ///< Zero.
} GVEC_Limits;

/** Which field of GVEC_Limits a GVEC_DIAG_LIMIT entry names (its `detail`). The numbers are stable. */
typedef enum GVEC_Limit_Field {
  GVEC_LIMIT_INPUT_BYTES = 0,
  GVEC_LIMIT_INFLATED_BYTES,
  GVEC_LIMIT_ELEMENTS,
  GVEC_LIMIT_DEPTH,
  GVEC_LIMIT_ATTRIBUTES,
  GVEC_LIMIT_ENTITY_EXPANDED_BYTES,
  GVEC_LIMIT_ENTITY_DEPTH,
  GVEC_LIMIT_ENTITY_REFERENCES,
  GVEC_LIMIT_DIAGNOSTICS,
  GVEC_LIMIT_FIELD_COUNT ///< The number of fields; not a field.
} GVEC_Limit_Field;

/**
 * Write the default of every field.
 *
 * @param limits The struct to fill; ignored when NULL.
 */
GVEC_API void gvec_limits_default(GVEC_Limits * limits);

//-----------------------------------------------------------------------------
// Diagnostics
//-----------------------------------------------------------------------------

/** How much a diagnostic matters. The numbers are stable. */
typedef enum GVEC_Diag_Severity {
  GVEC_DIAG_NOTICE = 0,  ///< Information: the document loads and draws as written.
  GVEC_DIAG_WARNING,     ///< Something was kept unresolved or not drawn; the rest is unaffected.
  GVEC_DIAG_ERROR,       ///< The call failed, or an element was refused.
  GVEC_DIAG_SEVERITY_COUNT
} GVEC_Diag_Severity;

/**
 * What a diagnostic reports. The numbers are stable; zero is never produced,
 * so a zeroed entry is recognisable.
 */
typedef enum GVEC_Diag_Code {
  GVEC_DIAG_XML = 1,        ///< The XML parser refused the document; `detail` is its status (GTEXT_XML_Status).
  GVEC_DIAG_NOT_SVG,        ///< The root element is not `svg` in the SVG namespace.
  GVEC_DIAG_ENTITY_SKIPPED, ///< A reference to an external entity was kept unresolved; nothing was fetched.
  GVEC_DIAG_DTD_INCOMPLETE, ///< An external DTD subset or parameter entity was not read; nothing was fetched.
  GVEC_DIAG_GZIP,           ///< The gzip transport could not be inflated; `detail` is the compress status.
  GVEC_DIAG_LIMIT,          ///< A cap was reached; `detail` is the GVEC_Limit_Field.
  GVEC_DIAG_LOWER_REFUSED,  ///< An element was not lowered; `detail` is a GVEC_Lower_Reason (lower.h).
  GVEC_DIAG_CODE_COUNT      ///< One past the last code; not a code.
} GVEC_Diag_Code;

/** One diagnostic. */
typedef struct GVEC_Diagnostic {
  GVEC_Diag_Severity severity;
  GVEC_Diag_Code code;
  uint32_t detail;       ///< Code-specific, as GVEC_Diag_Code says; 0 when none.
  size_t line;           ///< 1-based line, in characters of the original encoding; 0 when none.
  size_t column;         ///< 1-based column, in characters; 0 when none.
  size_t offset;         ///< Byte offset in the parsed UTF-8 form (of the inflated bytes for a gzip transport).
  const char * element;  ///< The element's local name, owned by the list; NULL when none.
  const char * message;  ///< A static description; never NULL.
} GVEC_Diagnostic;

/** A caller-owned list of diagnostics. Read `items[0 .. count)`; change it only through the functions below. */
typedef struct GVEC_Diagnostics {
  GVEC_Diagnostic * items;
  size_t count;
  size_t capacity;
  size_t dropped;        ///< Diagnostics past the cap, counted and not kept.
  size_t cap;            ///< The cap in effect, from the last load's limits; 0 until one sets it (then 4096).
  const GVEC_Allocator * allocator;
} GVEC_Diagnostics;

/**
 * Prepare an empty list.
 *
 * @param diagnostics The list; ignored when NULL.
 * @param allocator The allocator for its entries; NULL for the default.
 */
GVEC_API void gvec_diagnostics_init(GVEC_Diagnostics * diagnostics, const GVEC_Allocator * allocator);

/** Remove every entry and reset `dropped`, keeping the storage. Ignored when NULL. */
GVEC_API void gvec_diagnostics_clear(GVEC_Diagnostics * diagnostics);

/** Free the entries; the list is empty and may be initialised again. Ignored when NULL. */
GVEC_API void gvec_diagnostics_destroy(GVEC_Diagnostics * diagnostics);

/**
 * The number of entries of one code, for a caller that tests for one.
 *
 * @return 0 when `diagnostics` is NULL.
 */
GVEC_API size_t gvec_diagnostics_count_code(const GVEC_Diagnostics * diagnostics, GVEC_Diag_Code code);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_CORE_H
