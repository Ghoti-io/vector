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
 * Path data (SVG 2 section 9.3): the commands as written and the absolute form.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include "../../core/core_internal.h"
#include "svg_grammar.h"

typedef struct buffers {
  const GVEC_Allocator * allocator;
  GVEC_Svg_Path_Command * commands;
  size_t command_count, command_capacity;
  GVEC_Svg_Path_Segment * segments;
  size_t segment_count, segment_capacity;
} buffers;

static void fail(gvec_svg_parse_error * err, size_t offset, const char * reason) {
  if (err) {
    err->offset = offset;
    err->reason = reason;
  }
}

static bool grow(buffers * b) {
  if (b->command_count == b->command_capacity) {
    size_t capacity = b->command_capacity ? b->command_capacity * 2 : 64, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof(GVEC_Svg_Path_Command), &bytes)) {
      return false;
    }
    void * fresh = gvec_realloc(b->allocator, b->commands, bytes);
    if (!fresh) {
      return false;
    }
    b->commands = fresh;
    b->command_capacity = capacity;
  }
  if (b->segment_count == b->segment_capacity) {
    size_t capacity = b->segment_capacity ? b->segment_capacity * 2 : 64, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof(GVEC_Svg_Path_Segment), &bytes)) {
      return false;
    }
    void * fresh = gvec_realloc(b->allocator, b->segments, bytes);
    if (!fresh) {
      return false;
    }
    b->segments = fresh;
    b->segment_capacity = capacity;
  }
  return true;
}

static int arg_count(char upper) {
  switch (upper) {
  case 'M': case 'L': case 'T': return 2;
  case 'H': case 'V': return 1;
  case 'C': return 6;
  case 'S': case 'Q': return 4;
  case 'A': return 7;
  case 'Z': return 0;
  default: return -1;
  }
}

static bool is_number_start(char c) {
  return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* A separator between two arguments: wsp* (comma wsp*)?. */
static void skip_arg_separator(const char * s, size_t n, size_t * pos) {
  while (*pos < n && gvec_svg_is_wsp((unsigned char) s[*pos])) {
    (*pos)++;
  }
  if (*pos < n && s[*pos] == ',') {
    (*pos)++;
    while (*pos < n && gvec_svg_is_wsp((unsigned char) s[*pos])) {
      (*pos)++;
    }
  }
}

static bool finite(double v) {
  return v == v && v - v == 0;
}

gvec_svg_parse_status gvec_svg_parse_path(const char * s, size_t n, const gvec_svg_parse_context * ctx,
    gvec_svg_path * out, gvec_svg_parse_error * err) {
  buffers b;
  memset(&b, 0, sizeof b);
  b.allocator = ctx->allocator;
  gvec_svg_parse_status status = GVEC_SVG_PARSE_OK;
  size_t pos = 0;
  double cx = 0, cy = 0, sx = 0, sy = 0;
  double lcx = 0, lcy = 0;   /* the last control point */
  char last_kind = 0;        /* 'C' (cubic), 'Q' (quad) or 0 */
  bool first = true;

  while (pos < n && gvec_svg_is_wsp((unsigned char) s[pos])) {
    pos++;
  }
  while (pos < n) {
    size_t command_at = pos;
    char letter = s[pos];
    char upper = (letter >= 'a' && letter <= 'z') ? (char) (letter - 32) : letter;
    int argc = arg_count(upper);
    if (argc < 0) {
      fail(err, pos, first ? "path data must begin with a moveto" : "expected a path command");
      status = GVEC_SVG_PARSE_INVALID;
      break;
    }
    if (first && upper != 'M') {
      fail(err, pos, "path data must begin with a moveto");
      status = GVEC_SVG_PARSE_INVALID;
      break;
    }
    first = false;
    bool relative = letter != upper;
    pos++;
    char current_letter = letter;
    bool implicit = false;
    for (;;) {
      if (b.segment_count >= ctx->max_path_segments) {
        fail(err, command_at, "the path has more segments than the limit");
        status = GVEC_SVG_PARSE_LIMIT;
        goto done;
      }
      if (!grow(&b)) {
        status = GVEC_SVG_PARSE_OOM;
        goto done;
      }
      double a[7] = {0, 0, 0, 0, 0, 0, 0};
      size_t group_at = pos;
      if (upper != 'Z') {
        while (pos < n && gvec_svg_is_wsp((unsigned char) s[pos])) {
          pos++;
        }
        group_at = pos;
        for (int i = 0; i < argc; i++) {
          if (i > 0) {
            skip_arg_separator(s, n, &pos);
          }
          if (upper == 'A' && (i == 3 || i == 4)) {
            if (pos < n && (s[pos] == '0' || s[pos] == '1')) {
              a[i] = s[pos] == '1' ? 1.0 : 0.0;
              pos++;
            } else {
              fail(err, pos, "an arc flag must be 0 or 1");
              status = b.segment_count ? GVEC_SVG_PARSE_PARTIAL : GVEC_SVG_PARSE_INVALID;
              goto done;
            }
          } else if (!gvec_svg_scan_number(s, n, &pos, &a[i], err)) {
            status = b.segment_count ? GVEC_SVG_PARSE_PARTIAL : GVEC_SVG_PARSE_INVALID;
            goto done;
          }
        }
      }
      /* Absolute form. */
      GVEC_Svg_Path_Segment seg;
      memset(&seg, 0, sizeof seg);
      seg.command = (uint32_t) b.command_count;
      seg.x0 = cx;
      seg.y0 = cy;
      double ox = relative ? cx : 0, oy = relative ? cy : 0;
      switch (upper) {
      case 'M':
        seg.kind = GVEC_SVG_SEGMENT_MOVE;
        seg.x = a[0] + ox;
        seg.y = a[1] + oy;
        sx = seg.x;
        sy = seg.y;
        last_kind = 0;
        break;
      case 'L':
        seg.kind = GVEC_SVG_SEGMENT_LINE;
        seg.x = a[0] + ox;
        seg.y = a[1] + oy;
        last_kind = 0;
        break;
      case 'H':
        seg.kind = GVEC_SVG_SEGMENT_LINE;
        seg.x = a[0] + ox;
        seg.y = cy;
        last_kind = 0;
        break;
      case 'V':
        seg.kind = GVEC_SVG_SEGMENT_LINE;
        seg.x = cx;
        seg.y = a[0] + oy;
        last_kind = 0;
        break;
      case 'C':
        seg.kind = GVEC_SVG_SEGMENT_CUBIC;
        seg.x1 = a[0] + ox;
        seg.y1 = a[1] + oy;
        seg.x2 = a[2] + ox;
        seg.y2 = a[3] + oy;
        seg.x = a[4] + ox;
        seg.y = a[5] + oy;
        lcx = seg.x2;
        lcy = seg.y2;
        last_kind = 'C';
        break;
      case 'S':
        seg.kind = GVEC_SVG_SEGMENT_CUBIC;
        seg.x1 = last_kind == 'C' ? 2 * cx - lcx : cx;
        seg.y1 = last_kind == 'C' ? 2 * cy - lcy : cy;
        seg.x2 = a[0] + ox;
        seg.y2 = a[1] + oy;
        seg.x = a[2] + ox;
        seg.y = a[3] + oy;
        lcx = seg.x2;
        lcy = seg.y2;
        last_kind = 'C';
        break;
      case 'Q':
        seg.kind = GVEC_SVG_SEGMENT_QUAD;
        seg.x1 = a[0] + ox;
        seg.y1 = a[1] + oy;
        seg.x = a[2] + ox;
        seg.y = a[3] + oy;
        lcx = seg.x1;
        lcy = seg.y1;
        last_kind = 'Q';
        break;
      case 'T':
        seg.kind = GVEC_SVG_SEGMENT_QUAD;
        seg.x1 = last_kind == 'Q' ? 2 * cx - lcx : cx;
        seg.y1 = last_kind == 'Q' ? 2 * cy - lcy : cy;
        seg.x = a[0] + ox;
        seg.y = a[1] + oy;
        lcx = seg.x1;
        lcy = seg.y1;
        last_kind = 'Q';
        break;
      case 'A':
        seg.kind = GVEC_SVG_SEGMENT_ARC;
        seg.rx = a[0];
        seg.ry = a[1];
        seg.rotation = a[2];
        seg.large_arc = a[3] != 0;
        seg.sweep = a[4] != 0;
        seg.x = a[5] + ox;
        seg.y = a[6] + oy;
        last_kind = 0;
        break;
      default: /* 'Z' */
        seg.kind = GVEC_SVG_SEGMENT_CLOSE;
        seg.x = sx;
        seg.y = sy;
        last_kind = 0;
        break;
      }
      if (!finite(seg.x) || !finite(seg.y) || !finite(seg.x1) || !finite(seg.y1) || !finite(seg.x2) || !finite(seg.y2)) {
        fail(err, group_at, "a coordinate is too large");
        status = b.segment_count ? GVEC_SVG_PARSE_PARTIAL : GVEC_SVG_PARSE_INVALID;
        goto done;
      }
      GVEC_Svg_Path_Command cmd;
      memset(&cmd, 0, sizeof cmd);
      cmd.letter = current_letter;
      cmd.implicit = implicit;
      cmd.arg_count = (uint8_t) argc;
      cmd.offset = implicit ? group_at : command_at;
      memcpy(cmd.args, a, sizeof a);
      b.commands[b.command_count++] = cmd;
      b.segments[b.segment_count++] = seg;
      cx = seg.x;
      cy = seg.y;
      /* What may follow: another command, an implicit repeat, or the end. */
      if (upper == 'Z') {
        break;
      }
      size_t save = pos;
      while (pos < n && gvec_svg_is_wsp((unsigned char) s[pos])) {
        pos++;
      }
      if (pos >= n) {
        break;
      }
      if (s[pos] == ',') {
        pos++;
        while (pos < n && gvec_svg_is_wsp((unsigned char) s[pos])) {
          pos++;
        }
        if (pos >= n || !is_number_start(s[pos])) {
          fail(err, save, "a comma must be followed by a number");
          status = GVEC_SVG_PARSE_PARTIAL;
          goto done;
        }
      } else if (!is_number_start(s[pos])) {
        break;
      }
      /* An implicit repeat; after a moveto it is a lineto. */
      implicit = true;
      if (upper == 'M') {
        upper = 'L';
        current_letter = relative ? 'l' : 'L';
        argc = 2;
      }
    }
    while (pos < n && gvec_svg_is_wsp((unsigned char) s[pos])) {
      pos++;
    }
  }

done:
  if (status == GVEC_SVG_PARSE_OOM) {
    gvec_free(b.allocator, b.commands);
    gvec_free(b.allocator, b.segments);
    return status;
  }
  if (status == GVEC_SVG_PARSE_INVALID && b.segment_count == 0) {
    gvec_free(b.allocator, b.commands);
    gvec_free(b.allocator, b.segments);
    out->commands = NULL;
    out->command_count = 0;
    out->segments = NULL;
    out->segment_count = 0;
    return status;
  }
  GVEC_Svg_Path_Command * commands = gvec_arena_array(ctx->arena, b.command_count ? b.command_count : 1,
      sizeof(GVEC_Svg_Path_Command), 8);
  GVEC_Svg_Path_Segment * segments = gvec_arena_array(ctx->arena, b.segment_count ? b.segment_count : 1,
      sizeof(GVEC_Svg_Path_Segment), 8);
  if (!commands || !segments) {
    gvec_free(b.allocator, b.commands);
    gvec_free(b.allocator, b.segments);
    return GVEC_SVG_PARSE_OOM;
  }
  if (b.command_count) {
    memcpy(commands, b.commands, b.command_count * sizeof(GVEC_Svg_Path_Command));
    memcpy(segments, b.segments, b.segment_count * sizeof(GVEC_Svg_Path_Segment));
  }
  gvec_free(b.allocator, b.commands);
  gvec_free(b.allocator, b.segments);
  out->commands = commands;
  out->command_count = b.command_count;
  out->segments = segments;
  out->segment_count = b.segment_count;
  return status == GVEC_SVG_PARSE_INVALID ? GVEC_SVG_PARSE_PARTIAL : status;
}
