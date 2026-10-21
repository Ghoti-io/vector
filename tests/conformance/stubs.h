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
 * @file
 *
 * The planted lowerings the conformance runner can be told to use instead of
 * `gvec_doc_lower_list`, so that each outcome of the harness is produced and
 * tested before a real lowering exists. They draw on a canvas of any size; the
 * synthetic corpus the self-test builds has 16 by 16 references.
 *
 *   exact          fills the pixel-aligned rectangle (2,2)-(10,10) in opaque red
 *   exact-refused  the same, and reports one refusal as well
 *   off-by-one     the same rectangle one row taller
 *   exact-dropped  the same rectangle, and a diagnostic dropped past the cap
 *   minus1/minus2  the same rectangle with the red channel one or two levels low
 *
 * They are compiled only into the runner, which is not installed.
 */

#ifndef GHOTI_IO_GVEC_CONFORMANCE_STUBS_H
#define GHOTI_IO_GVEC_CONFORMANCE_STUBS_H

#include <ghoti.io/vector/vector.h>

#include <ghoti.io/shape-scene/shape-scene.h>

/** A lowering with the signature of gvec_doc_lower_list(); `store` is the list's store. */
typedef GVEC_Result (*Conf_Lower_Fn)(const GVEC_Doc * doc, const GVEC_Lower_Options * options, GSHSCENE_List * list,
    GVEC_Diagnostics * diagnostics);

/** The stub named `name`, or NULL. */
Conf_Lower_Fn conf_stub_find(const char * name);

/** The store the stubs intern their path in; set by the runner before each case. */
void conf_stub_set_store(GSHSCENE_Store * store);

#endif // GHOTI_IO_GVEC_CONFORMANCE_STUBS_H
