/**
 * @file
 *
 * Patterns: `pattern` as the waist's pattern resource. The cell in the units the document gives, the map of `patternTransform`,
 * the content's frame (a viewBox, or the scale of the box), what an `href` chain gives, the tile as a picture of the content,
 * and what a pattern that cannot be drawn amounts to.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

const char * kContent = "<rect width=\"10\" height=\"10\" fill=\"green\"/>";

/** The rect at (20, 20) of 100 by 40, filled with `url(#p)` over `defs`, which names its pattern `p`. */
bool Lower(const std::string & defs, Lowered & l, const std::string & root = "") {
  LowerSvg(Svg("width=\"200\" height=\"200\" " + root,
               "<defs>" + defs + "</defs><rect x=\"20\" y=\"20\" width=\"100\" height=\"40\" fill=\"url(#p)\"/>"),
      l);
  return l.Items() == 1;
}

bool Read(const Lowered & l, GSHSCENE_Pattern & out) {
  if (l.Items() != 1 || l.Item(0).paint.kind != GSHSCENE_PAINT_PATTERN) {
    return false;
  }
  return gshscene_resource_pattern(l.Item(0).paint.resource, &out) == GSHSCENE_OK;
}

}  // namespace

TEST(LowerPattern, ACellInUserUnitsIsTheCellAndTheStepsAreItsSize) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" x=\"3\" y=\"4\" width=\"20\" height=\"10\">") + kContent + "</pattern>", l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  EXPECT_EQ(p.x, 3.0);
  EXPECT_EQ(p.y, 4.0);
  EXPECT_EQ(p.w, 20.0);
  EXPECT_EQ(p.h, 10.0);
  EXPECT_EQ(p.x_step, 20.0);
  EXPECT_EQ(p.y_step, 10.0);
  EXPECT_EQ(p.repeat_x, (uint32_t) 1);
  EXPECT_EQ(p.repeat_y, (uint32_t) 1);
  EXPECT_EQ(p.map.m[0][0], 1.0);
  EXPECT_EQ(p.overflow, (uint32_t) 0);
  EXPECT_NE(p.tile, nullptr);
}

TEST(LowerPattern, TheCellIsAFractionOfTheBoundingBoxByDefault) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" x=\"0.1\" y=\"0.5\" width=\"0.25\" height=\"0.5\">") + kContent + "</pattern>", l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  EXPECT_EQ(p.x, 30.0);  // 20 + 0.1 * 100
  EXPECT_EQ(p.y, 40.0);  // 20 + 0.5 * 40
  EXPECT_EQ(p.w, 25.0);
  EXPECT_EQ(p.h, 20.0);
}

TEST(LowerPattern, APercentageOfTheBoxIsAFractionAndOfTheViewportIsOfTheViewport) {
  Lowered boxed;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" width=\"25%\" height=\"50%\">") + kContent + "</pattern>", boxed));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(boxed, p));
  EXPECT_EQ(p.w, 25.0);
  EXPECT_EQ(p.h, 20.0);
  Lowered user;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10%\" height=\"50%\">") + kContent + "</pattern>", user));
  ASSERT_TRUE(Read(user, p));
  EXPECT_EQ(p.w, 20.0);  // of the 200 wide viewport
  EXPECT_EQ(p.h, 100.0);
}

TEST(LowerPattern, ThePatternTransformIsTheMapFromPatternSpace) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\" patternTransform=\"translate(5 6) scale(2)\">") + kContent +
                        "</pattern>",
      l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  EXPECT_EQ(p.map.m[0][0], 2.0);
  EXPECT_EQ(p.map.m[1][1], 2.0);
  EXPECT_EQ(p.map.t[0], 5.0);
  EXPECT_EQ(p.map.t[1], 6.0);
}

TEST(LowerPattern, ATransformWithNoInverseDisablesThePaint) {
  Lowered l;
  EXPECT_FALSE(Lower(std::string("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\" patternTransform=\"scale(0)\">") + kContent + "</pattern>", l));
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_RENDERING_DISABLED), (size_t) 1);
}

TEST(LowerPattern, ASizeOfNothingDisablesThePaint) {
  for (const char * size : {"width=\"0\" height=\"10\"", "width=\"10\"", "height=\"10\"", "width=\"-3\" height=\"10\""}) {
    Lowered l;
    EXPECT_FALSE(Lower(std::string("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" ") + size + ">" + kContent + "</pattern>", l)) << size;
    EXPECT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_RENDERING_DISABLED), (size_t) 1) << size;
  }
}

TEST(LowerPattern, APatternWithNoContentPaintsNothing) {
  Lowered l;
  EXPECT_FALSE(Lower("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\"/>", l));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerPattern, AnHrefChainGivesTheAttributesAndTheContentOfTheFirstThatHasThem) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"base\" patternUnits=\"userSpaceOnUse\" width=\"12\" height=\"8\">") + kContent +
                        "</pattern><pattern id=\"p\" xlink:href=\"#base\" width=\"6\"/>",
      l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  EXPECT_EQ(p.w, 6.0);  // its own
  EXPECT_EQ(p.h, 8.0);  // the referent's
  EXPECT_NE(p.tile, nullptr);
}

TEST(LowerPattern, ACycleOfReferencesEndsAndUsesWhatTheChainHasGiven) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" xlink:href=\"#q\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\">") + kContent +
                        "</pattern><pattern id=\"q\" xlink:href=\"#p\" height=\"4\"/>",
      l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  EXPECT_EQ(p.h, 10.0);
}

TEST(LowerPattern, OverflowVisibleAsksForTheUnclippedTile) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\" overflow=\"visible\">") + kContent + "</pattern>", l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  EXPECT_EQ(p.overflow, (uint32_t) 1);
}

TEST(LowerPattern, TheTileHoldsTheContentUnderTheFrameOfTheCell) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" patternUnits=\"userSpaceOnUse\" x=\"7\" y=\"9\" width=\"10\" height=\"10\">") + kContent + "</pattern>", l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  const GSHSCENE_List * tile = gshscene_resource_picture(p.tile);
  ASSERT_NE(tile, nullptr);
  ASSERT_EQ(gshscene_list_count(tile), (size_t) 1);
  const GSHSCENE_Item * item = nullptr;
  ASSERT_EQ(gshscene_list_item(tile, 0, &item), GSHSCENE_OK);
  EXPECT_EQ(item->kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(item->map.t[0], 7.0);  // the content's origin is the cell's
  EXPECT_EQ(item->map.t[1], 9.0);
}

TEST(LowerPattern, ContentUnitsOfTheBoxScaleTheContentAndAViewBoxOverridesThem) {
  Lowered scaled;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" patternContentUnits=\"objectBoundingBox\" width=\"0.5\" height=\"0.5\">") +
                        "<rect width=\"0.1\" height=\"0.1\"/></pattern>",
      scaled));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(scaled, p));
  const GSHSCENE_List * tile = gshscene_resource_picture(p.tile);
  ASSERT_NE(tile, nullptr);
  const GSHSCENE_Item * item = nullptr;
  ASSERT_EQ(gshscene_list_item(tile, 0, &item), GSHSCENE_OK);
  EXPECT_EQ(item->map.m[0][0], 100.0);  // the box's width and height
  EXPECT_EQ(item->map.m[1][1], 40.0);

  Lowered boxed;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"p\" patternContentUnits=\"objectBoundingBox\" patternUnits=\"userSpaceOnUse\" width=\"20\" height=\"20\" viewBox=\"0 0 5 5\">") +
                        kContent + "</pattern>",
      boxed));
  ASSERT_TRUE(Read(boxed, p));
  tile = gshscene_resource_picture(p.tile);
  ASSERT_NE(tile, nullptr);
  ASSERT_EQ(gshscene_list_item(tile, 0, &item), GSHSCENE_OK);
  EXPECT_EQ(item->map.m[0][0], 4.0);  // 20 over 5, not the box's
}

TEST(LowerPattern, APatternThatPaintsItsOwnContentWithItselfIsCutByTheStyleLayerAndPaintsNothing) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               "<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\" fill=\"url(#p)\"/></pattern></defs>"
               "<rect width=\"50\" height=\"50\" fill=\"url(#p)\"/>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_GE(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_REFERENCE_CYCLE), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerPattern, ATileMayHoldAnotherPatternsPaint) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<pattern id=\"inner\" patternUnits=\"userSpaceOnUse\" width=\"2\" height=\"2\">") + kContent +
                        "</pattern><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\" fill=\"url(#inner)\"/></pattern>",
      l));
  GSHSCENE_Pattern p;
  ASSERT_TRUE(Read(l, p));
  const GSHSCENE_List * tile = gshscene_resource_picture(p.tile);
  ASSERT_NE(tile, nullptr);
  const GSHSCENE_Item * item = nullptr;
  ASSERT_EQ(gshscene_list_item(tile, 0, &item), GSHSCENE_OK);
  EXPECT_EQ(item->paint.kind, GSHSCENE_PAINT_PATTERN);
}

TEST(LowerPattern, AStrokeMayBePaintedWithAPatternToo) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\">") + kContent +
                   "</pattern></defs><rect x=\"20\" y=\"20\" width=\"100\" height=\"40\" fill=\"none\" stroke=\"url(#p)\" stroke-width=\"4\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(l.Item(0).paint.kind, GSHSCENE_PAINT_PATTERN);
}

TEST(LowerPattern, TheFillOpacityIsThePaintsAlpha) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\">") + kContent +
                   "</pattern></defs><rect width=\"50\" height=\"50\" fill=\"url(#p)\" fill-opacity=\"0.25\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).paint.rgba[3], 0.25);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
