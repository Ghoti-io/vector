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
 * The style of a document at a time (design.md A4, A5, A9): text's sampler is handed the time of every CSS Animation (each was made
 * by the first style, at 0, so its current time is the document time, or 0 while it is paused), and the SMIL animations of the
 * element's properties as keyframe effects of the class of a replaced CSS Transition, which the sampler stacks in the animation origin
 * below the element's CSS Animations: where both browsers put them. A value is written as its presentation attribute is and turned into
 * CSS by text's own conversion; one that is not a value of its property makes its animation apply nothing, as in both browsers.
 */

#include <ghoti.io/vector/macros.h>

#include <stdint.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_presentation.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>

#include "../animate/anim_internal.h"
#include "svg_style.h"

void gvec_svg_style_set_time(GVEC_Svg_Style_Context * context, double time_ms) {
  context->animated = true;
  context->animation_time_ms = time_ms;
}

void gvec_svg_style_animation_free(GVEC_Svg_Style_Context * context) {
  gvec_free(context->allocator, context->animation_effects);
  context->animation_effects = NULL;
  context->animation_effect_capacity = 0;
  gvec_arena_destroy(&context->animation_scratch);
}

static GTEXT_CSS_Status cb_time(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Selector_List * pseudo,
    const GTEXT_CSS_Anim_Effect * effect, double * time_ms, bool * known) {
  (void) element;
  (void) pseudo;
  const GVEC_Svg_Style_Context * context = user;
  /* A scroll or view timeline has nothing to scroll in a drawing: it is inactive, and such an animation is not sampled. */
  *known = effect->timeline_auto;
  *time_ms = effect->paused ? 0 : context->animation_time_ms;
  return GTEXT_CSS_OK;
}

/* A presentation attribute may write a length without its unit (SVG 2, 6.6: a number is in user units, which are `px`); CSS may not. The value
 * with `px` after each number that has no unit, for a second reading when the value as written is not a value of the property. */
static const char * with_px(gvec_arena * arena, const char * s, size_t n, size_t * out_length) {
  char * out = gvec_arena_alloc(arena, n * 3 + 1, 1);
  if (!out) {
    return NULL;
  }
  size_t k = 0;
  char quote = 0;
  for (size_t i = 0; i < n;) {
    char ch = s[i];
    if (quote) {
      out[k++] = ch;
      quote = ch == quote ? 0 : quote;
      i++;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      out[k++] = ch;
      i++;
      continue;
    }
    bool prev_word = k > 0 && ((out[k - 1] >= 'a' && out[k - 1] <= 'z') || (out[k - 1] >= 'A' && out[k - 1] <= 'Z') || out[k - 1] == '#' ||
                                  out[k - 1] == '-' || out[k - 1] == '_' || (out[k - 1] >= '0' && out[k - 1] <= '9'));
    bool starts = (ch >= '0' && ch <= '9') || (ch == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9') ||
                  ((ch == '-' || ch == '+') && i + 1 < n && ((s[i + 1] >= '0' && s[i + 1] <= '9') || s[i + 1] == '.'));
    if (!starts || prev_word) {
      out[k++] = ch;
      i++;
      continue;
    }
    size_t j = i;
    if (s[j] == '-' || s[j] == '+') {
      out[k++] = s[j++];
    }
    while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.')) {
      out[k++] = s[j++];
    }
    if (j < n && (s[j] == 'e' || s[j] == 'E') && j + 1 < n && ((s[j + 1] >= '0' && s[j + 1] <= '9') || s[j + 1] == '-' || s[j + 1] == '+')) {
      out[k++] = s[j++];
      while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == '-' || s[j] == '+')) {
        out[k++] = s[j++];
      }
    }
    bool unit = j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || s[j] == '%');
    if (!unit) {
      out[k++] = 'p';
      out[k++] = 'x';
    }
    i = j;
  }
  out[k] = '\0';
  *out_length = k;
  return out;
}

static bool valid_for(const GTEXT_CSS_Registry * registry, const char * name, size_t name_length, const char * css, size_t length, bool * oom) {
  GTEXT_CSS_Property_Value parsed;
  GTEXT_CSS_Status st = gtext_css_parse_property_text(registry, name, name_length, css, length, NULL, &parsed);
  bool valid = st == GTEXT_CSS_OK && (parsed.outcome == GTEXT_CSS_PROP_RESULT_VALID || parsed.outcome == GTEXT_CSS_PROP_RESULT_WIDE ||
                                      parsed.outcome == GTEXT_CSS_PROP_RESULT_PENDING);
  gtext_css_property_value_free(&parsed);
  *oom = st == GTEXT_CSS_E_OOM;
  return valid;
}

/* One SMIL effect as text's sampler takes it; false when a value is not one of the property (the effect then applies nothing). */
static bool convert(GVEC_Svg_Style_Context * context, const GVEC_Svg_Element * e, const gvec_anim_effect * ef, size_t rank,
    GTEXT_CSS_Script_Effect * out, bool * oom) {
  *oom = false;
  const GTEXT_CSS_Registry * registry = context->engine->reg.registry;
  const char * ns = e->ns ? e->ns : "";
  const GTEXT_CSS_Presentation_Row * row = gtext_css_presentation_find(&context->engine->reg.table, ns, strlen(ns), e->name, strlen(e->name),
      ef->attribute, strlen(ef->attribute));
  if (!row) {
    return false;
  }
  size_t name_length = strlen(row->property);
  GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(registry, row->property, name_length);
  if (id == GTEXT_CSS_PROPERTY_NONE) {
    return false;
  }
  gvec_arena * arena = &context->animation_scratch;
  GTEXT_CSS_Keyframe * frames = gvec_arena_array(arena, ef->frame_count, sizeof *frames, 8);
  GTEXT_CSS_Keyframe_Property * props = gvec_arena_array(arena, ef->frame_count, sizeof *props, 8);
  signed char * composites = gvec_arena_array(arena, ef->frame_count, sizeof *composites, 1);
  if (!frames || !props || !composites) {
    *oom = true;
    return false;
  }
  for (size_t i = 0; i < ef->frame_count; i++) {
    const gvec_anim_frame * f = &ef->frames[i];
    const char * css = f->text;
    size_t css_length = f->length;
    char * converted = NULL;
    if (!ef->css) {
      size_t length = 0;
      GTEXT_CSS_Status st = gtext_css_presentation_value_text(row, f->text, f->length, NULL, NULL, &converted, &length, NULL);
      if (st == GTEXT_CSS_E_OOM) {
        *oom = true;
        return false;
      }
      if (st != GTEXT_CSS_OK) {
        return false;
      }
      css = gvec_arena_strndup(arena, converted, length);
      css_length = length;
      gtext_allocator_free(NULL, converted);
      if (!css) {
        *oom = true;
        return false;
      }
    }
    bool valid = valid_for(registry, row->property, name_length, css, css_length, oom);
    if (!valid && !*oom && !ef->css) {
      size_t length = 0;
      const char * px = with_px(arena, css, css_length, &length);
      if (!px) {
        *oom = true;
        return false;
      }
      valid = valid_for(registry, row->property, name_length, px, length, oom);
      if (valid) {
        css = px;
        css_length = length;
      }
    }
    if (*oom || !valid) {
      return false;
    }
    memset(&props[i], 0, sizeof props[i]);
    props[i].property = id;
    props[i].name.data = row->property;
    props[i].name.length = name_length;
    props[i].text.data = css;
    props[i].text.length = css_length;
    props[i].set = NULL;
    memset(&frames[i], 0, sizeof frames[i]);
    frames[i].offset = f->offset;
    frames[i].properties = &props[i];
    frames[i].property_count = 1;
    composites[i] = f->composite;
  }
  memset(out, 0, sizeof *out);
  out->frames = frames;
  out->frame_count = ef->frame_count;
  out->easings = NULL;
  out->composites = composites;
  out->composite = ef->add ? GTEXT_CSS_ANIM_COMPOSITE_ADD : GTEXT_CSS_ANIM_COMPOSITE_REPLACE;
  out->iteration_accumulate = ef->accumulate;
  out->progress = ef->progress;
  out->current_iteration = ef->iteration;
  out->before_flag = false;
  /* The class of a replaced CSS Transition: in the animation origin, below the CSS Animations of the element (design A4). */
  out->css_class = 1;
  out->css_index = rank;
  out->css_foreign = false;
  out->css_order = SIZE_MAX;
  return true;
}

static GTEXT_CSS_Status cb_effects(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Selector_List * pseudo,
    const GTEXT_CSS_Script_Effect ** effects, size_t * count) {
  GVEC_Svg_Style_Context * context = user;
  *effects = NULL;
  *count = 0;
  if (pseudo) {
    return GTEXT_CSS_OK;
  }
  const GVEC_Svg_Element * e = ((const gvec_svg_handle *) element)->element;
  size_t n = 0;
  const gvec_anim_effect * list = gvec_anim_effects_of(context->document, e, &n);
  if (n == 0) {
    return GTEXT_CSS_OK;
  }
  gvec_arena_destroy(&context->animation_scratch);
  gvec_arena_init(&context->animation_scratch, context->allocator);
  if (n > context->animation_effect_capacity) {
    GTEXT_CSS_Script_Effect * fresh = gvec_realloc(context->allocator, context->animation_effects, n * sizeof *fresh);
    if (!fresh) {
      return GTEXT_CSS_E_OOM;
    }
    context->animation_effects = fresh;
    context->animation_effect_capacity = n;
  }
  size_t k = 0;
  bool reported = false;
  for (size_t i = 0; i < n; i++) {
    bool oom = false;
    if (convert(context, e, &list[i], i, &context->animation_effects[k], &oom)) {
      k++;
    } else if (oom) {
      return GTEXT_CSS_E_OOM;
    } else if (!reported) {
      reported = true;
      if (gvec_svg_once_first(context, GVEC_ONCE_ANIMATION, e)) {
        const GVEC_Svg_Element * a = list[i].animation;
        if (gvec_diag_add(context->diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_ANIMATION_INVALID, 0, a->node.line, a->node.column, a->node.offset,
                a->name, "a value of the animation is not a value of the property it animates; the animation is ignored") == GVEC_ERR_OOM) {
          return GTEXT_CSS_E_OOM;
        }
      }
    }
  }
  *effects = context->animation_effects;
  *count = k;
  return GTEXT_CSS_OK;
}

const GTEXT_CSS_Style_Animation * gvec_svg_style_animation(GVEC_Svg_Style_Context * context) {
  if (!context->animated) {
    return NULL;
  }
  memset(&context->animation, 0, sizeof context->animation);
  context->animation.size = sizeof context->animation;
  bool usable = context->atrules != NULL;
  if (usable && !context->atrules_frozen) {
    context->atrules_frozen = true;
    usable = gtext_css_atrule_set_freeze(context->atrules) == GTEXT_CSS_OK;
  }
  /* No sheets, or a set that could not be frozen: no `@keyframes`, so no CSS Animation (the SMIL effects still apply). */
  context->animation.keyframes = usable ? context->atrules : NULL;
  context->animation.user = context;
  context->animation.current_time = cb_time;
  context->animation.script_effects = cb_effects;
  return &context->animation;
}
