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
 * @file svg_internal.h
 *
 * The SVG codec's parts: its XML options (L9), probe (L7), load (L8) and
 * lowering (L11).
 */

#ifndef GHOTI_IO_GVEC_SVG_INTERNAL_H
#define GHOTI_IO_GVEC_SVG_INTERNAL_H

#include <ghoti.io/vector/macros.h>

#include <stddef.h>

#include <ghoti.io/text/xml.h>

#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>

#include "../codec_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The SVG codec. */
extern const GVEC_Codec gvec_svg_codec;

/** What story 1's SVG document keeps: where its root element is. */
struct GVEC_Svg_Document;

typedef struct gvec_svg_doc {
  size_t root_line;
  size_t root_column;
  size_t root_offset;
  struct GVEC_Svg_Document * document; ///< The typed document (svg_doc.h).
} gvec_svg_doc;

/**
 * The XML parser's options for an SVG load (L9): the DTD processed, nothing
 * external ever read, no resolver, and the caps of `limits`.
 *
 * @param input_bytes The cap on what the parser reads: max_input_bytes, or
 *   max_inflated_bytes for a gzip transport.
 */
GVEC_INTERNAL_API void gvec_svg_xml_options(const GVEC_Limits * limits, size_t input_bytes,
    const GVEC_Allocator * allocator, GTEXT_XML_Parse_Options * out);

/**
 * L7: 100 when the first element is `svg` in the SVG namespace, 0 when it is
 * not, and minus a GVEC_Result (GVEC_ERR_OOM, GVEC_ERR_LIMIT) when the probe
 * could not decide; the limit is reported in `why_not`. On 0,
 * `why_not` (may be NULL) receives the XML error that stopped the probe, or a
 * GVEC_DIAG_NOT_SVG entry naming the root element that was found.
 */
GVEC_INTERNAL_API int gvec_svg_probe(const unsigned char * data, size_t size, const GVEC_Limits * limits,
    const GVEC_Allocator * allocator, GVEC_Diagnostics * why_not);

/**
 * The result and the diagnostic for a failure of the XML parser: OOM, a limit
 * named by its GVEC_Limits field, a fixed limit of the parser, or a format
 * error, each located.
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_parse_failed(
    const GTEXT_XML_Error * error, GVEC_Transport transport, GVEC_Diagnostics * diagnostics);

/** L8. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_load(GVEC_Doc * doc, const unsigned char * data, size_t size,
    const GVEC_Limits * limits, const GVEC_Load_Options * options, GVEC_Diagnostics * diagnostics);

/** Free the SVG document. */
GVEC_INTERNAL_API void gvec_svg_free(GVEC_Doc * doc);

/** L11. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_lower_edit(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_Edit * edit, GVEC_Diagnostics * diagnostics);

/** L11. */
GVEC_INTERNAL_API GVEC_Result gvec_svg_lower_list(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_List * list, GVEC_Diagnostics * diagnostics);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_INTERNAL_H
