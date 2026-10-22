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
 * The typed document's arena (svg_arena.h).
 */

#include <ghoti.io/vector/macros.h>

#include <stdint.h>
#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include "../../core/core_internal.h"
#include "svg_arena.h"

struct gvec_arena_block {
  struct gvec_arena_block * next;
  size_t used;
  size_t capacity;
  /* The data follows, aligned for 16. */
};

#define BLOCK_HEADER ((sizeof(struct gvec_arena_block) + 15u) & ~(size_t) 15u)
#define MIN_BLOCK ((size_t) 16384)

void gvec_arena_init(gvec_arena * arena, const GVEC_Allocator * allocator) {
  arena->allocator = allocator;
  arena->head = NULL;
  arena->bytes = 0;
}

void * gvec_arena_alloc(gvec_arena * arena, size_t size, size_t align) {
  if (align == 0 || (align & (align - 1)) != 0 || align > 16) {
    return NULL;
  }
  if (size == 0) {
    size = 1;
  }
  struct gvec_arena_block * block = arena->head;
  if (block) {
    uintptr_t base = (uintptr_t) block + BLOCK_HEADER;
    size_t at = (size_t) ((((base + block->used) + align - 1) & ~(uintptr_t) (align - 1)) - base);
    size_t end;
    if (gcu_safe_add_size(at, size, &end) && end <= block->capacity) {
      block->used = end;
      arena->bytes += size;
      memset((void *) (base + at), 0, size);
      return (void *) (base + at);
    }
  }
  size_t want, total;
  if (!gcu_safe_add_size(size, align, &want)) {
    return NULL;
  }
  size_t capacity = want > MIN_BLOCK ? want : MIN_BLOCK;
  if (!gcu_safe_add_size(capacity, BLOCK_HEADER, &total)) {
    return NULL;
  }
  struct gvec_arena_block * fresh = gvec_alloc(arena->allocator, total);
  if (!fresh) {
    return NULL;
  }
  fresh->capacity = capacity;
  fresh->used = 0;
  /* A request bigger than a block gets a block of its own and does not displace the current one. */
  if (block && want > MIN_BLOCK) {
    fresh->next = block->next;
    block->next = fresh;
  } else {
    fresh->next = block;
    arena->head = fresh;
  }
  uintptr_t base = (uintptr_t) fresh + BLOCK_HEADER;
  size_t at = (size_t) ((((base) + align - 1) & ~(uintptr_t) (align - 1)) - base);
  fresh->used = at + size;
  arena->bytes += size;
  memset((void *) (base + at), 0, size);
  return (void *) (base + at);
}

char * gvec_arena_strndup(gvec_arena * arena, const char * text, size_t length) {
  size_t total;
  if (!gcu_safe_add_size(length, 1, &total)) {
    return NULL;
  }
  char * out = gvec_arena_alloc(arena, total, 1);
  if (out && length) {
    memcpy(out, text, length);
  }
  return out;
}

void * gvec_arena_array(gvec_arena * arena, size_t count, size_t size, size_t align) {
  size_t total;
  if (!gcu_safe_mul_size(count, size, &total)) {
    return NULL;
  }
  return gvec_arena_alloc(arena, total, align);
}

void gvec_arena_destroy(gvec_arena * arena) {
  struct gvec_arena_block * block = arena->head;
  while (block) {
    struct gvec_arena_block * next = block->next;
    gvec_free(arena->allocator, block);
    block = next;
  }
  arena->head = NULL;
  arena->bytes = 0;
}
