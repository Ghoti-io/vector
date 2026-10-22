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
 * @file codec_internal.h
 *
 * A codec's callbacks, the document's layout, and the transport step.
 */

#ifndef GHOTI_IO_GVEC_CODEC_INTERNAL_H
#define GHOTI_IO_GVEC_CODEC_INTERNAL_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/lower.h>
#include <ghoti.io/vector/stream.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether `data` is this codec's format: 100 for yes, 0 for no, or minus a
 * GVEC_Result when the probe could not decide (out of memory, or a limit
 * reached before the format showed itself). The bytes are
 * the whole input, after any transport. On no, `why_not` (which may be NULL)
 * receives the reason when the bytes come close: the XML error that stopped
 * the probe, or the root element that is not this format's. A load that no
 * codec claims reports those reasons.
 */
typedef int (*gvec_codec_probe_fn)(const unsigned char * data, size_t size, const GVEC_Limits * limits,
    const GVEC_Allocator * allocator, GVEC_Diagnostics * why_not);

/**
 * Load `data` into `doc`, whose codec, allocator and transport are already
 * set; on success `doc->priv` holds the codec's document. On failure nothing
 * is left in `doc->priv`.
 */
typedef GVEC_Result (*gvec_codec_load_fn)(GVEC_Doc * doc, const unsigned char * data, size_t size,
    const GVEC_Limits * limits, const GVEC_Load_Options * options, GVEC_Diagnostics * diagnostics);

/** Write the codec's lines of the dump, after the shared header. */
typedef GVEC_Result (*gvec_codec_dump_fn)(const GVEC_Doc * doc, GVEC_Stream * out);

/** Lower a document into an edit. */
typedef GVEC_Result (*gvec_codec_lower_edit_fn)(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_Edit * edit, GVEC_Diagnostics * diagnostics);

/** Lower a document into a list. */
typedef GVEC_Result (*gvec_codec_lower_list_fn)(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_List * list, GVEC_Diagnostics * diagnostics);

/** Free the codec's document. */
typedef void (*gvec_codec_free_fn)(GVEC_Doc * doc);

/** A format. Each is a `const` object in the library; the registry lists them. */
struct GVEC_Codec {
  const char * name;
  unsigned capabilities;
  gvec_codec_probe_fn probe;
  gvec_codec_load_fn load;
  gvec_codec_dump_fn dump;
  gvec_codec_lower_edit_fn lower_edit;
  gvec_codec_lower_list_fn lower_list;
  void (*save)(void); ///< Reserved for the writer; NULL in every codec yet.
  gvec_codec_free_fn free_private;
};

/** A loaded document. */
struct GVEC_Doc {
  const GVEC_Allocator * allocator;
  const GVEC_Codec * codec;
  GVEC_Transport transport;
  void * priv;
};

/**
 * Inflate a gzip transport, if `data` begins with one.
 *
 * @param out_data Receives the bytes the format sees: `data` itself when there
 *   is no transport, else an inflated buffer, allocated through `allocator`,
 *   to free with gvec_transport_free().
 * @param out_transport Receives the transport found.
 * @return GVEC_ERR_LIMIT past `limits->max_inflated_bytes`, GVEC_ERR_CORRUPT,
 *   GVEC_ERR_UNSUPPORTED for gzip inside gzip, GVEC_ERR_OOM; each with a
 *   diagnostic.
 */
GVEC_INTERNAL_API GVEC_Result gvec_transport_decode(const unsigned char * data, size_t size,
    const GVEC_Limits * limits, GVEC_Diagnostics * diagnostics, const GVEC_Allocator * allocator,
    const unsigned char ** out_data, size_t * out_size, GVEC_Transport * out_transport);

/** Free what gvec_transport_decode() allocated; nothing when the transport is none. */
GVEC_INTERNAL_API void gvec_transport_free(
    const unsigned char * data, GVEC_Transport transport, const GVEC_Allocator * allocator);

/** Whether `data` begins with a gzip member's magic and method (1f 8b 08). */
GVEC_INTERNAL_API bool gvec_transport_is_gzip(const unsigned char * data, size_t size);

/** Write `text` (a NUL-terminated string) to `out`. */
GVEC_INTERNAL_API GVEC_Result gvec_dump_text(GVEC_Stream * out, const char * text);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_CODEC_INTERNAL_H
