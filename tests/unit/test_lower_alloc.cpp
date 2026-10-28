/**
 * @file
 *
 * Allocation failure in a lowering (AD-19): a failure of any allocation of either entry point, in the walk, the emitters, the
 * trace and the scene's own, is out of memory and leaks nothing.
 */

#include "../lower_helpers.h"

#include <algorithm>
#include <cstdlib>

using namespace gvec_test;

namespace {

/** The calls made after each part of the sequence. */
struct Sweep {
  GVEC_Result result = GVEC_OK;
  long after_load = 0, after_setup = 0, after_list = 0, after_edit_open = 0, after_edit = 0, after_publish = 0;
};

const char * const kDocument =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"40\" viewBox=\"0 0 20 20\">"
    "<defs><g id=\"g\"><path d=\"M0 0H3V3Z\" fill=\"red\"/></g></defs>"
    "<g opacity=\".5\" transform=\"translate(1 1)\"><path d=\"M0 0H5V5Z\"/><use href=\"#g\" x=\"7\"/></g>"
    "<svg x=\"2\" y=\"10\" width=\"8\" height=\"8\"><path d=\"M-1 -1H9V9Z\" fill=\"blue\"/></svg>"
    "<path d=\"M0 0A1 1 0 0 1 2 2\"/><use href=\"#g\" x=\"12\" y=\"12\"/></svg>";

/** Load, lower both ways and publish, with every allocation of the sequence going through `alloc`. */
Sweep RunSequence(Failing & alloc, GVEC_Lower_Trace * trace) {
  Sweep sweep;
  std::string text = kDocument;
  GVEC_Stream * stream = nullptr;
  EXPECT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
  GVEC_Load_Options load;
  std::memset(&load, 0, sizeof load);
  load.allocator = &alloc.a;
  GVEC_Diagnostics diagnostics;
  gvec_diagnostics_init(&diagnostics, &alloc.a);
  GVEC_Doc * doc = nullptr;
  GVEC_Result r = gvec_doc_load(stream, &load, &diagnostics, &doc);
  gvec_stream_destroy(stream);
  sweep.after_load = alloc.attempts;
  GSHSCENE_Scene * scene = nullptr;
  GSHSCENE_List * list = nullptr;
  GSHSCENE_Edit * edit = nullptr;
  const GSHSCENE_Version * version = nullptr;
  if (r == GVEC_OK) {
    GSHSCENE_Result g = gshscene_scene_create(&alloc.a, nullptr, nullptr, &scene);
    if (g == GSHSCENE_OK) {
      g = gshscene_list_create(&alloc.a, nullptr, gshscene_scene_store(scene), &list);
    }
    if (g != GSHSCENE_OK) {
      r = g == GSHSCENE_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
    }
  }
  sweep.after_setup = alloc.attempts;
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  if (r == GVEC_OK) {
    options.store = gshscene_scene_store(scene);
    options.scene_allocator = &alloc.a;
    options.trace = trace;
    r = gvec_doc_lower_list(doc, &options, list, &diagnostics);
  }
  sweep.after_list = alloc.attempts;
  if (r == GVEC_OK) {
    GSHSCENE_Result g = gshscene_scene_open_edit(scene, nullptr, &edit);
    if (g != GSHSCENE_OK) {
      r = g == GSHSCENE_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
    }
  }
  sweep.after_edit_open = alloc.attempts;
  if (r == GVEC_OK) {
    r = gvec_doc_lower_edit(doc, &options, edit, &diagnostics);
  }
  sweep.after_edit = alloc.attempts;
  if (r == GVEC_OK) {
    GSHSCENE_Result g = gshscene_scene_publish(scene, edit, nullptr, &version);
    if (g != GSHSCENE_OK) {
      r = g == GSHSCENE_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
    }
  }
  sweep.after_publish = alloc.attempts;
  sweep.result = r;
  gshscene_version_release(version);
  gshscene_edit_destroy(edit);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&diagnostics);
  return sweep;
}

}  // namespace

TEST(LowerAllocation, EveryFailureIsOutOfMemoryAndNothingLeaks) {
  GVEC_Lower_Trace * trace = nullptr;
  ASSERT_EQ(gvec_lower_trace_create(nullptr, &trace), GVEC_OK);
  Failing baseline;
  Sweep base = RunSequence(baseline, trace);
  ASSERT_EQ(base.result, GVEC_OK);
  EXPECT_EQ(baseline.outstanding, 0);
  // What building the engine costs: the context of a style, which each lowering makes. Its calls are the CSS engine's, which
  // `text`'s own sweeps fail one by one; here a spread of them is failed, and every call of this library's own, unless
  // GVEC_SWEEP_EVERY says how many to skip (1, the thorough run, fails each).
  long engine = 0;
  {
    Failing probe;
    GVEC_Stream * stream = nullptr;
    std::string text = kDocument;
    ASSERT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
    GVEC_Load_Options load;
    std::memset(&load, 0, sizeof load);
    load.allocator = &probe.a;
    GVEC_Doc * doc = nullptr;
    ASSERT_EQ(gvec_doc_load(stream, &load, nullptr, &doc), GVEC_OK);
    gvec_stream_destroy(stream);
    long before = probe.attempts;
    GVEC_Svg_Style_Context * context = nullptr;
    ASSERT_EQ(gvec_svg_style_context_new(gvec_svg_document(doc), nullptr, &probe.a, &context), GVEC_OK);
    engine = probe.attempts - before;
    gvec_svg_style_context_free(context);
    gvec_doc_free(doc);
  }
  ASSERT_GT(engine, 1000);
  long every = 8;
  if (const char * text_every = std::getenv("GVEC_SWEEP_EVERY")) {
    every = std::max(1L, std::atol(text_every));
  }
  long stride = every == 1 ? 1 : std::max(1L, engine / 60);
  long failures = 0, oom = 0;
  for (long fail_at = base.after_load + 1; fail_at <= base.after_publish; fail_at++) {
    // The first `engine` calls of each lowering are the engine's.
    bool in_list_engine = fail_at > base.after_setup && fail_at <= base.after_setup + engine;
    bool in_edit_engine = fail_at > base.after_edit_open && fail_at <= base.after_edit_open + engine;
    long phase_start = in_list_engine ? base.after_setup : in_edit_engine ? base.after_edit_open : 0;
    if ((in_list_engine || in_edit_engine) && (fail_at - phase_start) % stride != 0) {
      continue;
    }
    if (!(in_list_engine || in_edit_engine) && fail_at % every != 0) {
      continue;
    }
    Failing alloc;
    alloc.fail_at = fail_at;
    Sweep sweep = RunSequence(alloc, trace);
    EXPECT_TRUE(sweep.result == GVEC_ERR_OOM || sweep.result == GVEC_OK) << fail_at << " " << gvec_result_string(sweep.result);
    oom += sweep.result == GVEC_ERR_OOM;
    EXPECT_EQ(alloc.outstanding, 0) << "leak after failing allocation " << fail_at;
    failures++;
  }
  EXPECT_GT(failures, 100);
  EXPECT_GT(oom, failures / 2);
  gvec_lower_trace_free(trace);
}

TEST(LowerAllocation, ATraceWhoseAllocatorFailsIsOutOfMemoryAndLeavesNothing) {
  Loaded loaded;
  LoadSvg(kDocument, loaded);
  ASSERT_EQ(loaded.result, GVEC_OK);
  Failing probe;
  GVEC_Lower_Trace * trace = nullptr;
  ASSERT_EQ(gvec_lower_trace_create(&probe.a, &trace), GVEC_OK);
  GSHSCENE_Scene * scene = nullptr;
  ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
  GSHSCENE_List * list = nullptr;
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.store = gshscene_scene_store(scene);
  options.trace = trace;
  long before = probe.attempts;
  ASSERT_EQ(gvec_doc_lower_list(loaded.doc, &options, list, nullptr), GVEC_OK);
  long used = probe.attempts - before;
  ASSERT_GT(used, 0);
  gshscene_list_destroy(list);
  gvec_lower_trace_free(trace);
  for (long k = 1; k <= used; k++) {
    Failing alloc;
    GVEC_Lower_Trace * t = nullptr;
    ASSERT_EQ(gvec_lower_trace_create(&alloc.a, &t), GVEC_OK);
    GSHSCENE_List * l = nullptr;
    ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &l), GSHSCENE_OK);
    GVEC_Lower_Options o = options;
    o.trace = t;
    alloc.fail_at = alloc.attempts + k;
    GVEC_Result r = gvec_doc_lower_list(loaded.doc, &o, l, nullptr);
    EXPECT_EQ(r, GVEC_ERR_OOM) << k;
    gshscene_list_destroy(l);
    gvec_lower_trace_free(t);
    EXPECT_EQ(alloc.outstanding, 0) << "leak after failing allocation " << k;
  }
  gshscene_scene_destroy(scene);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
