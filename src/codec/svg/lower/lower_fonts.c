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
 * @file lower_fonts.c
 *
 * GVEC_Fonts: the faces of a font set, opened as text needs them, and the scene fonts that bind a face to a scene's glyph provider.
 * The font library's fallback opens a face on first need and keeps it open until it is freed, so a face pointer is good for the life of
 * the fonts; one lock covers the fallback and the table of scene fonts, and shaping (which only reads a face) runs outside it.
 */

#include <ghoti.io/vector/macros.h>

#include <ctype.h>
#include <string.h>

#include <ghoti.io/cutil/mutex.h>
#include <ghoti.io/font/match.h>
#include <ghoti.io/font/scene.h>

#include "lower_internal.h"

typedef struct font_entry {
  const GFNT_Face * face;
  GFNT_SceneFont * scene;
} font_entry;

struct GVEC_Fonts {
  const GFNT_FontSet * set;
  GFNT_FontFallback * fallback;
  const GVEC_Allocator * allocator;
  GCU_MUTEX_T lock;
  font_entry * entries;
  size_t count, capacity;
};

GVEC_Result gvec_fonts_new(const GFNT_FontSet * set, const GVEC_Allocator * allocator, GVEC_Fonts ** out) {
  if (!set || !out) {
    return GVEC_ERR_INVALID;
  }
  *out = NULL;
  GVEC_Fonts * f = gvec_alloc(allocator, sizeof *f);
  if (!f) {
    return GVEC_ERR_OOM;
  }
  memset(f, 0, sizeof *f);
  f->set = set;
  f->allocator = allocator;
  if (GCU_MUTEX_CREATE(f->lock) != 0) {
    gvec_free(allocator, f);
    return GVEC_ERR_INTERNAL;
  }
  GFNT_FontFallbackOptions options;
  memset(&options, 0, sizeof options);
  GFNT_Result r = gfnt_fontfallback_create(set, &options, NULL, &f->fallback, NULL);
  if (r != GFNT_OK) {
    GCU_MUTEX_DESTROY(f->lock);
    gvec_free(allocator, f);
    return r == GFNT_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
  }
  *out = f;
  return GVEC_OK;
}

void gvec_fonts_free(GVEC_Fonts * f) {
  if (!f) {
    return;
  }
  for (size_t i = 0; i < f->count; i++) {
    gfnt_scene_font_destroy(f->entries[i].scene);
  }
  gvec_free(f->allocator, f->entries);
  gfnt_fontfallback_free(f->fallback);
  GCU_MUTEX_DESTROY(f->lock);
  gvec_free(f->allocator, f);
}

static bool same_name(const char * a, const char * b) {
  if (!a || !b) {
    return false;
  }
  for (; *a && *b; a++, b++) {
    if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) {
      return false;
    }
  }
  return *a == *b;
}

static bool record_family_is(const GFNT_FontRecord * r, const GFNT_FontRecord * wanted) {
  return r && wanted && (same_name(r->family, wanted->family) || (r->legacy_family && same_name(r->legacy_family, wanted->family)) ||
                            (wanted->legacy_family && same_name(r->family, wanted->legacy_family)));
}

GVEC_Result gvec_fonts_face(const GVEC_Fonts * constant, const char * const * families, size_t family_count, double weight, int italic, double width,
    uint32_t codepoint, const GFNT_Face ** out, bool * found) {
  GVEC_Fonts * f = (GVEC_Fonts *) constant; /* The lock and the open faces are the fonts' own state, not the caller's. */
  *found = false;
  *out = NULL;
  GVEC_Result result = GVEC_OK;
  GCU_MUTEX_LOCK(f->lock);
  GFNT_FontQuery query;
  gfnt_fontquery_init(&query);
  query.weight = (GFNT_F16Dot16) (weight * 65536.0);
  query.width = (GFNT_F16Dot16) (width * 65536.0);
  query.slant = italic == 1 ? GFNT_SLANT_ITALIC : italic == 2 ? GFNT_SLANT_OBLIQUE : GFNT_SLANT_ROMAN;
  for (size_t i = 0; i < family_count && !*found; i++) {
    const char * name = families[i];
    GFNT_FontMatch match;
    bool has = false;
    query.families = &name;
    query.family_count = 1;
    GFNT_Result r = gfnt_fontset_match_family(f->set, name, &query, NULL, &match, &has, NULL);
    if (r == GFNT_ERR_OOM) {
      result = GVEC_ERR_OOM;
      break;
    }
    if (r != GFNT_OK || !has) {
      continue;
    }
    const GFNT_Face * face = NULL;
    size_t index = 0;
    bool mapped = false;
    /* A generic family is an alias of the set: the fallback is asked for the family it stands for. */
    const GFNT_FontRecord * matched = gfnt_fontset_at(f->set, match.index);
    const char * real = matched ? matched->family : name;
    query.families = &real;
    r = gfnt_fontfallback_face(f->fallback, codepoint, 0, 0, &query, &face, &index, &mapped, NULL);
    if (r == GFNT_ERR_OOM) {
      result = GVEC_ERR_OOM;
      break;
    }
    if (r == GFNT_OK && mapped && record_family_is(gfnt_fontset_at(f->set, index), gfnt_fontset_at(f->set, match.index))) {
      *out = face;
      *found = true;
    }
  }
  if (result == GVEC_OK && !*found) {
    /* The document's families have no face for the character: the `serif` family of the set (the usual initial `font-family`), then any
     * face that maps it, as the fallback of the font library chooses; the style asked for is kept. */
    const char * generic = "serif";
    GFNT_FontMatch match;
    bool has = false;
    query.families = &generic;
    query.family_count = 1;
    if (gfnt_fontset_match_family(f->set, generic, &query, NULL, &match, &has, NULL) == GFNT_OK && has) {
      const GFNT_FontRecord * matched = gfnt_fontset_at(f->set, match.index);
      const char * real = matched ? matched->family : generic;
      const GFNT_Face * face = NULL;
      size_t index = 0;
      bool mapped = false;
      query.families = &real;
      if (gfnt_fontfallback_face(f->fallback, codepoint, 0, 0, &query, &face, &index, &mapped, NULL) == GFNT_OK && mapped &&
          record_family_is(gfnt_fontset_at(f->set, index), matched)) {
        *out = face;
        *found = true;
      }
    }
  }
  if (result == GVEC_OK && !*found) {
    const GFNT_Face * face = NULL;
    bool mapped = false;
    query.families = NULL;
    query.family_count = 0;
    GFNT_Result r = gfnt_fontfallback_face(f->fallback, codepoint, 0, 0, &query, &face, NULL, &mapped, NULL);
    if (r == GFNT_ERR_OOM) {
      result = GVEC_ERR_OOM;
    }
    else if (r == GFNT_OK && mapped) {
      *out = face;
      *found = true;
    }
  }
  GCU_MUTEX_UNLOCK(f->lock);
  return result;
}

GVEC_Result gvec_fonts_scene_face(const GVEC_Fonts * constant, const GFNT_Face * face, GSHSCENE_Store * store, const GSHSCENE_Resource ** out) {
  GVEC_Fonts * f = (GVEC_Fonts *) constant;
  GVEC_Result result = GVEC_OK;
  GFNT_SceneFont * scene = NULL;
  *out = NULL;
  GCU_MUTEX_LOCK(f->lock);
  for (size_t i = 0; i < f->count; i++) {
    if (f->entries[i].face == face) {
      scene = f->entries[i].scene;
      break;
    }
  }
  if (!scene) {
    GFNT_Result r = gfnt_scene_font_create(face, &scene, NULL);
    if (r != GFNT_OK) {
      result = r == GFNT_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_UNSUPPORTED;
    }
    else if (f->count == f->capacity) {
      size_t capacity = f->capacity ? f->capacity * 2 : 8;
      font_entry * grown = gvec_realloc(f->allocator, f->entries, capacity * sizeof *grown);
      if (!grown) {
        gfnt_scene_font_destroy(scene);
        scene = NULL;
        result = GVEC_ERR_OOM;
      }
      else {
        f->entries = grown;
        f->capacity = capacity;
      }
    }
    if (scene) {
      f->entries[f->count].face = face;
      f->entries[f->count].scene = scene;
      f->count++;
    }
  }
  GCU_MUTEX_UNLOCK(f->lock);
  if (result != GVEC_OK) {
    return result;
  }
  GFNT_Result r = gfnt_scene_intern_face(scene, store, NULL, out, NULL);
  if (r == GFNT_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  return r == GFNT_OK ? GVEC_OK : r == GFNT_ERR_LIMIT ? GVEC_ERR_LIMIT : GVEC_ERR_UNSUPPORTED;
}
