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
 * Keys of the typed document's elements (AD-9).
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/string.h>

#include "svg_doc.h"

uint64_t gvec_svg_hash64(const void * data, size_t length) {
  uint8_t out[16];
  gcu_string_murmur3_x64_128(data, length, 0, out);
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) {
    v = (v << 8) | out[i];
  }
  return v;
}

static void put64(uint8_t * p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t) (v >> (8 * i));
  }
}

uint64_t gvec_svg_key_for(uint64_t parent_key, const char * id, GVEC_Svg_Kind kind, const char * ns, const char * name,
    uint32_t ordinal) {
  uint8_t buf[21];
  memset(buf, 0, sizeof buf);
  put64(buf, parent_key);
  if (id) {
    buf[8] = 1;
    put64(buf + 9, gvec_svg_hash64(id, strlen(id)));
  } else if (kind == GVEC_SVG_KIND_UNKNOWN || kind == GVEC_SVG_KIND_FOREIGN) {
    /* The name, with its namespace, hashed on its own. */
    size_t nl = ns ? strlen(ns) : 0, ll = strlen(name);
    uint64_t h = gvec_svg_hash64(ns ? ns : "", nl) * 31 + gvec_svg_hash64(name, ll);
    buf[8] = 2;
    put64(buf + 9, h ^ (uint64_t) kind);
  } else {
    buf[8] = 3;
    put64(buf + 9, (uint64_t) kind);
  }
  /* An element with an id is named by it; the others by their place among their siblings of the same kind. */
  if (!id) {
    for (int i = 0; i < 4; i++) {
      buf[17 + i] = (uint8_t) (ordinal >> (8 * i));
    }
  }
  return gvec_svg_hash64(buf, sizeof buf);
}

uint64_t gvec_svg_key_rehash(uint64_t key, uint32_t attempt) {
  uint8_t buf[13];
  memset(buf, 0, sizeof buf);
  put64(buf, key);
  buf[8] = 4;
  for (int i = 0; i < 4; i++) {
    buf[9 + i] = (uint8_t) (attempt >> (8 * i));
  }
  return gvec_svg_hash64(buf, sizeof buf);
}
