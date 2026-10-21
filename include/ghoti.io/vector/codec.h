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
 * @file codec.h
 *
 * Formats, how one is recognised, and loading a document.
 *
 * **The registry.** Each format this library reads is a codec, registered
 * before `main` runs by a constructor in the library, in a fixed order. The
 * table is never written after that, so reading it is safe from any thread.
 * Today there is one codec, `svg`.
 *
 * **Transports.** A gzip member (`.svgz`) is a transport, not a format: a load
 * that finds one inflates it, one level only, and recognises what is inside.
 *
 * **Loading.** gvec_doc_load() reads the whole input, within
 * GVEC_Limits.max_input_bytes, inflates a transport, asks each codec in turn
 * whether the bytes are its format, and hands them to the first that says
 * yes. Nothing is read from the environment, the locale or the clock, and
 * nothing is fetched: an external entity or DTD named by the file is kept as
 * a reference and reported.
 *
 * A document is one thread at a time.
 */

#ifndef GHOTI_IO_GVEC_CODEC_H
#define GHOTI_IO_GVEC_CODEC_H

#include <ghoti.io/vector/macros.h>

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/stream.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A registered format. */
typedef struct GVEC_Codec GVEC_Codec;

/** A loaded document. */
typedef struct GVEC_Doc GVEC_Doc;

/** The codec can load documents. */
#define GVEC_CAP_LOAD (1u << 0)
/** The codec can lower a document into a scene (lower.h). */
#define GVEC_CAP_LOWER (1u << 1)
/** The codec can save a document. No codec can yet. */
#define GVEC_CAP_SAVE (1u << 2)

/** The number of registered codecs. */
GVEC_API size_t gvec_codec_count(void);

/** The codec at `index` in registration order, or NULL past the end. */
GVEC_API const GVEC_Codec * gvec_codec_by_index(size_t index);

/** The codec named `name`, or NULL. */
GVEC_API const GVEC_Codec * gvec_codec_by_name(const char * name);

/** The codec's name, for example "svg"; NULL for NULL. */
GVEC_API const char * gvec_codec_name(const GVEC_Codec * codec);

/** The codec's GVEC_CAP_* bits; 0 for NULL. */
GVEC_API unsigned gvec_codec_capabilities(const GVEC_Codec * codec);

/** How the bytes reached the format. The numbers are stable. */
typedef enum GVEC_Transport {
  GVEC_TRANSPORT_NONE = 0, ///< The format's bytes as they are.
  GVEC_TRANSPORT_GZIP,     ///< One gzip file (RFC 1952), of one or more members.
  GVEC_TRANSPORT_COUNT
} GVEC_Transport;

/** What gvec_probe() found. */
typedef struct GVEC_Probe_Result {
  const char * format_name; ///< The matching codec's name, or NULL when none matched.
  unsigned confidence;      ///< 100 for a match, 0 for none.
  GVEC_Transport transport; ///< The transport the format arrived in.
  uint8_t _reserved[4];
} GVEC_Probe_Result;

/**
 * Recognise the stream's format without consuming it: the next read returns
 * the same bytes.
 *
 * @param limits The caps on input and inflation; NULL for the defaults.
 * @return GVEC_OK whether or not a format matched (read
 *   `out_result->format_name`); GVEC_ERR_LIMIT, GVEC_ERR_CORRUPT,
 *   GVEC_ERR_IO, GVEC_ERR_OOM, GVEC_ERR_INVALID.
 */
GVEC_API GVEC_Result gvec_probe(GVEC_Stream * stream, const GVEC_Limits * limits, GVEC_Probe_Result * out_result);

/** What a load may use. Zero-fill it, or set only what you need. */
typedef struct GVEC_Load_Options {
  const GVEC_Limits * limits; ///< NULL for the defaults.
  const GVEC_Allocator * allocator; ///< For the document; NULL for the default.
  uint8_t _reserved[16];       ///< Zero.
} GVEC_Load_Options;

/**
 * Load a document.
 *
 * @param stream Read to its end (within the input limit).
 * @param options NULL for the defaults.
 * @param diagnostics Appended to on success and on failure; may be NULL.
 * @param out_doc Receives the document on success; free it with gvec_doc_free().
 * @return GVEC_ERR_UNSUPPORTED when no codec recognises the bytes;
 *   GVEC_ERR_FORMAT for a malformed document; GVEC_ERR_LIMIT, GVEC_ERR_CORRUPT,
 *   GVEC_ERR_IO, GVEC_ERR_OOM, GVEC_ERR_INVALID.
 */
GVEC_API GVEC_Result gvec_doc_load(GVEC_Stream * stream, const GVEC_Load_Options * options,
    GVEC_Diagnostics * diagnostics, GVEC_Doc ** out_doc);

/** Free a document. Ignored when NULL. */
GVEC_API void gvec_doc_free(GVEC_Doc * doc);

/** The codec that loaded the document; NULL for NULL. */
GVEC_API const GVEC_Codec * gvec_doc_codec(const GVEC_Doc * doc);

/** The name of the document's format, for example "svg"; NULL for NULL. */
GVEC_API const char * gvec_doc_format(const GVEC_Doc * doc);

/** The transport the document arrived in. */
GVEC_API GVEC_Transport gvec_doc_transport(const GVEC_Doc * doc);

/**
 * Write the document's dump: a line-oriented text, identical on every target,
 * that begins `vector-doc 1` (the dump format).
 *
 * @return GVEC_ERR_IO if a write fails; GVEC_ERR_INVALID for NULL.
 */
GVEC_API GVEC_Result gvec_doc_dump(const GVEC_Doc * doc, GVEC_Stream * out);

/**
 * The document's content hash: the low 64 bits of MurmurHash3 x64-128, seed
 * 0, over the dump's bytes, read little-endian. Identical on every target.
 */
GVEC_API GVEC_Result gvec_doc_hash(const GVEC_Doc * doc, uint64_t * out_hash);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_CODEC_H
