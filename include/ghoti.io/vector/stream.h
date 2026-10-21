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
 * @file stream.h
 *
 * The byte streams a load reads from and a dump writes to.
 *
 * A stream is opaque and owned by whoever created it. Reading sources are a
 * borrowed memory buffer or a file the caller names; the library opens no
 * other file. A writing stream collects bytes in memory.
 *
 * There is no `read_line`: nothing in this library reads by line, because the
 * XML parser takes the whole document at once.
 *
 * One thread at a time per stream.
 */

#ifndef GHOTI_IO_GVEC_STREAM_H
#define GHOTI_IO_GVEC_STREAM_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/vector/allocator.h>
#include <ghoti.io/vector/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A byte stream. */
typedef struct GVEC_Stream GVEC_Stream;

/** What gvec_stream_size() returns for a stream whose length is not known. */
#define GVEC_STREAM_SIZE_UNKNOWN ((size_t)-1)

/**
 * A reading stream over `size` bytes the caller keeps alive and unchanged
 * until the stream is destroyed.
 *
 * @return GVEC_ERR_INVALID if `out_stream` is NULL, or `data` is NULL with a
 *   nonzero size; GVEC_ERR_OOM.
 */
GVEC_API GVEC_Result gvec_stream_create_memory(const void * data, size_t size, GVEC_Stream ** out_stream);

/** As gvec_stream_create_memory(), allocating through `allocator` (NULL for the default). */
GVEC_API GVEC_Result gvec_stream_create_memory_with_allocator(
    const GVEC_Allocator * allocator, const void * data, size_t size, GVEC_Stream ** out_stream);

/**
 * A reading stream over the file at `path`, opened in binary mode. The file
 * is closed by gvec_stream_destroy().
 *
 * @return GVEC_ERR_IO if it cannot be opened; GVEC_ERR_INVALID for a NULL
 *   argument; GVEC_ERR_OOM.
 */
GVEC_API GVEC_Result gvec_stream_create_file(const char * path, GVEC_Stream ** out_stream);

/** As gvec_stream_create_file(), allocating through `allocator` (NULL for the default). */
GVEC_API GVEC_Result gvec_stream_create_file_with_allocator(
    const GVEC_Allocator * allocator, const char * path, GVEC_Stream ** out_stream);

/**
 * A writing stream that collects what is written in a buffer it owns,
 * readable with gvec_stream_output_buffer().
 */
GVEC_API GVEC_Result gvec_stream_create_memory_output(GVEC_Stream ** out_stream);

/** As gvec_stream_create_memory_output(), allocating through `allocator` (NULL for the default). */
GVEC_API GVEC_Result gvec_stream_create_memory_output_with_allocator(
    const GVEC_Allocator * allocator, GVEC_Stream ** out_stream);

/**
 * Read up to `size` bytes. Fewer than `size` is not an error; zero with
 * GVEC_OK means the end. A short read says nothing about the end: test
 * gvec_stream_eof().
 *
 * @param out_bytes_read Receives the count; may be NULL.
 * @return GVEC_ERR_IO on a read failure, which then sticks (gvec_stream_error()).
 */
GVEC_API GVEC_Result gvec_stream_read(GVEC_Stream * stream, void * buffer, size_t size, size_t * out_bytes_read);

/** Read exactly `size` bytes, or fail: GVEC_ERR_IO if the stream ends first. */
GVEC_API GVEC_Result gvec_stream_read_exact(GVEC_Stream * stream, void * buffer, size_t size);

/**
 * Copy up to `size` bytes from the current position without consuming them;
 * the next read returns the same bytes. Works on every reading stream,
 * seekable or not.
 *
 * @param out_bytes_available Receives how many were copied (fewer at the end); may be NULL.
 */
GVEC_API GVEC_Result gvec_stream_peek(GVEC_Stream * stream, void * buffer, size_t size, size_t * out_bytes_available);

/**
 * Write `size` bytes to a writing stream.
 *
 * @return GVEC_ERR_IO for a reading stream, or after a failure; GVEC_ERR_OOM.
 */
GVEC_API GVEC_Result gvec_stream_write(GVEC_Stream * stream, const void * buffer, size_t size);

/**
 * Move to an absolute position.
 *
 * @return GVEC_ERR_UNSUPPORTED for a stream that cannot seek; GVEC_ERR_IO
 *   past the end or on failure.
 */
GVEC_API GVEC_Result gvec_stream_seek(GVEC_Stream * stream, size_t offset);

/** The current position: bytes consumed from the start, or written. 0 for NULL. */
GVEC_API size_t gvec_stream_tell(const GVEC_Stream * stream);

/** The total length, or GVEC_STREAM_SIZE_UNKNOWN. */
GVEC_API size_t gvec_stream_size(const GVEC_Stream * stream);

/**
 * Whether a read has found the end: true after a read or peek returned fewer
 * bytes than asked because nothing more exists. False for NULL.
 */
GVEC_API bool gvec_stream_eof(const GVEC_Stream * stream);

/** The first failure the stream met, or GVEC_OK. */
GVEC_API GVEC_Result gvec_stream_error(const GVEC_Stream * stream);

/**
 * The bytes a writing stream holds. Valid until the next write or the
 * stream's destruction. `*out_data` is NULL and `*out_size` 0 for a reading
 * stream or an empty one.
 */
GVEC_API void gvec_stream_output_buffer(const GVEC_Stream * stream, const void ** out_data, size_t * out_size);

/** Destroy a stream, closing its file if it has one. Ignored when NULL. */
GVEC_API void gvec_stream_destroy(GVEC_Stream * stream);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_STREAM_H
