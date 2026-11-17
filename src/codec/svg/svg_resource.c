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
 * @file svg_resource.c
 *
 * The resolver's state, the resolution of a reference against a base and the call that asks the resolver (svg_resource.h).
 *
 * What the library does with a resolver, and nothing else: it passes strings it built from the document, it reads `size` bytes at the
 * address the reply gives once, copies them, and keeps the copy. It never opens the url, never follows one answer into another
 * request that the document did not make, and asks each (kind, url) once.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include <ghoti.io/vector/core.h>

#include "svg_resource.h"

//-----------------------------------------------------------------------------
// The state
//-----------------------------------------------------------------------------

static size_t clamp(size_t value, size_t fallback, size_t ceiling) {
  if (value == 0) {
    return fallback;
  }
  return value > ceiling ? ceiling : value;
}

void gvec_res_init(gvec_res_state * state, const GVEC_Allocator * allocator, const GVEC_Lower_Options * options) {
  memset(state, 0, sizeof *state);
  state->allocator = allocator;
  state->max_bytes = clamp(options ? options->max_resource_bytes : 0, GVEC_RES_DEFAULT_BYTES, GVEC_RES_CEILING_BYTES);
  state->max_count = clamp(options ? options->max_resources : 0, GVEC_RES_DEFAULT_COUNT, GVEC_RES_CEILING_COUNT);
  state->max_depth = clamp(options ? options->max_resource_depth : 0, GVEC_RES_DEFAULT_DEPTH, GVEC_RES_CEILING_DEPTH);
  state->max_instances = clamp(options ? options->max_resource_instances : 0, GVEC_RES_DEFAULT_INSTANCES, GVEC_RES_CEILING_INSTANCES);
  if (options) {
    state->fn = options->resolver;
    state->user = options->resolver_user;
  }
}

bool gvec_res_enabled(const gvec_res_state * state) {
  return state && state->fn;
}

void gvec_res_free(gvec_res_state * state) {
  for (size_t i = 0; i < state->count; i++) {
    gvec_res_entry * e = state->entries[i];
    if (e->document) {
      gvec_doc_free((GVEC_Doc *) e->document);
    }
    gvec_free(state->allocator, e->url);
    gvec_free(state->allocator, e->bytes);
    gvec_free(state->allocator, e->media_type);
    gvec_free(state->allocator, e);
  }
  gvec_free(state->allocator, state->entries);
  state->entries = NULL;
  state->count = state->capacity = 0;
}

//-----------------------------------------------------------------------------
// RFC 3986, section 5.2: a reference against a base
//-----------------------------------------------------------------------------

typedef struct uri_parts {
  const char * scheme;  size_t scheme_length;   /* length 0: none */
  const char * authority; size_t authority_length; bool has_authority;
  const char * path; size_t path_length;
  const char * query; size_t query_length; bool has_query;
} uri_parts;

static bool alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Split `s` (of `n` bytes, with no fragment) into its parts. */
static void split(const char * s, size_t n, uri_parts * p) {
  memset(p, 0, sizeof *p);
  size_t i = 0;
  if (n > 0 && alpha(s[0])) {
    size_t j = 1;
    while (j < n && (alpha(s[j]) || (s[j] >= '0' && s[j] <= '9') || s[j] == '+' || s[j] == '-' || s[j] == '.')) {
      j++;
    }
    if (j < n && s[j] == ':') {
      p->scheme = s;
      p->scheme_length = j;
      i = j + 1;
    }
  }
  if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
    size_t j = i + 2;
    while (j < n && s[j] != '/' && s[j] != '?') {
      j++;
    }
    p->has_authority = true;
    p->authority = s + i + 2;
    p->authority_length = j - (i + 2);
    i = j;
  }
  size_t j = i;
  while (j < n && s[j] != '?') {
    j++;
  }
  p->path = s + i;
  p->path_length = j - i;
  if (j < n) {
    p->has_query = true;
    p->query = s + j + 1;
    p->query_length = n - j - 1;
  }
}

/* A growing string. */
typedef struct builder {
  const GVEC_Allocator * allocator;
  char * data;
  size_t length, capacity;
  bool failed;
} builder;

static void put(builder * b, const char * s, size_t n) {
  if (b->failed || n == 0) {
    return;
  }
  if (b->length + n + 1 > b->capacity) {
    size_t capacity = b->capacity ? b->capacity : 64;
    while (capacity < b->length + n + 1) {
      if (capacity > ((size_t) -1) / 2) {
        b->failed = true;
        return;
      }
      capacity *= 2;
    }
    char * fresh = gvec_realloc(b->allocator, b->data, capacity);
    if (!fresh) {
      b->failed = true;
      return;
    }
    b->data = fresh;
    b->capacity = capacity;
  }
  memcpy(b->data + b->length, s, n);
  b->length += n;
  b->data[b->length] = 0;
}

/*
 * remove_dot_segments over the concatenation of `a` and `b` (the base's directory and the reference's path, or one path alone), appended to `out`.
 * `rooted`: the path begins with `/` (or the URI has an authority): a `..` with nothing above it is dropped, as the RFC says. Otherwise (a
 * relative path with nothing to anchor it) it is kept, so that the result names what the reference named and not something else.
 */
static void remove_dots(builder * out, const char * a, size_t an, const char * b, size_t bn, bool rooted) {
  /* The segments as (offset, length) pairs in the virtual concatenation; the work is bounded by the length of the input. */
  size_t total = an + bn;
  char * flat = gvec_realloc(out->allocator, NULL, total + 1);
  if (!flat) {
    out->failed = true;
    return;
  }
  memcpy(flat, a, an);
  memcpy(flat + an, b, bn);
  size_t * start = gvec_realloc(out->allocator, NULL, (total + 2) * sizeof *start * 2);
  if (!start) {
    gvec_free(out->allocator, flat);
    out->failed = true;
    return;
  }
  size_t * len = start + total + 2;
  size_t count = 0;
  size_t i = 0;
  bool trailing = false; /* the result ends with a directory: a slash */
  if (total > 0 && flat[0] == '/') {
    i = 1;
  }
  while (i <= total) {
    size_t j = i;
    while (j < total && flat[j] != '/') {
      j++;
    }
    size_t n = j - i;
    bool last = j == total;
    if (n == 1 && flat[i] == '.') {
      trailing = last;
    }
    else if (n == 2 && flat[i] == '.' && flat[i + 1] == '.') {
      if (count > 0 && !(len[count - 1] == 2 && flat[start[count - 1]] == '.' && flat[start[count - 1] + 1] == '.' && !rooted)) {
        count--;
        trailing = last;
      }
      else if (!rooted) {
        start[count] = i;
        len[count] = 2;
        count++;
        trailing = last;
      }
      else {
        trailing = last;
      }
    }
    else if (n == 0) {
      /* an empty segment ("//" inside a path, or a final slash) is kept as a segment unless it is the last */
      if (last) {
        trailing = true;
      }
      else {
        start[count] = i;
        len[count] = 0;
        count++;
        trailing = false;
      }
    }
    else {
      start[count] = i;
      len[count] = n;
      count++;
      trailing = false;
    }
    i = j + 1;
  }
  if (total > 0 && flat[0] == '/') {
    put(out, "/", 1);
  }
  for (size_t k = 0; k < count; k++) {
    put(out, flat + start[k], len[k]);
    if (k + 1 < count || trailing) {
      put(out, "/", 1);
    }
  }
  gvec_free(out->allocator, start);
  gvec_free(out->allocator, flat);
}

GVEC_Result gvec_res_resolve(const GVEC_Allocator * allocator, const char * base, const char * reference, char ** out) {
  *out = NULL;
  size_t rn = 0;
  while (reference[rn] && reference[rn] != '#') {
    rn++;
  }
  size_t bn = 0;
  if (base) {
    while (base[bn] && base[bn] != '#') {
      bn++;
    }
  }
  uri_parts r, b;
  split(reference, rn, &r);
  split(base ? base : "", bn, &b);
  builder t = {allocator, NULL, 0, 0, false};
  const char * scheme = NULL;
  size_t scheme_n = 0;
  const char * auth = NULL;
  size_t auth_n = 0;
  bool has_auth = false;
  const char * query = NULL;
  size_t query_n = 0;
  bool has_query = false;
  /* The path: either `r.path` alone or the merge of the base's directory and `r.path`. */
  const char * pa = "";
  size_t pan = 0;
  const char * pb = "";
  size_t pbn = 0;
  if (r.scheme_length) {
    scheme = r.scheme;
    scheme_n = r.scheme_length;
    auth = r.authority;
    auth_n = r.authority_length;
    has_auth = r.has_authority;
    pb = r.path;
    pbn = r.path_length;
    query = r.query;
    query_n = r.query_length;
    has_query = r.has_query;
  }
  else {
    scheme = b.scheme;
    scheme_n = b.scheme_length;
    if (r.has_authority) {
      auth = r.authority;
      auth_n = r.authority_length;
      has_auth = true;
      pb = r.path;
      pbn = r.path_length;
      query = r.query;
      query_n = r.query_length;
      has_query = r.has_query;
    }
    else {
      auth = b.authority;
      auth_n = b.authority_length;
      has_auth = b.has_authority;
      if (r.path_length == 0) {
        pb = b.path;
        pbn = b.path_length;
        if (r.has_query) {
          query = r.query;
          query_n = r.query_length;
          has_query = true;
        }
        else {
          query = b.query;
          query_n = b.query_length;
          has_query = b.has_query;
        }
      }
      else {
        if (r.path[0] == '/') {
          pb = r.path;
          pbn = r.path_length;
        }
        else {
          /* merge: the base's path up to and including its last slash (or "/" for an authority with an empty path) */
          if (b.has_authority && b.path_length == 0) {
            pa = "/";
            pan = 1;
          }
          else {
            size_t k = b.path_length;
            while (k > 0 && b.path[k - 1] != '/') {
              k--;
            }
            pa = b.path;
            pan = k;
          }
          pb = r.path;
          pbn = r.path_length;
        }
        query = r.query;
        query_n = r.query_length;
        has_query = r.has_query;
      }
    }
  }
  if (scheme_n) {
    put(&t, scheme, scheme_n);
    put(&t, ":", 1);
  }
  if (has_auth) {
    put(&t, "//", 2);
    put(&t, auth, auth_n);
  }
  bool rooted = has_auth || (pan > 0 ? pa[0] == '/' : (pbn > 0 && pb[0] == '/'));
  remove_dots(&t, pa, pan, pb, pbn, rooted);
  if (has_query) {
    put(&t, "?", 1);
    put(&t, query, query_n);
  }
  if (t.failed) {
    gvec_free(allocator, t.data);
    return GVEC_ERR_OOM;
  }
  if (!t.data) {
    t.data = gvec_realloc(allocator, NULL, 1);
    if (!t.data) {
      return GVEC_ERR_OOM;
    }
    t.data[0] = 0;
  }
  *out = t.data;
  return GVEC_OK;
}

const char * gvec_res_fragment(const char * reference, size_t * before) {
  size_t n = 0;
  while (reference[n] && reference[n] != '#') {
    n++;
  }
  if (before) {
    *before = n;
  }
  return reference[n] == '#' ? reference + n + 1 : NULL;
}

//-----------------------------------------------------------------------------
// Asking
//-----------------------------------------------------------------------------

static gvec_res_entry * find(const gvec_res_state * state, GVEC_Resource_Kind kind, const char * url) {
  for (size_t i = 0; i < state->count; i++) {
    if (state->entries[i]->kind == kind && strcmp(state->entries[i]->url, url) == 0) {
      return state->entries[i];
    }
  }
  return NULL;
}

static GVEC_Result add(gvec_res_state * state, gvec_res_entry * entry) {
  if (state->count == state->capacity) {
    size_t capacity = state->capacity ? state->capacity * 2 : 8, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof *state->entries, &bytes)) {
      return GVEC_ERR_OOM;
    }
    void * fresh = gvec_realloc(state->allocator, state->entries, bytes);
    if (!fresh) {
      return GVEC_ERR_OOM;
    }
    state->entries = fresh;
    state->capacity = capacity;
  }
  state->entries[state->count++] = entry;
  return GVEC_OK;
}

static void entry_free(const GVEC_Allocator * a, gvec_res_entry * e) {
  gvec_free(a, e->url);
  gvec_free(a, e->bytes);
  gvec_free(a, e->media_type);
  gvec_free(a, e);
}

GVEC_Result gvec_res_fetch(gvec_res_state * state, GVEC_Resource_Kind kind, const char * base, const char * reference, size_t cap,
    size_t depth, gvec_res_outcome * out_outcome, gvec_res_entry ** out_entry) {
  *out_entry = NULL;
  if (!gvec_res_enabled(state)) {
    *out_outcome = GVEC_RES_NO_RESOLVER;
    return GVEC_OK;
  }
  if (depth > state->max_depth) {
    *out_outcome = GVEC_RES_LIMIT_DEPTH;
    return GVEC_OK;
  }
  size_t before = 0;
  (void) gvec_res_fragment(reference, &before);
  char * written = gvec_realloc(state->allocator, NULL, before + 1);
  if (!written) {
    return GVEC_ERR_OOM;
  }
  memcpy(written, reference, before);
  written[before] = 0;
  char * url = NULL;
  GVEC_Result r = gvec_res_resolve(state->allocator, base, written, &url);
  if (r != GVEC_OK) {
    gvec_free(state->allocator, written);
    return r;
  }
  gvec_res_entry * known = find(state, kind, url);
  if (known) {
    gvec_free(state->allocator, url);
    gvec_free(state->allocator, written);
    *out_outcome = (gvec_res_outcome) known->outcome;
    *out_entry = known;
    return GVEC_OK;
  }
  if (state->calls >= state->max_count) {
    gvec_free(state->allocator, url);
    gvec_free(state->allocator, written);
    *out_outcome = GVEC_RES_LIMIT_COUNT;
    return GVEC_OK;
  }
  gvec_res_entry * entry = gvec_alloc(state->allocator, sizeof *entry);
  if (!entry) {
    gvec_free(state->allocator, url);
    gvec_free(state->allocator, written);
    return GVEC_ERR_OOM;
  }
  memset(entry, 0, sizeof *entry);
  entry->kind = kind;
  entry->url = url;
  if (cap > state->max_bytes) {
    cap = state->max_bytes;
  }
  GVEC_Resource_Request request;
  memset(&request, 0, sizeof request);
  request.kind = kind;
  request.reference = written;
  request.url = url;
  request.base = base;
  request.max_bytes = cap;
  request.depth = depth;
  GVEC_Resource_Reply reply;
  memset(&reply, 0, sizeof reply);
  state->calls++;
  GVEC_Resource_Status status = state->fn(state->user, &request, &reply);
  gvec_free(state->allocator, written);
  switch (status) {
  case GVEC_RESOURCE_OK:
    if (reply.size > cap) {
      entry->outcome = GVEC_RES_TOO_BIG;
    }
    else if (reply.size > 0 && !reply.bytes) {
      entry->outcome = GVEC_RES_FAILED;
    }
    else {
      entry->bytes = gvec_alloc(state->allocator, reply.size ? reply.size : 1);
      if (!entry->bytes) {
        entry_free(state->allocator, entry);
        return GVEC_ERR_OOM;
      }
      if (reply.size) {
        memcpy(entry->bytes, reply.bytes, reply.size);
      }
      entry->size = reply.size;
      if (reply.media_type) {
        /* A media type is short: one that is not (or has no end within a bound) is ignored, never read past. */
        size_t n = 0;
        while (n < 256 && reply.media_type[n]) {
          n++;
        }
        if (n < 256 && n > 0) {
          entry->media_type = gvec_alloc(state->allocator, n + 1);
          if (!entry->media_type) {
            entry_free(state->allocator, entry);
            return GVEC_ERR_OOM;
          }
          memcpy(entry->media_type, reply.media_type, n);
          entry->media_type[n] = 0;
        }
      }
      entry->outcome = GVEC_RES_OK;
    }
    break;
  case GVEC_RESOURCE_REFUSED:
    entry->outcome = GVEC_RES_REFUSED;
    break;
  case GVEC_RESOURCE_NOT_FOUND:
    entry->outcome = GVEC_RES_NOT_FOUND;
    break;
  case GVEC_RESOURCE_OOM:
    entry_free(state->allocator, entry);
    return GVEC_ERR_OOM;
  case GVEC_RESOURCE_FAILED:
  default:
    entry->outcome = GVEC_RES_FAILED;
    break;
  }
  r = add(state, entry);
  if (r != GVEC_OK) {
    entry_free(state->allocator, entry);
    return r;
  }
  *out_outcome = (gvec_res_outcome) entry->outcome;
  *out_entry = entry;
  return GVEC_OK;
}
