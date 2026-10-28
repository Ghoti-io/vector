/**
 * @file
 *
 * The report mapped back to the document (P9): an entry of a sink's report is an item of the lowered list, and the trace says
 * which element it came from, for the list and for the edit, through pictures, and for what it cannot place.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** A stream target that keeps nothing. */
struct Discard {
  GSHSCENE_Writer writer{};
  GSHSCENE_Target target{};
  Discard() {
    writer.write = [](void *, const void *, size_t) { return GSHSCENE_OK; };
    target.kind = GSHSCENE_TARGET_STREAM;
    target.surface = &writer;
    target.unit = GSHSCENE_UNIT_MILLIMETER;
  }
};

/** Render `list` on the test sink with `max_extent`, filling `report`. */
GSHSCENE_Result Render(const GSHSCENE_List * list, double max_extent, GSHSCENE_Report * report, uint32_t flags = 0) {
  GSHSCENE_Dump_Sink_Config config;
  gshscene_dump_sink_config_default(&config);
  config.max_extent = max_extent;
  GSHSCENE_Sink * sink = nullptr;
  EXPECT_EQ(gshscene_dump_sink_create(nullptr, nullptr, &config, &sink), GSHSCENE_OK);
  Discard discard;
  discard.target.flags = flags;
  GSHSCENE_Render_Request request{};
  request.list = list;
  request.target = &discard.target;
  request.report = report;
  GSHSCENE_Result r = gshscene_sink_render(sink, &request);
  gshscene_sink_destroy(sink);
  return r;
}

const char * const kSmall = "M0 0H4V4H0Z";
const char * const kBig = "M0 0H80V80H0Z";

/** The index of the first refused entry of a report, or the count. */
size_t FirstRefused(const GSHSCENE_Report * report) {
  for (size_t i = 0; i < gshscene_report_count(report); i++) {
    GSHSCENE_Report_Entry e;
    EXPECT_EQ(gshscene_report_entry(report, i, &e), GSHSCENE_OK);
    if (e.entry_class == GSHSCENE_ENTRY_REFUSED) {
      return i;
    }
  }
  return gshscene_report_count(report);
}

struct Reported {
  GSHSCENE_Report * report = nullptr;
  Reported() { EXPECT_EQ(gshscene_report_create(nullptr, nullptr, &report), GSHSCENE_OK); }
  ~Reported() { gshscene_report_destroy(report); }
  Reported(const Reported &) = delete;
  Reported & operator=(const Reported &) = delete;
};

}  // namespace

TEST(LowerReport, ARefusedItemOfAListIsMappedToItsElementAndLine) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"", std::string("\n<path d=\"") + kSmall + "\"/>\n<path d=\"" + kBig + "\" fill=\"red\"/>\n<path d=\"" + kSmall + "\"/>"), l);
  Reported r;
  ASSERT_EQ(Render(l.list, 30.0, r.report), GSHSCENE_OK);
  size_t index = FirstRefused(r.report);
  ASSERT_LT(index, gshscene_report_count(r.report));
  GVEC_Lower_Site site;
  ASSERT_EQ(gvec_lower_trace_element(l.list_trace, nullptr, r.report, index, &site), GVEC_OK);
  EXPECT_TRUE(site.placed);
  EXPECT_STREQ(site.element, "path");
  EXPECT_EQ(site.line, (size_t) 3);
  EXPECT_GT(site.column, (size_t) 0);
  EXPECT_EQ(site.use_depth, (size_t) 0);
}

TEST(LowerReport, TheSameEntryIsMappedThroughTheVersionsListAndTheNodesKey) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"", std::string("\n<path d=\"") + kSmall + "\"/>\n<path d=\"" + kBig + "\" fill=\"red\"/>\n<path d=\"" + kSmall + "\"/>"), l);
  Reported r;
  ASSERT_EQ(Render(l.emitted, 30.0, r.report), GSHSCENE_OK);
  size_t index = FirstRefused(r.report);
  ASSERT_LT(index, gshscene_report_count(r.report));
  GVEC_Lower_Site site;
  ASSERT_EQ(gvec_lower_trace_element(l.edit_trace, l.version, r.report, index, &site), GVEC_OK);
  EXPECT_TRUE(site.placed);
  EXPECT_STREQ(site.element, "path");
  EXPECT_EQ(site.line, (size_t) 3);
}

TEST(LowerReport, AnEntryInsideAPictureIsMappedThroughTheInstanceToTheReferentAndTheUsesDepth) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("\n<defs>\n<path id=\"big\" d=\"") + kBig + "\"/>\n</defs>\n<path d=\"" + kSmall + "\"/>\n<use href=\"#big\" fill=\"red\"/>"),
      l);
  for (int form = 0; form < 2; form++) {
    Reported r;
    ASSERT_EQ(Render(form == 0 ? l.list : l.emitted, 30.0, r.report), GSHSCENE_OK);
    size_t index = FirstRefused(r.report);
    ASSERT_LT(index, gshscene_report_count(r.report)) << form;
    GSHSCENE_Report_Entry e;
    ASSERT_EQ(gshscene_report_entry(r.report, index, &e), GSHSCENE_OK);
    EXPECT_EQ(e.chain_count, (size_t) 2) << "the instance, then the item of its picture";
    GVEC_Lower_Site site;
    ASSERT_EQ(gvec_lower_trace_element(form == 0 ? l.list_trace : l.edit_trace, form == 0 ? nullptr : l.version, r.report, index, &site), GVEC_OK);
    EXPECT_TRUE(site.placed) << form;
    EXPECT_STREQ(site.element, "path") << form;
    EXPECT_EQ(site.line, (size_t) 3) << "the referent's line, where the shape is written";
    EXPECT_EQ(site.use_depth, (size_t) 1) << form;
  }
}

TEST(LowerReport, AnEntryTheTraceCannotPlaceIsReturnedAsSuchAndNotDropped) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"", std::string("<path d=\"") + kBig + "\"/>"), l);
  // A render-level entry (the conflation flag) is about no item.
  Reported r;
  EXPECT_EQ(Render(l.list, 0.0, r.report, GSHSCENE_TARGET_SEAMLESS), GSHSCENE_ERR_REFUSED);
  ASSERT_GE(gshscene_report_count(r.report), (size_t) 1);
  GVEC_Lower_Site site;
  std::memset(&site, 0xff, sizeof site);
  ASSERT_EQ(gvec_lower_trace_element(l.list_trace, nullptr, r.report, 0, &site), GVEC_OK);
  EXPECT_FALSE(site.placed);
  EXPECT_EQ(site.element, nullptr);
  EXPECT_EQ(site.line, (size_t) 0);
  // An entry of a render of another list: its chain is not an item here.
  Lowered other;
  LowerSvg(Svg("width=\"200\" height=\"200\"", std::string("<path d=\"") + kBig + "\"/><path d=\"" + kBig + "\"/><path d=\"" + kBig + "\"/>"), other);
  Reported r2;
  ASSERT_EQ(Render(other.list, 30.0, r2.report), GSHSCENE_OK);
  size_t last = gshscene_report_count(r2.report) - 1;
  ASSERT_EQ(gvec_lower_trace_element(l.list_trace, nullptr, r2.report, last, &site), GVEC_OK);
  EXPECT_FALSE(site.placed);
  // Asked with a version, the render-level entry is unplaced too.
  ASSERT_EQ(gvec_lower_trace_element(l.edit_trace, l.version, r.report, 0, &site), GVEC_OK);
  EXPECT_FALSE(site.placed);
}

TEST(LowerReport, BadArgumentsAreInvalid) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"", std::string("<path d=\"") + kBig + "\"/>"), l);
  Reported r;
  ASSERT_EQ(Render(l.list, 30.0, r.report), GSHSCENE_OK);
  GVEC_Lower_Site site;
  EXPECT_EQ(gvec_lower_trace_element(nullptr, nullptr, r.report, 0, &site), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_lower_trace_element(l.list_trace, nullptr, nullptr, 0, &site), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_lower_trace_element(l.list_trace, nullptr, r.report, 0, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_lower_trace_element(l.list_trace, nullptr, r.report, 1000, &site), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_lower_trace_create(nullptr, nullptr), GVEC_ERR_INVALID);
  gvec_lower_trace_free(nullptr);
}

TEST(LowerReport, ATraceThatWasNeverFilledPlacesNothingAndALaterLoweringReplacesIt) {
  GVEC_Lower_Trace * trace = nullptr;
  ASSERT_EQ(gvec_lower_trace_create(nullptr, &trace), GVEC_OK);
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"", std::string("<path d=\"") + kBig + "\"/>"), l);
  Reported r;
  ASSERT_EQ(Render(l.list, 30.0, r.report), GSHSCENE_OK);
  GVEC_Lower_Site site;
  ASSERT_EQ(gvec_lower_trace_element(trace, nullptr, r.report, FirstRefused(r.report), &site), GVEC_OK);
  EXPECT_FALSE(site.placed);
  gvec_lower_trace_free(trace);
}

TEST(LowerReport, ALoweringReplacesWhatTheTraceHeld) {
  Lowered first;
  LowerSvg(Svg("width=\"200\" height=\"200\"", std::string("<path d=\"") + kBig + "\"/>"), first);
  // Lower a different document into the same trace.
  Loaded second;
  LoadSvg(Svg("width=\"200\" height=\"200\"", std::string("\n\n<path d=\"") + kSmall + "\"/>\n<path d=\"" + kBig + "\"/>"), second);
  GSHSCENE_Scene * scene = nullptr;
  GSHSCENE_List * list = nullptr;
  ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.store = gshscene_scene_store(scene);
  options.trace = first.list_trace;
  ASSERT_EQ(gvec_doc_lower_list(second.doc, &options, list, nullptr), GVEC_OK);
  ASSERT_EQ(gshscene_list_close(list), GSHSCENE_OK);
  Reported r;
  ASSERT_EQ(Render(list, 30.0, r.report), GSHSCENE_OK);
  GVEC_Lower_Site site;
  ASSERT_EQ(gvec_lower_trace_element(first.list_trace, nullptr, r.report, FirstRefused(r.report), &site), GVEC_OK);
  EXPECT_TRUE(site.placed);
  EXPECT_EQ(site.line, (size_t) 4);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
}

TEST(LowerReport, ALoweringIntoAListThatAlreadyHasItemsKeepsTheTraceAligned) {
  Loaded loaded;
  LoadSvg(Svg("width=\"200\" height=\"200\"", std::string("<path d=\"") + kSmall + "\"/>\n<path d=\"" + kBig + "\" fill=\"red\"/>"), loaded);
  ASSERT_EQ(loaded.result, GVEC_OK);
  GSHSCENE_Scene * scene = nullptr;
  GSHSCENE_List * list = nullptr;
  ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
  GSHSCENE_Store * store = gshscene_scene_store(scene);
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, store, &list), GSHSCENE_OK);
  // Two items of the caller's own, then the document.
  GSHGEOM_Path * path = nullptr;
  ASSERT_EQ(gshgeom_path_create(nullptr, nullptr, 2, &path), GSHGEOM_OK);
  gshgeom_path_move_to(path, GSHGEOM_Point{0, 0, 0});
  gshgeom_path_line_to(path, GSHGEOM_Point{3, 0, 0});
  gshgeom_path_line_to(path, GSHGEOM_Point{0, 3, 0});
  gshgeom_path_close(path);
  const GSHSCENE_Resource * resource = nullptr;
  ASSERT_EQ(gshscene_store_intern_path(store, path, &resource), GSHSCENE_OK);
  gshgeom_path_destroy(path);
  GSHSCENE_Paint paint;
  std::memset(&paint, 0, sizeof paint);
  paint.rgba[3] = 1.0;
  ASSERT_EQ(gshscene_list_fill(list, resource, nullptr, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0), GSHSCENE_OK);
  ASSERT_EQ(gshscene_list_fill(list, resource, nullptr, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0), GSHSCENE_OK);
  gshscene_resource_release(resource);
  GVEC_Lower_Trace * trace = nullptr;
  ASSERT_EQ(gvec_lower_trace_create(nullptr, &trace), GVEC_OK);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.store = store;
  options.trace = trace;
  ASSERT_EQ(gvec_doc_lower_list(loaded.doc, &options, list, nullptr), GVEC_OK);
  ASSERT_EQ(gshscene_list_count(list), (size_t) 4);
  ASSERT_EQ(gshscene_list_close(list), GSHSCENE_OK);
  Reported r;
  ASSERT_EQ(Render(list, 30.0, r.report), GSHSCENE_OK);
  GVEC_Lower_Site site;
  ASSERT_EQ(gvec_lower_trace_element(trace, nullptr, r.report, FirstRefused(r.report), &site), GVEC_OK);
  EXPECT_TRUE(site.placed);
  EXPECT_EQ(site.line, (size_t) 2) << "item 3 of the list is the document's second shape";
  // An entry about one of the caller's own items is not the document's.
  GSHSCENE_Report * own = nullptr;
  ASSERT_EQ(gshscene_report_create(nullptr, nullptr, &own), GSHSCENE_OK);
  GSHSCENE_Dump_Sink_Config config;
  gshscene_dump_sink_config_default(&config);
  config.max_extent = 1.0;
  GSHSCENE_Sink * sink = nullptr;
  ASSERT_EQ(gshscene_dump_sink_create(nullptr, nullptr, &config, &sink), GSHSCENE_OK);
  Discard discard;
  GSHSCENE_Render_Request request{};
  request.list = list;
  request.target = &discard.target;
  request.report = own;
  ASSERT_EQ(gshscene_sink_render(sink, &request), GSHSCENE_OK);
  GSHSCENE_Report_Entry first;
  ASSERT_EQ(gshscene_report_entry(own, 0, &first), GSHSCENE_OK);
  ASSERT_EQ(first.chain_count, (size_t) 1);
  EXPECT_EQ(first.chain[0], (size_t) 0);
  ASSERT_EQ(gvec_lower_trace_element(trace, nullptr, own, 0, &site), GVEC_OK);
  EXPECT_FALSE(site.placed);
  gshscene_sink_destroy(sink);
  gshscene_report_destroy(own);
  gvec_lower_trace_free(trace);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
}

TEST(LowerReport, AClosedListIsInvalid) {
  Loaded loaded;
  LoadSvg(Svg("width=\"20\" height=\"20\"", std::string("<path d=\"") + kSmall + "\"/>"), loaded);
  GSHSCENE_Scene * scene = nullptr;
  GSHSCENE_List * list = nullptr;
  ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
  ASSERT_EQ(gshscene_list_close(list), GSHSCENE_OK);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.store = gshscene_scene_store(scene);
  EXPECT_EQ(gvec_doc_lower_list(loaded.doc, &options, list, nullptr), GVEC_ERR_INVALID);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
