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
 * @file libver.h
 *
 * Version numbering and the symbol namespace for the Ghoti.io Vector library.
 *
 * Every exported symbol carries a per-version token so that two versions of
 * this library can be loaded into one process without the dynamic linker
 * binding one caller to the other version's implementation.
 */

#ifndef GHOTI_IO_GVEC_LIBVER_H
#define GHOTI_IO_GVEC_LIBVER_H

/**
 * GHOTIIO_VECTOR_NAME and GHOTIIO_VECTOR_VERSION come from here.  They are generated at
 * build time from the Makefile's BRANCH, so that the token inside every
 * exported symbol is the same one that names the .pc file, the install
 * directory and the shared library.
 */
#include <ghoti.io/vector/libver_gen.h>

/**
 * Produce the namespaced form of an identifier.
 *
 * @param NAME The identifier to prefix with GHOTIIO_VECTOR_NAME.
 */
#define GHOTIIO_VECTOR(NAME) GHOTIIO_VECTOR_RENAME(GHOTIIO_VECTOR_NAME, _##NAME)

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_VECTOR_RENAME_INNER(a, b) a##b

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_VECTOR_RENAME(a, b) GHOTIIO_VECTOR_RENAME_INNER(a, b)


//-----------------------------------------------------------------------------
// Version
//-----------------------------------------------------------------------------
//
// The numbers come from libver_gen.h, which the Makefile writes from
// MAJOR_VERSION and MINOR_VERSION. Writing them out here instead is correct
// only until someone bumps the Makefile, at which point the soname, the .pc
// Version: and the install directory all move and these do not.

/** This build's major version. */
#define GVEC_VERSION_MAJOR GHOTIIO_VECTOR_VERSION_MAJOR
/** This build's minor version. */
#define GVEC_VERSION_MINOR GHOTIIO_VECTOR_VERSION_MINOR
/** This build's patch version. */
#define GVEC_VERSION_PATCH GHOTIIO_VECTOR_VERSION_PATCH
/** This build's version as a string, e.g. "1.2.3" or "1.2.3-dev". */
#define GVEC_VERSION_STRING GHOTIIO_VECTOR_VERSION

/**
 * Pack a version into one comparable integer, one byte per component.
 *
 * This is libcurl's LIBCURL_VERSION_NUM layout, which is the common spelling
 * across C libraries: 1.2.3 becomes 0x010203, and a plain `<` compares two
 * versions correctly. Every library in the suite uses it, so a consumer
 * checking one checks them all the same way.
 */
#define GVEC_MAKE_VERSION(major, minor, patch)                                  \
  ((((unsigned)(major)) << 16) | (((unsigned)(minor)) << 8) |                  \
      ((unsigned)(patch)))

/** This build's version, packed. Compare against GVEC_MAKE_VERSION(1, 2, 3). */
#define GVEC_VERSION_NUMBER                                                     \
  GVEC_MAKE_VERSION(GVEC_VERSION_MAJOR, GVEC_VERSION_MINOR, GVEC_VERSION_PATCH)

#endif // GHOTI_IO_GVEC_LIBVER_H
