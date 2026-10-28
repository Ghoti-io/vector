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
 * @file core.c
 *
 * Results, limits, diagnostics and the allocator.
 */

#include <ghoti.io/vector/macros.h>

#include <string.h>

#include <ghoti.io/vector/allocator.h>
#include <ghoti.io/vector/core.h>
#include <ghoti.io/vector/vector.h>

#include "core_internal.h"

const char * gvec_result_string(GVEC_Result result) {
  switch (result) {
  case GVEC_OK:
    return "ok";
  case GVEC_ERR_IO:
    return "input or output failed";
  case GVEC_ERR_FORMAT:
    return "malformed input";
  case GVEC_ERR_UNSUPPORTED:
    return "unsupported";
  case GVEC_ERR_LIMIT:
    return "limit reached";
  case GVEC_ERR_CORRUPT:
    return "corrupt transport";
  case GVEC_ERR_OOM:
    return "out of memory";
  case GVEC_ERR_INVALID:
    return "invalid argument";
  case GVEC_ERR_INTERNAL:
    return "internal error";
  case GVEC_RESULT_COUNT:
    break;
  }
  return "unknown";
}

const GVEC_Allocator * gvec_allocator_default(void) {
  return gcu_allocator_default();
}

void * gvec_alloc(const GVEC_Allocator * allocator, size_t size) {
  if (size == 0) {
    return NULL;
  }
  return gcu_allocator_malloc(allocator ? allocator : gcu_allocator_default(), size);
}

void * gvec_realloc(const GVEC_Allocator * allocator, void * ptr, size_t size) {
  return gcu_allocator_realloc(allocator ? allocator : gcu_allocator_default(), ptr, size);
}

void gvec_free(const GVEC_Allocator * allocator, void * ptr) {
  if (ptr) {
    gcu_allocator_free(allocator ? allocator : gcu_allocator_default(), ptr);
  }
}

//-----------------------------------------------------------------------------
// Limits
//-----------------------------------------------------------------------------

static const size_t gvec_limit_defaults[GVEC_LIMIT_FIELD_COUNT] = {
    GVEC_DEFAULT_MAX_INPUT_BYTES,
    GVEC_DEFAULT_MAX_INFLATED_BYTES,
    GVEC_DEFAULT_MAX_ELEMENTS,
    GVEC_DEFAULT_MAX_DEPTH,
    GVEC_DEFAULT_MAX_ATTRIBUTES,
    GVEC_DEFAULT_MAX_ENTITY_EXPANDED_BYTES,
    GVEC_DEFAULT_MAX_ENTITY_DEPTH,
    GVEC_DEFAULT_MAX_ENTITY_REFERENCES,
    GVEC_DEFAULT_MAX_ENTITY_AMPLIFICATION,
    GVEC_DEFAULT_MAX_PATH_SEGMENTS,
    GVEC_DEFAULT_MAX_LIST_ITEMS,
    GVEC_DEFAULT_MAX_REFERENCE_EXPANSION,
    GVEC_DEFAULT_MAX_TEXT_BYTES,
    GVEC_DEFAULT_MAX_DIAGNOSTICS,
    GVEC_DEFAULT_MAX_STYLE_BYTES,
    GVEC_DEFAULT_MAX_DECLARATIONS,
    GVEC_DEFAULT_MAX_CUSTOM_EXPANSION,
};

size_t gvec_limits_ceiling(GVEC_Limit_Field field) {
  if (field == GVEC_LIMIT_DIAGNOSTICS) {
    return GVEC_CEILING_MAX_DIAGNOSTICS;
  }
  if ((unsigned) field >= GVEC_LIMIT_FIELD_COUNT) {
    return 0;
  }
  return gvec_limit_defaults[field];
}

/* The fields in GVEC_Limit_Field order, so one loop covers them all. */
static size_t * gvec_limits_field(GVEC_Limits * limits, GVEC_Limit_Field field) {
  switch (field) {
  case GVEC_LIMIT_INPUT_BYTES:
    return &limits->max_input_bytes;
  case GVEC_LIMIT_INFLATED_BYTES:
    return &limits->max_inflated_bytes;
  case GVEC_LIMIT_ELEMENTS:
    return &limits->max_elements;
  case GVEC_LIMIT_DEPTH:
    return &limits->max_depth;
  case GVEC_LIMIT_ATTRIBUTES:
    return &limits->max_attributes;
  case GVEC_LIMIT_ENTITY_EXPANDED_BYTES:
    return &limits->max_entity_expanded_bytes;
  case GVEC_LIMIT_ENTITY_DEPTH:
    return &limits->max_entity_depth;
  case GVEC_LIMIT_ENTITY_REFERENCES:
    return &limits->max_entity_references;
  case GVEC_LIMIT_ENTITY_AMPLIFICATION:
    return &limits->max_entity_amplification;
  case GVEC_LIMIT_PATH_SEGMENTS:
    return &limits->max_path_segments;
  case GVEC_LIMIT_LIST_ITEMS:
    return &limits->max_list_items;
  case GVEC_LIMIT_REFERENCE_EXPANSION:
    return &limits->max_reference_expansion;
  case GVEC_LIMIT_TEXT_BYTES:
    return &limits->max_text_bytes;
  case GVEC_LIMIT_DIAGNOSTICS:
    return &limits->max_diagnostics;
  case GVEC_LIMIT_STYLE_BYTES:
    return &limits->max_style_bytes;
  case GVEC_LIMIT_DECLARATIONS:
    return &limits->max_declarations;
  case GVEC_LIMIT_CUSTOM_EXPANSION:
    return &limits->max_custom_expansion;
  case GVEC_LIMIT_FIELD_COUNT:
    break;
  }
  return NULL;
}

void gvec_limits_default(GVEC_Limits * limits) {
  if (!limits) {
    return;
  }
  memset(limits, 0, sizeof(*limits));
  for (unsigned f = 0; f < GVEC_LIMIT_FIELD_COUNT; f++) {
    *gvec_limits_field(limits, (GVEC_Limit_Field) f) = gvec_limit_defaults[f];
  }
}

GVEC_Result gvec_limits_resolve(const GVEC_Limits * in, GVEC_Limits * out, GVEC_Limit_Field * out_field) {
  GVEC_Limits resolved;
  if (!out) {
    return GVEC_ERR_INVALID;
  }
  if (!in) {
    gvec_limits_default(out);
    return GVEC_OK;
  }
  resolved = *in;
  for (size_t i = 0; i < sizeof(resolved._reserved); i++) {
    if (resolved._reserved[i] != 0) {
      return GVEC_ERR_INVALID;
    }
  }
  for (unsigned f = 0; f < GVEC_LIMIT_FIELD_COUNT; f++) {
    size_t * value = gvec_limits_field(&resolved, (GVEC_Limit_Field) f);
    if (*value == 0) {
      *value = gvec_limit_defaults[f];
    }
    else if (*value > gvec_limits_ceiling((GVEC_Limit_Field) f)) {
      if (out_field) {
        *out_field = (GVEC_Limit_Field) f;
      }
      return GVEC_ERR_INVALID;
    }
  }
  *out = resolved;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// Diagnostics
//-----------------------------------------------------------------------------

void gvec_diagnostics_init(GVEC_Diagnostics * diagnostics, const GVEC_Allocator * allocator) {
  if (!diagnostics) {
    return;
  }
  memset(diagnostics, 0, sizeof(*diagnostics));
  diagnostics->allocator = allocator;
}

static void gvec_diagnostics_free_names(GVEC_Diagnostics * diagnostics) {
  for (size_t i = 0; i < diagnostics->count; i++) {
    gvec_free(diagnostics->allocator, (void *) diagnostics->items[i].element);
    diagnostics->items[i].element = NULL;
  }
}

void gvec_diagnostics_clear(GVEC_Diagnostics * diagnostics) {
  if (!diagnostics) {
    return;
  }
  gvec_diagnostics_free_names(diagnostics);
  diagnostics->count = 0;
  diagnostics->dropped = 0;
}

void gvec_diagnostics_destroy(GVEC_Diagnostics * diagnostics) {
  if (!diagnostics) {
    return;
  }
  gvec_diagnostics_free_names(diagnostics);
  gvec_free(diagnostics->allocator, diagnostics->items);
  diagnostics->items = NULL;
  diagnostics->count = 0;
  diagnostics->capacity = 0;
  diagnostics->dropped = 0;
}

size_t gvec_diagnostics_count_code(const GVEC_Diagnostics * diagnostics, GVEC_Diag_Code code) {
  size_t n = 0;
  if (!diagnostics) {
    return 0;
  }
  for (size_t i = 0; i < diagnostics->count; i++) {
    if (diagnostics->items[i].code == code) {
      n++;
    }
  }
  return n;
}

void gvec_diag_set_cap(GVEC_Diagnostics * diagnostics, size_t cap) {
  if (diagnostics) {
    diagnostics->cap = cap;
  }
}

GVEC_Result gvec_diag_add(GVEC_Diagnostics * diagnostics, GVEC_Diag_Severity severity, GVEC_Diag_Code code,
    uint32_t detail, size_t line, size_t column, size_t offset, const char * element, const char * message) {
  GVEC_Diagnostic * entry;
  char * name = NULL;
  size_t cap;
  if (!diagnostics) {
    return GVEC_OK;
  }
  cap = diagnostics->cap ? diagnostics->cap : GVEC_DEFAULT_MAX_DIAGNOSTICS;
  if (diagnostics->count >= cap) {
    diagnostics->dropped++;
    return GVEC_OK;
  }
  if (diagnostics->count == diagnostics->capacity) {
    size_t grown = diagnostics->capacity ? diagnostics->capacity * 2 : 8;
    GVEC_Diagnostic * items;
    if (grown > cap) {
      grown = cap;
    }
    items = gvec_realloc(diagnostics->allocator, diagnostics->items, grown * sizeof(*items));
    if (!items) {
      diagnostics->dropped++;
      return GVEC_ERR_OOM;
    }
    diagnostics->items = items;
    diagnostics->capacity = grown;
  }
  if (element) {
    size_t length = strlen(element);
    name = gvec_alloc(diagnostics->allocator, length + 1);
    if (!name) {
      diagnostics->dropped++;
      return GVEC_ERR_OOM;
    }
    memcpy(name, element, length + 1);
  }
  entry = &diagnostics->items[diagnostics->count++];
  entry->severity = severity;
  entry->code = code;
  entry->detail = detail;
  entry->line = line;
  entry->column = column;
  entry->offset = offset;
  entry->element = name;
  entry->message = message ? message : "";
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// Version
//-----------------------------------------------------------------------------

unsigned gvec_version_number(void) {
  return GVEC_VERSION_NUMBER;
}

const char * gvec_version_string(void) {
  return GVEC_VERSION_STRING;
}
