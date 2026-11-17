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
 * @file svg_resource.h
 *
 * The resolver's side inside the library: the state one lowering shares between the document and everything it loads (the options'
 * resolver, the limits, the cache of answers), the resolution of a reference against a base (RFC 3986, section 5.2), and the one
 * call that asks the resolver. It reads nothing but what the resolver hands over, and a hand-over is copied and bounded.
 */

#ifndef GHOTI_IO_GVEC_SVG_RESOURCE_H
#define GHOTI_IO_GVEC_SVG_RESOURCE_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/vector/lower.h>

#include "../../core/core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The defaults and ceilings of the limits of GVEC_Lower_Options. */
#define GVEC_RES_DEFAULT_BYTES ((size_t) 32 << 20)
#define GVEC_RES_CEILING_BYTES ((size_t) 1 << 30)
#define GVEC_RES_DEFAULT_COUNT 64
#define GVEC_RES_CEILING_COUNT 4096
#define GVEC_RES_DEFAULT_DEPTH 8
#define GVEC_RES_CEILING_DEPTH 64
#define GVEC_RES_DEFAULT_INSTANCES 1024
#define GVEC_RES_CEILING_INSTANCES ((size_t) 1 << 20)

/** One answer, kept for the rest of the lowering. */
typedef struct gvec_res_entry {
  GVEC_Resource_Kind kind;
  char * url;                      ///< The resolved reference (the key).
  int outcome;                     ///< A gvec_res_outcome.
  unsigned char * bytes;           ///< OK: the copy of the resource (never NULL, one byte at least is allocated).
  size_t size;
  char * media_type;               ///< NULL when the resolver gave none.
  void * document;                 ///< DOCUMENT: the loaded GVEC_Doc (owned by the entry), or NULL when it has not been loaded or could not be.
  bool document_tried;
} gvec_res_entry;

/** What asking for a resource came to. */
typedef enum gvec_res_outcome {
  GVEC_RES_OK = 0,        ///< The entry holds the resource.
  GVEC_RES_NO_RESOLVER,   ///< The options give none.
  GVEC_RES_REFUSED,       ///< The resolver refused the reference.
  GVEC_RES_NOT_FOUND,     ///< The resolver found nothing.
  GVEC_RES_FAILED,        ///< The resolver failed, or answered in a way a resolver may not (bytes missing, an unknown status).
  GVEC_RES_TOO_BIG,       ///< The resolver's answer is longer than the limit.
  GVEC_RES_LIMIT_COUNT,   ///< `max_resources` resolver calls have been made.
  GVEC_RES_LIMIT_DEPTH    ///< The request is nested deeper than `max_resource_depth`.
} gvec_res_outcome;

/** The state one lowering shares (in its outermost context; the contexts it makes for what it loads point to it). */
typedef struct gvec_res_state {
  const GVEC_Allocator * allocator;
  GVEC_Resource_Resolver fn;
  void * user;
  size_t max_bytes, max_count, max_depth, max_instances;
  size_t calls;                    ///< Calls of the resolver made.
  size_t instantiations;           ///< `use` references into other documents instantiated.
  gvec_res_entry ** entries;
  size_t count, capacity;
} gvec_res_state;

/** Read the options (NULL for none) into `state`. */
GVEC_INTERNAL_API void gvec_res_init(gvec_res_state * state, const GVEC_Allocator * allocator, const GVEC_Lower_Options * options);

/** Free the entries (and the documents they loaded). */
GVEC_INTERNAL_API void gvec_res_free(gvec_res_state * state);

/** Whether there is a resolver. */
GVEC_INTERNAL_API bool gvec_res_enabled(const gvec_res_state * state);

/**
 * `reference` resolved against `base` (RFC 3986, 5.2.2; the fragment is dropped); `*out` is allocated with `allocator`. With no base a
 * relative reference is only normalised (its dot segments removed, a leading `..` kept).
 */
GVEC_INTERNAL_API GVEC_Result gvec_res_resolve(const GVEC_Allocator * allocator, const char * base, const char * reference, char ** out);

/**
 * Ask for a resource: the cache first, then the resolver, within the limits. `cap` is the limit of the kind, which `max_resource_bytes` lowers.
 *
 * @param reference The reference as written, a fragment allowed (it is cut off).
 * @param depth The depth of this request (1 for a reference of the document being lowered).
 * @param out_outcome What came of it; `*out_entry` is the entry for OK (and for the cached failures, with their outcome), else NULL.
 * @return GVEC_OK; GVEC_ERR_OOM (the library's or the resolver's).
 */
GVEC_INTERNAL_API GVEC_Result gvec_res_fetch(gvec_res_state * state, GVEC_Resource_Kind kind, const char * base, const char * reference,
    size_t cap, size_t depth, gvec_res_outcome * out_outcome, gvec_res_entry ** out_entry);

/** The fragment of a reference (the text after the first `#`), or NULL; and the length of the part before it. */
GVEC_INTERNAL_API const char * gvec_res_fragment(const char * reference, size_t * before);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_SVG_RESOURCE_H
