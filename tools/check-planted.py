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
#   PLANT_FILES                 (optional) comma-separated source paths: only the plants that edit one of them
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
BS = "src/codec/svg/lower/lower_basic_shape.c"
# Text.
TX = "src/codec/svg/lower/lower_text.c"

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
    ("P4", W, "the viewport is past the range the waist can carry\");\n    }\n    clip = gvec_lw_new_node(ctx, GVEC_LW_CLIP);\n    if (!clip) {\n      return GVEC_ERR_OOM;\n    }\n    clip->map = c.local;\n",
     "the viewport is past the range the waist can carry\");\n    }\n    clip = gvec_lw_new_node(ctx, GVEC_LW_CLIP);\n    if (!clip) {\n      return GVEC_ERR_OOM;\n    }\n",
     "the viewport clip is not placed in the parent's space"),
    ("P5", W, "  if (!(opacity < 1) && c->composite == 0 && !c->isolate) {\n    return GVEC_OK;", "  if (!(opacity <= 1) && c->composite == 0 && !c->isolate) {\n    return GVEC_OK;",
     "a group is made for every element"),
    ("P5", W, "  c->draw = c->opacity > 0; /* Opacity zero draws nothing, whatever is inside. */", "  c->draw = true;",
     "an element with opacity zero is drawn"),
    ("P5", W, "  else if (!gvec_lw_compose(frame, &own_map, &c->local)) {", "  else if (!gvec_lw_compose(&own_map, frame, &c->local)) {",
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
    ("K8", M, "double dx = sign * dir.x, dy = sign * dir.y;", "double dx = sign + 0 * dir.x, dy = 0 * dir.y;", "auto takes no direction from the vertex"),
    ("K8", M, "gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, m->orient.angle, &rotation)", "gshgeom_map_rotate(GSHGEOM_ANGLE_RADIANS, m->orient.angle, &rotation)",
     "an angle is in radians"),
    ("K8", M, "w.scale[slot] = index == 0 ? 1 : stroke_width;", "w.scale[slot] = 1 + 0 * stroke_width;", "markerUnits strokeWidth does not scale"),
    ("K8", M, "w.scale[slot] = index == 0 ? 1 : stroke_width;", "w.scale[slot] = stroke_width;", "markerUnits userSpaceOnUse scales"),
    ("K8", M, "  m->fixed = move;", "  (void) move;\n  gshgeom_map_identity(2, &m->fixed);", "the reference point is not put on the vertex"),
    ("K8", M, "      v = origin + extent * v / 100.0;", "      v = 0 * origin + extent * v / 100.0;", "a reference point's percentage is not of the viewBox from its origin"),
    ("K8", M, "((w->step = step), true) &&\n         gvec_lw_compose(w->local, &step, out);", "((w->step = step), true) &&\n         ((*out = step), w->local != NULL);",
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
    ("H26", TX, "      anchor = 1.0 - anchor;", "      anchor = anchor;", "the start of right-to-left text is its left edge"),
    ("H27", TX, "(out->sideways ? 90.0 : 0.0)", "(out->sideways ? 0.0 : 0.0)", "a letter of vertical text does not lie on its side"),
    ("H27", TX, "    if (t->vertical && baseline == 0) {", "    if (t->vertical && baseline == 99) {", "vertical text is not set on its central baseline"),
    ("H28", TX, "!(mid >= 0 && mid <= length)", "!(mid >= 0 && mid <= length + 1e9)", "a glyph past the end of the path is drawn"),
    ("H29", TX, "      add_characters(t, buffer, length, preserve, style);", "      (void) buffer;", "a tref has no characters"),
    ("H31", TX, "      size = size0 * st->size_adjust / aspect;", "      size = size0 + 0 * aspect;", "font-size-adjust is ignored"),
    ("H32", TX, "(tl->length - tl->natural) / (double) (tl->total - 1)", "(tl->length - tl->natural) / (double) tl->total", "a length over several chunks is shared out over one gap too many"),
    ("H33", TX, "        if (t.vertical) {\n          const double down[4]", "        if (t.vertical && false) {\n          const double down[4]", "a stretch of vertical text is along the horizontal"),
    ("H34", TX, "- (t.vertical ? 90.0 : 0.0);", "- 0.0;", "vertical text on a path is turned a quarter too far"),
    ("H35", CL, "text_leaf = ck == GVEC_SVG_KIND_TEXT;", "text_leaf = ck == GVEC_SVG_KIND_TEXT && false;", "a group that holds text has no bounding box"),
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
    ("P4", W, "      if (!ref_attribute || gvec_svg_attribute_class(ref_attribute) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_length(ref_attribute, &length)) {\n        continue;",
     "      if (!ref_attribute || gvec_svg_attribute_class(ref_attribute) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_length(ref_attribute, &length)) {\n        length.value = 0;\n        length.unit = GVEC_SVG_UNIT_NONE;",
     "a symbol reference point that is not given is taken as zero"),
    ("H1", GR, "  if (cache->bounding_box) {\n    GSHGEOM_Bounds box;", "  if (!cache->bounding_box) {\n    GSHGEOM_Bounds box;",
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
    ("H9", CL, "return gvec_svg_attribute_keyword(attribute, &which, &name) && which == 1;", "return gvec_svg_attribute_keyword(attribute, &which, &name) && which == 0;",
     "clipPathUnits are exchanged"),
    ("H9", CL, "gvec_lw_compose(map, &placed, out);", "gvec_lw_compose(&placed, map, out);", "the bounding box is applied after the clip's own transform"),
    ("H9", CL, "!(box->max.y > box->min.y)) {\n      *all = true; /* A box of no area", "!(box->max.y > box->min.y - 1)) {\n      *all = true; /* A box of no area",
     "a box of no height is a place for a clip"),
    ("H11", CL, "    if (box->empty || !(box->max.x > box->min.x) || !(box->max.y > box->min.y)) {", "    if (box->empty || !(box->max.x > box->min.x) || !(box->max.y > box->min.y - 1)) {",
     "a group box of no height is a place for a clip"),
    ("H9", CL, "  if (shapes > 1 || list[0].preset || list[0].use_inst) {", "  if (shapes > 99 || list[0].preset || list[0].use_inst) {", "a clipPath of several shapes is clipped to the last"),
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
    ("H11", CL, "      t.count = a->terms[i].count + b->terms[j].count;", "      t.count = a->terms[i].count;", "a clip of a clip shape loses the second of its parts"),
    ("H11", CL, "      members[count].inner_count = t->count - 1;", "      members[count].inner_count = 0;", "a clip shape of several that has a clip-path is lowered as if it had none"),
    ("H11", CL, "    const GVEC_Svg_Instance * inst = which == 0 ? own_inst : ch->use_inst;", "    const GVEC_Svg_Instance * inst = which == 0 ? own_inst : NULL;", "a use child's own clip-path is ignored"),
    ("H6", GR, "g.space.transfer = GCOL_TRANSFER_LINEAR;", "g.space.transfer = GCOL_TRANSFER_SRGB;", "linearRGB interpolates in sRGB"),
    ("H6", GR, "  if (cache->linear_rgb) {\n    /* The stops", "  if (0 && cache->linear_rgb) {\n    /* The stops", "linearRGB is ignored"),
    ("H4", GR, "if (count > limit) {", "if (count > limit + 1) {", "one stop past the limit is drawn"),
    ("H11", BS, "if (top > w) {", "if (top > w * 2) {", "corners that overlap are not scaled"),
    ("H11", BS, "(b->w * b->w + b->h * b->h) / 2.0", "(b->w * b->w + b->h * b->h) / 1.0", "a percentage radius is of the wrong diagonal"),
    ("H11", BS, "  out->rule = GSHGEOM_FILL_NONZERO;\n  fill_rule_word(c, &out->rule);\n  size_t capacity", "  out->rule = GSHGEOM_FILL_NONZERO;\n  size_t capacity", "a polygon's fill rule is ignored"),
    ("H11", BS, "out->ox = b->x;", "out->ox = 0;", "a path() is not placed at the box origin"),
    ("H11", BS, "(v.x != 0 || v.y != 0)", "(false)", "a view box that does not start at the origin is used"),
    ("H11", BS, "if (!(radius > 0)) {", "if (radius < 0) {", "a circle of no radius has an area"),
    ("H11", BS, "sum = op == '+' ? sum + term : sum - term;", "sum = op == '+' ? sum - term : sum + term;", "calc() adds what it should subtract"),
    ("H11", BS, "      return BOX_REFUSED; /* it holds what is not measured (a foreignObject) */", "      return BOX_NONE; /* it holds what is not measured (a foreignObject) */", "a group that holds what is not measured is given a box"),
    ("H11", CL, "    if (!drawn) {\n      continue;", "    if (!drawn && 0) {\n      continue;", "a hidden shape adds to the union"),
    ("H11", CL, "    if (geometry != GVEC_LW_GEOMETRY_PATH) {\n      return GVEC_OK;", "    if (geometry != GVEC_LW_GEOMETRY_PATH && 0) {\n      return GVEC_OK;",
     "a shape with no area is a member"),
    ("H11", CL, "      members[count].rule = t->clips[0].rule;", "      members[count].rule = GSHGEOM_FILL_NONZERO;", "a member takes no clip-rule of its own"),
    ("H11", CL, "      members[count].map = t->clips[0].map;", "      memset(&members[count].map, 0, sizeof members[count].map);", "a member takes no transform of its own"),
    ("H11", CL, "  if (count == 0) {\n    *state = GVEC_LW_CLIP_ALL;", "  if (count == 99) {\n    *state = GVEC_LW_CLIP_ALL;", "a union of nothing lets something through"),
    ("H11", CL, "      if (tk != GVEC_SVG_KIND_TEXT && !is_shape(tk)) {\n        continue;", "      if (tk != GVEC_SVG_KIND_TEXT && !is_shape(tk)) {\n        *refused = true;\n        return gvec_lw_refuse(ctx, e, GVEC_LOWER_CLIP_PATH, \"planted\");",
     "a use of a group refuses the clip instead of adding nothing"),
    ("H11", CL, "gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&t, &shift, &placed)",
     "gshgeom_map_translate(2, x * 0, y, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&t, &shift, &placed)", "a use in a clip ignores its x"),
    ("H11", CL, "if (gvec_lw_keyword_is(&v, \"none\") || gvec_svg_element_conditional_false(child)) {\n      continue;",
     "if (0 || gvec_svg_element_conditional_false(child)) {\n      continue;", "a shape with display none is in the box of a group"),
    ("H11", CL, "  if (map->m[0][1] == 0 && map->m[1][0] == 0) {\n    GSHGEOM_Point lo, hi;", "  if (1) {\n    GSHGEOM_Point lo, hi;", "a rotated shape is boxed by moving its box"),
    ("H12", GR, "      if (paint->has_fallback) {", "      if (false && paint->has_fallback) {", "a gradient on a box of no area ignores its fallback paint"),
    ("H14", W, '    ctx->item_crisp = gvec_lw_keyword_is(&sr, "crispedges") || gvec_lw_keyword_is(&sr, "optimizespeed");', '    ctx->item_crisp = gvec_lw_keyword_is(&sr, "crispedges");', "optimizeSpeed asks for no antialiasing"),
    ("H14", W, '    ctx->item_crisp = gvec_lw_keyword_is(&sr, "crispedges") || gvec_lw_keyword_is(&sr, "optimizespeed");', '    ctx->item_crisp = gvec_lw_keyword_is(&sr, "optimizespeed");', "crispEdges asks for no antialiasing"),
    ("H14", W, "  node->crisp = ctx->item_crisp;", "  node->crisp = false;", "a shape with crispEdges is antialiased"),
    ("H14", "src/codec/svg/lower/lower_emit_list.c", "&n->paint, n->crisp ? GSHSCENE_ANTIALIAS_NONE : GSHSCENE_ANTIALIAS_AREA, 0, 0.0), &skipped);\n      if (r == GVEC_OK && !skipped) {\n        r = gvec_lw_table_add(ctx, table, n->site, 0);\n      }\n      break;\n    case GVEC_LW_GROUP", "&n->paint, GSHSCENE_ANTIALIAS_AREA, 0, 0.0), &skipped);\n      if (r == GVEC_OK && !skipped) {\n        r = gvec_lw_table_add(ctx, table, n->site, 0);\n      }\n      break;\n    case GVEC_LW_GROUP", "a crisp stroke is recorded antialiased"),
    ("H14", "src/codec/svg/lower/lower_emit_list.c", "n->rule, &n->paint, n->crisp ? GSHSCENE_ANTIALIAS_NONE", "n->rule, &n->paint, false ? GSHSCENE_ANTIALIAS_NONE", "a crisp fill is recorded antialiased"),
    ("H15", GR, "    if (paint->has_fallback && ctx->options && (ctx->options->flags & GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK)) {", "    if (0 && paint->has_fallback && ctx->options && (ctx->options->flags & GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK)) {", "a gradient with no stops ignores the flag and its fallback"),
    ("H15", GR, "    if (paint->has_fallback && ctx->options && (ctx->options->flags & GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK)) {", "    if (paint->has_fallback && ctx->options && !(ctx->options->flags & GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK)) {", "a gradient with no stops takes its fallback without being asked"),
    ("H16", W, "    if (t && n > 1 && t[0] == '-' && ((t[1] >= '0' && t[1] <= '9') || t[1] == '.')) {", "    if (0 && t && n > 1 && t[0] == '-' && ((t[1] >= '0' && t[1] <= '9') || t[1] == '.')) {", "a negative size of the root draws the document"),
    ("H13", W, "        *height = no_h ? box.max.y : *height;", "        *height = no_h ? box.max.x : *height;", "a document with no height takes its width from the drawing"),
    ("H13", W, "        *width = no_w ? box.max.x : *width;", "        *width = *width;", "a document with no width is not as wide as its drawing"),
    ("H12", CL, "    if (g == GSHGEOM_ERR_UNSUPPORTED) {", "    if (g == GSHGEOM_ERR_UNSUPPORTED && 0) {", "a path with an arc is not bounded under a map that turns it"),
    ("H12", M, "      if (slot == SLOT_START && (v->flags & GSHGEOM_VERTEX_HAS_OUT)) {", "      if (slot == SLOT_END && (v->flags & GSHGEOM_VERTEX_HAS_OUT)) {", "the start of a closed path is not oriented along its first segment when asked"),
    ("H12", M, "      else if (slot == SLOT_END && (v->flags & GSHGEOM_VERTEX_HAS_IN)) {", "      else if (0 && slot == SLOT_END && (v->flags & GSHGEOM_VERTEX_HAS_IN)) {", "the end of a closed path is not oriented along its closing segment when asked"),
    ("H12", M, "(w->ctx->options->flags & GVEC_LOWER_MARKER_CLOSED_ENDS_OPEN) && (v->flags", "(w->ctx->options->flags & GVEC_LOWER_MARKER_CLOSED_ENDS_OPEN) == 0 && (v->flags", "the ends of a closed path are oriented as an open path's by default instead of when asked"),
    ("H12", M, "  ctx->context_inst = w->inst;\n  if (w->inst &&", "  ctx->context_inst = NULL;\n  if (w->inst &&", "a gradient context paint in a marker is refused"),
    ("H12", M, "!gvec_lw_invert(&w->step, &ctx->context_back)", "!(ctx->context_back = w->step, true)", "the marker's placement is not undone for a context gradient"),
    ("H12", S, "gvec_lw_compose(&own_back, &ctx->context_back, &into)", "gvec_lw_compose(&ctx->context_back, &own_back, &into)", "a context gradient undoes the shape's map and the marker's in the wrong order"),
    ("H12", W, "        ctx->context_box.min.x += x;", "        ctx->context_box.min.x -= x;", "the box of a use is not moved by its x"),
    ("H12", GR, "      box = *given;", "      box = *given;\n      box.empty = 1;", "a given box is taken as empty"),
    ("H12", W, "  double det = a * d - b * c;", "  double det = a * d + b * c;", "an inverse has the wrong determinant"),
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
    # The filter lowering (lower_filter.c): the primitives' fields, the inputs, the order of the wrappers, href, and the CSS functions.
    ("F1", "src/codec/svg/lower/lower_filter.c", "      p->kind = GSHSCENE_FILTER_OFFSET;\n", "      p->kind = GSHSCENE_FILTER_FLOOD;\n", "an feOffset is lowered as a flood"),
    ("F2", W, "    ctx->opacity_moved = inst;\n", "", "the element's opacity group stays inside its filter, so it fades what the filter reads"),
    ("F3", "src/codec/svg/lower/lower_filter.c", "        v2[0] = v2[1] = 0; /* a negative deviation disables the effect: the result is the input (Filter Effects) */\n",
     "        b->invalid = true;\n        return GVEC_OK;\n", "a negative deviation makes the element not rendered instead of passing the input"),
    ("F4", "src/codec/svg/lower/lower_filter.c", "  for (int hops = 0; e && hops < 16; hops++, e = href_of(ctx, e)) {\n    if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_FILTER && gvec_svg_element_find_attribute(e, name)) {",
     "  for (int hops = 0; e && hops < 1; hops++, e = href_of(ctx, e)) {\n    if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_FILTER && gvec_svg_element_find_attribute(e, name)) {", "a filter does not take its attributes from the filter its href names"),
    ("F5", "src/codec/svg/lower/lower_filter.c", "    in->kind = GSHSCENE_FILTER_INPUT_SOURCE_ALPHA;\n    return GVEC_OK;\n  }\n  if (word_is(s, n, \"BackgroundImage\")",
     "    in->kind = GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC;\n    return GVEC_OK;\n  }\n  if (word_is(s, n, \"BackgroundImage\")", "SourceAlpha names the source graphic"),
    ("F6", "src/codec/svg/lower/lower_filter.c", "        p->transfer[c].intercept = text[0] == 'c' ? 0.5 - 0.5 * v : 0.0;", "        p->transfer[c].intercept = 0.0;", "contrast() has no intercept"),
    ("F8", "src/codec/svg/lower/lower_filter.c", "  p->color_space = GSHSCENE_FILTER_SPACE_SRGB; /* the functions work in sRGB */\n", "", "a filter function works in linearRGB"),
    # A filter on an element that paints nothing (lower_walk.c): the element is rendered, so its filtered group is drawn; one that is not rendered has none.
    ("F9", W, "    filter_content->content = true;\n", "    (void) filter_content;\n", "an element that paints nothing loses its filter, so a flood or a tile of one is not drawn"),
    ("F10", W, "if (filtered == GVEC_LW_FILTER_NODE && ctx->filter_rendered) {", "if (filtered == GVEC_LW_FILTER_NODE) {",
     "every filtered element has its group drawn, rendered or not (display none, opacity zero, a rect of no width)"),
    ("F11", W, "  if (geometry == GVEC_LW_GEOMETRY_PATH) {\n    mark_rendered(ctx, inst);", "  if (true) {\n    mark_rendered(ctx, inst);",
     "a shape of no geometry is rendered, so a rect of no width has a filtered result"),
    ("F12", W, "  if (leaf && !c->visible) {\n    return GVEC_OK;\n  }\n  /* The features of the later stories", "  if (false && leaf && !c->visible) {\n    return GVEC_OK;\n  }\n  /* The features of the later stories",
     "a hidden shape is rendered, so it has a filtered result"),
    ("F13", W, "  ctx->filter_owner = saved_owner;\n  ctx->filter_rendered = saved_rendered;\n", "  ctx->filter_owner = saved_owner;\n  (void) saved_rendered;\n",
     "a child's filter forgets what its parent was: a parent that is rendered loses its group when the child is not"),
    ("F14", W, "  mark_rendered(ctx, inst);\n  gvec_lw_node * group;\n  bool skip;\n  r = opacity_group(ctx, gvec_svg_instance_element(inst), &c, &group, &skip);\n  if (r != GVEC_OK || skip) {\n    return r;\n  }\n  r = gvec_lw_children(",
     "  gvec_lw_node * group;\n  bool skip;\n  r = opacity_group(ctx, gvec_svg_instance_element(inst), &c, &group, &skip);\n  if (r != GVEC_OK || skip) {\n    return r;\n  }\n  r = gvec_lw_children(",
     "a group is not rendered when nothing is inside it, so an empty group has no filtered result"),
    ("F15", W, "  if (r == GVEC_OK) {\n    mark_rendered(ctx, inst);\n    r = gvec_lw_attach(ctx, TARGET(group, parent), use);", "  if (r == GVEC_OK) {\n    r = gvec_lw_attach(ctx, TARGET(group, parent), use);",
     "a use of an unpainted shape is not rendered, so its filter draws nothing"),
    ("FD1", "src/codec/svg/lower/lower_filter.c", "    double dev[2] = {len[2], len[2]};", "    double dev[2] = {len[2] / 2.0, len[2] / 2.0};",
     "the blur of a drop-shadow() is halved as if it were box-shadow's radius"),
    ("FD2", "src/codec/svg/lower/lower_filter.c", "    if (current) {\n      GVEC_Result cr = element_color(b, rgba);", "    if (current && !current) {\n      GVEC_Result cr = element_color(b, rgba);",
     "a drop-shadow() with no color, or with currentcolor, is black and not the element's color"),
    ("FD4", "src/codec/svg/lower/lower_filter.c", "      current = is_current_color(args + pos, end - pos);\n", "      current = false;\n",
     "a drop-shadow() whose first argument is currentcolor does not take the element's color"),
    ("FM1", "src/codec/svg/lower/lower_filter.c", "      if (merge_count == 0) { /* no feMergeNode", "      if (merge_count == 99) { /* no feMergeNode",
     "an feMerge with no nodes is not transparent black"),
    ("FM2", "src/codec/svg/lower/lower_filter.c", "            continue; /* a list that does not parse is no list: the function is the identity",
     "            b->invalid = true; return GVEC_OK; /* a list that does not parse is no list: the function is the identity",
     "a tableValues that does not parse makes the filter an error and not the identity"),
    ("FM3", "src/codec/svg/lower/lower_filter.c", "        if (lt->spot_exponent < 0) {\n          lt->spot_exponent = 1;", "        if (lt->spot_exponent < 0) {\n          return false;",
     "a spot light's negative specularExponent is an error and not the default"),
    ("FM4", "src/codec/svg/lower/lower_filter.c", "      *state = GVEC_LW_FILTER_NONE;\n      return GVEC_OK;\n    }\n    targets[k] = target;", "      *state = GVEC_LW_FILTER_ALL;\n      return GVEC_OK;\n    }\n    targets[k] = target;",
     "a url() that names no filter makes the element not rendered and does not ignore the chain"),
    ("FM5", "src/codec/svg/lower/lower_filter.c", ": x0 + b->region[2]; /* a size not given", ": b->region[0] + b->region[2]; /* a size not given",
     "an feImage's width not given runs to the region's edge and not the region's width from x"),
    ("TX1", "src/codec/svg/lower/lower_clip.c", "  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_TEXT) {\n    /* The box of the text is the cells",
     "  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_TEXT && !e) {\n    /* The box of the text is the cells",
     "a text has no box of its own for a filter in the units of its box, so it draws nothing"),
    ("TX2", "src/codec/svg/lower/lower_text.c", "    const double x1 = glyphs[g].cell_x1 - (along_y ? 0 : glyphs[g].trail);",
     "    const double x1 = glyphs[g].cell_x1 - 0 * (along_y ? 0 : glyphs[g].trail);", "the box of a text holds the letter-spacing after its last glyph"),
    ("TX3", "src/codec/svg/lower/lower_text.c", "    const double y1 = glyphs[g].cell_y1 - (along_y ? glyphs[g].trail : 0);",
     "    const double y1 = glyphs[g].cell_y1 - 0 * (along_y ? glyphs[g].trail : 0);", "the box of a vertical text holds the letter-spacing after its last glyph"),
    ("TX4", "src/codec/svg/lower/lower_text.c", "      if ((head == ch || out->sideways) && !of_cluster_before) {", "      if ((head == ch || out->sideways) && true) {",
     "a mark shaped as a glyph of its own turns about its own origin and not its base's"),
    ("TX5", "src/codec/svg/lower/lower_text.c", "          grow += extra; /* each glyph's advance takes the spacing", "          grow += 0 * extra; /* each glyph's advance takes the spacing",
     "the last glyph of a spaced textLength span takes none of the spacing, so the text after it starts early"),
    ("TL1", "src/codec/svg/lower/lower_text.c", "            if (q == k || d->start < tl->start", "            if (true || q == k || d->start < tl->start",
     "no descendant is found, so the spacing of an ancestor's textLength spreads a descendant's glyphs too"),
    ("TL2", "src/codec/svg/lower/lower_text.c", "        hi = glyphs[g].cell_x1 > hi ? glyphs[g].cell_x1 : hi;", "        hi = glyphs[g].cell_x1;",
     "the extent of a textLength is to the last glyph's end, not the greatest"),
    ("FN1", "src/codec/svg/style/svg_fonthooks.c", "      if (advance_em(face, cp, &a)) {", "      if (advance_em(face, cp, &a) && 0) {",
     "the advance of \"0\" is half an em whatever the face's is"),
    ("FN2", "src/codec/svg/style/svg_fonthooks.c", "(double) (line.ascent - line.descent + line.line_gap)", "(double) (line.ascent + line.line_gap)",
     "the normal line height leaves out the face's descent"),
    ("FN3", "src/codec/svg/lower/lower_walk.c", "fonted ? &hooks : NULL", "NULL",
     "the style's font units never ask the fonts"),
    ("FN4", "src/codec/svg/lower/lower_walk.c", "    hooks.reuse = parent && parent->style ? parent->style->engine : NULL;", "    hooks.reuse = NULL;",
     "a nested document reads its text through an engine other than the one its parent's styles came from"),
    ("SF1", "src/codec/svg/lower/lower_walk.c", "  set.groups[0]->opacity = alpha; /* the opacity applies", "  set.groups[0]->opacity = 1; /* the opacity applies",
     "the opacity of a filtered span is not on its filter's group"),
    ("SF2", "src/codec/svg/lower/lower_text.c", "    if (b->empty || !gvec_lw_instance_within(laid.pieces[i].inst, span)) {", "    if (b->empty) {",
     "the box of a span holds the glyphs of the whole text"),
    ("SF3", "src/codec/svg/lower/lower_text.c", "          gl->placed = true;", "          gl->placed = false;",
     "the box of a glyph on a path is its cell along the line, unplaced"),
    ("SF4", "src/codec/svg/lower/lower_walk.c", "    if (found) {\n      *abandon = true;", "    if (found && false) {\n      *abandon = true;",
     "a filtered span inside a filtered span is filtered once, by the nearer, and the outer filter is dropped"),
    ("FT1", "src/codec/svg/lower/lower_filter.c", "      p->sx = freq[0] / b->map.a;\n      p->sy = freq[1] / b->map.d;\n",
     "      p->sx = freq[0];\n      p->sy = freq[1] / b->map.d;\n", "a turbulence's x frequency is not divided by the map's scale"),
    ("FT2", "src/codec/svg/lower/lower_filter.c", "      p->sx = freq[0] / b->map.a;\n      p->sy = freq[1] / b->map.d;\n",
     "      p->sx = freq[0] / b->map.a;\n      p->sy = freq[1];\n", "a turbulence's y frequency is not divided by the map's scale"),
    ("FT3", "src/codec/svg/lower/lower_filter.c", "      p->sx = freq[0] / b->map.a;\n      p->sy = freq[1] / b->map.d;\n",
     "      p->sx = freq[0] / b->map.d;\n      p->sy = freq[1] / b->map.a;\n", "a turbulence's frequencies are divided by the other axis's scale"),
    ("FT4", "src/codec/svg/lower/lower_filter.c", "      p->dx = b->map.e;\n      p->dy = b->map.f;\n",
     "      p->dy = b->map.f;\n", "a turbulence's origin has no x"),
    ("FT5", "src/codec/svg/lower/lower_filter.c", "      p->dx = b->map.e;\n      p->dy = b->map.f;\n",
     "      p->dx = b->map.e;\n", "a turbulence's origin has no y"),
    ("FT6", "src/codec/svg/lower/lower_filter.c", "      p->dx = b->map.e;\n      p->dy = b->map.f;\n",
     "      p->dx = b->map.f;\n      p->dy = b->map.e;\n", "a turbulence's origin is transposed"),
    ("FT8", "src/codec/svg/lower/lower_filter.c", "      if (b->map.a <= 0 || b->map.d <= 0) { /* a quarter turn has a == 0 */\n",
     "      if (b->map.d <= 0) { /* a quarter turn has a == 0 */\n", "a turbulence under a mirror in x is lowered"),
    ("FT9", "src/codec/svg/lower/lower_filter.c", "      if (b->map.a <= 0 || b->map.d <= 0) { /* a quarter turn has a == 0 */\n",
     "      if (b->map.a <= 0) { /* a quarter turn has a == 0 */\n", "a turbulence under a mirror in y is lowered"),
    # Images (lower_image.c, lower_image_file.c, the image primitive of lower_filter.c, the boxes): sizes, the clip, the decoders' limits, the resource policy.
    ('IM1', 'src/codec/svg/lower/lower_image.c', '    *width = *height * iw / ih;\n', '    *width = *height * ih / iw;\n',
     'an image sized by its height alone takes the inverse of its aspect ratio'),
    ('IM2', W, '  if (clips && g.par.align != GVEC_SVG_ALIGN_NONE && g.par.slice) {\n', '  if (clips && g.par.align != GVEC_SVG_ALIGN_NONE) {\n',
     'a meet is clipped to the viewport as a slice is'),
    ('IM3', W, '  const bool clips = !(gvec_lw_keyword_is(&overflow, "visible") || gvec_lw_keyword_is(&overflow, "auto"));\n  GSHGEOM_Map placed;\n', '  const bool clips = true;\n  GSHGEOM_Map placed;\n',
     'overflow: visible still clips a slice'),
    ('IM4', 'src/codec/svg/lower/lower_image.c', '  if ((gw && !(w > 0)) || (gh && !(h > 0))) {\n', '  if ((gw && w < 0) || (gh && h < 0)) {\n',
     'an image of width zero is drawn (as a fit into nothing) instead of disabling the element'),
    ('IM5', 'src/codec/svg/lower/lower_image.c', '  if (n >= 2 && b[0] == 0x1f && b[1] == 0x8b) {\n    return true;\n  }\n', '  if (n >= 2 && b[0] == 0x1f && b[1] == 0x8b) {\n    return false;\n  }\n',
     'a compressed SVG image is sent to the raster decoders'),
    ('IM6', 'src/codec/svg/lower/lower_image.c', "  return i < n && b[i] == '<';\n", "  return i < n && b[i] == '<' && i == 0;\n",
     'markup after a byte order mark or white space is not taken for an SVG document'),
    ('IM7', 'src/codec/svg/lower/lower_image.c', '    *magnify = *minify = GSHSCENE_IMAGE_FILTER_NEAREST;\n', '    *magnify = GSHSCENE_IMAGE_FILTER_NEAREST;\n',
     'optimizeSpeed minifies with the smooth filter'),
    ('IM8', 'src/codec/svg/lower/lower_image.c', '    *magnify = GSHSCENE_IMAGE_FILTER_LINEAR;\n', '    *magnify = GSHSCENE_IMAGE_FILTER_CUBIC;\n',
     'smooth magnifies with the cubic filter'),
    ('IM9', 'src/codec/svg/lower/lower_image.c', '  ctx->image_decoded += decoded;\n', '',
     'the decoded bytes of an image are never counted against the budget'),
    ('IM10', 'src/codec/svg/lower/lower_image.c', '    if (entries[i].element == e) {\n', '    if (entries[i].element == NULL && e) {\n',
     'an image drawn twice is decoded twice'),
    ('IM11', 'src/codec/svg/lower/lower_image.c', '(uint64_t) width * (uint64_t) height > (uint64_t) limits->max_image_pixels ||', '(uint64_t) width * (uint64_t) height >= (uint64_t) limits->max_image_pixels ||',
     'an image of exactly max_image_pixels is refused'),
    ('IM12', 'src/codec/svg/lower/lower_image.c', '    size_t capacity = bound < limits->max_image_bytes ? bound : limits->max_image_bytes;\n', '    size_t capacity = bound;\n',
     'a data: URI larger than max_image_bytes is read'),
    ('IM13', 'src/codec/svg/lower/lower_image.c', '  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, "href");\n  return a ? a : gvec_svg_element_find_attribute(e, "xlink:href");\n', '  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, "xlink:href");\n  return a ? a : gvec_svg_element_find_attribute(e, "href");\n',
     'xlink:href takes precedence over href'),
    ('IM14', 'src/codec/svg/lower/lower_image.c', '  if (g == GIMG_ERR_LIMIT) {\n    *state = GVEC_LW_IMAGE_REFUSED;\n    result = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image is larger than max_image_pixels or max_image_decoded_bytes allows");', '  if (g == GIMG_ERR_LIMIT) {\n    *state = GVEC_LW_IMAGE_NOTHING;\n    result = GVEC_OK;',
     "a decoder's limit is a broken image rather than a refusal"),
    ('IM15', 'src/codec/svg/lower/lower_image_file.c', "    if (c == 0 || c < 0x20 || c == 0x7f || c == '\\\\') {\n", '    if (c == 0 || c < 0x20 || c == 0x7f) {\n',
     'a backslash in a reference is followed'),
    ('IM16', 'src/codec/svg/lower/lower_image_file.c', '  if (root->abs != path->abs || root->count > path->count) {\n', '  if (root->count > path->count) {\n',
     'an absolute path is inside a relative root'),
    ('IM17', 'src/codec/svg/lower/lower_image_file.c', 'path_c.count == root_c.count) {\n', 'false) {\n',
     'the root itself is a file inside it'),
    ('IM18', 'src/codec/svg/lower/lower_image_file.c', "  if (decoded[0] == '/') {\n", '  if (false) {\n',
     'an escaped slash makes a path from the base'),
    ('IM19', 'src/codec/svg/lower/lower_image_file.c', "  return file[n] == '/';\n", '  return true;\n',
     "a directory whose name begins with the root's is inside it"),
    ('IM20', 'src/codec/svg/lower/lower_image_file.c', '  if (used > max_bytes) {\n', '  if (used > max_bytes + 1) {\n',
     'a file one byte over max_image_bytes is read'),
    ('IM21', 'src/codec/svg/lower/lower_image_file.c', "  if (has_scheme(reference, n) || (n >= 2 && reference[0] == '/' && reference[1] == '/')) {\n", '  if (has_scheme(reference, n)) {\n',
     'a network-path reference is a path'),
    ('IM22', 'src/codec/svg/lower/lower_filter.c', '  p->kind = GSHSCENE_FILTER_IMAGE;\n  p->image = image.resource;\n', '  p->kind = GSHSCENE_FILTER_FLOOD;\n  p->image = image.resource;\n',
     'an feImage is lowered as a flood'),
    ('IM23', 'src/codec/svg/lower/lower_filter.c', ': x0 + b->region[2]; /* a size not given', ': b->region[2]; /* a size not given',
     'a viewport with no width given runs to the width of the region, not to its right edge'),
    ('IM24', 'src/codec/svg/lower/lower_filter.c', '    if (target) {\n      return element_primitive(b, el, p, target, sub);', '    if (0 && target) {\n      return element_primitive(b, el, p, target, sub);',
     'an feImage of an element of the document is an empty result'),
    ('IM25', 'src/codec/svg/lower/lower_filter.c', '!gvec_lw_compose(&b->local, &placed, &p->placement)) {\n', '(p->placement = placed, 0)) {\n',
     "the image's placement is not put through the element's map"),
    ('IM26', CL, '    const bool image_leaf = ck == GVEC_SVG_KIND_IMAGE;\n', '    const bool image_leaf = false;\n',
     'a group that holds an image has no box'),
    ('IM27', CL, '  if (gvec_svg_element_kind(e) == GVEC_SVG_KIND_IMAGE) {\n    GSHGEOM_Map identity;\n    bool ok;\n', '  if (false) {\n    GSHGEOM_Map identity;\n    bool ok;\n',
     'an image has no box of its own for a mask or a filter'),
    ('IM29', 'src/codec/svg/style/svg_prop_tables.c', 'auto | optimizeSpeed | optimizeQuality | smooth | high-quality | crisp-edges | pixelated', 'auto | smooth | high-quality | crisp-edges | pixelated',
     'the SVG keywords of image-rendering are not read'),
    ('RS1', 'src/codec/svg/svg_resource.c', "            while (k > 0 && b.path[k - 1] != '/') {\n              k--;\n            }", "            while (k > 0 && b.path[k - 1] != '/' && 0) {\n              k--;\n            }",
     'a reference is merged with the whole base path, its file name kept'),
    ('RS2', 'src/codec/svg/svg_resource.c', "      if (count > 0 && !(len[count - 1] == 2 && flat[start[count - 1]] == '.' && flat[start[count - 1] + 1] == '.' && !rooted)) {\n        count--;", "      if (count > 0 && !(len[count - 1] == 2 && flat[start[count - 1]] == '.' && flat[start[count - 1] + 1] == '.' && !rooted)) {\n        ;",
     'a `..` segment removes nothing'),
    ('RS3', 'src/codec/svg/svg_resource.c', '      else if (!rooted) {\n        start[count] = i;', '      else if (0) {\n        start[count] = i;',
     'a `..` with nothing above it in a relative path is dropped, so the reference names something else'),
    ('RS4', 'src/codec/svg/svg_resource.c', '  (void) gvec_res_fragment(reference, &before);', '  before = strlen(reference);',
     'the fragment is passed to the resolver as part of the reference'),
    ('RS5', 'src/codec/svg/svg_resource.c', '  gvec_res_entry * known = find(state, kind, url);', '  gvec_res_entry * known = NULL;',
     'a reference is asked again each time it is named'),
    ('RS6', 'src/codec/svg/svg_resource.c', '    if (state->entries[i]->kind == kind && strcmp(', '    if ((state->entries[i]->kind | kind) != 0 && strcmp(',
     'the cache ignores the kind: a url asked as an image answers a style sheet'),
    ('RS7', 'src/codec/svg/svg_resource.c', '  if (state->calls >= state->max_count) {', '  if (state->calls > state->max_count) {',
     'one resolver call past max_resources is made'),
    ('RS8', 'src/codec/svg/svg_resource.c', '  if (depth > state->max_depth) {', '  if (depth > state->max_depth + 1) {',
     'a reference one level past max_resource_depth is asked'),
    ('RS9', 'src/codec/svg/svg_resource.c', '    if (reply.size > cap) {\n      entry->outcome = GVEC_RES_TOO_BIG;', '    if (0) {\n      entry->outcome = GVEC_RES_TOO_BIG;',
     'an answer longer than the limit is copied'),
    ('RS10', 'src/codec/svg/svg_resource.c', '    else if (reply.size > 0 && !reply.bytes) {', '    else if (0) {',
     'a size with no bytes is read from address zero'),
    ('RS11', 'src/codec/svg/svg_resource.c', '  if (cap > state->max_bytes) {\n    cap = state->max_bytes;\n  }', '',
     'max_resource_bytes does not lower the limit of a kind'),
    ('RS12', 'src/codec/svg/svg_resource.c', '  return value > ceiling ? ceiling : value;', '  (void) ceiling;\n  return value;',
     'a limit above its ceiling is taken as given'),
    ('RS13', 'src/codec/svg/svg_resource.c', '  if (value == 0) {\n    return fallback;', '  if (value == 1) {\n    return fallback;',
     'zero does not select the default of a resolver limit'),
    ('RS14', 'src/codec/svg/svg_resource.c', '  case GVEC_RESOURCE_OOM:\n    entry_free(state->allocator, entry);\n    return GVEC_ERR_OOM;', '  case GVEC_RESOURCE_OOM:\n    entry->outcome = GVEC_RES_FAILED;\n    break;',
     'a resolver that is out of memory is a failure to find'),
    ('RS15', 'src/codec/svg/svg_resource.c', '    entry->outcome = GVEC_RES_REFUSED;', '    entry->outcome = GVEC_RES_NOT_FOUND;',
     'a refusal by the resolver is taken for a missing resource'),
    ('RS16', 'src/codec/svg/svg_resource.c', '  if (!gvec_res_enabled(state)) {\n    *out_outcome = GVEC_RES_NO_RESOLVER;', '  if (0) {\n    *out_outcome = GVEC_RES_NO_RESOLVER;',
     'a lowering with no resolver calls a null function'),
    ('RS17', 'src/codec/svg/svg_resource.c', '  if (has_auth) {\n    put(&t, "//", 2);', '  if (0) {\n    put(&t, "//", 2);',
     'the authority of a resolved URL is lost'),
    ('RS18', 'src/codec/svg/svg_resource.c', '        if (r.has_query) {\n          query = r.query;', '        if (0) {\n          query = r.query;',
     "an empty-path reference with a query keeps the base's query"),
    ('RS19', 'src/codec/svg/svg_resource.c', '    entry->media_type[n] = 0;\n        }\n      }\n', '    entry->media_type[n] = 0;\n        }\n        entry->media_type[0] = 0;\n      }\n',
     'the media type is lost'),
    ('RS20', 'src/codec/svg/style/svg_sheet.c', '  if (request->has_scope) {', '  if (false) {',
     'an import with a scope condition is fetched and applied unscoped'),
    ('RS21', 'src/codec/svg/style/svg_sheet.c', 'media.prefers_color_scheme = context->color_scheme == GVEC_COLOR_SCHEME_DARK ? GTEXT_CSS_MQ_SCHEME_DARK : GTEXT_CSS_MQ_SCHEME_LIGHT;', 'media.prefers_color_scheme = GTEXT_CSS_MQ_SCHEME_LIGHT; media.viewport.width = 5;',
     'the media of an import and of a rule are answered for a viewport of 5'),
    ('RS22', 'src/codec/svg/style/svg_sheet.c', '  const char * base = frame->url ? frame->url : context->base;', '  const char * base = context->base;',
     'an import in an imported sheet is resolved against the document, not the sheet'),
    ('RS23', 'src/codec/svg/style/svg_sheet.c', '      context->res_depth + frame->depth + 1, &outcome, &entry);', '      context->res_depth + frame->depth, &outcome, &entry);',
     'an import does not count its own level'),
    ('RS24', 'src/codec/svg/style/svg_sheet.c', '    if (f->url && strcmp(f->url, entry->url) == 0) {', '    if (f->url && strcmp(f->url, entry->url) == 0 && 0) {',
     'a sheet that imports itself is not a cycle'),
    ('RS25', 'src/codec/svg/style/svg_sheet.c', 'media.prefers_color_scheme = context->color_scheme == GVEC_COLOR_SCHEME_DARK ? GTEXT_CSS_MQ_SCHEME_DARK : GTEXT_CSS_MQ_SCHEME_LIGHT;', 'media.prefers_color_scheme = GTEXT_CSS_MQ_SCHEME_LIGHT;',
     'a media condition on the color scheme is answered as light whatever the document prefers'),
    ('RS40', 'src/codec/svg/style/svg_sheet.c', 'gtext_css_media_environment_default(&context->engine->env);', 'gtext_css_media_environment_default(NULL);',
     'a media condition is answered for a 1280 by 800 screen whatever the viewport is'),
    ('CL1', 'src/codec/svg/style/svg_color.c', '  xyz[0] = x * 0.3457 / 0.3585;', '  xyz[0] = x;', 'CIE Lab is not taken to the D50 white'),
    ('CL2', 'src/codec/svg/style/svg_color.c', '  xyz[0] = 1.2268798758459243 * lms[0] - 0.5578149944602171 * lms[1]', '  xyz[0] = 1.2268798758459243 * lms[0] + 0.5578149944602171 * lms[1]', 'Oklab is taken to XYZ with a wrong sign'),
    ('CL3', 'src/codec/svg/style/svg_color.c', '    v2 = chroma * gmath_sin(h);', '    v2 = chroma * gmath_cos(h);', 'a polar color takes its b from the cosine'),
    ('CL4', 'src/codec/svg/style/svg_color.c', '  s.current = v && v->kind == GTEXT_CSS_VAL_COLOR ? v->color : NULL;', '  s.current = NULL;', 'currentcolor inside a mix is not the color of the element'),
    ('CL5', 'src/codec/svg/style/svg_color.c', '  const GTEXT_CSS_Color * replaced;\n  if (!subst_color(&s, c, 0, &replaced)) {', '  const GTEXT_CSS_Color * replaced = c;\n  if (false && !subst_color(&s, c, 0, &replaced)) {', 'currentcolor in a mix is left in it'),
    ('BX1', 'src/codec/svg/lower/lower_clip.c', 'const GVEC_Svg_Element * chosen = switch_leaf ? gvec_svg_element_switch_selected(child) : NULL;', 'const GVEC_Svg_Element * chosen = NULL;', 'the box of a switch takes in every child, not the one it draws'),
    ('BX2', 'src/codec/svg/lower/lower_clip.c', '  if (state != 0 || !gvec_lw_compose(placed, &inner, &moved)) {', '  if (state != 0 || !gvec_lw_compose(placed, placed, &moved)) {', 'the box of a viewport leaves out its viewBox map'),
    ('TD9', 'src/codec/svg/lower/lower_text.c', '      d->inst = chain[k];', '      d->inst = inst;', 'a decoration declared by an ancestor of the text is painted with the text\'s own paint'),
    ('CP1', 'src/codec/svg/lower/lower_clip.c', '    if (!place_on_box(&map, &box, &map)) {\n      *refused = true;', '    if (false && !place_on_box(&map, &box, &map)) {\n      *refused = true;',
     "a clip in the units of the box, on a clip's shape, is not placed on the box"),
    ('CP2', 'src/codec/svg/lower/lower_clip.c', '    r = subject_box(c, subject, &box, &all, refused);', '    r = subject_box(c, c->inst, &box, &all, refused);',
     "a clip in the units of the box, on a clip's shape, is placed on the box of the clipped element"),
    ('CP3', 'src/codec/svg/lower/lower_clip.c', '  r = inst_clip_dnf(c, clip_inst, subject, space, depth, subject, &nested, &nests, refused);', '  r = inst_clip_dnf(c, clip_inst, clip_inst, space, depth, subject, &nested, &nests, refused);',
     'a basic shape on a clipPath is measured against the clipPath, not the element it clips'),
    ('CP4', 'src/codec/svg/lower/lower_clip.c', '    own_inst = ch->own_inst;', '    own_inst = NULL;',
     'a text in a clip is not cut by its own clip-path'),
    ('CP5', 'src/codec/svg/lower/lower_clip.c', '    return add_union(c, list, &union_base, shapes, state); /*', '    (void)union_base; return add_union(c, list, &map, shapes, state); /*',
     'the only shape of a clip, with a basic shape, is placed by a map that holds its transform twice'),
    ('CP6', 'src/codec/svg/lower/lower_clip.c', '  if (gshgeom_map_translate(2, shape.ox, shape.oy, 0, &moved) != GSHGEOM_OK || !gvec_lw_compose(space, &moved, &one->map)) {', '  if (gshgeom_map_translate(2, shape.ox, 0, 0, &moved) != GSHGEOM_OK || !gvec_lw_compose(space, &moved, &one->map)) {',
     "the path of a basic shape on a clip's shape does not begin at its box's top"),
    ('BS1', 'src/codec/svg/lower/lower_basic_shape.c', '    sides[1] = sides[3] = across;', '    sides[1] = sides[3] = sides[0];',
     'a single inset value takes its left and right of the height'),
    ('BS2', 'src/codec/svg/lower/lower_basic_shape.c', "      *w++ = horizontal ? (by ? 'h' : 'H') : (by ? 'v' : 'V');", "      *w++ = horizontal ? 'H' : 'V';",
     'a shape() hline or vline by is an absolute move'),
    ('BS4', 'src/codec/svg/lower/lower_basic_shape.c', '    ctx->box_stroke = kind == BOX_STROKE;', '    ctx->box_stroke = false;',
     'the stroke box of a group leaves out the strokes'),
    ('SC1', 'src/codec/svg/style/svg_color.c', ' ? system_colors[i].dark : system_colors[i].light;', ' ? system_colors[i].light : system_colors[i].dark;',
     'a system color of the dark scheme is the light one'),
    ('SC2', 'src/codec/svg/style/svg_color.c', '{"accentcolor", 0x0075ffffu, 0x0075ffffu},', '{"accentcolor", 0x0175ffffu, 0x0075ffffu},',
     'AccentColor is a different blue'),
    ('AL1', 'src/codec/svg/lower/lower_text.c', 'snprintf(name, sizeof name, "ss%02u", (unsigned) v->values[k]);', 'snprintf(name, sizeof name, "cv%02u", (unsigned) v->values[k]);',
     'a styleset asks for a character variant'),
    ('AL2', 'src/codec/svg/lower/lower_text.c', 'type = GTEXT_CSS_FEATURE_STYLISTIC, tag = "salt";', 'type = GTEXT_CSS_FEATURE_STYLISTIC, tag = "swsh";',
     'stylistic() asks for a swash'),
    ('AL3', 'src/codec/svg/style/svg_sheet.c', '  if (context->atrules) {\n    /* The at-rules the style layer reads', '  if (false) {\n    /* The at-rules the style layer reads',
     'the @font-feature-values of the sheets are not collected'),
    ('AL4', 'src/codec/svg/lower/lower_text.c', 'st->families[f], strlen(st->families[f]));', 'st->families[f], 0);',
     'the feature values of a family are looked up by an empty name'),
    ('SB1', 'src/codec/svg/lower/lower_clip.c', '    *box = grown;\n', '    (void) grown;\n',
     'the stroke box is the fill box, the outline of the stroke is not taken in'),
    ('SB2', 'src/codec/svg/lower/lower_clip.c', '      scaled.m[i][j] *= SCALE;', '      scaled.m[i][j] *= 1;',
     'the outline is read at the wrong scale'),
    ('SC3', 'src/codec/svg/style/svg_color.c', '  if (s->context && system_rgba(s->context, s->instance, c, system)) {', '  if (false && s->context && system_rgba(s->context, s->instance, c, system)) {',
     'a system color in a mix is left to the engine, which cannot compute it'),
    ('PF1', 'src/codec/svg/style/svg_profile.c', 'GCOL_CMYK_POLARITY_REFLECTION', 'GCOL_CMYK_POLARITY_INK',
     'a CMYK profile reads 0 as full ink'),
    ('PF2', 'src/codec/svg/style/svg_profile.c', '  dst.transfer = GCOL_TRANSFER_LINEAR;\n', '  dst.transfer = GCOL_TRANSFER_SRGB;\n',
     'a profile color is converted to a curve the solid paint does not carry it in'),
    ('PF3', 'src/codec/svg/style/svg_color.c', '      if (c->space == GTEXT_CSS_SPACE_CUSTOM) {\n        return profile_color(', '      if (false) {\n        return profile_color(',
     'a color in a custom space is refused'),
    ('MC1', 'src/codec/svg/style/svg_refs.c', '        else if (state[target] == 0) {\n          state[target] = 1;', '        else if (false) {\n          state[target] = 1;',
     'a marker named from another is not walked from there, so its content keeps what only the walk through the first cuts'),
    ('MC2', 'src/codec/svg/style/svg_paint.c', 'marker ? gvec_svg_marker_reference_cut(context, instance, out->target) : gvec_svg_reference_cut', 'marker ? false : gvec_svg_reference_cut',
     'a reference that closes a cycle of markers is not cut'),
    ('EP1', 'src/codec/svg/lower/lower_external.c', '  return found ? found : ctx;', '  (void)found; return ctx;',
     'an element of another document is read through the context of the document that names it'),
    ('EP2', 'src/codec/svg/lower/lower_walk.c', '    gvec_svg_style_set_external(ctx->style, gvec_lw_external_property, ctx);', '    gvec_svg_style_set_external(ctx->style, NULL, ctx);',
     'a url that names another document is never asked of the resolver for a property'),
    ('EP4', 'src/codec/svg/lower/lower_marker.c', '  const GVEC_Svg_Instance * inst = gvec_svg_instance_root(owner->style);', '  const GVEC_Svg_Instance * inst = gvec_svg_instance_root(ctx->style);',
     'a marker of another document is made down from the root of the document that names it'),
    ('RS26', 'src/codec/svg/style/svg_sheet.c', '  sheet_frame inner = {frame, context, frame->element, entry->url, frame->depth + 1, false};', '  sheet_frame inner = {frame, context, frame->element, NULL, frame->depth + 1, false};',
     'an imported sheet does not know its own url'),
    ('RS27', 'src/codec/svg/style/svg_sheet.c', '  case GVEC_RES_TOO_BIG:\n    why = "an imported style sheet is longer than the limit allows";\n    break;', '  case GVEC_RES_TOO_BIG:\n    break;',
     'an imported sheet longer than the limit is read'),
    ('RS28', 'src/codec/svg/style/svg_sheet.c', '  if (!context->res || !gvec_res_enabled(context->res)) {', '  if (!context->res) {',
     'an import with no resolver asks a null function'),
    ('RS29', 'src/codec/svg/lower/lower_image.c', '  if (gvec_res_enabled(ctx->res)) {\n    *borrowed = true;', '  if (0) {\n    *borrowed = true;',
     'the resolver is not asked for an image'),
    ('RS30', 'src/codec/svg/lower/lower_image.c', '  if (!borrowed) {\n    gvec_free(ctx->allocator, bytes);', '  if (1) {\n    gvec_free(ctx->allocator, bytes);',
     "the cache's bytes of an image are freed by the image"),
    ('RS31', 'src/codec/svg/lower/lower_image.c', '  *svg = is_svg_document(entry->bytes, entry->size, entry->media_type);', '  *svg = false;',
     'an SVG image from the resolver is decoded as a raster'),
    ('RS32', 'src/codec/svg/lower/lower_image.c', 'ctx->store, NULL, ctx, uri);', 'ctx->store, NULL, ctx, uri ? NULL : NULL);',
     'the size of an SVG image is read without its url'),
    ('RS33', 'src/codec/svg/lower/lower_walk.c', '    ctx->res_depth = parent->res_depth + 1;', '    ctx->res_depth = 0;',
     'a document loaded through the resolver is at depth zero'),
    ('RS34', 'src/codec/svg/lower/lower_walk.c', '    ctx->style->res_depth = ctx->res_depth;', '    ctx->style->res_depth = 0;',
     "a nested document's imports start at depth one"),
    ('RS35', 'src/codec/svg/lower/lower_walk.c', '  r = gvec_lw_begin_in(nested, image->svg, ctx->options, ctx->store, ctx->diagnostics, ctx, image->uri);', '  r = gvec_lw_begin_in(nested, image->svg, ctx->options, ctx->store, ctx->diagnostics, ctx, NULL);',
     "the drawing of an SVG image resolves its references against the document's base"),
    ('RS36', 'src/codec/svg/lower/lower_walk.c', '  if (r == GVEC_OK && usable && external && (use_fill.kind == GVEC_SVG_PAINT_SERVER || use_stroke.kind == GVEC_SVG_PAINT_SERVER)) {', '  if (r == GVEC_OK && usable && 0 && (use_fill.kind == GVEC_SVG_PAINT_SERVER || use_stroke.kind == GVEC_SVG_PAINT_SERVER)) {',
     "a paint server of the using document fills the other document's element"),
    ('RS37', 'src/codec/svg/lower/lower_walk.c', '        r = gshgeom_map_identity(2, &identity) == GSHGEOM_OK ? lw_element(external, clone, &identity, use) : GVEC_ERR_INTERNAL;', '        r = gshgeom_map_identity(2, &identity) == GSHGEOM_OK ? lw_element(ctx, clone, &identity, use) : GVEC_ERR_INTERNAL;',
     'the element of another document is walked by the using document: its sheets and ids are the wrong ones'),
    ('RS38', 'src/codec/svg/lower/lower_external.c', '    if (up->external_use == use) {', '    if (0) {',
     'documents that name one another recurse to the depth limit'),
    ('RS39', 'src/codec/svg/lower/lower_external.c', '  if (res->instantiations >= res->max_instances) {\n    return gvec_lw_refuse(ctx, use,', '  if (res->instantiations > res->max_instances) {\n    return gvec_lw_refuse(ctx, use,',
     'one instantiation past max_resource_instances is made'),
    ('RS40', 'src/codec/svg/lower/lower_external.c', '  nested->context_set = ctx->context_set;', '  nested->context_set = false;',
     "the use's context-fill is not the other document's"),
    ('RS41', 'src/codec/svg/lower/lower_external.c', "    if (c == '%') {", '    if (0) {',
     'a percent-escape in a fragment is not decoded'),
    ('RS42', 'src/codec/svg/lower/lower_external.c', '  if (!fragment || !fragment[0]) {', '  if (0) {',
     'a use of a document with no id draws the whole file'),
    ('RS43', 'src/codec/svg/lower/lower_external.c', '  GVEC_Result r = gvec_lw_begin_in(n, doc, ctx->options, ctx->store, ctx->diagnostics, ctx, url);', '  GVEC_Result r = gvec_lw_begin_in(n, doc, ctx->options, ctx->store, ctx->diagnostics, ctx, NULL); (void)url;',
     'the references of an external document are resolved against the document that names it'),
    ('RS45', 'src/codec/svg/lower/lower_external.c', '  ctx->res->instantiations++;', '  ;',
     'instantiations are not counted'),
    ('RS46', 'src/codec/svg/lower/lower_external.c', '  ctx->marker_instances = nested->marker_instances;', '  ;',
     'the markers an external element drew are not counted in the lowering'),
    ('NF1', 'src/codec/svg/style/svg_sheet.c', '    return status == GTEXT_CSS_OK && matches;', '    return status == GTEXT_CSS_OK;',
     'a style element whose media does not match is applied all the same'),
    ('NF2', 'src/codec/svg/style/svg_sheet.c', '    if (!media_admits(context, e)) {', '    if (!media_admits(context, e) && false) {',
     'the media of a style element is not asked'),
    ('NF3', 'src/codec/svg/lower/lower_text.c', '  *keep = v.keyword && strcmp(v.keyword, "collapse") != 0;', '  *keep = false;',
     'white-space never keeps the spaces'),
    ('NF4', 'src/codec/svg/lower/lower_stroke.c', '        double scale = gshgeom_measured_length(measured) / wanted;', '        double scale = wanted / gshgeom_measured_length(measured);',
     'pathLength scales the dashes by its inverse'),
    ('NF5', 'src/codec/svg/lower/lower_stroke.c', '        style.dash_offset *= scale;', '        style.dash_offset *= 1;',
     'pathLength does not scale the dash offset'),
    ('NF6', 'src/codec/svg/lower/lower_text.c', '            dr->thickness = dec->thickness / 100.0 * ds->size;', '            dr->thickness = dec->thickness / 100.0;',
     'a percentage thickness of a decoration is not of the font size'),
    ('NF7', 'src/codec/svg/lower/lower_fonts.c', '  return rule->range_count == 0;', '  return true;',
     'the unicode-range of an @font-face rule is not looked at'),
    ('NF8', 'src/codec/svg/lower/lower_fonts.c', '    if (!best || d <= best_distance) {', '    if (!best || (d <= best_distance && d < 0)) {',
     'the first rule of a family is used, not the nearest in weight'),
    ('NF9', 'src/codec/svg/lower/lower_text.c', '    if (st->transform == 1) {', '    if (st->transform == 2) {',
     'text-transform: uppercase lowercases'),
    ('NF10', 'src/codec/svg/lower/lower_text.c', '      if (st->full_width) {', '      if (st->full_width && cp == 0) {',
     'text-transform: full-width is not applied'),
    ('NF12', 'src/codec/svg/lower/lower_mask.c', '    if (!v.references[i].is_url || !v.references[i].local || v.reference_count > 1) {', '    if (!v.references[i].is_url || !v.references[i].local) {',
     'the first of a list of masks is drawn and the rest dropped'),
    ('NF13', 'src/codec/svg/style/svg_transform.c', '    if (!individual_ops(translate, rotate, scale, ops, &count)) {', '    if (false && !individual_ops(translate, rotate, scale, ops, &count)) {',
     'translate, rotate and scale are not applied'),
    ('VA1', 'src/codec/svg/lower/lower_text.c', '      value = st->weight;', '      value = (double) axis.def / 65536.0;', 'font-weight does not move the wght axis'),
    ('VA2', 'src/codec/svg/lower/lower_text.c', '      value = st->width;', '      value = (double) axis.def / 65536.0;', 'font-stretch does not move the wdth axis'),
    ('VA3', 'src/codec/svg/lower/lower_text.c', '        value = -st->oblique;', '        value = st->oblique;', 'an oblique slants the wrong way'),
    ('VA4', 'src/codec/svg/lower/lower_text.c', '      if (st->italic == 1) {\n        value = 1;', '      if (st->italic == 0) {\n        value = 1;', 'italic does not set the ital axis'),
    ('VA5', 'src/codec/svg/lower/lower_text.c', '      if (!st->optical_none) {\n        value = st->size;', '      if (st->optical_none) {\n        value = st->size;', 'the optical size ignores font-optical-sizing'),
    ('VA6', 'src/codec/svg/lower/lower_text.c', '        value = st->var_values[k];', '        (void) st->var_values[k];', 'font-variation-settings do not move an axis'),
    ('VA7', 'src/codec/svg/lower/lower_text.c', '    if (other[cls[fr->face]].runs) {', '    if (false && other[cls[fr->face]].runs) {', 'a face of a fallback is shaped at the location of the first'),
    ('VA8', 'src/codec/svg/lower/lower_text.c', '    while (j > 0 && ranks[j - 1] > rk) {', '    while (j > 0 && ranks[j - 1] > rk && false) {', 'the faces are not in the order of the families'),
    ('VA9', 'src/codec/svg/lower/lower_text.c', '      r = gvec_fonts_scene_face(fonts, glyphs[g].face, glyphs[g].coord_count ? &variation : NULL,', '      r = gvec_fonts_scene_face(fonts, glyphs[g].face, glyphs[g].coord_count && false ? &variation : NULL,', 'the glyphs are drawn at the default instance'),
    ('VA11', 'src/codec/svg/lower/lower_text.c', '\n    if (!close || close - tag != 4) {', '\n    if (!close || close - tag != 3) {', 'an axis tag is three letters'),
    ('TO1', 'src/codec/svg/lower/lower_text.c', '  return st->orientation == 1 ? true : st->orientation == 2 ? false : upright_default(cp);', '  return st->orientation == 1 ? false : st->orientation == 2 ? false : upright_default(cp);', 'text-orientation: upright lies the letters on their side'),
    ('TO2', 'src/codec/svg/lower/lower_text.c', '      st->orientation = angle == 0 ? 1 : angle == 90 ? 2 : 0;', '      st->orientation = angle == 0 ? 2 : angle == 90 ? 2 : 0;', 'glyph-orientation-vertical: 0 lies the letters on their side'),
    ('TO3', 'src/codec/svg/lower/lower_text.c', '  st->orientation = gvec_lw_keyword_is(&v, "upright") ? 1 :', '  st->orientation = gvec_lw_keyword_is(&v, "upright") ? 0 :', 'text-orientation: upright is not read'),
    ('LG1', 'src/codec/svg/lower/lower_text.c', '  shape.language = st->language;', '  shape.language = 0;', 'the lang does not reach the shaper'),
    ('LG2', 'src/codec/svg/lower/lower_text.c', 'guni_to_lower_at(source, count, i, (GUNI_CaseTailoring) st->tailoring,', 'guni_to_lower_at(source, count, i, GUNI_LANG_NONE,', 'a lowercase is not tailored by the language'),
    ('LG3', 'src/codec/svg/lower/lower_text.c', 'guni_to_upper_at(source, count, i, (GUNI_CaseTailoring) st->tailoring,', 'guni_to_upper_at(source, count, i, GUNI_LANG_NONE,', 'an uppercase is not tailored by the language'),
    ('LG4', 'src/codec/svg/lower/lower_text.c', 'find_attribute(e, "xml:lang");', 'find_attribute(e, "xml:lng");', 'xml:lang is not read'),
    ('LG5', 'src/codec/svg/lower/lower_text.c', '    if (!tag || n == 0) {\n      return;', '    if (!tag || n == 0) {\n      continue;', 'an empty lang does not end the search'),
    ('LG6', 'src/codec/svg/lower/lower_text.c', 'strncasecmp(tag, "az", 2) == 0', 'strncasecmp(tag, "aq", 2) == 0', 'Azerbaijani is not tailored'),
    ('LG7', 'src/codec/svg/lower/lower_text.c', '    else if (k == 2 && strncasecmp(tag, "lt", 2) == 0) {', '    else if (k == 2 && strncasecmp(tag, "lq", 2) == 0) {', 'Lithuanian is not tailored'),
    ('LG8', 'src/codec/svg/lower/lower_text.c', 'cps[i], st->language, &face, &found, &rank);', 'cps[i], 0, &face, &found, &rank);', 'the lang does not reach the face matching'),
    ('VA10', 'src/codec/svg/lower/lower_text.c', '  if (!moved) {\n    out->count = 0;', '  if (false) {\n    out->count = 0;', 'the default instance is not recognised'),
]

PLANTS += [
    # The box of a `use` (design X6) and the list of filters (nested groups, the first innermost).
    ("X6a", CL, "gshgeom_map_translate(2, xy[0], xy[1], 0, &shift)", "gshgeom_map_translate(2, 0 * xy[0], 0 * xy[1], 0, &shift)", "the x and y of a use are left out of its box"),
    ("X6b", CL, "  if (gvec_lw_keyword_is(&display, \"none\") || gvec_svg_element_conditional_false(gvec_svg_instance_element(clone))) {", "  if (0) {", "a use of an element that display removes still has a box"),
    ("X6c", CL, "    return use_box(ctx, inst, &identity, box, known); /* in the use's own space: its `x` and `y` are inside it */", "    return GVEC_OK;", "a filter in the units of the box of a use has an empty box and is not rendered"),
    ("X6d", CL, "    if (use_leaf) {\n      bool ok;\n      r = use_box(ctx, ci, &inner, box, &ok);", "    if (use_leaf) {\n      bool ok;\n      r = use_box(ctx, ci, map, box, &ok);", "a use in a group is boxed without its own transform"),
    ("FL1", "src/codec/svg/lower/lower_filter.c", "    const size_t k = step_count - 1 - j;", "    const size_t k = j;", "a list of filters is applied last to first"),
    ("FL2", W, "      filter.groups[0]->opacity = opacity.number < 0 ? 0 : (opacity.number > 1 ? 1 : opacity.number);", "      filter.groups[filter.count - 1]->opacity = opacity.number < 0 ? 0 : (opacity.number > 1 ? 1 : opacity.number);", "the opacity of a filtered element with a list is applied inside the first filter, not to the result of the last"),
    ("FL3", "src/codec/svg/lower/lower_filter.c", "    if (!v.references[i].is_url && step_count && !steps[step_count - 1].is_url) {", "    if (0) {", "each filter function of a list is a filter group of its own"),
    # An image that is an SVG document (design R3).
    ("R3a", "src/codec/svg/lower/lower_image.c", "        w = box.width;\n        h = box.height;", "        w = 1;\n        h = 1;", "a document that gives no size is one pixel, not the viewBox's size"),
    ("R3b", "src/codec/svg/lower/lower_image.c", "  if (ctx->image_nesting >= GVEC_LW_IMAGE_NESTING_MAX) {", "  if (0) {", "SVG images nest without a bound"),
    ("R3c", W, "  nested->image_nesting = ctx->image_nesting + 1;", "  nested->image_nesting = 0;", "a document that names itself nests without a bound"),
    ("R3e", "src/codec/svg/lower/lower_filter.c", "  if (image.svg) {\n    return document_primitive(b, el, p, &image, &placed);\n  }\n  if (!gvec_lw_compose", "  if (0) {\n    return document_primitive(b, el, p, &image, &placed);\n  }\n  if (!gvec_lw_compose", "an feImage of an SVG document is drawn as an image of no raster"),
    ("R3f", W, "  clip->resource = rect;\n  clip->rule = GSHGEOM_FILL_NONZERO;\n  gshgeom_map_identity(2, &clip->map);", "  clip->resource = rect;\n  clip->rule = GSHGEOM_FILL_NONZERO;\n  gshgeom_map_identity(2, &clip->map);\n  clip->map.m[0][0] = 2;", "the clip of an SVG image's own box is the wrong size"),
    ("FL4", "src/codec/svg/lower/lower_filter.c", "    g->key = gvec_lw_key(e->key, hash2(ctx->chain, j == 0 ? 0 : 16u + (uint64_t) j), GVEC_LW_ROLE_FILTER);", "    g->key = gvec_lw_key(e->key, hash2(ctx->chain, 0), GVEC_LW_ROLE_FILTER);", "the groups of a list of filters share one key"),
    # feImage of an element (FL25).
    ("FE1", "src/codec/svg/lower/lower_filter.c", "gshgeom_map_translate(2, x0, y0, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&b->local, &shift, &placed)", "gshgeom_map_translate(2, 0 * x0, y0, 0, &shift) != GSHGEOM_OK || !gvec_lw_compose(&b->local, &shift, &placed)", "the subregion's x does not place the element"),
    ("FE2", "src/codec/svg/lower/lower_filter.c", "    if (ctx->filter_active[i] == filter) {", "    if (0) {", "a filter met again through its own feImage is not a cycle"),
    ("FE3", "src/codec/svg/lower/lower_filter.c", "  p->slot = (uint32_t) b->slot_count;", "  p->slot = 0;", "every feImage of an element draws the first slot"),
    ("FE4", "src/codec/svg/lower/lower_filter.c", "    g = gshscene_list_instance(list, b->slot_pictures[i], &b->slot_maps[i], 0, 0.0);", "    g = gshscene_list_instance(list, b->slot_pictures[i], &b->slot_maps[0], 0, 0.0);", "every slot is placed as the first"),
    ("FE5", "src/codec/svg/lower/lower_filter.c", "  if (cycled || !holder->first) {", "  if (cycled) {", "an element that draws nothing is a slot of an empty picture"),
    ("FE6", "src/codec/svg/lower/lower_filter.c", " y0 = (p->subregion_mask & 2u) ? sub[1] : b->region[1];\n  if (x0 - x0 != 0 || y0 - y0 != 0) {\n    transparent_result(p); /* an edge that is not finite */", " y0 = (p->subregion_mask & 2u) ? sub[1] : 0;\n  if (x0 - x0 != 0 || y0 - y0 != 0) {\n    transparent_result(p); /* an edge that is not finite */", "the filter region's y is not the default"),
    # context paint outside a context, held magnitudes, currentColor in a filter.
    ("CX1", "src/codec/svg/lower/lower_marker.c", "      memset(out, 0, sizeof *out);\n      out->kind = GVEC_SVG_PAINT_NONE;", "      *out = v.paint;", "a context paint outside a context is passed on unresolved"),
    ("HV1", "src/codec/svg/lower/lower_filter.c", "  if (*x > max) {\n    *x = max;", "  if (*x > max + 1.0e300) {\n    *x = max;", "a coefficient past the bound is not held to it"),
    ("HV2", "src/codec/svg/lower/lower_filter.c", "    hold_all(list_region + 2, 2, &held); /* the size is a length too", "    (void) held; /* the size is a length too", "a region's size past the bound is not held"),
    ("CC1", "src/codec/svg/lower/lower_filter.c", "    out->rgba[3] = 1;\n  }\n  out->rgba[3] *= c->color.rgba[3];", "    out->rgba[3] = 0;\n  }\n  out->rgba[3] *= c->color.rgba[3];", "currentColor in a flood is transparent when the lowering sets no foreground"),
    # an SVG image that is the image of itself (design R3).
    ("RC1", "src/codec/svg/lower/lower_image.c", "    if (up->source_hash == source_hash) {", "    if (up->source_hash == source_hash + 1) {", "a document that is the image of itself nests to the limit"),
    ("RC2", "src/codec/svg/lower/lower_walk.c", "  nested->source_hash = image->source_hash;", "  nested->source_hash = 0;", "the context of an image does not know which document it is"),
    # text positions are indexed by Unicode code point (SVG 2, addressable character).
    ("TU1", "src/codec/svg/lower/lower_text.c", "for (size_t at = l->start; at < l->end && at < t->count && at - l->start < l->count; at++) {", "for (size_t at = l->start; at < l->end && at < t->count && at - l->start < l->count; at += t->chars[at].cp > 0xFFFF ? 2u : 1u) {", "a character beyond plane 0 takes two position values"),
    # the font features of the cascade (read_features and what it applies)
    ("FV1", TX, "    if (strcmp(v.keyword, \"none\") == 0) {\n      fits = fits && set_feature(st, \"kern\", 0);", "    if (strcmp(v.keyword, \"none\") == 0) {\n      fits = fits && set_feature(st, \"kern\", 1);", "font-kerning none turns kerning on"),
    ("FV2", TX, "      st->features[i].value = value;\n      return true;", "      return true;", "a feature set again keeps its first value"),
    ("FV3", TX, "        if (!set_feature(st, ligatures[k], 0)) {", "        if (!set_feature(st, ligatures[k], 1)) {", "none turns the ligatures on"),
    ("FV4", TX, "{\"no-common-ligatures\", {\"liga\", \"clig\", NULL}, false}", "{\"no-common-ligatures\", {\"liga\", \"clig\", NULL}, true}", "no-common-ligatures asks for them"),
    ("FV5", TX, "{\"discretionary-ligatures\", {\"dlig\", NULL}, true}", "{\"discretionary-ligatures\", {\"hlig\", NULL}, true}", "discretionary ligatures are the historical ones"),
    ("FV7", TX, "      const GFNT_Tag off = offs[o];\n      bool asked = false;", "      const GFNT_Tag off = offs[o];\n      bool asked = true;", "letter-spacing keeps a ligature the text asks for off"),
    ("FV8", TX, "      else if (strncmp(p, \"off\", 3) == 0) {\n        value = 0;", "      else if (strncmp(p, \"off\", 3) == 0) {\n        value = 1;", "off is on"),
    ("FV9", TX, "      if (!close || close - tag != 4) {", "      if (!close || close - tag != 3) {", "a feature tag is three letters"),
    ("FV11", TX, "  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_FEATURE_SETTINGS, &v, &ok);", "  r = read_style(ctx, inst, GVEC_SVG_PROP_FONT_KERNING, &v, &ok);", "font-feature-settings is not read"),
    # the units of a length attribute (gvec_lw_unit_length): font-relative, the root's, the window's, and whose style an em is in
    ("UL1", W, "(length->unit == GVEC_SVG_UNIT_EM ? 1.0 : 0.5)", "(length->unit == GVEC_SVG_UNIT_EM ? 0.5 : 1.0)", "an em is half a font size and an ex a whole one"),
    ("UL2", W, "    r = gvec_lw_instance_of(ctx, owner, &at);\n    if (r != GVEC_OK) {\n      return r;\n    }\n    r = gvec_svg_get(ctx->style, at, GVEC_SVG_PROP_FONT_SIZE, &v);", "    r = gvec_lw_instance_of(ctx, owner, &at);\n    if (r != GVEC_OK) {\n      return r;\n    }\n    r = gvec_svg_get(ctx->style, gvec_svg_instance_root(ctx->style), GVEC_SVG_PROP_FONT_SIZE, &v);", "an em is the root's font size, not the element's"),
    ("UL3", W, "    *out = length->value * side / 100.0;", "    *out = length->value * side / 10.0;", "a vw is a tenth of the viewport"),
    ("UL4", W, "GVEC_SVG_UNIT_VMIN ? (w < h ? w : h)", "GVEC_SVG_UNIT_VMIN ? (w < h ? h : w)", "a vmin is the larger side"),
    ("UL5", W, "length->unit == GVEC_SVG_UNIT_VH ? h :", "length->unit == GVEC_SVG_UNIT_VH ? w :", "a vh is of the width"),
    ("UL6", W, "    r = gvec_svg_get(ctx->style, gvec_svg_instance_root(ctx->style), GVEC_SVG_PROP_FONT_SIZE, &v);\n    if (r != GVEC_OK) {\n      *usable = false;\n      return r == GVEC_ERR_OOM ? r : GVEC_OK;\n    }\n    *out = length->value * v.number;", "    r = gvec_lw_instance_of(ctx, owner, &at);\n    if (r != GVEC_OK) {\n      return r;\n    }\n    r = gvec_svg_get(ctx->style, at, GVEC_SVG_PROP_FONT_SIZE, &v);\n    if (r != GVEC_OK) {\n      *usable = false;\n      return r == GVEC_ERR_OOM ? r : GVEC_OK;\n    }\n    *out = length->value * v.number;", "a rem is the element's font size"),
    ("UL7", "src/codec/svg/lower/lower_gradient.c", "  return e ? e : start;\n}", "  return e ? start : start;\n}", "a gradient's length is in the em of the gradient that was asked, not the one that has the attribute"),
    ("UL8", "src/codec/svg/lower/lower_pattern.c", "    if (attribute && gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_RAW) {\n      return e;\n    }\n  }\n  return start;", "    if (attribute && gvec_svg_attribute_class(attribute) != GVEC_SVG_ATTR_RAW) {\n      return start;\n    }\n  }\n  return start;", "a pattern's length is in the em of the pattern that was asked"),
    ("UL9", "src/codec/svg/lower/lower_mask.c", "  *error = gvec_lw_unit_length(ctx, mask, &length, out, &usable);", "  *error = gvec_lw_unit_length(ctx, ctx->document->root, &length, out, &usable);", "a mask's length is in the em of the root"),
    ("UL10", "src/codec/svg/lower/lower_text.c", "          GVEC_Result ur = gvec_lw_unit_length(ctx, child, &sl, &tp->offset, &usable);", "          GVEC_Result ur = gvec_lw_unit_length(ctx, ctx->document->root, &sl, &tp->offset, &usable);", "a startOffset is in the em of the root"),
    # a transform whose origin and percentages are of a bounding box (gvec_lw_box_transform and its sites)
    ("TB1", "src/codec/svg/style/svg_transform.c", "if ((percent || count > 0) && out->box != GVEC_SVG_BOX_VIEW) {", "if (percent && out->box != GVEC_SVG_BOX_VIEW) {", "a transform in a fill box with an origin of 0% is not of the box"),
    ("TB2", CL, "transform->box == GVEC_SVG_BOX_STROKE || transform->box == GVEC_SVG_BOX_BORDER;", "transform->box == GVEC_SVG_BOX_STROKE;", "the border box is not the stroke box"),
    ("TB3", CL, "const bool stroke = transform->box == GVEC_SVG_BOX_STROKE || transform->box == GVEC_SVG_BOX_BORDER;", "const bool stroke = false;", "the stroke box is the fill box"),
    ("TB4", CL, "const double x = box.empty ? 0 : box.min.x, y = box.empty ? 0 : box.min.y;", "const double x = box.empty ? 0 : box.min.x, y = box.empty ? 0 : box.min.x;", "the box's top is its left"),
    ("TB5", CL, "const double w = box.empty ? 0 : box.max.x - box.min.x, h = box.empty ? 0 : box.max.y - box.min.y;", "const double w = box.empty ? 0 : box.max.x - box.min.x, h = box.empty ? 0 : box.max.x - box.min.x;", "the box's height is its width"),
    ("TB7", CL, "  ctx->box_stroke = saved;\n  if (r != GVEC_OK || !*known) {", "  ctx->box_stroke = !saved;\n  if (r != GVEC_OK || !*known) {", "the stroke flag of an enclosing box is not restored"),
    ("TB8", W, "else if (!gvec_lw_compose(frame, &own_map, &c->local)) {", "else if (!gvec_lw_compose(frame, &v.transform.map, &c->local)) {", "an element's transform ignores its box"),
    ("TB9", "src/codec/svg/lower/lower_mask.c", "  if (!gvec_lw_compose(frame, &own_map, &local)) {", "  if (!gvec_lw_compose(frame, &v.transform.map, &local)) {", "a masked element's transform ignores its box"),
    ("TB10", "src/codec/svg/lower/lower_filter.c", "  if (!gvec_lw_compose(frame, &own_map, &local)) {", "  if (!gvec_lw_compose(frame, &v.transform.map, &local)) {", "a filtered element's transform ignores its box"),
    ("TB12", "src/codec/svg/lower/lower_text.c", "          r = gvec_lw_box_transform(ctx, target_inst, &tv.transform, &path_map, &known);", "          known = true; r = GVEC_OK;", "a path's transform in a box is not made"),
    # colour bitmap glyphs (CBDT): the picture of a glyph a strike carries, the blank glyph of a face of pictures only, the placement, the budget.
    ("CB1", TX, "  if (fr != GFNT_OK) {\n    return GVEC_OK; /* the strike does not carry this glyph", "  if (0) {\n    return GVEC_OK; /* the strike does not carry this glyph", "a glyph the strike lacks is taken for a picture of nothing"),
    ("CB2", TX, "      if (!is_colour && glyphs[g].face && !gfnt_face_has_outlines(glyphs[g].face)) {", "      if (0 && !is_colour && glyphs[g].face && !gfnt_face_has_outlines(glyphs[g].face)) {", "a glyph of a face of pictures that its strike lacks is not blank"),
    ("CB3", TX, "      if (!is_colour && glyphs[g].face && !gfnt_face_has_outlines(glyphs[g].face)) {", "      if (glyphs[g].face && !gfnt_face_has_outlines(glyphs[g].face)) {", "a colour glyph of a face of pictures is skipped as blank"),
    ("CB4", TX, "  ctx->image_decoded += bytes;\n  const double sx", "  const double sx", "a colour glyph's picture does not count against the decoded bytes"),
    ("CB5", TX, "ctx->image_decoded + bytes > (uint64_t) limits->max_image_decoded_bytes) {\n    result = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, \"the colour glyphs", "ctx->image_decoded + bytes >= (uint64_t) limits->max_image_decoded_bytes) {\n    result = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, \"the colour glyphs", "a picture of exactly the budget is refused"),
    ("CB6", TX, "const double sx = size / (double) strike.ppem_x, sy = size / (double) strike.ppem_y;", "const double sx = size / (double) strike.ppem_y, sy = size / (double) strike.ppem_x;", "the strike's ppem of each axis scales the other"),
    ("CB7", TX, "const double t[2] = {x + (double) bitmap.bearing_x * sx, y - (double) bitmap.bearing_y * sy};", "const double t[2] = {x + (double) bitmap.bearing_x * sx, y + (double) bitmap.bearing_y * sy};", "a picture hangs below the baseline by its bearing"),
    ("CB8", TX, "const double t[2] = {x + (double) bitmap.bearing_x * sx, y - (double) bitmap.bearing_y * sy};", "const double t[2] = {x, y - (double) bitmap.bearing_y * sy};", "a picture's left bearing is dropped"),
    ("CB9", TX, "  uint32_t ppem = size >= 1 ? (uint32_t) (size + 0.5) : 1;", "  uint32_t ppem = size >= 1 ? (uint32_t) size : 1;", "the size is truncated, not rounded, to the strike's ppem"),
    ("CB10", TX, "  const uint64_t bytes = (uint64_t) bitmap.width * (uint64_t) bitmap.height * 4u;", "  const uint64_t bytes = (uint64_t) bitmap.width * (uint64_t) bitmap.height * 3u;", "a picture's decoded size is counted at three bytes a pixel"),
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


def stale_anchors(cases):
    """The plants whose text is not in the source exactly once: a stale anchor found now, not hours into a run."""
    stale = []
    cache = {}
    for rule, edits, _ in cases:
        for path, old, _new in edits:
            if path not in cache:
                with open(os.path.join(ROOT, path)) as h:
                    cache[path] = h.read()
            count = cache[path].count(old)
            if count != 1:
                stale.append("%s: %s occurs %d times" % (rule, path, count))
    return stale


def main():
    work = env("PLANT_WORK")
    shutil.rmtree(work, ignore_errors=True)
    everything = [(rule, [(path, old, new)], what) for rule, path, old, new, what in PLANTS] + PAIRED
    stale = stale_anchors(everything)
    if stale:
        print("check-planted: %d plants name text that is not in the source exactly once:\n  %s" % (len(stale), "\n  ".join(stale)), file=sys.stderr)
        return 1
    if os.environ.get("PLANT_ANCHORS_ONLY"):
        print("check-planted: all %d plants name text that is in the source exactly once" % len(everything))
        return 0
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
    # Resume a run that stopped (a stale anchor, a lost session): the rule to start from, in the order the plants are written.
    start = os.environ.get("PLANT_FROM")
    if start:
        names = [c[0] for c in cases]
        if start not in names:
            sys.exit("check-planted: PLANT_FROM names no rule: %s" % start)
        cases = cases[names.index(start):]
    # A change to some files is held to the plants in those files: a comma-separated list of source paths, as the plants name them.
    files = os.environ.get("PLANT_FILES")
    if files:
        wanted = set(files.split(","))
        cases = [c for c in cases if any(edit[0] in wanted for edit in c[1])]
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
