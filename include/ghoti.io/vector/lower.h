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
 * @file lower.h
 *
 * Lowering a loaded document into the shape waist.
 *
 * A document is lowered into one of `shape-scene`'s two recording forms: an
 * edit of the retained scene (keyed, so that lowering an unchanged document
 * again changes nothing), or an immediate-mode list. Both entry points walk
 * the document once and emit the same drawing. Neither publishes, renders or
 * touches a sink: the caller owns the edit or list and decides what happens
 * to it.
 *
 * What cannot be lowered is reported, never dropped: each refused element
 * gets one GVEC_DIAG_LOWER_REFUSED entry with its source location and a
 * GVEC_Lower_Reason in `detail`.
 *
 * In this version nothing is lowered yet: each entry point emits nothing and
 * refuses the document's root element with GVEC_LOWER_NOT_YET_LOWERED.
 */

#ifndef GHOTI_IO_GVEC_LOWER_H
#define GHOTI_IO_GVEC_LOWER_H

#include <ghoti.io/vector/macros.h>

#include <stdint.h>

#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>

#include <ghoti.io/shape-scene/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Why an element was not lowered (the `detail` of GVEC_DIAG_LOWER_REFUSED). The numbers are stable. */
typedef enum GVEC_Lower_Reason {
  GVEC_LOWER_NOT_YET_LOWERED = 1, ///< This version lowers nothing; the whole document is refused at its root.
  GVEC_LOWER_REASON_COUNT
} GVEC_Lower_Reason;

/** Options for a lowering. Zero-fill it. */
typedef struct GVEC_Lower_Options {
  uint8_t _reserved[16]; ///< Zero.
} GVEC_Lower_Options;

/**
 * Lower a document into an edit of the retained scene.
 *
 * @param options NULL for the defaults.
 * @param edit The edit to fill; it is not published.
 * @param diagnostics Receives each refusal; may be NULL.
 * @return GVEC_OK when the document was walked, whatever was refused;
 *   GVEC_ERR_INVALID for a NULL document or edit; GVEC_ERR_UNSUPPORTED when
 *   the document's codec cannot lower; GVEC_ERR_OOM.
 */
GVEC_API GVEC_Result gvec_doc_lower_edit(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_Edit * edit, GVEC_Diagnostics * diagnostics);

/** As gvec_doc_lower_edit(), into an immediate-mode list. */
GVEC_API GVEC_Result gvec_doc_lower_list(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_List * list, GVEC_Diagnostics * diagnostics);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_LOWER_H
