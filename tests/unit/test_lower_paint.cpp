/**
 * @file
 *
 * The solid paint of a fill (P7, AD-23): straight double components in their space, the alpha of `fill-opacity`,
 * `currentColor` as the waist's foreground, and the paints that are not lowered yet.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** The only fill of a document with one shape, painted with `style`. */
bool OneFill(const std::string & style, Lowered & l, const std::string & around_open = "", const std::string & around_close = "") {
  LowerSvg(Svg("width=\"20\" height=\"20\"", around_open + "<path d=\"M0 0H4V4H0Z\" " + style + "/>" + around_close), l);
  return l.Items() == 1 && l.Item(0).kind == GSHSCENE_ITEM_FILL;
}

}  // namespace

TEST(LowerPaint, TheInitialFillIsOpaqueBlackInSRGB) {
  Lowered l;
  ASSERT_TRUE(OneFill("", l));
  const GSHSCENE_Paint & p = l.Item(0).paint;
  EXPECT_EQ(p.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(p.rgba[0], 0.0);
  EXPECT_EQ(p.rgba[1], 0.0);
  EXPECT_EQ(p.rgba[2], 0.0);
  EXPECT_EQ(p.rgba[3], 1.0);
  EXPECT_EQ(p.space.primaries, GCOL_PRIMARIES_SRGB);
}

TEST(LowerPaint, ColorsAreStraightComponentsNotBytes) {
  Lowered l;
  ASSERT_TRUE(OneFill("fill=\"#336699\"", l));
  EXPECT_EQ(l.Item(0).paint.rgba[0], 0x33 / 255.0);
  EXPECT_EQ(l.Item(0).paint.rgba[1], 0x66 / 255.0);
  EXPECT_EQ(l.Item(0).paint.rgba[2], 0x99 / 255.0);
  Lowered named;
  ASSERT_TRUE(OneFill("fill=\"rebeccapurple\"", named));
  EXPECT_EQ(named.Item(0).paint.rgba[0], 0x66 / 255.0);
  Lowered functional;
  ASSERT_TRUE(OneFill("fill=\"rgb(10% 20% 30% / 40%)\"", functional));
  EXPECT_EQ(functional.Item(0).paint.rgba[0], 0.1);
  EXPECT_EQ(functional.Item(0).paint.rgba[3], 0.4);
}

TEST(LowerPaint, HslIsExactAtThePrimaries) {
  Lowered green;
  ASSERT_TRUE(OneFill("fill=\"hsl(120 100% 50%)\"", green));
  EXPECT_EQ(green.Item(0).paint.rgba[0], 0.0);
  EXPECT_EQ(green.Item(0).paint.rgba[1], 1.0);
  EXPECT_EQ(green.Item(0).paint.rgba[2], 0.0);
  Lowered grey;
  ASSERT_TRUE(OneFill("fill=\"hsl(200 0% 50%)\"", grey));
  EXPECT_EQ(grey.Item(0).paint.rgba[0], 0.5);
  EXPECT_EQ(grey.Item(0).paint.rgba[2], 0.5);
}

TEST(LowerPaint, AWideGamutColorKeepsItsSpace) {
  Lowered l;
  ASSERT_TRUE(OneFill("fill=\"color(display-p3 1 0.5 0)\"", l));
  EXPECT_EQ(l.Item(0).paint.rgba[0], 1.0);
  EXPECT_EQ(l.Item(0).paint.rgba[1], 0.5);
  EXPECT_EQ(l.Item(0).paint.space.primaries, GCOL_PRIMARIES_DISPLAY_P3);
}

TEST(LowerPaint, AColorTheWaistCannotCarryRefusesTheElement) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4H0Z\" fill=\"lab(50% 40 59)\"/><path d=\"M0 0H4V4H0Z\"/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 1);
  ASSERT_NE(l.Refusal(GVEC_LOWER_UNSUPPORTED_COLOR_SPACE), nullptr);
  EXPECT_EQ(l.Refusals(), (size_t) 1);
  EXPECT_EQ(l.by_list.items[0].severity != GVEC_DIAG_NOTICE, true);
}

TEST(LowerPaint, FillOpacityMultipliesTheAlphaOfTheColor) {
  Lowered l;
  ASSERT_TRUE(OneFill("fill=\"rgb(255 0 0 / 50%)\" fill-opacity=\".5\"", l));
  EXPECT_EQ(l.Item(0).paint.rgba[3], 0.25);
}

TEST(LowerPaint, FillNoneAndAnAlphaOfZeroEmitNothing) {
  for (const char * style : {"fill=\"none\"", "fill-opacity=\"0\"", "fill=\"transparent\"", "fill=\"rgb(0 0 0 / 0)\""}) {
    Lowered l;
    LowerSvg(Svg("width=\"20\" height=\"20\"", std::string("<path d=\"M0 0H4V4H0Z\" ") + style + "/>"), l);
    EXPECT_EQ(l.Items(), (size_t) 0) << style;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << style;
  }
}

TEST(LowerPaint, CurrentColorIsTheForegroundUntilTheDocumentSetsAColor) {
  Lowered l;
  ASSERT_TRUE(OneFill("fill=\"currentColor\" fill-opacity=\".5\"", l));
  EXPECT_EQ(l.Item(0).paint.kind, GSHSCENE_PAINT_FOREGROUND);
  EXPECT_EQ(l.Item(0).paint.rgba[3], 0.5);
  Lowered set;
  ASSERT_TRUE(OneFill("fill=\"currentColor\"", set, "<g color=\"#0000ff\">", "</g>"));
  EXPECT_EQ(set.Item(0).paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(set.Item(0).paint.rgba[2], 1.0);
}

TEST(LowerPaint, TheForegroundOptionIsSetOnTheListAndOnTheEdit) {
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.flags = GVEC_LOWER_SET_FOREGROUND;
  gshscene_color_init(&options.foreground);
  options.foreground.rgba[0] = 0.25;
  options.foreground.rgba[1] = 0.5;
  options.foreground.rgba[2] = 0.75;
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4H0Z\" fill=\"currentColor\"/>"), l, 0, 0, &options);
  GSHSCENE_Color from_list, from_version;
  ASSERT_EQ(gshscene_list_foreground(l.list, &from_list), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_foreground(l.version, &from_version), GSHSCENE_OK);
  EXPECT_EQ(from_list.rgba[0], 0.25);
  EXPECT_EQ(from_version.rgba[1], 0.5);
  EXPECT_EQ(from_version.rgba[2], 0.75);
}

TEST(LowerPaint, AnInvalidForegroundIsRefusedBeforeAnythingIsDone) {
  Lowered l;
  LoadSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4H0Z\"/>"), l.loaded);
  GSHSCENE_Scene * scene = nullptr;
  GSHSCENE_List * list = nullptr;
  ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.store = gshscene_scene_store(scene);
  options.flags = GVEC_LOWER_SET_FOREGROUND;
  gshscene_color_init(&options.foreground);
  options.foreground.rgba[3] = 2.0;
  EXPECT_EQ(gvec_doc_lower_list(l.loaded.doc, &options, list, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gshscene_list_count(list), (size_t) 0);
  options.flags = 1u << 7;
  EXPECT_EQ(gvec_doc_lower_list(l.loaded.doc, &options, list, nullptr), GVEC_ERR_INVALID);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
}

TEST(LowerPaint, AGradientPaintThatCannotBeUsedRefusesTheElementWhole) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<defs><linearGradient id=\"g\"><stop stop-color=\"lab(50% 40 59)\"/></linearGradient></defs>"
               "<path d=\"M0 0H4V4H0Z\" fill=\"url(#g)\"/>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_NE(l.Refusal(GVEC_LOWER_GRADIENT), nullptr);
}

TEST(LowerPaint, AReferenceToNothingIsItsFallbackOrNone) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<path d=\"M0 0H4V4H0Z\" fill=\"url(#nope) #00ff00\"/><path d=\"M0 0H4V4H0Z\" fill=\"url(#nope)\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).paint.rgba[1], 1.0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerPaint, ContextPaintIsRefused) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4H0Z\" fill=\"context-fill\"/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_NE(l.Refusal(GVEC_LOWER_CONTEXT_PAINT), nullptr);
}

TEST(LowerPaint, AStrokeThatCannotBeSeenDrawsNothingAndOneThatCanIsAnItem) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<path d=\"M0 0H4V4H0Z\" stroke=\"red\"/>"
               "<line x1=\"0\" y1=\"0\" x2=\"5\" y2=\"5\" stroke=\"red\"/>"
               "<path d=\"M0 0H4V4H0Z\" stroke=\"red\" stroke-width=\"0\"/>"
               "<path d=\"M0 0H4V4H0Z\" stroke=\"red\" stroke-opacity=\"0\"/>"
               "<path d=\"M0 0H4V4H0Z\" stroke=\"none\"/>"),
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_STROKE), (size_t) 2);  // the first path's and the line's
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 4);    // the four paths, filled black; the line has no area
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
