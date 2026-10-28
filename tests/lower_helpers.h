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
 * What the lowering tests share: a document lowered both ways, with the equality gate checked every time.
 */

#ifndef GHOTI_IO_GVEC_LOWER_HELPERS_H
#define GHOTI_IO_GVEC_LOWER_HELPERS_H

#include "test_helpers.h"

#include <cstdio>
#include <cstring>

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/shape-scene.h>

namespace gvec_test {

/** Load SVG text with a viewport (0 for the default). */
inline void LoadSvg(const std::string & svg, Loaded & out, double viewport_width = 0, double viewport_height = 0,
    const GVEC_Limits * limits = nullptr) {
  std::vector<unsigned char> bytes = Bytes(svg);
  GVEC_Stream * stream = nullptr;
  ASSERT_EQ(gvec_stream_create_memory(bytes.data(), bytes.size(), &stream), GVEC_OK);
  GVEC_Load_Options options;
  std::memset(&options, 0, sizeof(options));
  options.limits = limits;
  options.viewport_width = viewport_width;
  options.viewport_height = viewport_height;
  out.result = gvec_doc_load(stream, &options, &out.diagnostics, &out.doc);
  gvec_stream_destroy(stream);
}

/** The text of a document: the root element with the namespace, and `body` inside it. */
inline std::string Svg(const std::string & attributes, const std::string & body) {
  return "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" " + attributes + ">" + body + "</svg>";
}

/** One document lowered into a list and into an edit of a scene that shares the list's store. */
struct Lowered {
  Loaded loaded;
  GSHSCENE_Scene * scene = nullptr;
  GSHSCENE_List * list = nullptr;                 ///< The recorded list, closed.
  const GSHSCENE_Version * version = nullptr;     ///< What the edit published.
  const GSHSCENE_List * emitted = nullptr;        ///< The version's list.
  GVEC_Diagnostics by_list{};
  GVEC_Diagnostics by_edit{};
  GVEC_Lower_Trace * list_trace = nullptr;
  GVEC_Lower_Trace * edit_trace = nullptr;
  GVEC_Result list_result = GVEC_ERR_INTERNAL;
  GVEC_Result edit_result = GVEC_ERR_INTERNAL;
  const GSHSCENE_Limits * scene_limits = nullptr;  ///< The limits the scene and the list are made with, if not the defaults.
  const GSHSCENE_Limits * walk_limits = nullptr;   ///< The limits the lowering is told, when they are not the scene's.
  GSHSCENE_Result publish_result = GSHSCENE_OK;    ///< What publishing the edit returned (checked OK by LowerBoth unless `expect_publish` is false).
  bool expect_publish = true;

  Lowered() {
    gvec_diagnostics_init(&by_list, nullptr);
    gvec_diagnostics_init(&by_edit, nullptr);
    gvec_lower_trace_create(nullptr, &list_trace);
    gvec_lower_trace_create(nullptr, &edit_trace);
  }
  Lowered(const Lowered &) = delete;
  Lowered & operator=(const Lowered &) = delete;
  ~Lowered() {
    gshscene_version_release(version);
    gshscene_list_destroy(list);
    gshscene_scene_destroy(scene);
    gvec_lower_trace_free(list_trace);
    gvec_lower_trace_free(edit_trace);
    gvec_diagnostics_destroy(&by_list);
    gvec_diagnostics_destroy(&by_edit);
  }

  size_t Items() const { return gshscene_list_count(list); }
  const GSHSCENE_Item & Item(size_t i) const {
    const GSHSCENE_Item * item = nullptr;
    EXPECT_EQ(gshscene_list_item(list, i, &item), GSHSCENE_OK);
    static const GSHSCENE_Item zero{};
    return item ? *item : zero;
  }
  size_t Count(GSHSCENE_Item_Kind kind) const {
    size_t n = 0;
    for (size_t i = 0; i < Items(); i++) {
      n += Item(i).kind == kind;
    }
    return n;
  }
  size_t Refusals() const { return gvec_diagnostics_count_code(&by_list, GVEC_DIAG_LOWER_REFUSED); }
  /** The first refusal with this reason, or nullptr. */
  const GVEC_Diagnostic * Refusal(GVEC_Lower_Reason reason) const {
    for (size_t i = 0; i < by_list.count; i++) {
      if (by_list.items[i].code == GVEC_DIAG_LOWER_REFUSED && by_list.items[i].detail == (uint32_t) reason) {
        return &by_list.items[i];
      }
    }
    return nullptr;
  }
};

/**
 * Lower `loaded` both ways into `out` and check the equality gate: the list the published edit emits equals the list
 * recorded directly, bit for bit, and both report the same diagnostics.
 */
inline void LowerBoth(Loaded & loaded, Lowered & out, const GVEC_Lower_Options * extra = nullptr) {
  ASSERT_EQ(loaded.result, GVEC_OK);
  ASSERT_EQ(gshscene_scene_create(nullptr, out.scene_limits, nullptr, &out.scene), GSHSCENE_OK);
  GSHSCENE_Store * store = gshscene_scene_store(out.scene);
  ASSERT_EQ(gshscene_list_create(nullptr, out.scene_limits, store, &out.list), GSHSCENE_OK);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof(options));
  if (extra) {
    options = *extra;
  }
  options.store = store;
  options.scene_limits = out.walk_limits ? out.walk_limits : out.scene_limits;
  options.trace = out.list_trace;
  out.list_result = gvec_doc_lower_list(loaded.doc, &options, out.list, &out.by_list);
  ASSERT_EQ(out.list_result, GVEC_OK);
  ASSERT_EQ(gshscene_list_close(out.list), GSHSCENE_OK);
  GSHSCENE_Edit * edit = nullptr;
  ASSERT_EQ(gshscene_scene_open_edit(out.scene, nullptr, &edit), GSHSCENE_OK);
  options.trace = out.edit_trace;
  out.edit_result = gvec_doc_lower_edit(loaded.doc, &options, edit, &out.by_edit);
  ASSERT_EQ(out.edit_result, GVEC_OK);
  GSHSCENE_Problems * problems = nullptr;
  GSHSCENE_Result published = gshscene_scene_publish(out.scene, edit, &problems, &out.version);
  out.publish_result = published;
  if (out.expect_publish) {
    EXPECT_EQ(published, GSHSCENE_OK);
  }
  gshscene_problems_destroy(problems);
  gshscene_edit_destroy(edit);
  if (published != GSHSCENE_OK) {
    return;
  }
  ASSERT_EQ(gshscene_version_list(out.version, &out.emitted), GSHSCENE_OK);
  EXPECT_TRUE(gshscene_list_equal(out.list, out.emitted)) << "the edit's list differs from the recorded one";
  EXPECT_EQ(gshscene_list_hash(out.list), gshscene_list_hash(out.emitted));
  ASSERT_EQ(out.by_list.count, out.by_edit.count);
  for (size_t i = 0; i < out.by_list.count; i++) {
    EXPECT_EQ(out.by_list.items[i].code, out.by_edit.items[i].code);
    EXPECT_EQ(out.by_list.items[i].detail, out.by_edit.items[i].detail);
    EXPECT_EQ(out.by_list.items[i].line, out.by_edit.items[i].line);
  }
}

/** A path resource as text: `M x,y L x,y Q cx,cy x,y C c1x,c1y c2x,c2y x,y Z`, numbers as %g. */
inline std::string Describe(const GSHSCENE_Resource * resource) {
  const GSHGEOM_Path * path = gshscene_resource_path(resource);
  if (!path) {
    return "(not a path)";
  }
  std::string out;
  char buffer[128];
  for (size_t i = 0; i < gshgeom_path_element_count(path); i++) {
    GSHGEOM_Element e;
    if (gshgeom_path_element(path, i, &e) != GSHGEOM_OK) {
      return out + " (unreadable)";
    }
    if (!out.empty()) {
      out += ' ';
    }
    switch (e.verb) {
    case GSHGEOM_VERB_MOVE:
      std::snprintf(buffer, sizeof(buffer), "M%g,%g", e.curve.p[0].x, e.curve.p[0].y);
      break;
    case GSHGEOM_VERB_LINE:
      std::snprintf(buffer, sizeof(buffer), "L%g,%g", e.curve.p[1].x, e.curve.p[1].y);
      break;
    case GSHGEOM_VERB_QUADRATIC:
      std::snprintf(buffer, sizeof(buffer), "Q%g,%g %g,%g", e.curve.p[1].x, e.curve.p[1].y, e.curve.p[2].x, e.curve.p[2].y);
      break;
    case GSHGEOM_VERB_CUBIC:
      std::snprintf(buffer, sizeof(buffer), "C%g,%g %g,%g %g,%g", e.curve.p[1].x, e.curve.p[1].y, e.curve.p[2].x, e.curve.p[2].y,
          e.curve.p[3].x, e.curve.p[3].y);
      break;
    case GSHGEOM_VERB_CLOSE:
      std::snprintf(buffer, sizeof(buffer), "Z");
      break;
    default:
      std::snprintf(buffer, sizeof(buffer), "?%d", (int) e.verb);
      break;
    }
    out += buffer;
  }
  return out;
}

/** Whether a map is the identity, in every entry. */
inline bool IsIdentity(const GSHGEOM_Map & map) {
  return map.dim == 2 && map.m[0][0] == 1 && map.m[0][1] == 0 && map.m[1][0] == 0 && map.m[1][1] == 1 && map.t[0] == 0 && map.t[1] == 0;
}

/** Lower the document again into `scene` as an edit on its current version, and publish it. */
inline GSHSCENE_Result Relower(GSHSCENE_Scene * scene, const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    const GSHSCENE_Version ** out_version, GVEC_Diagnostics * diagnostics = nullptr) {
  GSHSCENE_Edit * edit = nullptr;
  GSHSCENE_Result r = gshscene_scene_open_edit(scene, nullptr, &edit);
  if (r != GSHSCENE_OK) {
    return r;
  }
  GVEC_Result lowered = gvec_doc_lower_edit(doc, options, edit, diagnostics);
  EXPECT_EQ(lowered, GVEC_OK);
  r = gshscene_scene_publish(scene, edit, nullptr, out_version);
  gshscene_edit_destroy(edit);
  return r;
}

/** Load `svg` and lower it both ways. */
inline void LowerSvg(const std::string & svg, Lowered & out, double viewport_width = 0, double viewport_height = 0,
    const GVEC_Lower_Options * extra = nullptr) {
  LoadSvg(svg, out.loaded, viewport_width, viewport_height);
  LowerBoth(out.loaded, out, extra);
}

} // namespace gvec_test

#endif /* GHOTI_IO_GVEC_LOWER_HELPERS_H */
