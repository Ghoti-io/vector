#include "../lower_helpers.h"

using namespace gvec_test;

TEST(LowerUse, AUseOfAShapeIsOnePictureAndOneInstance) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<defs><path id=\"p\" d=\"M0 0H5V5H0Z\"/></defs><use href=\"#p\" x=\"10\" y=\"20\" fill=\"red\"/>"), l, 40, 40);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_INSTANCE);
  EXPECT_EQ(l.Item(0).map.t[0], 10.0);
  EXPECT_EQ(l.Item(0).map.t[1], 20.0);
  const GSHSCENE_List * picture = gshscene_resource_picture(l.Item(0).resource);
  ASSERT_NE(picture, nullptr);
  EXPECT_EQ(gshscene_list_count(picture), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerUse, ASymbolIsDrawnAsItsViewport) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<symbol id=\"s\" viewBox=\"0 0 10 10\"><path d=\"M0 0H10V10H0Z\"/></symbol><use href=\"#s\" width=\"50\" height=\"50\"/>"),
      l, 100, 100);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_EQ(l.Items(), (size_t) 1);
  const GSHSCENE_List * picture = gshscene_resource_picture(l.Item(0).resource);
  ASSERT_NE(picture, nullptr);
  // A clip push, the fill under the viewBox map, and the pop.
  ASSERT_EQ(gshscene_list_count(picture), (size_t) 3);
  const GSHSCENE_Item * fill = nullptr;
  ASSERT_EQ(gshscene_list_item(picture, 1, &fill), GSHSCENE_OK);
  EXPECT_EQ(fill->kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(fill->map.m[0][0], 5.0);
}


namespace {
const GSHSCENE_Item & PictureItem(const GSHSCENE_Resource * picture, size_t index) {
  const GSHSCENE_Item * item = nullptr;
  EXPECT_EQ(gshscene_list_item(gshscene_resource_picture(picture), index, &item), GSHSCENE_OK);
  static const GSHSCENE_Item zero{};
  return item ? *item : zero;
}
}  // namespace

TEST(LowerUse, TwoUsesOfOneDrawingShareOnePicture) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<defs><g id=\"g\"><path d=\"M0 0H5V5Z\"/></g></defs><use href=\"#g\" x=\"1\"/><use href=\"#g\" x=\"20\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 2);
  EXPECT_EQ(l.Item(0).resource, l.Item(1).resource);
  EXPECT_EQ(l.Item(0).map.t[0], 1.0);
  EXPECT_EQ(l.Item(1).map.t[0], 20.0);
}

TEST(LowerUse, DifferentInheritanceIsADifferentPicture) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<defs><path id=\"p\" d=\"M0 0H5V5Z\"/></defs><use href=\"#p\" fill=\"red\"/><use href=\"#p\" fill=\"blue\"/>"
               "<use href=\"#p\" fill=\"red\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_NE(l.Item(0).resource, l.Item(1).resource);
  EXPECT_EQ(l.Item(0).resource, l.Item(2).resource);
  EXPECT_EQ(PictureItem(l.Item(0).resource, 0).paint.rgba[0], 1.0);
  EXPECT_EQ(PictureItem(l.Item(1).resource, 0).paint.rgba[2], 1.0);
}

TEST(LowerUse, TheReferentsOwnPaintBeatsTheUsesAndItsTransformIsInThePicture) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<defs><path id=\"p\" d=\"M0 0H5V5Z\" fill=\"#00ff00\" transform=\"scale(3)\"/></defs><use href=\"#p\" fill=\"red\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  const GSHSCENE_Item & fill = PictureItem(l.Item(0).resource, 0);
  EXPECT_EQ(fill.paint.rgba[1], 1.0);
  EXPECT_EQ(fill.map.m[0][0], 3.0);
}

TEST(LowerUse, TheInstanceMapIsTheUsesTransformThenItsTranslation) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<defs><path id=\"p\" d=\"M0 0H5V5Z\"/></defs><use href=\"#p\" transform=\"scale(2)\" x=\"3\" y=\"4\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).map.m[0][0], 2.0);
  EXPECT_EQ(l.Item(0).map.t[0], 6.0);
  EXPECT_EQ(l.Item(0).map.t[1], 8.0);
}

TEST(LowerUse, AUseOfAUseIsAPictureHoldingAnInstance) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<defs><path id=\"p\" d=\"M0 0H5V5Z\"/><use id=\"inner\" href=\"#p\" x=\"2\"/></defs><use href=\"#inner\" x=\"10\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).map.t[0], 10.0);
  const GSHSCENE_Item & inner = PictureItem(l.Item(0).resource, 0);
  EXPECT_EQ(inner.kind, GSHSCENE_ITEM_INSTANCE);
  EXPECT_EQ(inner.map.t[0], 2.0);
  EXPECT_EQ(PictureItem(inner.resource, 0).kind, GSHSCENE_ITEM_FILL);
}

TEST(LowerUse, AnSvgReferentTakesTheWidthAndHeightOfTheUse) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<defs><svg id=\"s\" width=\"10\" height=\"10\" viewBox=\"0 0 1 1\" overflow=\"visible\"><path d=\"M0 0H1V1Z\"/></svg></defs>"
               "<use href=\"#s\" width=\"50\" height=\"30\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  const GSHSCENE_Item & fill = PictureItem(l.Item(0).resource, 0);
  EXPECT_EQ(fill.kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(fill.map.m[0][0], 30.0);  // meet: the smaller of 50 and 30
  EXPECT_EQ(fill.map.m[1][1], 30.0);
  EXPECT_EQ(fill.map.t[0], 10.0);     // centered in 50
}

TEST(LowerUse, ASymbolWithVisibleOverflowIsNotClipped) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<symbol id=\"s\" viewBox=\"0 0 10 10\" style=\"overflow: visible\"><path d=\"M0 0H10V10Z\"/></symbol><use href=\"#s\" width=\"50\" height=\"50\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(gshscene_list_count(gshscene_resource_picture(l.Item(0).resource)), (size_t) 1);
}

TEST(LowerUse, ASymbolsReferencePointIsPlacedAtItsPositionAndAMissingOneMovesNothing) {
  struct Case {
    const char * attributes;
    double tx, ty;  // the translation of the content under the viewBox's scale of 2
  } const cases[] = {
      {"", 0, 0},                          // none given: the corner of the viewport is at x, y
      {"refX=\"5\" refY=\"5\"", -10, -10},   // the point (5, 5) of the content is at x, y: 10 units of viewport
      {"refX=\"5\"", -10, 0},                // only one axis moves
      {"refX=\"right\" refY=\"center\"", -20, -10},
      {"refX=\"0\" refY=\"0\"", 0, 0},
  };
  for (const Case & c : cases) {
    Lowered l;
    LowerSvg(Svg("width=\"100\" height=\"100\"",
                 std::string("<symbol id=\"s\" viewBox=\"0 0 10 10\" style=\"overflow: visible\" ") + c.attributes +
                     "><path d=\"M0 0H10V10Z\"/></symbol><use href=\"#s\" width=\"20\" height=\"20\"/>"),
        l);
    ASSERT_EQ(l.Items(), (size_t) 1) << c.attributes;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << c.attributes;
    const GSHSCENE_Item & fill = PictureItem(l.Item(0).resource, 0);
    EXPECT_EQ(fill.map.m[0][0], 2.0) << c.attributes;
    EXPECT_EQ(fill.map.t[0], c.tx) << c.attributes;
    EXPECT_EQ(fill.map.t[1], c.ty) << c.attributes;
  }
}

TEST(LowerUse, AMissingReferencePointIsNotTheContentsOriginWhenTheViewBoxIsOffset) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<symbol id=\"s\" viewBox=\"5 5 10 10\" refX=\"5\" style=\"overflow: visible\"><path d=\"M5 5H15V15Z\"/></symbol>"
               "<use href=\"#s\" width=\"20\" height=\"20\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  const GSHSCENE_Item & fill = PictureItem(l.Item(0).resource, 0);
  EXPECT_EQ(fill.map.t[0], -10.0);  // refX 5 is the box's own left side: the viewport's corner stays at x
  EXPECT_EQ(fill.map.t[1], -10.0);  // refY is not given: nothing moves, the box's origin is still mapped to the corner
}

TEST(LowerUse, ASymbolOutsideAUseIsNeverDrawn) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<symbol id=\"s\"><path d=\"M0 0H10V10Z\"/></symbol><g><symbol><path d=\"M0 0H1V1Z\"/></symbol></g>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerUse, OpacityOnAUseIsAGroupAroundTheInstance) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<defs><path id=\"p\" d=\"M0 0H5V5Z\"/></defs><use href=\"#p\" opacity=\".5\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_INSTANCE);
  EXPECT_EQ(l.Item(2).kind, GSHSCENE_ITEM_GROUP_POP);
}

TEST(LowerUse, AUseOfNothingDrawnIsNoInstanceAndAMissingReferentIsNoRefusal) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<defs><g id=\"empty\"><path d=\"M0 0\"/></g><path id=\"hidden\" d=\"M0 0H5V5Z\" display=\"none\"/></defs>"
               "<use href=\"#empty\"/><use href=\"#hidden\"/><use href=\"#nothing\"/><use/><use href=\"#empty\" display=\"none\"/>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerUse, AChainOfUsesWithinThePictureDepthLowersWithoutDeepRecursion) {
  std::string body = "<defs><path id=\"u0\" d=\"M0 0H5V5Z\"/>";
  for (int i = 1; i <= 20; i++) {
    body += "<use id=\"u" + std::to_string(i) + "\" href=\"#u" + std::to_string(i - 1) + "\"/>";
  }
  body += "</defs><use href=\"#u20\"/>";
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", body), l);
  EXPECT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerUse, AChainPastThePictureDepthIsRefusedAsALimitInBothFormsAndStillPublishes) {
  std::string body = "<defs><path id=\"u0\" d=\"M0 0H5V5Z\"/>";
  for (int i = 1; i <= 60; i++) {
    body += "<use id=\"u" + std::to_string(i) + "\" href=\"#u" + std::to_string(i - 1) + "\"/>";
  }
  body += "</defs><use href=\"#u60\"/>";
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", body), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerUse, ACycleAndAPyramidAreReportedNotLooped) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<defs><g id=\"a\"><use id=\"ua\" href=\"#b\"/></g><g id=\"b\"><use id=\"ub\" href=\"#a\"/></g></defs><use href=\"#a\"/>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_GE(l.Refusals(), (size_t) 1);
  EXPECT_NE(l.Refusal(GVEC_LOWER_REFERENCE_CYCLE), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
