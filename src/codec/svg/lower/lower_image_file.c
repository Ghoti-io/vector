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
 * @file lower_image_file.c
 *
 * The resource policy for the image files an `image` or an `feImage` may name (R2 of the Images section of design.md): which reference is a path
 * inside the directory the caller allows, and the bytes of the file at it.
 *
 * - A reference with a scheme (`http:`, `https:`, `file:`, a Windows drive), a network-path reference (`//host/path`) and an
 *   absolute path are refused. Nothing is fetched from a network, ever.
 * - A relative reference is resolved against the base directory, its `.` and `..` segments applied, its percent-escapes decoded
 *   first (so `%2e%2e` is a `..`); the result must stay inside the root directory, as a list of path components, not as a string
 *   prefix (`/root-other` is not inside `/root`).
 * - Where the platform can say (POSIX), the file and the root are then resolved through symbolic links and the file must still lie
 *   inside the root, and must be a regular file (a device or a pipe is not read). The file that is opened is the resolved one.
 * - At most `max_bytes` are read; a file longer than that is refused before it is all read.
 */

#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#endif

#include <ghoti.io/vector/macros.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "lower_internal.h"

/* One path as components: `abs` for a path that begins with a separator; leading `..` components are kept for a relative path. */
typedef struct components {
  char ** part;
  size_t count;
  size_t capacity;
  bool abs;
} components;

static void components_free(const GVEC_Allocator * a, components * c) {
  for (size_t i = 0; i < c->count; i++) {
    gvec_free(a, c->part[i]);
  }
  gvec_free(a, c->part);
  memset(c, 0, sizeof *c);
}

static GVEC_Result components_push(const GVEC_Allocator * a, components * c, const char * s, size_t n) {
  if (c->count == c->capacity) {
    size_t capacity = c->capacity ? c->capacity * 2 : 8;
    char ** fresh = gvec_realloc(a, c->part, capacity * sizeof *fresh);
    if (!fresh) {
      return GVEC_ERR_OOM;
    }
    c->part = fresh;
    c->capacity = capacity;
  }
  char * copy = gvec_realloc(a, NULL, n + 1);
  if (!copy) {
    return GVEC_ERR_OOM;
  }
  memcpy(copy, s, n);
  copy[n] = 0;
  c->part[c->count++] = copy;
  return GVEC_OK;
}

/* Apply `text`'s segments to `c`: `.` and empty ones are dropped, `..` removes the last component (and is kept, in a relative path with
 * nothing to remove, as a leading one). `*escaped` is set when a `..` would go above the root of an absolute path. */
static GVEC_Result components_apply(const GVEC_Allocator * a, components * c, const char * text, size_t length, bool * escaped) {
  size_t i = 0;
  while (i <= length) {
    size_t j = i;
    while (j < length && text[j] != '/') {
      j++;
    }
    size_t n = j - i;
    if (n == 0 || (n == 1 && text[i] == '.')) {
      /* dropped */
    }
    else if (n == 2 && text[i] == '.' && text[i + 1] == '.') {
      if (c->count > 0 && !(c->count > 0 && strcmp(c->part[c->count - 1], "..") == 0)) {
        gvec_free(a, c->part[--c->count]);
      }
      else if (c->abs) {
        *escaped = true;
      }
      else {
        GVEC_Result r = components_push(a, c, "..", 2);
        if (r != GVEC_OK) {
          return r;
        }
      }
    }
    else {
      GVEC_Result r = components_push(a, c, text + i, n);
      if (r != GVEC_OK) {
        return r;
      }
    }
    i = j + 1;
  }
  return GVEC_OK;
}

static GVEC_Result components_of(const GVEC_Allocator * a, const char * path, components * c, bool * escaped) {
  memset(c, 0, sizeof *c);
  size_t n = strlen(path);
  c->abs = n > 0 && (path[0] == '/' || path[0] == '\\');
  GVEC_Result r;
#if defined(_WIN32)
  /* A Windows path may be written with either separator. */
  char * copy = gvec_realloc(a, NULL, n + 1);
  if (!copy) {
    return GVEC_ERR_OOM;
  }
  for (size_t i = 0; i <= n; i++) {
    copy[i] = path[i] == '\\' ? '/' : path[i];
  }
  r = components_apply(a, c, copy, n, escaped);
  gvec_free(a, copy);
#else
  r = components_apply(a, c, path, n, escaped);
#endif
  if (r != GVEC_OK) {
    components_free(a, c);
  }
  return r;
}

/* Whether `root` is a prefix of `path`, component by component. */
static bool components_inside(const components * root, const components * path) {
  if (root->abs != path->abs || root->count > path->count) {
    return false;
  }
  for (size_t i = 0; i < root->count; i++) {
    if (strcmp(root->part[i], path->part[i]) != 0) {
      return false;
    }
  }
  /* A root that is kept as `..` components names a directory by what it is above the working directory: the path must not go further up. */
  return true;
}

/* The path as one string; "." for a relative path with nothing in it. */
static GVEC_Result components_join(const GVEC_Allocator * a, const components * c, char ** out) {
  size_t total = 2;
  for (size_t i = 0; i < c->count; i++) {
    total += strlen(c->part[i]) + 1;
  }
  char * s = gvec_realloc(a, NULL, total);
  if (!s) {
    return GVEC_ERR_OOM;
  }
  size_t o = 0;
  if (c->abs) {
    s[o++] = '/';
  }
  for (size_t i = 0; i < c->count; i++) {
    size_t n = strlen(c->part[i]);
    memcpy(s + o, c->part[i], n);
    o += n;
    if (i + 1 < c->count) {
      s[o++] = '/';
    }
  }
  if (o == 0) {
    s[o++] = '.';
  }
  s[o] = 0;
  *out = s;
  return GVEC_OK;
}

static int hex_digit(char c) {
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

/* The reference with a scheme: a letter, then letters, digits, `+`, `-`, `.`, and a colon. */
static bool has_scheme(const char * s, size_t n) {
  if (n == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
    return false;
  }
  for (size_t i = 1; i < n; i++) {
    char c = s[i];
    if (c == ':') {
      return true;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
      return false;
    }
  }
  return false;
}

GVEC_Result gvec_lw_image_resolve(const GVEC_Allocator * allocator, const char * root, const char * base, const char * reference,
    gvec_lw_image_path_status * status, char ** out_path) {
  *out_path = NULL;
  *status = GVEC_LW_IMAGE_PATH_NO_ROOT;
  if (!root || !root[0]) {
    return GVEC_OK;
  }
  size_t n = strlen(reference);
  if (has_scheme(reference, n) || (n >= 2 && reference[0] == '/' && reference[1] == '/')) {
    *status = GVEC_LW_IMAGE_PATH_NETWORK;
    return GVEC_OK;
  }
  if (n > 0 && reference[0] == '/') {
    *status = GVEC_LW_IMAGE_PATH_ABSOLUTE;
    return GVEC_OK;
  }
  /* The path of the URL ends at the query or the fragment. */
  size_t end = 0;
  while (end < n && reference[end] != '?' && reference[end] != '#') {
    end++;
  }
  char * decoded = gvec_realloc(allocator, NULL, end + 1);
  if (!decoded) {
    return GVEC_ERR_OOM;
  }
  size_t d = 0;
  bool bad = false;
  for (size_t i = 0; i < end; i++) {
    unsigned char c = (unsigned char) reference[i];
    if (c == '%') {
      if (i + 2 >= end) {
        bad = true;
        break;
      }
      int hi = hex_digit(reference[i + 1]), lo = hex_digit(reference[i + 2]);
      if (hi < 0 || lo < 0) {
        bad = true;
        break;
      }
      c = (unsigned char) (hi * 16 + lo);
      i += 2;
    }
    /* A NUL ends a C string early, and a control character or a backslash is no part of a name this policy follows. */
    if (c == 0 || c < 0x20 || c == 0x7f || c == '\\') {
      bad = true;
      break;
    }
    decoded[d++] = (char) c;
  }
  decoded[d] = 0;
  if (bad || d == 0) {
    gvec_free(allocator, decoded);
    *status = bad ? GVEC_LW_IMAGE_PATH_INVALID : GVEC_LW_IMAGE_PATH_EMPTY;
    return GVEC_OK;
  }
  /* A percent-decoded leading slash is an absolute path too, and a decoded scheme is no better than a written one. */
  if (decoded[0] == '/') {
    gvec_free(allocator, decoded);
    *status = GVEC_LW_IMAGE_PATH_ABSOLUTE;
    return GVEC_OK;
  }
  components root_c, path_c;
  bool root_escaped = false, path_escaped = false;
  GVEC_Result r = components_of(allocator, root, &root_c, &root_escaped);
  if (r != GVEC_OK) {
    gvec_free(allocator, decoded);
    return r;
  }
  r = components_of(allocator, base && base[0] ? base : root, &path_c, &path_escaped);
  if (r != GVEC_OK) {
    components_free(allocator, &root_c);
    gvec_free(allocator, decoded);
    return r;
  }
  r = components_apply(allocator, &path_c, decoded, d, &path_escaped);
  gvec_free(allocator, decoded);
  if (r != GVEC_OK) {
    components_free(allocator, &root_c);
    components_free(allocator, &path_c);
    return r;
  }
  if (root_escaped || path_escaped || !components_inside(&root_c, &path_c) || path_c.count == root_c.count) {
    *status = GVEC_LW_IMAGE_PATH_OUTSIDE;
  }
  else {
    r = components_join(allocator, &path_c, out_path);
    *status = GVEC_LW_IMAGE_PATH_OK;
  }
  components_free(allocator, &root_c);
  components_free(allocator, &path_c);
  return r;
}

#if !defined(_WIN32)
/* `path` resolved through symbolic links; NULL when it cannot be (it does not exist). The result is malloc'd by the C library. */
static char * real_path(const char * path) {
  return realpath(path, NULL);
}

/* Whether the resolved `file` lies inside the resolved `root`, as components. */
static bool resolved_inside(const char * root, const char * file) {
  size_t n = strlen(root);
  while (n > 1 && root[n - 1] == '/') {
    n--;
  }
  if (strncmp(file, root, n) != 0) {
    return false;
  }
  if (n == 1) {
    return true; /* the root is `/` */
  }
  return file[n] == '/';
}
#endif

GVEC_Result gvec_lw_image_read_file(const GVEC_Allocator * allocator, const char * root, const char * path, size_t max_bytes,
    gvec_lw_image_file_status * status, void ** out, size_t * size) {
  *out = NULL;
  *size = 0;
  *status = GVEC_LW_IMAGE_FILE_UNREADABLE;
  const char * open_path = path;
#if defined(_WIN32)
  (void) root; /* only the lexical check applies on Windows */
#else
  char * resolved_file = real_path(path);
  char * resolved_root = real_path(root);
  if (!resolved_file || !resolved_root) {
    free(resolved_file);
    free(resolved_root);
    return GVEC_OK; /* does not exist: nothing is drawn */
  }
  bool inside = resolved_inside(resolved_root, resolved_file);
  free(resolved_root);
  if (!inside) {
    free(resolved_file);
    *status = GVEC_LW_IMAGE_FILE_OUTSIDE;
    return GVEC_OK;
  }
  struct stat st;
  if (stat(resolved_file, &st) != 0 || !S_ISREG(st.st_mode)) {
    free(resolved_file);
    return GVEC_OK;
  }
  open_path = resolved_file;
#endif
  FILE * f = fopen(open_path, "rb");
#if !defined(_WIN32)
  free(resolved_file);
#endif
  if (!f) {
    return GVEC_OK;
  }
  /* Read in blocks, up to one byte past the cap: a file longer than the cap is refused without being read whole. */
  size_t capacity = 4096, used = 0;
  unsigned char * buffer = gvec_realloc(allocator, NULL, capacity);
  if (!buffer) {
    fclose(f);
    return GVEC_ERR_OOM;
  }
  for (;;) {
    if (used == capacity) {
      if (capacity > max_bytes) {
        break;
      }
      size_t next = capacity > SIZE_MAX / 2 ? SIZE_MAX : capacity * 2;
      if (next > max_bytes + 1) {
        next = max_bytes + 1;
      }
      if (next <= capacity) {
        break;
      }
      unsigned char * fresh = gvec_realloc(allocator, buffer, next);
      if (!fresh) {
        gvec_free(allocator, buffer);
        fclose(f);
        return GVEC_ERR_OOM;
      }
      buffer = fresh;
      capacity = next;
    }
    size_t got = fread(buffer + used, 1, capacity - used, f);
    used += got;
    if (got == 0) {
      break;
    }
  }
  bool failed = ferror(f) != 0;
  fclose(f);
  if (failed) {
    gvec_free(allocator, buffer);
    return GVEC_OK;
  }
  if (used > max_bytes) {
    gvec_free(allocator, buffer);
    *status = GVEC_LW_IMAGE_FILE_TOO_BIG;
    return GVEC_OK;
  }
  *status = GVEC_LW_IMAGE_FILE_OK;
  *out = buffer;
  *size = used;
  return GVEC_OK;
}
