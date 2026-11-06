#!/usr/bin/env python3
#
# Plant one defect at a time in a rule of the load (L1 to L11 of
# documentation/design.md) and require the tests to catch it.
#
# For each plant, the one source file it touches is copied, the defect is
# written into the copy (the text it replaces must occur exactly once, so an
# edit that silently matches nothing cannot pass), the copy is compiled with
# the library's own flags, and the library's archive is rebuilt with it in
# place of the original. Every unit test, and the fuzz replay over the
# committed corpus, is then linked against that archive and run. The plant is
# caught when any of them fails or dies by a signal. Before any plant, the
# unmodified control is built the same way and must pass, so that a harness
# that fails everything cannot pass this check.
#
# Environment, supplied by `make check-planted`:
#   PLANT_CC, PLANT_CFLAGS      compiler and the library's compile flags with includes
#   PLANT_CCFLAGS               flags for the fuzz replay (CFLAGS and includes)
#   PLANT_CXX                   C++ compiler and flags
#   PLANT_OBJ_DIR               the library's object directory
#   PLANT_TEST_OBJS             the unit tests' objects
#   PLANT_LDFLAGS               link flags (dependencies, gtest)
#   PLANT_LD_PATH               LD_LIBRARY_PATH for running
#   PLANT_WORK                  a scratch directory
#   PLANT_ONLY                  (optional) a rule prefix: only those plants
#   PLANT_BUILD_ONLY            (optional) apply and compile each plant, run nothing: to write plants quickly

import os
import shutil
import signal
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The lowering's walk, which most of the lowering's rules live in.
W = "src/codec/svg/lower/lower_walk.c"
# The paints and the strokes.
S = "src/codec/svg/lower/lower_stroke.c"
# The markers.
M = "src/codec/svg/lower/lower_marker.c"
PR = "src/codec/svg/lower/lower_primitive.c"
LP = "src/codec/svg/lower/lower_path.c"
# Gradients.
GR = "src/codec/svg/lower/lower_gradient.c"
# Clips.
CL = "src/codec/svg/lower/lower_clip.c"

# (rule, source file, text, replacement, what the defect is)
PLANTS = [
    ("L1", "src/core/core.c", 'return "input or output failed";', 'return "ok";',
     "two results share a name"),
    ("L2", "src/stream/stream.c", "    if (n < size && s->source_pos == s->size) {\n      s->eof = true;\n    }\n", "",
     "a short read that reaches the end does not set eof"),
    ("L2", "src/stream/stream.c", "return stream->source_pos - stream->look_len;", "return stream->source_pos;",
     "tell counts peeked bytes as consumed"),
    ("L2", "src/stream/stream.c", "  if (stream->kind == GVEC_STREAM_OUTPUT || !stream->seekable) {",
     "  if (stream->kind == GVEC_STREAM_OUTPUT) {", "a stream that cannot seek seeks"),
    ("L3", "src/core/core.c", "    if (*value == 0) {\n      *value = gvec_limit_defaults[f];",
     "    if (*value == 0) {\n      *value = SIZE_MAX;", "zero means unlimited"),
    ("L3", "src/core/core.c", "else if (*value > gvec_limits_ceiling((GVEC_Limit_Field) f)) {",
     "else if (*value >= gvec_limits_ceiling((GVEC_Limit_Field) f)) {", "a limit at its ceiling is refused"),
    ("L3", "src/core/core.c", "    if (resolved._reserved[i] != 0) {", "    if (0) {", "a reserved byte is accepted"),
    ("L4", "src/core/core.c", "if (diagnostics->count >= cap) {", "if (diagnostics->count > cap) {",
     "one entry past the cap is kept"),
    ("L4", "src/core/core.c", "  entry->element = name;", "  entry->element = element;",
     "the element name is borrowed, not copied"),
    ("L4", "src/core/core.c", "  if (diagnostics->count >= cap) {\n    diagnostics->dropped++;",
     "  if (diagnostics->count >= cap) {\n    ;", "entries past the cap are not counted"),
    ("L5", "src/codec/codec.c", "    if (strcmp(gvec_codecs[i]->name, name) == 0) {",
     "    if (strcmp(gvec_codecs[i]->name, name) != 0) {", "lookup by name returns the wrong codec"),
    ("L6", "src/codec/transport.c", "  if (gvec_transport_is_gzip(out.data, out.size)) {", "  if (0) {",
     "gzip inside gzip is read"),
    ("L6", "src/codec/transport.c", "      gvec_diag_add(diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_GZIP, 0, 0, 0, pos, NULL,\n"
     "          \"bytes after the last gzip member were ignored\");\n", "",
     "trailing bytes are ignored silently"),
    ("L6", "src/codec/transport.c", "  return size >= 3 && data[0] == 0x1f && data[1] == 0x8b && data[2] == 0x08;",
     "  return size >= 2 && data[0] == 0x1f && data[1] == 0x8b;", "a gzip member with another method is taken for gzip"),
    ("L7", "src/codec/svg/svg_codec.c",
     "if (event.namespace_uri && strcmp(event.namespace_uri, GTEXT_MARKUP_NS_SVG) == 0 && event.local_name &&",
     "if ((!event.namespace_uri || strcmp(event.namespace_uri, GTEXT_MARKUP_NS_SVG) == 0) && event.local_name &&",
     "a root in no namespace is taken for SVG"),
    ("L7", "src/codec/svg/svg_codec.c", "    if (event.type == GTEXT_XML_EVENT_ELEMENT_START) {", "    if (1) {",
     "the probe decides at the first event, not the first start tag"),
    ("L7", "src/codec/svg/svg_codec.c", "        if (r == GVEC_ERR_OOM || r == GVEC_ERR_LIMIT) {", "        if (r == GVEC_ERR_OOM) {",
     "a limit reached before the root is reported as not SVG"),
    ("L7", "src/codec/codec.c", "        diagnostics->dropped += why_not.dropped;\n", "",
     "a reason the probe could not keep is lost"),
    ("L8", "src/codec/svg/svg_load.c", "      if (++elements > limits->max_elements) {", "      if (++elements >= limits->max_elements) {",
     "a document of exactly max_elements elements is refused"),
    ("L8", "src/codec/svg/svg_load.c", "        resource.kind != GTEXT_MARKUP_DTD_RESOURCE_GENERAL_ENTITY) {", "        1) {",
     "a skipped general entity is reported as an unread DTD"),
    ("L8", "src/codec/svg/svg_load.c", "      if (gvec_diag_add(diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_ENTITY_SKIPPED,",
     "      if (gvec_diag_add(NULL, GVEC_DIAG_WARNING, GVEC_DIAG_ENTITY_SKIPPED,",
     "a kept external entity is not reported"),
    ("L8", "src/codec/svg/svg_load.c", "  case GTEXT_XML_E_LIMIT_DEPTH:\n    *out = GVEC_LIMIT_DEPTH;",
     "  case GTEXT_XML_E_LIMIT_DEPTH:\n    *out = GVEC_LIMIT_ATTRIBUTES;", "the depth limit names the wrong field"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->dtd_mode = GTEXT_XML_DTD_PROCESS;", "  out->dtd_mode = GTEXT_XML_DTD_OFF;",
     "the internal subset is not processed"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->external_access = GTEXT_XML_EXTERNAL_ACCESS_DENY;",
     "  out->external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;", "external access is allowed"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->dtd_external_policy = GTEXT_XML_DTD_EXTERNAL_SKIP;",
     "  out->dtd_external_policy = GTEXT_XML_DTD_EXTERNAL_DENY;", "an external entity fails the load"),
    ("L9", "src/codec/svg/svg_codec.c", "  out->max_depth = limits->max_depth;", "",
     "the depth limit is not passed to the parser"),
    ("L10", "src/codec/codec.c", '{"none", "gzip"}', '{"gzip", "none"}', "the dump names the wrong transport"),
    ("L10", "src/codec/codec.c", "  for (int i = 7; i >= 0; i--) {", "  for (int i = 0; i < 8; i++) {",
     "the hash is read big-endian"),
    # The lowering (story 7): the walk, the two emitters, the viewport maths, the trace.
    ("P1", "src/codec/svg/lower/lower_emit_list.c", "      group.opacity = n->opacity;", "      group.opacity = n->opacity * 0.5;",
     "the list emitter decides an opacity the edit emitter does not (the equality gate)"),
    ("P1", "src/codec/svg/lower/lower_emit_edit.c", "      props.fill_rule = n->rule;\n", "",
     "the edit emitter drops the fill rule the list emitter keeps"),
    ("P1", "src/codec/svg/lower/lower_emit_edit.c", "  return trim(edit, group, position);", "  return GVEC_OK;",
     "a node the document no longer has is left in the scene"),
    ("P1", "src/codec/svg/lower/lower_emit_edit.c",
     "  if (info.parent != parent || gshscene_edit_child(edit, parent, position, &at) != GSHSCENE_OK || at != id) {",
     "  if ((at & 0) != 0) {", "a node found by its key is not moved to where the document now has it"),
    ("P1", "src/codec/svg/lower/lower_entry.c",
     "      gshscene_edit_set_foreground(edit, &options->foreground) != GSHSCENE_OK) {",
     "      0) {", "the foreground option is not set on the edit"),
    ("P2", "src/codec/svg/lower/lower_walk.c", "  uint64_t parts[3] = {element_key, chain, (uint64_t) role};",
     "  uint64_t parts[3] = {element_key, chain & 0, (uint64_t) role};", "two uses of one referent have the same node keys"),
    ("P3", "src/codec/svg/lower/lower_viewport.c", "    double s = par->slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);",
     "    double s = (sx < sy ? sx : sy);", "slice is meet"),
    ("P3", "src/codec/svg/lower/lower_viewport.c", "    double ax = (double) (index % 3) / 2.0;", "    double ax = (double) (index / 3) / 2.0;",
     "the horizontal alignment is the vertical one"),
    ("P3", "src/codec/svg/lower/lower_viewport.c", "  *out = round_expansion(&e);",
     "  *out = align * extent - scale * (origin + align * size) + 0 * round_expansion(&e);",
     "the translation is evaluated in plain double arithmetic, not exactly"),
    ("P3", "src/codec/svg/lower/lower_viewport.c", "  if (!grow(&e, align * extent)) {", "  if (!grow(&e, align * extent * 0)) {",
     "the alignment offset is left out of the translation"),
    ("P3", W, "(has_box && (!(box.width > 0) || !(box.height > 0)))", "(0)",
     "a viewBox with no width does not disable rendering"),
    ("P4", W, '  bool clips = !(gvec_lw_keyword_is(&overflow, "visible") || gvec_lw_keyword_is(&overflow, "auto"));', "  bool clips = true;",
     "a nested svg with visible overflow is clipped"),
    ("P4", W, "    clip->map = c.local;\n", "",
     "the viewport clip is not placed in the parent's space"),
    ("P5", W, "  if (!(opacity < 1)) {\n    return GVEC_OK;\n  }\n  bool entered;", "  if (!(opacity <= 1)) {\n    return GVEC_OK;\n  }\n  bool entered;",
     "a group is made for every element"),
    ("P5", W, "  c->draw = c->opacity > 0; /* Opacity zero draws nothing, whatever is inside. */", "  c->draw = true;",
     "an element with opacity zero is drawn"),
    ("P5", W, "  else if (!gvec_lw_compose(frame, &v.transform.map, &c->local)) {", "  else if (!gvec_lw_compose(&v.transform.map, frame, &c->local)) {",
     "a transform composes the wrong way round"),
    ("P6", W, "  if (leaf && !c->visible) {", "  if (leaf && 0 && !c->visible) {",
     "visibility: hidden does not remove a shape"),
    ("P6", W, '  if (r != GVEC_OK || !usable || gvec_lw_keyword_is(&v, "none")) {\n    return r;\n  }\n  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);\n  if (r != GVEC_OK || !usable) {\n    return r;\n  }\n  c->visible',
     '  if (r != GVEC_OK || !usable) {\n    return r;\n  }\n  r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_VISIBILITY, GVEC_LOWER_STYLE, &v, &usable);\n  if (r != GVEC_OK || !usable) {\n    return r;\n  }\n  c->visible',
     "display: none does not remove an element"),
    ("P6", W, "  const GVEC_Svg_Element * selected = gvec_svg_element_switch_selected(e);",
     "  const GVEC_Svg_Element * selected = gvec_svg_element_first_child(e);", "a switch draws its first child"),
    ("K7", M, "  if (!start && !end) {\n    slots[n++] = SLOT_MID;", "  if (!start) {\n    slots[n++] = SLOT_MID;", "the last vertex is a mid as well as an end"),
    ("K7", M, "  if (end) {\n    slots[n++] = SLOT_END;", "  if (end) {\n    slots[n++] = SLOT_START;", "the end marker is the start's"),
    ("K7", M, "  if (start) {\n    slots[n++] = SLOT_START;", "  if (start) {\n    slots[n++] = SLOT_MID;", "the start marker is the mid's"),
    ("K8", M, "slot == SLOT_START ? -1 : 1;", "slot == SLOT_START ? 1 : 1;", "auto-start-reverse does not turn the start"),
    ("K8", M, "double dx = sign * v->direction.x, dy = sign * v->direction.y;", "double dx = sign, dy = 0;", "auto takes no direction from the vertex"),
    ("K8", M, "gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, m->orient.angle, &rotation)", "gshgeom_map_rotate(GSHGEOM_ANGLE_RADIANS, m->orient.angle, &rotation)",
     "an angle is in radians"),
    ("K8", M, "w.scale[slot] = index == 0 ? 1 : stroke_width;", "w.scale[slot] = 1 + 0 * stroke_width;", "markerUnits strokeWidth does not scale"),
    ("K8", M, "w.scale[slot] = index == 0 ? 1 : stroke_width;", "w.scale[slot] = stroke_width;", "markerUnits userSpaceOnUse scales"),
    ("K8", M, "  m->fixed = move;", "  (void) move;\n  gshgeom_map_identity(2, &m->fixed);", "the reference point is not put on the vertex"),
    ("K8", M, "      v = origin + extent * v / 100.0;", "      v = 0 * origin + extent * v / 100.0;", "a reference point's percentage is not of the viewBox from its origin"),
    ("K8", M, "gvec_lw_compose(&step, &m->fixed, &step) && gvec_lw_compose(w->local, &step, out);", "gvec_lw_compose(&step, &m->fixed, &step) && ((*out = step), w->local != NULL);",
     "the shape's own map does not carry its markers"),
    ("K8", M, "  if (!(gvec_lw_keyword_is(&overflow, \"visible\") || gvec_lw_keyword_is(&overflow, \"auto\"))) {", "  if (0) {",
     "a marker never clips"),
    ("K8", M, "  if (!(gvec_lw_keyword_is(&overflow, \"visible\") || gvec_lw_keyword_is(&overflow, \"auto\"))) {", "  if (1) {",
     "a marker with overflow visible clips"),
    ("K8", M, "    r = gvec_lw_rect_path(ctx, 0, 0, w, h, &geometry, &m->clip);", "    r = gvec_lw_rect_path(ctx, 0, 0, h, w, &geometry, &m->clip);",
     "the clip swaps the viewport's sides"),
    ("K9", M, "  ctx->context_fill = w->fill;\n  ctx->context_stroke = w->stroke;", "  ctx->context_fill = w->stroke;\n  ctx->context_stroke = w->fill;",
     "context-fill and context-stroke are swapped"),
    ("K9", M, "  ctx->context_set = true;", "  ctx->context_set = false;", "context paint is not available in a marker"),
    ("K9", M, "  if (w.hits > ctx->caps.marker_instances - (ctx->marker_instances < ctx->caps.marker_instances ? ctx->marker_instances : ctx->caps.marker_instances)) {",
     "  if (w.hits > ctx->caps.marker_instances) {", "the instance limit is not the document's"),
    ("K9", M, "  if (w.hits > ctx->caps.marker_instances - (", "  if (w.hits >= ctx->caps.marker_instances - (", "an element at the instance limit is refused"),
    ("K9", M, "  ctx->marker_instances++;\n", "", "the instances made are not counted"),
    ("K9", M, "    out->refused = true;\n    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, \"the element has more marker instances",
     "    out->refused = false;\n    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, \"the element has more marker instances",
     "an element past the limit is drawn without its markers"),
    ("K9", M, "  if (ctx->levels + 3 > ctx->caps.node_depth || ctx->picture_depth + 1 > ctx->caps.picture_depth) {\n    w->refused = true;",
     "  if (ctx->levels + 3 > ctx->caps.node_depth) {\n    w->refused = true;", "marker pictures nest without a bound"),
    ("K9", W, "    ctx->context_set = true;\n    ctx->context_fill = use_fill;", "    ctx->context_set = false;\n    ctx->context_fill = use_fill;",
     "a use's referent does not see the use's paint"),
    ("X1", LP, "s->large_arc,\n          s->sweep, NULL);", "s->sweep,\n          s->large_arc, NULL);", "an arc's two flags are exchanged"),
    ("X1", LP, "s->rx, s->ry, s->rotation, GSHGEOM_ANGLE_DEGREES", "s->rx, s->ry, -s->rotation, GSHGEOM_ANGLE_DEGREES",
     "an arc's rotation turns the other way"),
    ("X1", LP, "s->rx, s->ry, s->rotation, GSHGEOM_ANGLE_DEGREES", "s->ry, s->rx, s->rotation, GSHGEOM_ANGLE_DEGREES", "an arc's radii are exchanged"),
    ("X1", LP, "    case GVEC_SVG_SEGMENT_ARC: /* An arc to its own start adds nothing (SVG 2, 9.5.1). */\n",
     "    case GVEC_SVG_SEGMENT_ARC:\n      drawn = true;\n", "an arc to its own start is drawn"),
    ("X2", PR, "  if (rx > w / 2) {", "  if (rx > w) {", "a horizontal corner radius is not clamped to half the width"),
    ("X2", PR, "  if (ry > h / 2) {", "  if (ry > h) {", "a vertical corner radius is not clamped to half the height"),
    ("X2", PR, "    rx = ry; /* Both `auto` leaves both at 0: square. */", "    rx = 0;", "an auto rx does not take ry"),
    ("X2", PR, "  if (!(w > 0) || !(h > 0)) {", "  if (!(w > 0) && !(h > 0)) {", "a rect of no width or no height is drawn"),
    ("X2", PR, "      p.r[i].rx = rx;\n      p.r[i].ry = ry;", "      p.r[i].rx = ry;\n      p.r[i].ry = rx;", "a corner's radii are exchanged"),
    ("X3", PR, "      ry = rx;\n      ry_auto = rx_auto;", "      ry = rx * 2;\n      ry_auto = rx_auto;", "a circle is an ellipse of unequal radii"),
    ("P7", W, '  *rule = gvec_lw_keyword_is(&n, "evenodd") ? GSHGEOM_FILL_EVEN_ODD : GSHGEOM_FILL_NONZERO;', "  *rule = GSHGEOM_FILL_NONZERO;",
     "evenodd is nonzero"),
    ("P7", S, "    double alpha = paint->color.color.rgba[3] * opacity;", "    double alpha = paint->color.color.rgba[3] * (opacity * 0 + 1);",
     "fill-opacity and stroke-opacity are not applied"),
    ("P7", S, "    if (paint->color.kind == GVEC_SVG_COLOR_FOREGROUND) {", "    if (0) {", "currentColor is not the foreground"),
    ("K1", S, "  if (!(width > 0) || !(opacity > 0)) {", "  if ((width + opacity) < 0) {", "a stroke that cannot be seen is drawn"),
    ("K1", S, "  style.cap = (uint32_t) index;", "  style.cap = (uint32_t) (index + 1) % 3;", "a cap is named to the waist as the next one"),
    ("K1", S, "  style.join = (uint32_t) index;", "  style.join = (uint32_t) (index + 1) % 5;", "a join is named to the waist as the next one"),
    ("K1", S, "    style.dash_offset = o.kind == GVEC_SVG_STYLE_LENGTH ? o.number : 0;", "    style.dash_offset = (o.kind & 0) ? o.number : 0;",
     "the dash offset is dropped"),
    ("K1", S, "    style.dash_count = w.number_count;", "    style.dash_count = w.number_count > 1 ? w.number_count - 1 : w.number_count;",
     "the last dash is dropped"),
    ("K4", S, "    style.width_space = GSHSCENE_STROKE_WIDTH_DEVICE;", "    style.width_space = GSHSCENE_STROKE_WIDTH_LOCAL;",
     "non-scaling-stroke is a local width"),
    ("K4", S, "  if (fx == EFFECT_OTHER) {", "  if (0) {", "a vector-effect the model does not have is drawn"),
    ("K5", S, "    if (!seen[i] && count < 3) {\n      order[count++]", "    if (!seen[2 - i] && count < 3) {\n      order[count++]",
     "the keywords paint-order leaves out do not follow in the default order"),
    ("K5", S, "        order[count++] = (gvec_lw_part) i;\n      }\n    }\n  }\n  for", "        order[count++] = (gvec_lw_part) (2 - i);\n      }\n    }\n  }\n  for",
     "paint-order is read backwards"),
    ("K6", "src/codec/svg/lower/lower_path.c", "    bool closed = gvec_svg_element_kind(element) == GVEC_SVG_KIND_POLYGON || !for_stroke;",
     "    bool closed = true;", "a polyline is stroked closed"),
    ("K6", "src/codec/svg/lower/lower_path.c", "      drawn = true;\n    }\n    switch (s->kind) {", "      drawn = drawn || 0;\n    }\n    switch (s->kind) {",
     "a subpath of no length is dropped from a stroke"),
    ("K6", W, "  if (parts > 1) {\n    bool usable;\n    r = gvec_lw_paint_order", "  if (0) {\n    bool usable;\n    r = gvec_lw_paint_order",
     "paint-order is not applied"),
    ("K6", W, "    else if (order[i] == GVEC_LW_PART_MARKERS) {", "    else if (0) {", "markers are not drawn"),
    ("P4", W, "        x -= at.x;", "        x += at.x;", "a symbol's reference point is placed on the wrong side of its position"),
    ("P4", W, "      if (!attribute || gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_length(attribute, &length)) {\n        continue;",
     "      if (!attribute || gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_length(attribute, &length)) {\n        length.value = 0;\n        length.unit = GVEC_SVG_UNIT_NONE;",
     "a symbol reference point that is not given is taken as zero"),
    ("H1", GR, "  if (cache->bounding_box) {\n    gvec_lw_geometry geometry;", "  if (!cache->bounding_box) {\n    gvec_lw_geometry geometry;",
     "a gradient in user-space units is placed in the bounding box"),
    ("H1", GR, "!gvec_lw_compose(&placed, &cache->transform, &map)", "!gvec_lw_compose(&cache->transform, &placed, &map)",
     "the gradient transform is applied after the bounding box"),
    ("H1", GR, "!(box.max.y > box.min.y)) {", "!(box.max.y > box.min.y - 1)) {", "a bounding box of no height is a place for a gradient"),
    ("H2", GR, "c->singular = !(det != 0) || det - det != 0;", "c->singular = det - det != 0;", "a gradient transform with no inverse is drawn"),
    ("H3", GR, "c->extend = index == 1 ? GSHSCENE_EXTEND_REFLECT : index == 2 ? GSHSCENE_EXTEND_REPEAT : GSHSCENE_EXTEND_PAD;",
     "c->extend = index == 1 ? GSHSCENE_EXTEND_REPEAT : index == 2 ? GSHSCENE_EXTEND_REFLECT : GSHSCENE_EXTEND_PAD;", "reflect and repeat are exchanged"),
    ("H4", GR, "    offset = clamp01(offset);\n", "    offset = offset + 0;\n", "a stop offset is not clamped"),
    ("H4", GR, "    if (offset < previous) {", "    if (0) {", "a stop offset may go backwards"),
    ("H4", GR, "  if (cache->stop_count == 1) {", "  if (cache->stop_count == 99) {", "a gradient of one stop is drawn as a gradient"),
    ("H4", GR, "out->rgba[3] *= opacity;\n  *decision", "out->rgba[3] *= opacity > 0 ? 1 : 0;\n  *decision", "the opacity of the fill is not applied to a solid stop"),
    ("H5", GR, "for (int hops = 0; e && hops < CHAIN_LIMIT && is_gradient(e); hops++, e = gvec_svg_element_href_target_uncut(e)) {\n    if (same_kind",
     "for (int hops = 0; e && hops < CHAIN_LIMIT && is_gradient(e); hops++, e = NULL) {\n    if (same_kind", "an attribute is read from the gradient itself only"),
    ("H5", GR, "gvec_svg_element_href_target_uncut(e)) {\n    for (const GVEC_Svg_Element * c", "NULL) {\n    for (const GVEC_Svg_Element * c",
     "the stops of a referenced gradient are not inherited"),
    ("H6", GR, "  if (cache->linear_rgb) {", "  if (0) {", "linearRGB interpolation is drawn in sRGB"),
    ("H7", GR, "    g.r0 = fr;", "    g.r0 = 0;", "the start circle has no radius"),
    ("H7", GR, "      fx = cx;", "      fx = 0;", "the focal point does not default to the center"),
    ("H7", GR, "    if (rr == 0) {", "    if (rr == -99) {", "a radius of zero is a radial gradient"),
    ("H7", GR, "    if (rr < 0 || fr < 0) {", "    if (fr < 0) {", "a negative radius is a gradient"),
    ("H8", GR, "  out->rgba[3] = opacity;\n  *decision = opacity > 0", "  out->rgba[3] = opacity > 0 ? 1 : 0;\n  *decision = opacity > 0",
     "the opacity of the fill is not applied to a gradient"),
    ("H8", "src/codec/svg/style/svg_color.c", 'strncasecmp(value->text.data, "currentcolor", value->text.length) == 0) {',
     'strncasecmp(value->text.data, "currentcolor", value->text.length) != 0) {', "a property valued currentColor is not its color"),
    ("H5", "src/codec/svg/svg_api.c", "return e->href_target ? e->href_target : e->cut_target;", "return e->href_target;",
     "the chain of a paint server stops at a reference cycle's cut"),
    ("H9", CL, '"evenodd") ? GSHGEOM_FILL_EVEN_ODD', '"evenoddx") ? GSHGEOM_FILL_EVEN_ODD', "a clip-rule of evenodd is nonzero"),
    ("H9", CL, "gvec_svg_attribute_keyword(units, &index, &word) && index == 1) {", "gvec_svg_attribute_keyword(units, &index, &word) && index == 0) {",
     "clipPathUnits are exchanged"),
    ("H9", CL, "!gvec_lw_compose(&map, &placed, &map)", "!gvec_lw_compose(&placed, &map, &map)", "the bounding box is applied after the clip's own transform"),
    ("H9", CL, "!(box.max.y > box.min.y)) {\n      *state = GVEC_LW_CLIP_ALL;", "!(box.max.y > box.min.y - 1)) {\n      *state = GVEC_LW_CLIP_ALL;",
     "a box of no height is a place for a clip"),
    ("H9", CL, "  if (shapes > 1) {", "  if (shapes > 99) {", "a clipPath of several shapes is clipped to the last"),
    ("H9", CL, "bool shown = !gvec_lw_keyword_is(&v, \"none\") && !gvec_svg_element_conditional_false(child);", "bool shown = !gvec_svg_element_conditional_false(child);",
     "a shape of display none is part of the clip"),
    ("H9", CL, "    *state = GVEC_LW_CLIP_ALL; /* A clip of nothing lets nothing through. */", "    *state = GVEC_LW_CLIP_NONE;",
     "a clip of nothing lets everything through"),
    ("H9", CL, "if (kind == 0 || gvec_svg_element_kind(clip_path) != GVEC_SVG_KIND_CLIP_PATH) {", "if (kind == 0) {", "a reference to what is no clipPath is a clip"),
    ("H10", CL, "gvec_svg_element_kind(nested_target) == GVEC_SVG_KIND_CLIP_PATH && !seen_before(c, nested_target)) {",
     "gvec_svg_element_kind(nested_target) == GVEC_SVG_KIND_CLIP_PATH && 0) {", "a clipPath's own clip-path is ignored"),
    ("H10", CL, "gvec_svg_element_kind(own_target) == GVEC_SVG_KIND_CLIP_PATH && !seen_before(c, own_target)) {",
     "gvec_svg_element_kind(own_target) == GVEC_SVG_KIND_CLIP_PATH && 0) {", "a clip shape's own clip-path is ignored"),
    ("H10", CL, "    if (c->seen[i] == clip_path) {\n      return true;", "    if (c->seen[i] == clip_path) {\n      return false;", "a clip chain that comes back is followed again"),
    ("H10", W, "    r = gvec_lw_attach(ctx, clips[i - 1], clips[i]);", "    r = GVEC_OK;", "the nested clips are not attached to each other"),
    ("H11", CL, "own == -1 || (own == 1 &&", "own == -5 || (own == 1 &&", "a clip shape of several that is clipped by a basic shape is lowered"),
    ("H11", CL, "(own == 1 && gvec_svg_element_kind(own_target) == GVEC_SVG_KIND_CLIP_PATH)) {\n      return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, \"a shape of a clipPath of several",
     "(own == 1 && gvec_svg_element_kind(own_target) == 99)) {\n      return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, \"a shape of a clipPath of several",
     "a clip shape of several that has a clip-path is lowered as if it had none"),
    ("H11", CL, "    if (!drawn) {\n      continue;", "    if (!drawn && 0) {\n      continue;", "a hidden shape adds to the union"),
    ("H11", CL, "    if (geometry != GVEC_LW_GEOMETRY_PATH) {\n      continue;", "    if (geometry != GVEC_LW_GEOMETRY_PATH && 0) {\n      continue;",
     "a shape with no area is a member"),
    ("H11", CL, "    members[count].rule = rule;", "    members[count].rule = GSHGEOM_FILL_NONZERO;", "a member takes no clip-rule of its own"),
    ("H11", CL, "    members[count].map = t;", "    memset(&members[count].map, 0, sizeof members[count].map);", "a member takes no transform of its own"),
    ("H11", CL, "  if (count == 0) {\n    *state = GVEC_LW_CLIP_ALL;", "  if (count == 99) {\n    *state = GVEC_LW_CLIP_ALL;", "a union of nothing lets something through"),
    ("K10", W, "  bool marked = true; /* Every shape has markers (SVG 2, 13.7). */",
     "  bool marked = kind != GVEC_SVG_KIND_RECT;", "a rect has no markers"),
    ("X1", LP, "s->large_arc,\n          s->sweep, NULL);", "s->sweep,\n          s->large_arc, NULL);", "an arc's two flags are exchanged"),
    ("X1", LP, "s->rx, s->ry, s->rotation, GSHGEOM_ANGLE_DEGREES", "s->rx, s->ry, -s->rotation, GSHGEOM_ANGLE_DEGREES",
     "an arc's rotation turns the other way"),
    ("X1", LP, "s->rx, s->ry, s->rotation, GSHGEOM_ANGLE_DEGREES", "s->ry, s->rx, s->rotation, GSHGEOM_ANGLE_DEGREES", "an arc's radii are exchanged"),
    ("X1", LP, "    case GVEC_SVG_SEGMENT_ARC: /* An arc to its own start adds nothing (SVG 2, 9.5.1). */\n",
     "    case GVEC_SVG_SEGMENT_ARC:\n      drawn = true;\n", "an arc to its own start is drawn"),
    ("X2", PR, "  if (rx > w / 2) {", "  if (rx > w) {", "a horizontal corner radius is not clamped to half the width"),
    ("X2", PR, "  if (ry > h / 2) {", "  if (ry > h) {", "a vertical corner radius is not clamped to half the height"),
    ("X2", PR, "    rx = ry; /* Both `auto` leaves both at 0: square. */", "    rx = 0;", "an auto rx does not take ry"),
    ("X2", PR, "  if (!(w > 0) || !(h > 0)) {", "  if (!(w > 0) && !(h > 0)) {", "a rect of no width or no height is drawn"),
    ("X2", PR, "      p.r[i].rx = rx;\n      p.r[i].ry = ry;", "      p.r[i].rx = ry;\n      p.r[i].ry = rx;", "a corner's radii are exchanged"),
    ("X3", PR, "      ry = rx;\n      ry_auto = rx_auto;", "      ry = rx * 2;\n      ry_auto = rx_auto;", "a circle is an ellipse of unequal radii"),
    ("P7", W, '  *rule = gvec_lw_keyword_is(&n, "evenodd") ? GSHGEOM_FILL_EVEN_ODD : GSHGEOM_FILL_NONZERO;', "  *rule = GSHGEOM_FILL_NONZERO;",
     "evenodd is nonzero"),
    ("P7", S, "    double alpha = paint->color.color.rgba[3] * opacity;", "    double alpha = paint->color.color.rgba[3] * (opacity * 0 + 1);",
     "fill-opacity and stroke-opacity are not applied"),
    ("P7", S, "    if (paint->color.kind == GVEC_SVG_COLOR_FOREGROUND) {", "    if (0) {", "currentColor is not the foreground"),
    ("K1", S, "  if (!(width > 0) || !(opacity > 0)) {", "  if ((width + opacity) < 0) {", "a stroke that cannot be seen is drawn"),
    ("K1", S, "  style.cap = (uint32_t) index;", "  style.cap = (uint32_t) (index + 1) % 3;", "a cap is named to the waist as the next one"),
    ("K1", S, "  style.join = (uint32_t) index;", "  style.join = (uint32_t) (index + 1) % 5;", "a join is named to the waist as the next one"),
    ("K1", S, "    style.dash_offset = o.kind == GVEC_SVG_STYLE_LENGTH ? o.number : 0;", "    style.dash_offset = (o.kind & 0) ? o.number : 0;",
     "the dash offset is dropped"),
    ("K1", S, "    style.dash_count = w.number_count;", "    style.dash_count = w.number_count > 1 ? w.number_count - 1 : w.number_count;",
     "the last dash is dropped"),
    ("K4", S, "    style.width_space = GSHSCENE_STROKE_WIDTH_DEVICE;", "    style.width_space = GSHSCENE_STROKE_WIDTH_LOCAL;",
     "non-scaling-stroke is a local width"),
    ("K4", S, "  if (fx == EFFECT_OTHER) {", "  if (0) {", "a vector-effect the model does not have is drawn"),
    ("K5", S, "    if (!seen[i] && count < 3) {\n      order[count++]", "    if (!seen[2 - i] && count < 3) {\n      order[count++]",
     "the keywords paint-order leaves out do not follow in the default order"),
    ("K5", S, "        order[count++] = (gvec_lw_part) i;\n      }\n    }\n  }\n  for", "        order[count++] = (gvec_lw_part) (2 - i);\n      }\n    }\n  }\n  for",
     "paint-order is read backwards"),
    ("K6", "src/codec/svg/lower/lower_path.c", "    bool closed = gvec_svg_element_kind(element) == GVEC_SVG_KIND_POLYGON || !for_stroke;",
     "    bool closed = true;", "a polyline is stroked closed"),
    ("K6", "src/codec/svg/lower/lower_path.c", "      drawn = true;\n    }\n    switch (s->kind) {", "      drawn = drawn || 0;\n    }\n    switch (s->kind) {",
     "a subpath of no length is dropped from a stroke"),
    ("P7", "src/codec/svg/lower/lower_path.c", "      drawn = drawn || s->x != s->x0 || s->y != s->y0;\n      break;\n    case GVEC_SVG_SEGMENT_QUAD:",
     "      drawn = true;\n      break;\n    case GVEC_SVG_SEGMENT_QUAD:", "a zero-length line is filled"),
    ("P7", W, "GVEC_SVG_PROP_FILL, GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, &v, &usable);",
     "GVEC_SVG_PROP_FILL, GVEC_LOWER_STYLE, &v, &usable);", "a colour the waist cannot carry is refused for another reason"),
    ("P8", W, "  if (gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&c.local, &shift, &placement)) {",
     "  if (gshgeom_map_translate(2, 0, 0, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&c.local, &shift, &placement)) {",
     "a use is not moved by its x and y"),
    ("P8", W, "  if (ctx->refused[element->order] & bit) {\n    return GVEC_OK;\n  }", "",
     "an element drawn twice is refused twice"),
    ("P8", W, "  ctx->levels += 2;\n  ctx->picture_depth++;", "  ctx->levels += 1;\n  ctx->picture_depth++;",
     "an instance and its target count as one level"),
    ("P8", "src/codec/svg/lower/lower_emit_list.c", "  return gvec_lw_table_add(ctx, table, use->site, ctx->tracing ? (uint32_t) (picture_table + 1) : 0);",
     "  return gvec_lw_table_add(ctx, table, use->site, 0);", "the trace does not follow an instance into its picture"),
    ("P9", "src/codec/svg/lower/lower_trace.c", "info.has_key && info.key.domain == GVEC_LOWER_KEY_DOMAIN", "info.has_key && 0",
     "an entry of an edit's report is never placed"),
    ("P9", "src/codec/svg/lower/lower_trace.c", "      if (table >= trace->table_count || entry.chain[i] >= trace->tables[table].count) {",
     "      if (table >= trace->table_count) {", "an item the trace does not hold is read past its table"),
    ("P10", W, "  bool fits = saturating_add(parent->weight, weight) <= ctx->caps.expansion &&", "  bool fits = 1; __attribute__((unused)) bool plant_unused = saturating_add(parent->weight, weight) <= ctx->caps.expansion &&",
     "the drawing is not held to the scene's limits"),
    ("P10", W, "  if (ctx->levels + 3 > ctx->caps.node_depth || ctx->picture_depth + 1 > ctx->caps.picture_depth) {",
     "  if (ctx->levels + 3 > ctx->caps.node_depth) {", "pictures nest without a bound"),

    # The typed document (story 5): grammars, build, graph, conditional processing, keys, dump.
    ("T-number", "src/codec/svg/svg_number.c", "  if (flags & GMATH_DECIMAL_OVERFLOW) {", "  if (0) {",
     "a number past the largest double is accepted"),
    ("T-number", "src/codec/svg/svg_number.c", "    if (j == len) {\n      *unit = k_units[i].unit;", "    if (j == len && 0) {\n      *unit = k_units[i].unit;",
     "no unit is recognised"),
    ("T-number", "src/codec/svg/svg_number.c", "    while (j < len && lower((unsigned char) s[start + j]) == (unsigned char) k_units[i].name[j]) {",
     "    while (j < len && (unsigned char) s[start + j] == (unsigned char) k_units[i].name[j]) {",
     "units are case-sensitive"),
    ("T-list", "src/codec/svg/svg_number.c", "    if (comma) {\n      fail(err, comma_at, \"a list may not end in a comma\");",
     "    if (0 && comma) {\n      fail(err, comma_at, \"a list may not end in a comma\");", "a list may end in a comma"),
    ("T-list", "src/codec/svg/svg_number.c", "  size_t keep = count - (count % multiple);", "  size_t keep = count;",
     "an odd point list keeps its dangling coordinate"),
    ("T-list", "src/codec/svg/svg_number.c", "    if (adjacent_negative && s[p] == '-') {", "    if (!adjacent_negative && s[p] == '-') {",
     "a negative coordinate directly after a number is refused in points"),
    ("T-path", "src/codec/svg/svg_path.c", "seg.x1 = last_kind == 'C' ? 2 * cx - lcx : cx;", "seg.x1 = 2 * cx - lcx;",
     "a smooth cubic reflects a control point that is not a cubic's"),
    ("T-path", "src/codec/svg/svg_path.c", "seg.x1 = last_kind == 'Q' ? 2 * cx - lcx : cx;", "seg.x1 = last_kind != 0 ? 2 * cx - lcx : cx;",
     "a smooth quadratic reflects a cubic's control point"),
    ("T-path", "src/codec/svg/svg_path.c", "        seg.x = a[5] + ox;", "        seg.x = a[5];", "an arc's relative end point is not made absolute"),
    ("T-path", "src/codec/svg/svg_path.c", "        seg.kind = GVEC_SVG_SEGMENT_CLOSE;\n        seg.x = sx;",
     "        seg.kind = GVEC_SVG_SEGMENT_CLOSE;\n        seg.x = cx + (sx - sx);", "closepath goes back to the current point"),
    ("T-path", "src/codec/svg/svg_path.c", "    if (first && upper != 'M') {", "    if (0) {", "path data may begin with a lineto"),
    ("T-path", "src/codec/svg/svg_path.c", "if (pos < n && (s[pos] == '0' || s[pos] == '1')) {",
     "if (pos < n && s[pos] >= '0' && s[pos] <= '9') {", "an arc flag may be any digit"),
    ("T-path", "src/codec/svg/svg_path.c", "      if (b.segment_count >= ctx->max_path_segments) {", "      if (b.segment_count > ctx->max_path_segments) {",
     "one segment past the bound is kept"),
    ("T-path", "src/codec/svg/svg_path.c", "        current_letter = relative ? 'l' : 'L';", "        current_letter = 'L';",
     "an implicit repeat after a relative moveto is an absolute lineto"),
    ("T-misc", "src/codec/svg/svg_misc.c", "  if (v[2] < 0 || v[3] < 0) {", "  if (0) {", "a viewBox may have a negative size"),
    ("T-misc", "src/codec/svg/svg_misc.c", "      value.slice = 0;\n      stage = 3;", "      value.slice = 0;\n      stage = 2;",
     "preserveAspectRatio takes meet and slice both"),
    ("T-misc", "src/codec/svg/svg_misc.c", "if (v[i] == '%' && i + 2 < len &&", "if (v[i] == '%' && 0 &&",
     "a fragment is not percent-decoded"),
    ("T-misc", "src/codec/svg/svg_misc.c", "      base64 = 1;\n      head -= 7;", "      base64 = 1;",
     "the base64 marker stays in the parameters"),
    ("T-doc", "src/codec/svg/svg_build.c", "    if (!is_text_reader(e->kind) && all_wsp(data, length)) {", "    if (0) {",
     "white space between the elements of a group is kept"),
    ("T-doc", "src/codec/svg/svg_build.c", "    if (first_time(ctx, \"a:\", a->name)) {", "    if (1) {", "an unknown attribute is noted every time"),
    ("T-doc", "src/codec/svg/svg_build.c", "  if (presentation && strcmp(a->name, \"d\") != 0) {", "  if (0) {",
     "a presentation attribute is parsed as a typed attribute"),
    ("T-doc", "src/codec/svg/svg_build.c", "  if (strcmp(a->name, \"style\") == 0) {", "  if (0) {", "style is a typed string"),
    ("T-doc", "src/codec/svg/svg_build.c", "    if ((view.ns && strcmp(view.ns, GVEC_XMLNS_NS) == 0) || (!view.ns && strcmp(view.local, \"xmlns\") == 0)) {",
     "    if (0) {", "a namespace declaration is kept as an attribute"),
    ("T-doc", "src/codec/svg/svg_build.c", "total > ctx->limits->max_text_bytes", "total >= ctx->limits->max_text_bytes",
     "character data exactly at the bound is refused"),
    ("T-doc", "src/codec/svg/svg_build.c", "  return a->element->order < b->element->order ? -1 : a->element->order > b->element->order ? 1 : 0;",
     "  return a->element->order < b->element->order ? 1 : a->element->order > b->element->order ? -1 : 0;",
     "the last element with an id wins"),
    ("T-graph", "src/codec/svg/svg_graph.c", "      if (reports_missing(e->kind)) {", "      if (0) {", "a missing reference is not reported"),
    ("T-graph", "src/codec/svg/svg_graph.c", "        mark_cycle(diag, path[k]);", "        (void) diag;", "a gradient chain cycle is not marked"),
    ("T-graph", "src/codec/svg/svg_graph.c", "total = sat_add(t->subtree_size, segtree_query(&expansion_sum, lo[u], hi[u]));",
     "total = sat_add(1, segtree_query(&expansion_sum, lo[u], hi[u]));",
     "a use counts one element for its referent's whole subtree"),
    ("T-graph", "src/codec/svg/svg_graph.c", "total = sat_add(t->subtree_size, segtree_query(&expansion_sum, lo[u], hi[u]));",
     "total = sat_add(t->subtree_size, 0 * segtree_query(&expansion_sum, lo[u], hi[u]));",
     "a use does not count what the uses inside its referent instantiate"),
    ("T-graph", "src/codec/svg/svg_graph.c", "    if (u->href_target && u->expansion > limits->max_reference_expansion) {",
     "    if (u->href_target && u->expansion >= limits->max_reference_expansion) {", "a use exactly at the bound is refused"),
    ("T-graph", "src/codec/svg/svg_graph.c", " || inside > 0) {", " || inside < 0) {", "a use inside another use's referent is counted twice in the total"),
    ("T-graph", "src/codec/svg/svg_graph.c", "          if (loop) {\n            cyclic[m] = 1;\n          }", "          (void) loop;",
     "the members of a cycle of several uses are not marked"),
    ("T-graph", "src/codec/svg/svg_graph.c", "        bool loop = members > 1 || cyclic[u];", "        bool loop = cyclic[u];",
     "a component of several uses is not a cycle"),
    ("T-graph", "src/codec/svg/svg_graph.c", "        cyclic[i] = 1; /* a use of itself, or of an ancestor */", "        ;",
     "a use of its own ancestor is not a cycle"),
    ("T-cond", "src/codec/svg/svg_conditional.c", "  return ul == tl || tag[ul] == '-';", "  return 1;",
     "a language tag matches by any prefix"),
    ("T-cond", "src/codec/svg/svg_conditional.c", "        ok = false; /* no extension is supported, and an empty list is false */", "        ;",
     "an extension the library does not support evaluates true"),
    ("T-cond", "src/codec/svg/svg_conditional.c", "      if (is_switch_candidate(c->kind) && !(c->flags & GVEC_SVG_ELEMENT_COND_FALSE)) {",
     "      if (is_switch_candidate(c->kind)) {", "a switch selects a child whose conditions are false"),
    ("T-key", "src/codec/svg/svg_key.c", "  if (!id) {\n    for (int i = 0; i < 4; i++) {", "  if (0) {\n    for (int i = 0; i < 4; i++) {",
     "siblings of one kind get the same key"),
    ("T-dump", "src/codec/svg/svg_dump.c", "if (c >= 0x20 && c <= 0x7e && c != '\\\\' && c != '\"') {", "if (c >= 0x20 && c <= 0x7e) {",
     "the dump does not escape a quote or a backslash"),
    ("T-dump", "src/codec/svg/svg_dump.c", "  if (d.negative) {\n    put(w, \"-\", 1);\n  }", "",
     "the dump drops the sign of a number"),
    # The style of the document (C1 to C14, documentation/design.md).
    ("C3", "src/codec/svg/style/svg_instance.c", "context, target, use_instance, use_instance, use_instance->viewport_width,",
     "context, target, use_instance->parent, use_instance, use_instance->viewport_width,",
     "a use clone inherits from where it is written, not from the use"),
    ("C4", "src/codec/svg/style/svg_instance.c", "  context->style_options.presentation.table = &context->engine->reg.table;\n",
     "  context->style_options.presentation.table = &context->engine->reg.table;\n  context->style_options.presentation.unitless_lengths = false;\n",
     "a unitless number is not a length in an attribute"),
    ("C4", "src/codec/svg/style/svg_instance.c", "  context->style_options.presentation.table = &context->engine->reg.table;\n",
     "  context->style_options.presentation.table = &context->engine->reg.table;\n  context->style_options.sources = GTEXT_CSS_STYLE_STYLE_ATTRIBUTE;\n",
     "presentation attributes are not read"),
    ("C4", "src/codec/svg/style/svg_instance.c", "  bool was = (context->once[kind][bit >> 3] & mask) != 0;", "  bool was = false;",
     "what is dropped is reported every time the style is read"),
    ("C6", "src/codec/svg/style/svg_get.c", "    return w;\n", "    return h;\n", "a percentage of a width is of the height"),
    ("C6", "src/codec/svg/style/svg_get.c", "(w * w + h * h) / 2.0", "(w * w + h * h) / 1.0", "the normalised diagonal is not halved"),
    ("C6", "src/codec/svg/style/svg_instance.c", "  if (usable_view_box(e, &box)) {", "  if (0 && usable_view_box(e, &box)) {",
     "the viewBox is not the basis of percentages"),
    ("C6", "src/codec/svg/style/svg_instance.c", "    if (instance->has_override_width) {", "    if (0) {",
     "a use's width does not size a symbol's viewport"),
    ("C6", "src/codec/svg/style/svg_instance.c", "  if (e->kind == GVEC_SVG_KIND_MARKER) {", "  if (0) {",
     "a marker does not establish a viewport"),
    ("C7", "src/codec/svg/style/svg_color.c",
     "        return refuse(context, where, 0, \"currentColor has no color to be the value of\");\n      }\n",
     "        return refuse(context, where, 0, \"currentColor has no color to be the value of\");\n      }\n      foreground(out, 1.0);\n      return GVEC_OK;\n",
     "currentColor is always the foreground"),
    ("C7", "src/codec/svg/style/svg_color.c", "  return dark && (!light || context->color_scheme == GVEC_COLOR_SCHEME_DARK);",
     "  return context->color_scheme == GVEC_COLOR_SCHEME_DARK;", "the preference decides the color scheme of a document that does not support dark"),
    ("C8", "src/codec/svg/style/svg_color.c", "gmath_pow((a + alpha - 1.0) / alpha, 1.0 / 0.45)", "gmath_pow((a + alpha - 1.0) / alpha, 1.0 / 0.5)",
     "the Rec. 2020 transfer function is wrong"),
    ("C9", "src/codec/svg/style/svg_transform.c", "        ops[i].v[0] *= instance->viewport_width;", "        ops[i].v[0] *= instance->viewport_height;",
     "a percentage translation is of the wrong side of the viewport"),
    ("C9", "src/codec/svg/style/svg_transform.c", "    if (r != GVEC_OK || gshgeom_map_compose(&result, &step, &result) != GSHGEOM_OK) {",
     "    if (r != GVEC_OK || gshgeom_map_compose(&step, &result, &result) != GSHGEOM_OK) {", "a transform list is composed in the wrong order"),
    ("C13", "src/codec/svg/style/svg_ua.c", "    out[n++] = \"transform-origin:0 0\";", "    ;",
     "an SVG element transforms about the center of its box and not about the origin"),
    ("C13", "src/codec/svg/style/svg_ua.c", "  case GVEC_SVG_KIND_DEFS:\n", "  case GVEC_SVG_KIND_IMAGE:\n",
     "a defs element is displayed"),
    ("C13", "src/codec/svg/style/svg_ua.c", "instance->use && instance->parent == instance->use ?", "0 ?",
     "a symbol is not drawn when a use instantiates it"),
    ("C13", "src/codec/svg/style/svg_ua.c", "    out[n++] = \"overflow:hidden\";", "    ;", "a nested svg, a marker and a symbol do not clip"),
    ("C9", "src/codec/svg/style/svg_dash.c", "  if (n % 2 == 1) {", "  if (0) {", "an odd dash array is not repeated"),
    ("C9", "src/codec/svg/style/svg_paint.c",
     "     * the browsers draw none. */\n    if (has_fallback) {", "     * the browsers draw none. */\n    if (0) {",
     "a missing paint server does not take the fallback"),
    ("C10", "src/codec/svg/style/svg_refs.c", "  return c != 0 && c == context->component[to->order];", "  return c == UINT32_MAX && c == context->component[to->order];",
     "the reference that closes a cycle is not cut"),
    ("C11", "src/codec/svg/style/svg_instance.c", "  context->style_options.parse.max_input_bytes = context->limits.max_style_bytes;\n", "",
     "the bound of a style attribute is not applied"),
    ("C11", "src/codec/svg/style/svg_instance.c", "  context->style_options.parse.max_declarations = context->limits.max_declarations;\n", "",
     "the bound of declarations is not applied"),
    ("C3", "src/codec/svg/style/svg_instance.c", "      context->instances >= bound) {", "      context->instances > bound) {",
     "one instance past the bound is made"),
    ("C14", "src/codec/svg/style/svg_engine.c", "    if (e->viewport_width == w && e->viewport_height == h && e->color_scheme == scheme &&",
     "    if ((e->viewport_width == w || 1) && e->viewport_height == h && e->color_scheme == scheme &&", "an engine is shared by environments that differ in width"),
    ("C2", "src/codec/svg/style/svg_prop_tables.c",
     "{\"opacity\", GVEC_SVG_PT_NUMBER, GVEC_SVG_BASIS_NONE, GVEC_SVG_SOURCE_GENERATED, false, \"opacity\", NULL, NULL, NULL},",
     "{\"opacity\", GVEC_SVG_PT_NUMBER, GVEC_SVG_BASIS_NONE, GVEC_SVG_SOURCE_REGISTERED, true, \"opacity\", \"1\", \"<number>\", \"planted\"},",
     "a property the pin has is registered again"),
    ("C2", "src/codec/svg/style/svg_prop_tables.c", "GVEC_SVG_SOURCE_REGISTERED, true, \"color-rendering\", \"auto\",",
     "GVEC_SVG_SOURCE_GENERATED, true, \"color-rendering\", \"auto\",", "a property the pin lacks is taken to be the pin's"),
    ("C12", "src/codec/svg/style/svg_get.c", "    out_specified->important = entry.important;", "    out_specified->important = false;",
     "an important declaration is not said to be"),
]

# The root check of L8 is unreachable while the probe is correct (the probe
# refuses every document whose root it would refuse), so a plant that removes
# it alone cannot be caught by any test. It is planted together with the probe
# defect it guards against, and must then be caught.
PAIRED = [
    ("L8", [("src/codec/svg/svg_load.c", '  if (!root || !gtext_markup_element_is(root, GTEXT_MARKUP_NS_SVG, "svg")) {',
             "  if (!root) {"),
            ("src/codec/svg/svg_codec.c",
             "if (event.namespace_uri && strcmp(event.namespace_uri, GTEXT_MARKUP_NS_SVG) == 0 && event.local_name &&",
             "if (event.local_name &&")],
     "the root check removed, behind a probe that accepts any svg"),
    # The inflation cap is enforced twice, where the buffer grows and on the
    # final size, each covering the other; loosening one alone is caught by
    # neither test. Loosened together (by 4 KiB), the cap must be seen to fail.
    ("L6", [("src/codec/transport.c", "  if (out->capacity > out->max) {", "  if (out->capacity > out->max + 4096) {"),
            ("src/codec/transport.c", "  if (grown > out->max + 1) {\n    grown = out->max + 1;",
             "  if (grown > out->max + 4096) {\n    grown = out->max + 4096;"),
            ("src/codec/transport.c", "  if (r == GVEC_OK && out->size > out->max) {", "  if (r == GVEC_OK && out->size > out->max + 4096) {")],
     "the inflation cap is 4 KiB too loose"),
]


def env(name):
    value = os.environ.get(name)
    if value is None:
        sys.exit("check-planted: %s is not set; run this through make check-planted" % name)
    return value


def run(cmd, timeout=None, **kw):
    # Its own process group, so that a run that is cut off at the timeout takes everything it started with it: killing only the
    # shell leaves the runaway test spinning on a core for as long as the machine is up.
    proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True, **kw)
    try:
        out, _ = proc.communicate(timeout=timeout)
        return subprocess.CompletedProcess(cmd, proc.returncode, out, None)
    except subprocess.TimeoutExpired:
        # A planted defect that makes a test or a replay run away has been caught: it is a failure, not a crash of this script.
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        out, _ = proc.communicate()
        return subprocess.CompletedProcess(cmd, 124, (out or "") + "\ntimed out", None)


def build_and_test(work, edits):
    """Build the library with `edits` (list of (file, old, new)) and run everything.

    Returns ("pass" | "fail" | "broken", log). A build that does not compile or
    link is "broken": it says nothing about the tests, so it is never counted
    as a plant caught."""
    os.makedirs(work, exist_ok=True)
    cc, cflags = env("PLANT_CC"), env("PLANT_CFLAGS")
    obj_dir = env("PLANT_OBJ_DIR")
    replaced = {}
    for path, old, new in edits:
        source = replaced.get(path)
        if source is None:
            with open(os.path.join(ROOT, path)) as h:
                source = h.read()
        count = source.count(old)
        if count != 1:
            sys.exit("check-planted: the text to plant in %s occurs %d times, not once:\n%s" % (path, count, old))
        replaced[path] = source.replace(old, new)
    objects = []
    for root, _, files in os.walk(obj_dir):
        if "/tests" in root[len(obj_dir):]:
            continue
        for f in files:
            if f.endswith(".o"):
                objects.append(os.path.join(root, f))
    swapped = {}
    for path, source in replaced.items():
        rel = os.path.relpath(path, "src")
        planted_src = os.path.join(work, rel)
        os.makedirs(os.path.dirname(planted_src), exist_ok=True)
        with open(planted_src, "w") as h:
            h.write(source)
        obj = os.path.join(work, rel[:-2] + ".o")
        # Compile from the original directory's view of relative includes.
        inc = "-I" + os.path.join(ROOT, os.path.dirname(path))
        r = run("%s %s %s -c %s -o %s" % (cc, cflags, inc, planted_src, obj), cwd=ROOT)
        if r.returncode != 0:
            return "broken", "compile failed:\n" + r.stdout
        swapped[os.path.normpath(os.path.join(obj_dir, rel[:-2] + ".o"))] = obj
    if os.environ.get("PLANT_BUILD_ONLY"):
        return "fail", "compiled"  # a dry run: do the plants apply and build, without the (long) tests
    objects = [swapped.get(os.path.normpath(o), o) for o in objects]
    archive = os.path.join(work, "libplanted.a")
    if os.path.exists(archive):
        os.remove(archive)
    r = run("ar rcs %s %s" % (archive, " ".join(objects)))
    if r.returncode != 0:
        return "broken", "ar failed:\n" + r.stdout
    cxx, ldflags = env("PLANT_CXX"), env("PLANT_LDFLAGS")
    ld_path = env("PLANT_LD_PATH")
    log = []
    failed = False
    for t in env("PLANT_TEST_OBJS").split():
        exe = os.path.join(work, os.path.basename(t)[:-2])
        r = run("%s -o %s %s -Wl,--whole-archive %s -Wl,--no-whole-archive %s" % (cxx, exe, t, archive, ldflags),
                cwd=ROOT)
        if r.returncode != 0:
            return "broken", "link failed:\n" + r.stdout
        r = run("LD_LIBRARY_PATH='%s' %s --gtest_brief=1" % (ld_path, exe), cwd=ROOT, timeout=600)
        log.append("%s: exit %d\n%s" % (os.path.basename(exe), r.returncode, r.stdout[-3000:]))
        if r.returncode != 0:
            failed = True
    for target in ("load", "style"):
        replay = os.path.join(work, "replay_" + target)
        r = run("%s %s -Itests/fuzz -o %s tests/fuzz/fuzz_%s.c tests/fuzz/replay_main.c %s %s"
                % (env("PLANT_CC"), env("PLANT_CCFLAGS"), replay, target, archive, ldflags), cwd=ROOT)
        if r.returncode != 0:
            return "broken", "replay link failed:\n" + r.stdout
        directory = os.path.join("tests/fuzz/corpus", target)
        corpus = sorted(os.path.join(directory, f) for f in os.listdir(os.path.join(ROOT, directory)))
        r = run("LD_LIBRARY_PATH='%s' %s %s" % (ld_path, replay, " ".join(corpus)), cwd=ROOT, timeout=600)
        log.append("replay %s: exit %d\n%s" % (target, r.returncode, r.stdout[-2000:]))
        if r.returncode != 0:
            failed = True
    return ("fail" if failed else "pass"), "\n".join(log)


def main():
    work = env("PLANT_WORK")
    shutil.rmtree(work, ignore_errors=True)
    if not os.environ.get("PLANT_BUILD_ONLY"):
        status, log = build_and_test(os.path.join(work, "control"), [])
        if status != "pass":
            print("check-planted: the control (no plant) fails, so no plant can be judged:\n" + log, file=sys.stderr)
            return 1
        print("  ok   control: the unplanted library passes every test and the replay")
    missed = 0
    cases = [(rule, [(path, old, new)], what) for rule, path, old, new, what in PLANTS] + PAIRED
    only = os.environ.get("PLANT_ONLY")  # a rule prefix, to rerun the plants of one rule while writing them
    if only:
        cases = [c for c in cases if c[0].startswith(only)]
    for n, (rule, edits, what) in enumerate(cases):
        status, log = build_and_test(os.path.join(work, "plant-%02d" % n), edits)
        if status == "broken":
            print("check-planted: the plant %s (%s) does not build:\n%s" % (rule, what, log), file=sys.stderr)
            return 1
        if status == "pass":
            missed += 1
            print("check-planted: MISSED %s: %s" % (rule, what), file=sys.stderr)
        else:
            print("  ok   %-4s %s: caught" % (rule, what))
    if missed:
        print("check-planted: %d of %d plants were not caught" % (missed, len(cases)), file=sys.stderr)
        return 1
    print("check-planted: all %d plants caught, control passes" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
