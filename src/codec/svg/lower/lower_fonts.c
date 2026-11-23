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
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/match.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/scene.h>

#include <ghoti.io/text/css/css_value.h>

#include "../style/svg_style.h"
#include "lower_internal.h"

typedef struct font_entry {
  const GFNT_Face * face;
  GFNT_SceneFont * scene;
} font_entry;

/* A face the documents lowered with these fonts gave (an `@font-face`): its bytes, and the face loaded from them, kept as long as the fonts are, since the scene's
 * glyph runs refer to what is made of it. The same bytes are one face. */
typedef struct doc_face {
  unsigned char * bytes;
  size_t size;
  uint64_t hash;
  GFNT_Blob * blob;
  GFNT_Face * face;
} doc_face;

#define DOC_FACE_MAX 256
#define DOC_FACE_BYTES_MAX ((size_t) 128 << 20)

struct GVEC_Fonts {
  const GFNT_FontSet * set;
  doc_face * doc;
  size_t doc_count, doc_capacity;
  size_t doc_bytes;
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
  for (size_t i = 0; i < f->doc_count; i++) {
    gfnt_face_free(f->doc[i].face);
    gfnt_blob_destroy(f->doc[i].blob);
    gvec_free(f->allocator, f->doc[i].bytes);
  }
  gvec_free(f->allocator, f->doc);
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

/* The face of the caller's fonts that is of the family `name`, in the style of `query`, and maps `codepoint` (the fonts' fallback is asked for that family only). The
 * lock is held by the caller. */
static GVEC_Result set_family_face(GVEC_Fonts * f, const char * name, GFNT_FontQuery * query, uint32_t codepoint, const GFNT_Face ** out, bool * found) {
  GFNT_FontMatch match;
  bool has = false;
  query->families = &name;
  query->family_count = 1;
  GFNT_Result r = gfnt_fontset_match_family(f->set, name, query, NULL, &match, &has, NULL);
  if (r == GFNT_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (r != GFNT_OK || !has) {
    return GVEC_OK;
  }
  const GFNT_Face * face = NULL;
  size_t index = 0;
  bool mapped = false;
  /* A generic family is an alias of the set: the fallback is asked for the family it stands for. */
  const GFNT_FontRecord * matched = gfnt_fontset_at(f->set, match.index);
  const char * real = matched ? matched->family : name;
  query->families = &real;
  r = gfnt_fontfallback_face(f->fallback, codepoint, 0, 0, query, &face, &index, &mapped, NULL);
  if (r == GFNT_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (r == GFNT_OK && mapped && record_family_is(gfnt_fontset_at(f->set, index), gfnt_fontset_at(f->set, match.index))) {
    *out = face;
    *found = true;
  }
  return GVEC_OK;
}

/* The fonts' own face for a character no family of the span gave: the `serif` family of the set (the usual initial `font-family`), then any face that maps it, as the
 * fallback of the font library chooses; the style asked for is kept. The lock is held by the caller. */
static GVEC_Result set_fallback_face(GVEC_Fonts * f, GFNT_FontQuery * query, uint32_t codepoint, const GFNT_Face ** out, bool * found) {
  const char * generic = "serif";
  GFNT_FontMatch match;
  bool has = false;
  query->families = &generic;
  query->family_count = 1;
  if (gfnt_fontset_match_family(f->set, generic, query, NULL, &match, &has, NULL) == GFNT_OK && has) {
    const GFNT_FontRecord * matched = gfnt_fontset_at(f->set, match.index);
    const char * real = matched ? matched->family : generic;
    const GFNT_Face * face = NULL;
    size_t index = 0;
    bool mapped = false;
    query->families = &real;
    if (gfnt_fontfallback_face(f->fallback, codepoint, 0, 0, query, &face, &index, &mapped, NULL) == GFNT_OK && mapped &&
        record_family_is(gfnt_fontset_at(f->set, index), matched)) {
      *out = face;
      *found = true;
      return GVEC_OK;
    }
  }
  const GFNT_Face * face = NULL;
  bool mapped = false;
  query->families = NULL;
  query->family_count = 0;
  GFNT_Result r = gfnt_fontfallback_face(f->fallback, codepoint, 0, 0, query, &face, NULL, &mapped, NULL);
  if (r == GFNT_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (r == GFNT_OK && mapped) {
    *out = face;
    *found = true;
  }
  return GVEC_OK;
}

static void query_of(GFNT_FontQuery * query, double weight, int italic, double width) {
  gfnt_fontquery_init(query);
  query->weight = (GFNT_F16Dot16) (weight * 65536.0);
  query->width = (GFNT_F16Dot16) (width * 65536.0);
  query->slant = italic == 1 ? GFNT_SLANT_ITALIC : italic == 2 ? GFNT_SLANT_OBLIQUE : GFNT_SLANT_ROMAN;
}

GVEC_Result gvec_fonts_face(const GVEC_Fonts * constant, const char * const * families, size_t family_count, double weight, int italic, double width,
    uint32_t codepoint, const GFNT_Face ** out, bool * found) {
  GVEC_Fonts * f = (GVEC_Fonts *) constant; /* The lock and the open faces are the fonts' own state, not the caller's. */
  *found = false;
  *out = NULL;
  GVEC_Result result = GVEC_OK;
  GCU_MUTEX_LOCK(f->lock);
  GFNT_FontQuery query;
  query_of(&query, weight, italic, width);
  for (size_t i = 0; i < family_count && !*found && result == GVEC_OK; i++) {
    result = set_family_face(f, families[i], &query, codepoint, out, found);
  }
  if (result == GVEC_OK && !*found) {
    result = set_fallback_face(f, &query, codepoint, out, found);
  }
  GCU_MUTEX_UNLOCK(f->lock);
  return result;
}

/* A face from bytes (a font file as the `@font-face` of a document gives it): the same bytes are the face made before. The bytes are copied. */
static GVEC_Result adopt_face(GVEC_Fonts * f, const unsigned char * bytes, size_t size, const GFNT_Face ** out) {
  *out = NULL;
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; i++) {
    hash = (hash ^ bytes[i]) * 1099511628211ull;
  }
  GVEC_Result result = GVEC_OK;
  GCU_MUTEX_LOCK(f->lock);
  for (size_t i = 0; i < f->doc_count; i++) {
    if (f->doc[i].hash == hash && f->doc[i].size == size && memcmp(f->doc[i].bytes, bytes, size) == 0) {
      *out = f->doc[i].face;
      GCU_MUTEX_UNLOCK(f->lock);
      return GVEC_OK;
    }
  }
  if (f->doc_count >= DOC_FACE_MAX || size > DOC_FACE_BYTES_MAX - f->doc_bytes) {
    GCU_MUTEX_UNLOCK(f->lock);
    return GVEC_ERR_LIMIT;
  }
  if (f->doc_count == f->doc_capacity) {
    size_t capacity = f->doc_capacity ? f->doc_capacity * 2 : 8;
    doc_face * grown = gvec_realloc(f->allocator, f->doc, capacity * sizeof *grown);
    if (!grown) {
      GCU_MUTEX_UNLOCK(f->lock);
      return GVEC_ERR_OOM;
    }
    f->doc = grown;
    f->doc_capacity = capacity;
  }
  doc_face * d = &f->doc[f->doc_count];
  memset(d, 0, sizeof *d);
  d->bytes = gvec_alloc(f->allocator, size ? size : 1);
  if (!d->bytes) {
    GCU_MUTEX_UNLOCK(f->lock);
    return GVEC_ERR_OOM;
  }
  memcpy(d->bytes, bytes, size);
  d->size = size;
  d->hash = hash;
  GFNT_Result r = gfnt_blob_create_memory(d->bytes, size, GFNT_BLOB_BORROWED, NULL, NULL, &d->blob, NULL);
  if (r == GFNT_OK) {
    r = gfnt_face_load(d->blob, 0, NULL, NULL, &d->face, NULL);
  }
  if (r != GFNT_OK) {
    gfnt_blob_destroy(d->blob);
    gvec_free(f->allocator, d->bytes);
    result = r == GFNT_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_UNSUPPORTED; /* not a font this reads */
  }
  else {
    f->doc_count++;
    f->doc_bytes += size;
    *out = d->face;
  }
  GCU_MUTEX_UNLOCK(f->lock);
  return result;
}

/* ---- the faces of a document's `@font-face` rules ---- */

typedef struct face_memo {
  const void * rule;       ///< The rule (the key).
  const GFNT_Face * face;  ///< The face it came to, or NULL when it gave none (said once).
} face_memo;

/* The bytes of one `src` entry of a rule, or none: a `data:` URI is decoded, any other reference is the resolver's. `*bytes` is freed by the caller when `*owned`. */
static GVEC_Result src_bytes(gvec_lw_ctx * ctx, const GTEXT_CSS_Font_Src * src, unsigned char ** bytes, size_t * size, bool * owned) {
  *bytes = NULL;
  *size = 0;
  *owned = false;
  if (src->kind != GTEXT_CSS_FONT_SRC_URL || !src->url.data || src->url.length == 0) {
    return GVEC_OK;
  }
  const GVEC_Limits * limits = &ctx->document->limits;
  gvec_svg_parse_context pc;
  memset(&pc, 0, sizeof pc);
  pc.arena = &ctx->arena;
  pc.allocator = ctx->allocator;
  pc.max_list_items = limits->max_list_items;
  pc.max_path_segments = limits->max_path_segments;
  GVEC_Svg_Reference ref;
  gvec_svg_parse_error err;
  gvec_svg_parse_status st = gvec_svg_parse_reference(src->url.data, src->url.length, &pc, &ref, &err);
  if (st == GVEC_SVG_PARSE_OOM) {
    return GVEC_ERR_OOM;
  }
  if (st == GVEC_SVG_PARSE_INVALID) {
    return GVEC_OK;
  }
  if (ref.kind == GVEC_SVG_REF_DATA) {
    size_t length = ref.data.payload_length;
    if (length == 0) {
      return GVEC_OK;
    }
    size_t bound = ref.data.base64 ? (length / 4 + 1) * 3 : length;
    size_t capacity = bound < limits->max_image_bytes ? bound : limits->max_image_bytes;
    unsigned char * buffer = gvec_alloc(ctx->allocator, capacity ? capacity : 1);
    if (!buffer) {
      return GVEC_ERR_OOM;
    }
    size_t written = 0;
    if (gvec_svg_data_uri_decode(&ref.data, buffer, capacity, &written) != GVEC_OK || written == 0) {
      gvec_free(ctx->allocator, buffer);
      return GVEC_OK;
    }
    *bytes = buffer;
    *size = written;
    *owned = true;
    return GVEC_OK;
  }
  if (ref.kind != GVEC_SVG_REF_EXTERNAL || !ref.url || !gvec_res_enabled(ctx->res)) {
    return GVEC_OK;
  }
  gvec_res_outcome outcome;
  gvec_res_entry * entry = NULL;
  GVEC_Result r = gvec_res_fetch(ctx->res, GVEC_RESOURCE_FONT, ctx->base, ref.url, limits->max_image_bytes, ctx->res_depth + 1, &outcome, &entry);
  if (r != GVEC_OK) {
    return r;
  }
  if (outcome == GVEC_RES_OK && entry && entry->size > 0) {
    *bytes = entry->bytes;
    *size = entry->size;
  }
  return GVEC_OK;
}

/* The face a rule gives: the first of its `src` entries that is a font this reads. Asked of once for each rule of a lowering. */
static GVEC_Result rule_face(gvec_lw_ctx * ctx, const GTEXT_CSS_Font_Face * rule, const GFNT_Face ** out) {
  face_memo * memos = ctx->font_faces;
  for (size_t i = 0; i < ctx->font_face_count; i++) {
    if (memos[i].rule == rule->rule) {
      *out = memos[i].face;
      return GVEC_OK;
    }
  }
  const GFNT_Face * face = NULL;
  for (size_t k = 0; !face && k < rule->src_count; k++) {
    unsigned char * bytes;
    size_t size;
    bool owned;
    GVEC_Result r = src_bytes(ctx, &rule->src[k], &bytes, &size, &owned);
    if (r != GVEC_OK) {
      return r;
    }
    if (bytes) {
      r = adopt_face((GVEC_Fonts *) ctx->options->fonts, bytes, size, &face);
      if (owned) {
        gvec_free(ctx->allocator, bytes);
      }
      if (r == GVEC_ERR_OOM) {
        return r;
      }
      if (r != GVEC_OK) {
        face = NULL; /* not a font, or past what is kept: the next source */
      }
    }
  }
  if (ctx->font_face_count == ctx->font_face_capacity) {
    size_t capacity = ctx->font_face_capacity ? ctx->font_face_capacity * 2 : 8;
    face_memo * grown = gvec_arena_array(&ctx->arena, capacity, sizeof *grown, sizeof(void *));
    if (!grown) {
      return GVEC_ERR_OOM;
    }
    if (ctx->font_face_count) {
      memcpy(grown, memos, ctx->font_face_count * sizeof *grown);
    }
    ctx->font_faces = memos = grown;
    ctx->font_face_capacity = capacity;
  }
  memos[ctx->font_face_count].rule = rule->rule;
  memos[ctx->font_face_count].face = face;
  ctx->font_face_count++;
  *out = face;
  return GVEC_OK;
}

static bool range_has(const GTEXT_CSS_Font_Face * rule, uint32_t cp) {
  for (size_t i = 0; i < rule->range_count; i++) {
    if (cp >= rule->ranges[i].first && cp <= rule->ranges[i].last) {
      return true;
    }
  }
  return rule->range_count == 0;
}

/* How far a rule is from the style asked for (CSS Fonts 4, 5.2: the style, then the width, then the weight; each is a distance, a range that holds the value is 0). */
static double rule_distance(const GTEXT_CSS_Font_Face * rule, double weight, int italic, double width) {
  double distance = 0;
  const int has_style = rule->style == GTEXT_CSS_FACE_STYLE_ITALIC ? 1 : rule->style == GTEXT_CSS_FACE_STYLE_OBLIQUE ? 2 : 0;
  if (has_style != italic) {
    distance += (has_style && italic) ? 1e6 : 1e9;
  }
  if (!rule->width_auto && (width < rule->width_min || width > rule->width_max)) {
    distance += 1e3 * (width < rule->width_min ? rule->width_min - width : width - rule->width_max);
  }
  if (!rule->weight_auto && (weight < rule->weight_min || weight > rule->weight_max)) {
    distance += weight < rule->weight_min ? rule->weight_min - weight : weight - rule->weight_max;
  }
  return distance;
}

/* The face of the document's own `@font-face` rules for a family and a character, if the document defines the family and one of its rules has the character. */
static GVEC_Result document_face(gvec_lw_ctx * ctx, const char * name, double weight, int italic, double width, uint32_t cp, const GFNT_Face ** out, bool * found) {
  GVEC_Svg_Style_Context * style = ctx->style;
  if (!style || gvec_svg_style_sheets(style) != GVEC_OK || !style->atrules) {
    return GVEC_OK;
  }
  const GTEXT_CSS_Font_Face * rules[32];
  size_t n = gvec_svg_style_font_faces(style, name, strlen(name), rules, 32);
  if (n > 32) {
    n = 32;
  }
  /* The rules are in cascade order; the later of two at the same distance wins. */
  const GTEXT_CSS_Font_Face * best = NULL;
  double best_distance = 0;
  for (size_t i = 0; i < n; i++) {
    if (!range_has(rules[i], cp)) {
      continue;
    }
    double d = rule_distance(rules[i], weight, italic, width);
    if (!best || d <= best_distance) {
      best = rules[i];
      best_distance = d;
    }
  }
  if (!best) {
    return GVEC_OK;
  }
  const GFNT_Face * face = NULL;
  GVEC_Result r = rule_face(ctx, best, &face);
  if (r != GVEC_OK) {
    return r;
  }
  uint32_t glyph = 0;
  if (face && gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) == GFNT_OK && glyph != 0) {
    *out = face;
    *found = true;
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_font_face(gvec_lw_ctx * ctx, const char * const * families, size_t family_count, double weight, int italic, double width, uint32_t codepoint,
    const GFNT_Face ** out, bool * found, size_t * rank) {
  const GVEC_Fonts * fonts = ctx->options->fonts;
  GVEC_Fonts * f = (GVEC_Fonts *) fonts;
  *found = false;
  *out = NULL;
  GFNT_FontQuery query;
  query_of(&query, weight, italic, width);
  for (size_t i = 0; i < family_count && !*found; i++) {
    GVEC_Result r = document_face(ctx, families[i], weight, italic, width, codepoint, out, found);
    if (r != GVEC_OK) {
      return r;
    }
    if (*found) {
      if (rank) {
        *rank = i;
      }
      break;
    }
    GCU_MUTEX_LOCK(f->lock);
    r = set_family_face(f, families[i], &query, codepoint, out, found);
    GCU_MUTEX_UNLOCK(f->lock);
    if (r != GVEC_OK) {
      return r;
    }
    if (*found && rank) {
      *rank = i;
    }
  }
  if (*found) {
    return GVEC_OK;
  }
  if (rank) {
    *rank = family_count; /* the fonts' own fallback comes after every family named */
  }
  return gvec_fonts_face(fonts, NULL, 0, weight, italic, width, codepoint, out, found);
}

GVEC_Result gvec_fonts_scene_face(const GVEC_Fonts * constant, const GFNT_Face * face, const GFNT_Variation * variation, GSHSCENE_Store * store, const GSHSCENE_Resource ** out) {
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
  GFNT_Result r = gfnt_scene_intern_face(scene, store, variation, out, NULL);
  if (variation && (r == GFNT_ERR_UNSUPPORTED || r == GFNT_ERR_INVALID)) {
    /* A face whose outlines do not move in its design space is drawn at its default, as it was shaped. */
    r = gfnt_scene_intern_face(scene, store, NULL, out, NULL);
  }
  if (r == GFNT_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  return r == GFNT_OK ? GVEC_OK : r == GFNT_ERR_LIMIT ? GVEC_ERR_LIMIT : GVEC_ERR_UNSUPPORTED;
}
