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
 * @file fonts.h
 *
 * The fonts a lowering draws text with.
 *
 * This library scans nothing and reads no environment: a drawing of text is a function of the document and of the fonts it is
 * given. The caller builds a `GFNT_FontSet` (the font library's `gfnt_fontset_scan()` over the directories it chooses, with the
 * aliases it wants for the generic families: `sans-serif`, `serif`, `monospace`, `cursive` and `fantasy` are whatever the set's
 * aliases make them) and wraps it in a `GVEC_Fonts`, which opens the faces on first need and keeps them open.
 *
 * **Lifetime.** A scene that has text in it holds the faces the lowering chose (the scene's glyph provider reads their outlines when it
 * draws), so the `GVEC_Fonts`, and the set under it, must outlive every list, edit and scene a lowering with them has made. One
 * `GVEC_Fonts` may serve any number of lowerings, one after another or at once.
 *
 * A character no family of the document's `font-family` list has a face for is drawn with the set's `serif` (an alias the caller
 * makes, like the other generic families), and then with any face of the set that maps it; a character no face maps is the first face's
 * `.notdef`.
 *
 * Without fonts in `GVEC_Lower_Options`, an element with text is refused (`GVEC_LOWER_TEXT`), never drawn in a face the document
 * did not name.
 */

#ifndef GHOTI_IO_GVEC_FONTS_H
#define GHOTI_IO_GVEC_FONTS_H

#include <ghoti.io/vector/macros.h>

#include <ghoti.io/font/discover.h>

#include <ghoti.io/vector/allocator.h>
#include <ghoti.io/vector/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The faces of a set, opened as text needs them. Opaque. */
typedef struct GVEC_Fonts GVEC_Fonts;

/**
 * Make the fonts of @p set.
 *
 * @param set The set; not copied, and it must outlive the result.
 * @param allocator NULL for the default.
 * @param out Receives the fonts; free them with gvec_fonts_free().
 * @return GVEC_OK, GVEC_ERR_INVALID (NULL), GVEC_ERR_OOM.
 */
GVEC_API GVEC_Result gvec_fonts_new(const GFNT_FontSet * set, const GVEC_Allocator * allocator, GVEC_Fonts ** out);

/** Close every face and release the fonts; NULL is ignored. Every scene made with them must be gone. */
GVEC_API void gvec_fonts_free(GVEC_Fonts * fonts);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GVEC_FONTS_H */
