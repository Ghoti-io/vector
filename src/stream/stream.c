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
 * @file stream.c
 *
 * Memory, file and output streams.
 *
 * Every reading stream has a lookahead buffer in front of its source. A read
 * takes buffered bytes first; a peek fills the buffer and copies from it. So
 * peek works on a source that cannot seek, and gvec_stream_tell() is the
 * source's position less what is buffered.
 */

#include <ghoti.io/vector/macros.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <ghoti.io/vector/stream.h>

#include "../core/core_internal.h"
#include "stream_internal.h"

typedef enum { GVEC_STREAM_MEMORY, GVEC_STREAM_FILE, GVEC_STREAM_OUTPUT } gvec_stream_kind;

struct GVEC_Stream {
  const GVEC_Allocator * allocator;
  gvec_stream_kind kind;
  bool seekable;
  bool eof;
  GVEC_Result error;

  /* Memory source. */
  const unsigned char * data;
  size_t size;
  size_t chunk; /* the most one source read returns; 0 for no bound */

  /* File source. */
  FILE * file;
  size_t file_size; /* GVEC_STREAM_SIZE_UNKNOWN when not known */

  /* Bytes taken from the source so far (memory or file). */
  size_t source_pos;

  /* Lookahead. */
  unsigned char * look;
  size_t look_len;
  size_t look_cap;

  /* Output. */
  unsigned char * out;
  size_t out_len;
  size_t out_cap;
};

static GVEC_Result gvec_stream_new(const GVEC_Allocator * allocator, gvec_stream_kind kind, GVEC_Stream ** out) {
  GVEC_Stream * s = gvec_alloc(allocator, sizeof(*s));
  if (!s) {
    return GVEC_ERR_OOM;
  }
  memset(s, 0, sizeof(*s));
  s->allocator = allocator;
  s->kind = kind;
  s->error = GVEC_OK;
  s->file_size = GVEC_STREAM_SIZE_UNKNOWN;
  *out = s;
  return GVEC_OK;
}

static GVEC_Result gvec_stream_memory(const GVEC_Allocator * allocator, const void * data, size_t size,
    bool seekable, size_t chunk, GVEC_Stream ** out_stream) {
  GVEC_Stream * s;
  GVEC_Result r;
  if (!out_stream || (!data && size)) {
    return GVEC_ERR_INVALID;
  }
  r = gvec_stream_new(allocator, GVEC_STREAM_MEMORY, &s);
  if (r != GVEC_OK) {
    return r;
  }
  s->data = data;
  s->size = size;
  s->seekable = seekable;
  s->chunk = chunk;
  *out_stream = s;
  return GVEC_OK;
}

GVEC_Result gvec_stream_create_memory(const void * data, size_t size, GVEC_Stream ** out_stream) {
  return gvec_stream_memory(NULL, data, size, true, 0, out_stream);
}

GVEC_Result gvec_stream_create_memory_with_allocator(
    const GVEC_Allocator * allocator, const void * data, size_t size, GVEC_Stream ** out_stream) {
  return gvec_stream_memory(allocator, data, size, true, 0, out_stream);
}

GVEC_Result gvec_stream_create_memory_no_seek_with_allocator(
    const GVEC_Allocator * allocator, const void * data, size_t size, GVEC_Stream ** out_stream) {
  return gvec_stream_memory(allocator, data, size, false, 0, out_stream);
}

GVEC_Result gvec_stream_create_memory_chunked_with_allocator(const GVEC_Allocator * allocator, const void * data,
    size_t size, size_t max_bytes_per_read, GVEC_Stream ** out_stream) {
  if (max_bytes_per_read == 0) {
    return GVEC_ERR_INVALID;
  }
  return gvec_stream_memory(allocator, data, size, true, max_bytes_per_read, out_stream);
}

GVEC_Result gvec_stream_create_file(const char * path, GVEC_Stream ** out_stream) {
  return gvec_stream_create_file_with_allocator(NULL, path, out_stream);
}

GVEC_Result gvec_stream_create_file_with_allocator(
    const GVEC_Allocator * allocator, const char * path, GVEC_Stream ** out_stream) {
  GVEC_Stream * s;
  GVEC_Result r;
  FILE * file;
  if (!path || !out_stream) {
    return GVEC_ERR_INVALID;
  }
  file = fopen(path, "rb");
  if (!file) {
    return GVEC_ERR_IO;
  }
  r = gvec_stream_new(allocator, GVEC_STREAM_FILE, &s);
  if (r != GVEC_OK) {
    fclose(file);
    return r;
  }
  s->file = file;
  /* A regular file has a size and can seek; a pipe or a device has neither,
   * and is read as a stream that cannot seek. */
  if (fseek(file, 0, SEEK_END) == 0) {
    long end = ftell(file);
    if (end >= 0 && fseek(file, 0, SEEK_SET) == 0) {
      s->file_size = (size_t) end;
      s->seekable = true;
    }
  }
  clearerr(file);
  *out_stream = s;
  return GVEC_OK;
}

GVEC_Result gvec_stream_create_memory_output(GVEC_Stream ** out_stream) {
  return gvec_stream_create_memory_output_with_allocator(NULL, out_stream);
}

GVEC_Result gvec_stream_create_memory_output_with_allocator(
    const GVEC_Allocator * allocator, GVEC_Stream ** out_stream) {
  if (!out_stream) {
    return GVEC_ERR_INVALID;
  }
  return gvec_stream_new(allocator, GVEC_STREAM_OUTPUT, out_stream);
}

/* One read from the source, past the lookahead. */
static GVEC_Result gvec_stream_source_read(GVEC_Stream * s, unsigned char * buffer, size_t size, size_t * got) {
  *got = 0;
  if (s->error != GVEC_OK) {
    return s->error;
  }
  if (size == 0) {
    return GVEC_OK;
  }
  if (s->kind == GVEC_STREAM_MEMORY) {
    size_t n = s->size - s->source_pos;
    if (n > size) {
      n = size;
    }
    if (s->chunk && n > s->chunk) {
      n = s->chunk;
    }
    memcpy(buffer, s->data + s->source_pos, n);
    s->source_pos += n;
    *got = n;
    if (n < size && s->source_pos == s->size) {
      s->eof = true;
    }
  }
  else if (s->kind == GVEC_STREAM_FILE) {
    size_t n = fread(buffer, 1, size, s->file);
    if (n == 0 && ferror(s->file)) {
      s->error = GVEC_ERR_IO;
      return GVEC_ERR_IO;
    }
    s->source_pos += n;
    *got = n;
    if (n < size && feof(s->file)) {
      s->eof = true;
    }
  }
  else {
    return GVEC_ERR_IO;
  }
  return GVEC_OK;
}

GVEC_Result gvec_stream_read(GVEC_Stream * stream, void * buffer, size_t size, size_t * out_bytes_read) {
  size_t got = 0;
  GVEC_Result r = GVEC_OK;
  if (out_bytes_read) {
    *out_bytes_read = 0;
  }
  if (!stream || (!buffer && size)) {
    return GVEC_ERR_INVALID;
  }
  if (stream->kind == GVEC_STREAM_OUTPUT) {
    return GVEC_ERR_IO;
  }
  if (stream->look_len) {
    got = stream->look_len < size ? stream->look_len : size;
    memcpy(buffer, stream->look, got);
    gvec_stream_consume(stream, got);
  }
  else {
    r = gvec_stream_source_read(stream, buffer, size, &got);
  }
  if (out_bytes_read) {
    *out_bytes_read = got;
  }
  return r;
}

GVEC_Result gvec_stream_read_exact(GVEC_Stream * stream, void * buffer, size_t size) {
  size_t done = 0;
  if (!stream || (!buffer && size)) {
    return GVEC_ERR_INVALID;
  }
  while (done < size) {
    size_t got;
    GVEC_Result r = gvec_stream_read(stream, (unsigned char *) buffer + done, size - done, &got);
    if (r != GVEC_OK) {
      return r;
    }
    if (got == 0) {
      return GVEC_ERR_IO;
    }
    done += got;
  }
  return GVEC_OK;
}

/* Fill the lookahead to `want` bytes or the end. */
static GVEC_Result gvec_stream_fill(GVEC_Stream * s, size_t want) {
  while (s->look_len < want) {
    size_t got;
    GVEC_Result r;
    if (s->look_cap < want) {
      size_t grown = s->look_cap ? s->look_cap : 4096;
      unsigned char * look;
      while (grown < want) {
        grown = grown > SIZE_MAX / 2 ? want : grown * 2;
      }
      look = gvec_realloc(s->allocator, s->look, grown);
      if (!look) {
        return GVEC_ERR_OOM;
      }
      s->look = look;
      s->look_cap = grown;
    }
    r = gvec_stream_source_read(s, s->look + s->look_len, want - s->look_len, &got);
    if (r != GVEC_OK) {
      return r;
    }
    if (got == 0) {
      break;
    }
    s->look_len += got;
  }
  return GVEC_OK;
}

GVEC_Result gvec_stream_peek(GVEC_Stream * stream, void * buffer, size_t size, size_t * out_bytes_available) {
  GVEC_Result r;
  size_t n;
  if (out_bytes_available) {
    *out_bytes_available = 0;
  }
  if (!stream || (!buffer && size)) {
    return GVEC_ERR_INVALID;
  }
  if (stream->kind == GVEC_STREAM_OUTPUT) {
    return GVEC_ERR_IO;
  }
  r = gvec_stream_fill(stream, size);
  if (r != GVEC_OK) {
    return r;
  }
  n = stream->look_len < size ? stream->look_len : size;
  memcpy(buffer, stream->look, n);
  if (out_bytes_available) {
    *out_bytes_available = n;
  }
  return GVEC_OK;
}

GVEC_Result gvec_stream_lookahead_all(
    GVEC_Stream * stream, size_t max, const unsigned char ** out_data, size_t * out_size, bool * out_over) {
  GVEC_Result r;
  size_t want;
  if (!stream || !out_data || !out_size || !out_over || stream->kind == GVEC_STREAM_OUTPUT) {
    return GVEC_ERR_INVALID;
  }
  want = max == SIZE_MAX ? max : max + 1;
  /* Grow in steps so that a short input does not reserve `max` bytes. */
  for (size_t step = 65536; ; step = step > SIZE_MAX / 2 ? want : step * 2) {
    size_t target = step < want ? step : want;
    r = gvec_stream_fill(stream, target);
    if (r != GVEC_OK) {
      return r;
    }
    if (stream->look_len < target || target == want) {
      break;
    }
  }
  *out_over = stream->look_len > max;
  *out_data = stream->look;
  *out_size = stream->look_len > max ? max : stream->look_len;
  return GVEC_OK;
}

void gvec_stream_consume(GVEC_Stream * stream, size_t count) {
  if (!stream) {
    return;
  }
  if (count > stream->look_len) {
    count = stream->look_len;
  }
  memmove(stream->look, stream->look + count, stream->look_len - count);
  stream->look_len -= count;
}

GVEC_Result gvec_stream_write(GVEC_Stream * stream, const void * buffer, size_t size) {
  if (!stream || (!buffer && size)) {
    return GVEC_ERR_INVALID;
  }
  if (stream->kind != GVEC_STREAM_OUTPUT || stream->error != GVEC_OK) {
    return GVEC_ERR_IO;
  }
  if (size > SIZE_MAX - stream->out_len) {
    stream->error = GVEC_ERR_OOM;
    return GVEC_ERR_OOM;
  }
  if (stream->out_len + size > stream->out_cap) {
    size_t grown = stream->out_cap ? stream->out_cap : 256;
    unsigned char * out;
    while (grown < stream->out_len + size) {
      grown = grown > SIZE_MAX / 2 ? stream->out_len + size : grown * 2;
    }
    out = gvec_realloc(stream->allocator, stream->out, grown);
    if (!out) {
      stream->error = GVEC_ERR_OOM;
      return GVEC_ERR_OOM;
    }
    stream->out = out;
    stream->out_cap = grown;
  }
  if (size) {
    memcpy(stream->out + stream->out_len, buffer, size);
  }
  stream->out_len += size;
  return GVEC_OK;
}

GVEC_Result gvec_stream_seek(GVEC_Stream * stream, size_t offset) {
  if (!stream) {
    return GVEC_ERR_INVALID;
  }
  if (stream->kind == GVEC_STREAM_OUTPUT || !stream->seekable) {
    return GVEC_ERR_UNSUPPORTED;
  }
  if (stream->error != GVEC_OK) {
    return stream->error;
  }
  if (stream->kind == GVEC_STREAM_MEMORY) {
    if (offset > stream->size) {
      return GVEC_ERR_IO;
    }
  }
  else {
    if (offset > stream->file_size || offset > (size_t) LONG_MAX || fseek(stream->file, (long) offset, SEEK_SET) != 0) {
      return GVEC_ERR_IO;
    }
    clearerr(stream->file);
  }
  stream->source_pos = offset;
  stream->look_len = 0;
  stream->eof = false;
  return GVEC_OK;
}

size_t gvec_stream_tell(const GVEC_Stream * stream) {
  if (!stream) {
    return 0;
  }
  if (stream->kind == GVEC_STREAM_OUTPUT) {
    return stream->out_len;
  }
  return stream->source_pos - stream->look_len;
}

size_t gvec_stream_size(const GVEC_Stream * stream) {
  if (!stream) {
    return GVEC_STREAM_SIZE_UNKNOWN;
  }
  switch (stream->kind) {
  case GVEC_STREAM_MEMORY:
    return stream->seekable ? stream->size : GVEC_STREAM_SIZE_UNKNOWN;
  case GVEC_STREAM_FILE:
    return stream->file_size;
  case GVEC_STREAM_OUTPUT:
    return stream->out_len;
  }
  return GVEC_STREAM_SIZE_UNKNOWN;
}

bool gvec_stream_eof(const GVEC_Stream * stream) {
  return stream && stream->eof && stream->look_len == 0;
}

GVEC_Result gvec_stream_error(const GVEC_Stream * stream) {
  return stream ? stream->error : GVEC_ERR_INVALID;
}

void gvec_stream_output_buffer(const GVEC_Stream * stream, const void ** out_data, size_t * out_size) {
  if (out_data) {
    *out_data = NULL;
  }
  if (out_size) {
    *out_size = 0;
  }
  if (!stream || stream->kind != GVEC_STREAM_OUTPUT) {
    return;
  }
  if (out_data) {
    *out_data = stream->out_len ? stream->out : NULL;
  }
  if (out_size) {
    *out_size = stream->out_len;
  }
}

void gvec_stream_destroy(GVEC_Stream * stream) {
  if (!stream) {
    return;
  }
  if (stream->file) {
    fclose(stream->file);
  }
  gvec_free(stream->allocator, stream->look);
  gvec_free(stream->allocator, stream->out);
  gvec_free(stream->allocator, stream);
}
