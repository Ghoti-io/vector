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
 * viewBox, preserveAspectRatio, orient, offsets, keywords, references, data: URIs
 * and language lists (svg_grammar.h).
 */

#include <ghoti.io/vector/macros.h>

#include <limits.h>
#include <string.h>

#include "svg_grammar.h"

static void fail(gvec_svg_parse_error * err, size_t offset, const char * reason) {
  if (err) {
    err->offset = offset;
    err->reason = reason;
  }
}

static size_t skip_wsp(const char * s, size_t n, size_t pos) {
  while (pos < n && gvec_svg_is_wsp((unsigned char) s[pos])) {
    pos++;
  }
  return pos;
}

/* The span with its white space trimmed. */
static void trim(const char * s, size_t n, size_t * begin, size_t * end) {
  size_t b = skip_wsp(s, n, 0), e = n;
  while (e > b && gvec_svg_is_wsp((unsigned char) s[e - 1])) {
    e--;
  }
  *begin = b;
  *end = e;
}

gvec_svg_parse_status gvec_svg_parse_view_box(const char * s, size_t n, GVEC_Svg_View_Box * out, gvec_svg_parse_error * err) {
  double v[4];
  size_t pos = skip_wsp(s, n, 0);
  for (int i = 0; i < 4; i++) {
    if (!gvec_svg_scan_number(s, n, &pos, &v[i], err)) {
      return GVEC_SVG_PARSE_INVALID;
    }
    if (i < 3) {
      size_t before = pos;
      pos = skip_wsp(s, n, pos);
      if (pos < n && s[pos] == ',') {
        pos = skip_wsp(s, n, pos + 1);
      } else if (pos == before) {
        fail(err, before, "expected a separator");
        return GVEC_SVG_PARSE_INVALID;
      }
    }
  }
  pos = skip_wsp(s, n, pos);
  if (pos != n) {
    fail(err, pos, "unexpected text after the viewBox");
    return GVEC_SVG_PARSE_INVALID;
  }
  if (v[2] < 0 || v[3] < 0) {
    fail(err, 0, "a viewBox may not have a negative width or height");
    return GVEC_SVG_PARSE_INVALID;
  }
  out->x = v[0];
  out->y = v[1];
  out->width = v[2];
  out->height = v[3];
  return GVEC_SVG_PARSE_OK;
}

static bool token_is(const char * s, size_t b, size_t e, const char * word) {
  return strlen(word) == e - b && memcmp(s + b, word, e - b) == 0;
}

gvec_svg_parse_status gvec_svg_parse_par(const char * s, size_t n, GVEC_Svg_Preserve_Aspect_Ratio * out, gvec_svg_parse_error * err) {
  static const char * const aligns[] = {"none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid",
      "xMinYMax", "xMidYMax", "xMaxYMax"};
  GVEC_Svg_Preserve_Aspect_Ratio value = {GVEC_SVG_ALIGN_X_MID_Y_MID, 0, 0};
  size_t pos = skip_wsp(s, n, 0);
  int stage = 0; /* 0: defer or align; 1: align; 2: meet or slice; 3: done */
  bool seen_align = false;
  while (pos < n) {
    size_t b = pos;
    while (pos < n && !gvec_svg_is_wsp((unsigned char) s[pos])) {
      pos++;
    }
    if (stage == 0 && token_is(s, b, pos, "defer")) {
      value.defer = 1;
      stage = 1;
    } else if (stage <= 1 && !seen_align) {
      bool found = false;
      for (size_t i = 0; i < sizeof aligns / sizeof aligns[0]; i++) {
        if (token_is(s, b, pos, aligns[i])) {
          value.align = (GVEC_Svg_Align) i;
          found = true;
          break;
        }
      }
      if (!found) {
        fail(err, b, "expected an alignment");
        return GVEC_SVG_PARSE_INVALID;
      }
      seen_align = true;
      stage = 2;
    } else if (stage == 2 && token_is(s, b, pos, "meet")) {
      value.slice = 0;
      stage = 3;
    } else if (stage == 2 && token_is(s, b, pos, "slice")) {
      value.slice = 1;
      stage = 3;
    } else {
      fail(err, b, "unexpected token");
      return GVEC_SVG_PARSE_INVALID;
    }
    pos = skip_wsp(s, n, pos);
  }
  if (!seen_align) {
    fail(err, n, "expected an alignment");
    return GVEC_SVG_PARSE_INVALID;
  }
  *out = value;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_orient(const char * s, size_t n, GVEC_Svg_Orient * out, gvec_svg_parse_error * err) {
  size_t b, e;
  trim(s, n, &b, &e);
  if (token_is(s, b, e, "auto")) {
    out->mode = GVEC_SVG_ORIENT_AUTO;
    out->angle = 0;
    return GVEC_SVG_PARSE_OK;
  }
  if (token_is(s, b, e, "auto-start-reverse")) {
    out->mode = GVEC_SVG_ORIENT_AUTO_START_REVERSE;
    out->angle = 0;
    return GVEC_SVG_PARSE_OK;
  }
  GVEC_Svg_Length angle;
  if (gvec_svg_parse_angle(s, n, &angle, err) != GVEC_SVG_PARSE_OK) {
    return GVEC_SVG_PARSE_INVALID;
  }
  out->mode = GVEC_SVG_ORIENT_ANGLE;
  out->angle = gvec_svg_angle_degrees(&angle);
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_offset(const char * s, size_t n, double * out, gvec_svg_parse_error * err) {
  size_t pos = skip_wsp(s, n, 0);
  double v;
  if (!gvec_svg_scan_number(s, n, &pos, &v, err)) {
    return GVEC_SVG_PARSE_INVALID;
  }
  if (pos < n && s[pos] == '%') {
    v /= 100.0;
    pos++;
  }
  pos = skip_wsp(s, n, pos);
  if (pos != n) {
    fail(err, pos, "unexpected text after the offset");
    return GVEC_SVG_PARSE_INVALID;
  }
  *out = v;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_integer(const char * s, size_t n, long * out, gvec_svg_parse_error * err) {
  size_t pos = skip_wsp(s, n, 0);
  bool negative = false;
  if (pos < n && (s[pos] == '+' || s[pos] == '-')) {
    negative = s[pos] == '-';
    pos++;
  }
  size_t digits = pos;
  unsigned long value = 0;
  while (pos < n && s[pos] >= '0' && s[pos] <= '9') {
    unsigned d = (unsigned) (s[pos] - '0');
    if (value > (unsigned long) (LONG_MAX - (long) d) / 10) {
      fail(err, digits, "the integer is too large");
      return GVEC_SVG_PARSE_INVALID;
    }
    value = value * 10 + d;
    pos++;
  }
  if (pos == digits) {
    fail(err, pos, "expected an integer");
    return GVEC_SVG_PARSE_INVALID;
  }
  pos = skip_wsp(s, n, pos);
  if (pos != n) {
    fail(err, pos, "unexpected text after the integer");
    return GVEC_SVG_PARSE_INVALID;
  }
  *out = negative ? -(long) value : (long) value;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_keyword(const char * s, size_t n, const char * const * keywords, size_t count,
    size_t * out, gvec_svg_parse_error * err) {
  size_t b, e;
  trim(s, n, &b, &e);
  for (size_t i = 0; i < count; i++) {
    if (token_is(s, b, e, keywords[i])) {
      *out = i;
      return GVEC_SVG_PARSE_OK;
    }
  }
  fail(err, b, "not one of the allowed values");
  return GVEC_SVG_PARSE_INVALID;
}

gvec_svg_parse_status gvec_svg_parse_ref_coord(const char * s, size_t n, bool horizontal, GVEC_Svg_Length * out,
    gvec_svg_parse_error * err) {
  size_t b, e;
  trim(s, n, &b, &e);
  const char * low = horizontal ? "left" : "top";
  const char * high = horizontal ? "right" : "bottom";
  if (token_is(s, b, e, low) || token_is(s, b, e, "center") || token_is(s, b, e, high)) {
    out->value = token_is(s, b, e, low) ? 0.0 : token_is(s, b, e, "center") ? 50.0 : 100.0;
    out->unit = GVEC_SVG_UNIT_PERCENT;
    return GVEC_SVG_PARSE_OK;
  }
  return gvec_svg_parse_length(s, n, out, err);
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static bool starts_with_nocase(const char * s, size_t n, const char * prefix) {
  size_t len = strlen(prefix);
  if (n < len) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char) (c + 32);
    }
    if (c != prefix[i]) {
      return false;
    }
  }
  return true;
}

static gvec_svg_parse_status parse_data(const char * s, size_t n, const gvec_svg_parse_context * ctx, GVEC_Svg_Data_Uri * out,
    gvec_svg_parse_error * err) {
  const char * body = s + 5;
  size_t length = n - 5, comma = 0;
  while (comma < length && body[comma] != ',') {
    comma++;
  }
  if (comma == length) {
    fail(err, n, "a data: URI needs a comma before its data");
    return GVEC_SVG_PARSE_INVALID;
  }
  size_t head = comma, base64 = 0;
  /* ";base64" ends the header. */
  if (head >= 7) {
    bool match = body[head - 7] == ';';
    static const char word[] = "base64";
    for (size_t i = 0; match && i < 6; i++) {
      char c = body[head - 6 + i];
      if (c >= 'A' && c <= 'Z') {
        c = (char) (c + 32);
      }
      match = c == word[i];
    }
    if (match) {
      base64 = 1;
      head -= 7;
    }
  }
  size_t type_end = 0;
  while (type_end < head && body[type_end] != ';') {
    type_end++;
  }
  size_t slash = 0;
  while (slash < type_end && body[slash] != '/') {
    slash++;
  }
  const char * media = "";
  size_t media_len = 0, params_at = 0;
  if (slash < type_end) {
    media = body;
    media_len = type_end;
    params_at = type_end;
  }
  char * lowered = gvec_arena_strndup(ctx->arena, media, media_len);
  if (!lowered) {
    return GVEC_SVG_PARSE_OOM;
  }
  for (size_t i = 0; i < media_len; i++) {
    if (lowered[i] >= 'A' && lowered[i] <= 'Z') {
      lowered[i] = (char) (lowered[i] + 32);
    }
  }
  size_t params_from = media_len ? params_at : 0;
  const char * params = body + params_from;
  size_t params_len = head - params_from;
  /* The parameters keep their leading semicolon out. */
  if (params_len && params[0] == ';') {
    params++;
    params_len--;
  }
  char * parameters = gvec_arena_strndup(ctx->arena, params, params_len);
  char * payload = gvec_arena_strndup(ctx->arena, body + comma + 1, length - comma - 1);
  if (!parameters || !payload) {
    return GVEC_SVG_PARSE_OOM;
  }
  out->media_type = lowered;
  out->parameters = parameters;
  out->base64 = (uint8_t) base64;
  out->payload = payload;
  out->payload_length = length - comma - 1;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_reference(const char * s, size_t n, const gvec_svg_parse_context * ctx,
    GVEC_Svg_Reference * out, gvec_svg_parse_error * err) {
  size_t b, e;
  trim(s, n, &b, &e);
  memset(out, 0, sizeof *out);
  const char * v = s + b;
  size_t len = e - b;
  if (len == 0) {
    out->kind = GVEC_SVG_REF_EMPTY;
    return GVEC_SVG_PARSE_OK;
  }
  if (v[0] == '#') {
    /* A fragment: percent-decoded, so that an id spelled with an escape matches. */
    char * id = gvec_arena_alloc(ctx->arena, len, 1);
    if (!id) {
      return GVEC_SVG_PARSE_OOM;
    }
    size_t o = 0;
    for (size_t i = 1; i < len; i++) {
      if (v[i] == '%' && i + 2 < len && hex_value(v[i + 1]) >= 0 && hex_value(v[i + 2]) >= 0) {
        id[o++] = (char) (hex_value(v[i + 1]) * 16 + hex_value(v[i + 2]));
        i += 2;
      } else {
        id[o++] = v[i];
      }
    }
    id[o] = 0;
    out->kind = GVEC_SVG_REF_INTERNAL;
    out->fragment = id;
    return GVEC_SVG_PARSE_OK;
  }
  if (starts_with_nocase(v, len, "data:")) {
    gvec_svg_parse_status st = parse_data(v, len, ctx, &out->data, err);
    if (st != GVEC_SVG_PARSE_OK) {
      return st;
    }
    out->kind = GVEC_SVG_REF_DATA;
    return GVEC_SVG_PARSE_OK;
  }
  char * url = gvec_arena_strndup(ctx->arena, v, len);
  if (!url) {
    return GVEC_SVG_PARSE_OOM;
  }
  out->kind = GVEC_SVG_REF_EXTERNAL;
  out->url = url;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_language_list(const char * s, size_t n, const gvec_svg_parse_context * ctx,
    const char *** out_tags, size_t * out_count, gvec_svg_parse_error * err) {
  (void) err;
  size_t count = 0;
  for (size_t pass = 0; pass < 2; pass++) {
    const char ** tags = NULL;
    if (pass == 1) {
      tags = gvec_arena_array(ctx->arena, count ? count : 1, sizeof(char *), 8);
      if (!tags) {
        return GVEC_SVG_PARSE_OOM;
      }
    }
    size_t index = 0, pos = 0;
    while (pos <= n) {
      size_t start = pos;
      while (pos < n && s[pos] != ',') {
        pos++;
      }
      size_t b = start, e = pos;
      while (b < e && gvec_svg_is_wsp((unsigned char) s[b])) {
        b++;
      }
      while (e > b && gvec_svg_is_wsp((unsigned char) s[e - 1])) {
        e--;
      }
      if (e > b) {
        if (pass == 1) {
          char * tag = gvec_arena_strndup(ctx->arena, s + b, e - b);
          if (!tag) {
            return GVEC_SVG_PARSE_OOM;
          }
          tags[index] = tag;
        }
        index++;
      }
      pos++;
    }
    if (pass == 0) {
      count = index;
    } else {
      *out_tags = tags;
    }
  }
  *out_count = count;
  return GVEC_SVG_PARSE_OK;
}

gvec_svg_parse_status gvec_svg_parse_extension_list(const char * s, size_t n, size_t * out_count) {
  size_t count = 0, pos = skip_wsp(s, n, 0);
  while (pos < n) {
    while (pos < n && !gvec_svg_is_wsp((unsigned char) s[pos])) {
      pos++;
    }
    count++;
    pos = skip_wsp(s, n, pos);
  }
  *out_count = count;
  return GVEC_SVG_PARSE_OK;
}
