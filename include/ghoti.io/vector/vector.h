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
 * @file vector.h
 *
 * The umbrella header of Ghoti.io Vector: vector formats, SVG first, read into
 * a document and lowered into the shape waist.
 *
 * **Threads.** The codec registry is written before `main` and only read
 * after, so its functions may be called from any thread. Every other object
 * (a stream, a document, a diagnostics list) is used by one thread at a time;
 * different objects may be used on different threads at once.
 */

#ifndef GHOTI_IO_GVEC_VECTOR_H
#define GHOTI_IO_GVEC_VECTOR_H

#include <ghoti.io/vector/macros.h>

#include <ghoti.io/vector/allocator.h>
#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/lower.h>
#include <ghoti.io/vector/stream.h>
#include <ghoti.io/vector/style.h>
#include <ghoti.io/vector/svg.h>
#include <ghoti.io/vector/svg_prop.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The library's version, packed as GVEC_MAKE_VERSION() packs it, as built. */
GVEC_API unsigned gvec_version_number(void);

/** The library's version as a string, for example "0.0.0", as built. */
GVEC_API const char * gvec_version_string(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_VECTOR_H
