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
 * @file allocator.h
 *
 * The allocator every object of this library allocates through.
 *
 * `GVEC_Allocator` is cutil's `GCU_Allocator`, so an allocator built for any
 * library of the suite works here unchanged. A NULL `const GVEC_Allocator *`
 * anywhere in this library means gvec_allocator_default(). Nothing here calls
 * `malloc` directly.
 */

#ifndef GHOTI_IO_GVEC_ALLOCATOR_H
#define GHOTI_IO_GVEC_ALLOCATOR_H

#include <ghoti.io/vector/macros.h>

#include <ghoti.io/cutil/allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The allocator vtable: cutil's, under this library's name. */
typedef GCU_Allocator GVEC_Allocator;

/**
 * The allocator a NULL allocator argument selects: cutil's default, backed by
 * the C library.
 *
 * @return A process-wide allocator; never NULL.
 */
GVEC_API const GVEC_Allocator * gvec_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_ALLOCATOR_H
