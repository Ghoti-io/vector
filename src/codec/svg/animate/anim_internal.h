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
 * The animation of a lowering at a time (design.md, A1 to A12): the timing model of SMIL 3.0 (anim_time.c), the values
 * (anim_value.c), motion (anim_motion.c) and the animated document (anim_doc.c).
 */

#ifndef GHOTI_IO_GVEC_SVG_ANIM_INTERNAL_H
#define GHOTI_IO_GVEC_SVG_ANIM_INTERNAL_H

#include <ghoti.io/vector/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** No timed element: a time that comes from an offset, not from another element's interval. */
#define GVEC_ANIM_NONE UINT32_MAX

/** What a begin or end value is measured from. */
typedef enum gvec_anim_sync {
  GVEC_ANIM_SYNC_OFFSET = 0, ///< The document's begin.
  GVEC_ANIM_SYNC_BEGIN,      ///< `id.begin`: the begin of each interval of `source`.
  GVEC_ANIM_SYNC_END         ///< `id.end`: the end of each interval of `source`.
} gvec_anim_sync;

/** One value of a `begin` or `end` list that resolves without events. */
typedef struct gvec_anim_spec {
  uint8_t sync;    ///< A gvec_anim_sync.
  uint32_t source; ///< The timed element for a syncbase value; GVEC_ANIM_NONE for an offset.
  double offset;   ///< Seconds.
} gvec_anim_spec;

/** `restart`. */
typedef enum gvec_anim_restart {
  GVEC_ANIM_RESTART_ALWAYS = 0,
  GVEC_ANIM_RESTART_WHEN_NOT_ACTIVE,
  GVEC_ANIM_RESTART_NEVER
} gvec_anim_restart;

/**
 * The timing of one timed element, as parsed. A duration is in seconds: NAN for an attribute that is not specified (or was
 * ignored for an error), INFINITY for `indefinite`.
 */
typedef struct gvec_anim_timing {
  const gvec_anim_spec * begin; ///< The values of `begin` that can resolve; `{0}` when the attribute is absent.
  size_t begin_count;
  const gvec_anim_spec * end;   ///< The values of `end` that can resolve.
  size_t end_count;
  bool has_end;                 ///< An `end` attribute is specified.
  bool end_events;              ///< Its list has values that never resolve here (events, `indefinite`, ...): SMIL's endHasEventConditions().
  double dur;                   ///< The simple duration: INFINITY when unspecified, `indefinite` or `media`.
  bool dur_given;               ///< `dur` was specified (and valid).
  double repeat_count;          ///< NAN, INFINITY or a positive count.
  double repeat_dur;            ///< NAN, INFINITY or a duration.
  double min;                   ///< 0 when unspecified.
  double max;                   ///< INFINITY when unspecified.
  uint8_t restart;              ///< A gvec_anim_restart.
  bool freeze;                  ///< `fill="freeze"`.
} gvec_anim_timing;

/** Where a timed element is at the time sampled. */
typedef struct gvec_anim_sample {
  bool applies;           ///< Active, or frozen.
  bool frozen;
  double begin;           ///< The begin of the interval it is in or was frozen at (the sandwich's first key).
  uint32_t begin_source;  ///< The timed element whose interval made that begin (a syncbase), or GVEC_ANIM_NONE.
  double progress;        ///< The simple time over the simple duration, 0 to 1; 0 when the simple duration is indefinite.
  double iteration;       ///< The iteration of the simple duration.
  bool indefinite;        ///< The simple duration is indefinite (`set` and the first value of an animation with no `dur`).
} gvec_anim_sample;

/**
 * Parse a SMIL clock value (SMIL 3.0 Timing, "Clock values"): `hh:mm:ss.f`, `mm:ss.f` or a timecount with a metric
 * (`h`, `min`, `s`, `ms`; seconds by default), surrounded by optional white space.
 * @return true with `*seconds` set, false for anything else.
 */
GVEC_INTERNAL_API bool gvec_anim_clock(const char * text, size_t length, double * seconds);

/**
 * Run the timing model of `count` timed elements from the document's begin to `time` seconds and say where each is.
 * @param max_events The interval boundaries it may pass; past them GVEC_ERR_LIMIT, with nothing written.
 * @param out One sample per timed element.
 * @param events Receives the boundaries passed; may be NULL.
 * @return GVEC_OK, GVEC_ERR_LIMIT, GVEC_ERR_OOM.
 */
GVEC_INTERNAL_API GVEC_Result gvec_anim_time_run(const gvec_anim_timing * timing, size_t count, double time, size_t max_events,
    const GVEC_Allocator * allocator, gvec_anim_sample * out, size_t * events);

/** The active duration of an interval that begins at `begin` and is held to `end` (INFINITY for none): SMIL's calcActiveEnd() less `begin`. */
GVEC_INTERNAL_API double gvec_anim_active_duration(const gvec_anim_timing * timing, double begin, double end);

/** One keyframe of an animation of a property, as the sampler of `text` takes it (the style layer converts the text). */
typedef struct gvec_anim_frame {
  double offset;
  const char * text;   ///< The value in the attribute's syntax (or CSS, when the effect says so).
  size_t length;
  signed char composite; ///< -1: the effect's.
} gvec_anim_frame;

/** An animation of a property of one element at the time sampled: a keyframe effect in the animation origin (design A4, A5). */
typedef struct gvec_anim_effect {
  const char * attribute;              ///< The presentation attribute animated (`fill`, `gradientTransform`).
  const struct GVEC_Svg_Element * animation; ///< The animation element, in the document as written (for what is reported).
  const gvec_anim_frame * frames;
  size_t frame_count;
  bool add;                            ///< Composite `add` (else `replace`).
  bool accumulate;                     ///< Iteration composite `accumulate`.
  bool css;                            ///< The frames are CSS already (path data this library interpolated).
  double progress;                     ///< The iteration progress the frames are read at.
  double iteration;
} gvec_anim_effect;

struct GVEC_Svg_Document;
struct GVEC_Svg_Element;

/**
 * The document at `time_ms` (design A1 to A12): built again from the markup of `base` with the animated attributes in place, the
 * discarded elements gone, and the animations of properties and the motions kept for the style layer and the lowering.
 * `*out` is NULL, with GVEC_OK, for a document with no animation element. What is in error is reported to `diagnostics`.
 * @return GVEC_OK; GVEC_ERR_LIMIT (max_animation_intervals, reported); GVEC_ERR_OOM.
 */
GVEC_INTERNAL_API GVEC_Result gvec_svg_animate(const struct GVEC_Svg_Document * base, double time_ms, GVEC_Diagnostics * diagnostics,
    struct GVEC_Svg_Document ** out);

/** The animations of the properties of an element of an animated document, the lowest first; NULL (and 0) for none. */
GVEC_INTERNAL_API const gvec_anim_effect * gvec_anim_effects_of(const struct GVEC_Svg_Document * document, const struct GVEC_Svg_Element * element,
    size_t * count);

/** Whether an animation of an element of an animated document animates the presentation attribute `attribute` (it counts as given, like
 *  one that is written: the transform of a gradient or pattern of a chain is the one of the first that gives one). */
GVEC_INTERNAL_API bool gvec_anim_animates(const struct GVEC_Svg_Document * document, const struct GVEC_Svg_Element * element, const char * attribute);

/** The element of the document as written that an element of an animated document was built for (`element` itself in a document as written). */
GVEC_INTERNAL_API const struct GVEC_Svg_Element * gvec_anim_written_element(const struct GVEC_Svg_Document * document, const struct GVEC_Svg_Element * element);

/** The motion of an element of an animated document, a matrix `a b c d e f` applied outside its transform; false for none. */
GVEC_INTERNAL_API bool gvec_anim_motion_of(const struct GVEC_Svg_Document * document, const struct GVEC_Svg_Element * element, double m[6]);

#ifdef __cplusplus
}
#endif

#endif
