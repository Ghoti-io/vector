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
 * @file svg_arena.h
 *
 * The typed document's allocator: a chain of blocks taken from the document's
 * allocator and released together, so that no element, attribute, string or
 * path has a lifetime of its own.
 */

#ifndef GHOTI_IO_GVEC_SVG_ARENA_H
#define GHOTI_IO_GVEC_SVG_ARENA_H

#include <ghoti.io/vector/macros.h>

#include <stddef.h>

#include <ghoti.io/vector/allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gvec_arena_block;

/** An arena. Zero it, then gvec_arena_init(). */
typedef struct gvec_arena {
  const GVEC_Allocator * allocator;
  struct gvec_arena_block * head;
  size_t bytes; ///< Bytes handed out, for the tests and the limits.
} gvec_arena;

/** Prepare an empty arena that takes its blocks from `allocator` (NULL for the default). */
GVEC_INTERNAL_API void gvec_arena_init(gvec_arena * arena, const GVEC_Allocator * allocator);

/** `size` bytes aligned to `align` (a power of two, at most 16), zeroed; NULL on failure or a size that wraps. */
GVEC_INTERNAL_API void * gvec_arena_alloc(gvec_arena * arena, size_t size, size_t align);

/** A NUL-terminated copy of `length` bytes of `text`; NULL on failure. */
GVEC_INTERNAL_API char * gvec_arena_strndup(gvec_arena * arena, const char * text, size_t length);

/** An array of `count` elements of `size` bytes, zeroed; NULL on failure or a product that wraps. */
GVEC_INTERNAL_API void * gvec_arena_array(gvec_arena * arena, size_t count, size_t size, size_t align);

/** Release every block. The arena may be gvec_arena_init()ed again. */
GVEC_INTERNAL_API void gvec_arena_destroy(gvec_arena * arena);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_ARENA_H
