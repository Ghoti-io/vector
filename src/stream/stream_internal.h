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
 * @file stream_internal.h
 *
 * The test-only stream constructors, and the whole-input lookahead a load
 * reads through.
 */

#ifndef GHOTI_IO_GVEC_STREAM_INTERNAL_H
#define GHOTI_IO_GVEC_STREAM_INTERNAL_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/vector/stream.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A memory stream that refuses gvec_stream_seek() and reports an unknown size. */
GVEC_INTERNAL_API GVEC_Result gvec_stream_create_memory_no_seek_with_allocator(
    const GVEC_Allocator * allocator, const void * data, size_t size, GVEC_Stream ** out_stream);

/** A memory stream whose reads return at most `max_bytes_per_read` bytes (short reads). */
GVEC_INTERNAL_API GVEC_Result gvec_stream_create_memory_chunked_with_allocator(const GVEC_Allocator * allocator,
    const void * data, size_t size, size_t max_bytes_per_read, GVEC_Stream ** out_stream);

/**
 * Buffer everything from the current position to the end without consuming
 * it, and point at it. The bytes stay valid until the next call on the
 * stream.
 *
 * @param max At most this many bytes are wanted; one more is read to tell.
 * @param out_over Receives whether the input is longer than `max`.
 * @return GVEC_ERR_IO, GVEC_ERR_OOM, GVEC_ERR_INVALID.
 */
GVEC_INTERNAL_API GVEC_Result gvec_stream_lookahead_all(GVEC_Stream * stream, size_t max,
    const unsigned char ** out_data, size_t * out_size, bool * out_over);

/** Consume `count` buffered bytes (at most what is buffered). */
GVEC_INTERNAL_API void gvec_stream_consume(GVEC_Stream * stream, size_t count);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_STREAM_INTERNAL_H
