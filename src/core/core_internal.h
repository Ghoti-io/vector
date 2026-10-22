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
 * @file core_internal.h
 *
 * Limits resolution and diagnostic appends, shared by every module.
 */

#ifndef GHOTI_IO_GVEC_CORE_INTERNAL_H
#define GHOTI_IO_GVEC_CORE_INTERNAL_H

#include <ghoti.io/vector/macros.h>

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The XML parser's own limits, which are both the defaults and the ceilings (L3). */
#define GVEC_DEFAULT_MAX_INPUT_BYTES ((size_t)64 * 1024 * 1024)
#define GVEC_DEFAULT_MAX_INFLATED_BYTES ((size_t)64 * 1024 * 1024)
#define GVEC_DEFAULT_MAX_ELEMENTS ((size_t)10000000)
#define GVEC_DEFAULT_MAX_DEPTH ((size_t)256)
#define GVEC_DEFAULT_MAX_ATTRIBUTES ((size_t)1024)
#define GVEC_DEFAULT_MAX_ENTITY_EXPANDED_BYTES ((size_t)64 * 1024 * 1024)
#define GVEC_DEFAULT_MAX_ENTITY_DEPTH ((size_t)32)
#define GVEC_DEFAULT_MAX_ENTITY_REFERENCES ((size_t)1000000)
#define GVEC_DEFAULT_MAX_PATH_SEGMENTS ((size_t)4194304)
#define GVEC_DEFAULT_MAX_LIST_ITEMS ((size_t)1048576)
#define GVEC_DEFAULT_MAX_REFERENCE_EXPANSION ((size_t)1000000)
#define GVEC_DEFAULT_MAX_TEXT_BYTES ((size_t)64 * 1024 * 1024)
#define GVEC_DEFAULT_MAX_DIAGNOSTICS ((size_t)4096)
#define GVEC_CEILING_MAX_DIAGNOSTICS ((size_t)1 << 20)

/**
 * The limits a load uses: each zero field replaced by its default, each field
 * checked against its ceiling.
 *
 * @param in The caller's limits; NULL for all defaults.
 * @param out Receives the resolved limits.
 * @param out_field Receives the first field above its ceiling, on GVEC_ERR_INVALID; may be NULL.
 * @return GVEC_ERR_INVALID if a field is above its ceiling or `_reserved` is not zero.
 */
GVEC_INTERNAL_API GVEC_Result gvec_limits_resolve(const GVEC_Limits * in, GVEC_Limits * out, GVEC_Limit_Field * out_field);

/** The ceiling of one field. */
GVEC_INTERNAL_API size_t gvec_limits_ceiling(GVEC_Limit_Field field);

/**
 * Append one entry, or count it in `dropped` past the list's cap. The
 * element name is copied. A NULL list is a no-op.
 *
 * @return GVEC_ERR_OOM if the entry could not be stored (it is then counted
 *   in `dropped`, so it is not lost silently).
 */
GVEC_INTERNAL_API GVEC_Result gvec_diag_add(GVEC_Diagnostics * diagnostics, GVEC_Diag_Severity severity,
    GVEC_Diag_Code code, uint32_t detail, size_t line, size_t column, size_t offset, const char * element,
    const char * message);

/** Set the cap a list keeps entries to; called by a load with its resolved limits. */
GVEC_INTERNAL_API void gvec_diag_set_cap(GVEC_Diagnostics * diagnostics, size_t cap);

/** Allocate through `allocator`, or the default when NULL; NULL for zero bytes or failure. */
GVEC_INTERNAL_API void * gvec_alloc(const GVEC_Allocator * allocator, size_t size);

/** Reallocate through `allocator`, or the default when NULL. */
GVEC_INTERNAL_API void * gvec_realloc(const GVEC_Allocator * allocator, void * ptr, size_t size);

/** Free through `allocator`, or the default when NULL. Ignores NULL. */
GVEC_INTERNAL_API void gvec_free(const GVEC_Allocator * allocator, void * ptr);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_CORE_INTERNAL_H
