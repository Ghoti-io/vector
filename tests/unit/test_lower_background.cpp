/**
 * @file
 *
 * The background of the root element: its `background-color` is the color of the whole viewport, drawn under everything else.
 */

#include <ghoti.io/shape-scene/resource.h>
#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

std::string Doc(const std::string & root_attributes, const std::string & body = "<rect width=\"10\" height=\"10\" fill=\"red\"/>") {
  return Svg("width=\"100\" height=\"50\" " + root_attributes, body);
}

}  // namespace

TEST(LowerBackground, TheRootsBackgroundColorFillsTheViewportUnderTheDrawing) {
  Lowered plain, painted;
  LowerSvg(Doc(""), plain);
  LowerSvg(Doc("style=\"background-color: #00ff00\""), painted);
  ASSERT_EQ(painted.Items(), plain.Items() + 1);
  const GSHSCENE_Item & first = painted.Item(0);
  EXPECT_EQ(first.kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(first.paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_NEAR(first.paint.rgba[1], 1.0, 1e-6);
  EXPECT_NEAR(first.paint.rgba[0], 0.0, 1e-6);
  GSHGEOM_Bounds box;
  box.empty = 1;
  const GSHGEOM_Path * path = gshscene_resource_path(first.resource);
  ASSERT_NE(path, nullptr);
  ASSERT_EQ(gshgeom_path_bounds(path, &box), GSHGEOM_OK);
  EXPECT_EQ(box.min.x, 0.0);
  EXPECT_EQ(box.min.y, 0.0);
  EXPECT_EQ(box.max.x, 100.0) << "the whole width of the viewport";
  EXPECT_EQ(box.max.y, 50.0) << "the whole height of the viewport";
  // it is the whole viewport, drawn first: the drawing's own fill is the second item
  EXPECT_EQ(painted.Item(1).kind, GSHSCENE_ITEM_FILL);
  EXPECT_NEAR(painted.Item(1).paint.rgba[0], 1.0, 1e-6);
}

TEST(LowerBackground, TheShorthandSetsItAndTheDrawingsViewBoxDoesNotMoveIt) {
  Lowered l;
  LowerSvg(Doc("viewBox=\"0 0 10 5\" style=\"background: rgba(0, 0, 255, 0.5) url(x.png) no-repeat\""), l);
  ASSERT_GE(l.Items(), (size_t) 2);
  EXPECT_EQ(l.Item(0).paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_NEAR(l.Item(0).paint.rgba[2], 1.0, 1e-6);
  EXPECT_NEAR(l.Item(0).paint.rgba[3], 0.5, 1e-6);
  EXPECT_EQ(l.Item(0).map.m[0][0], 1.0) << "the viewport's own space, not the viewBox's";
}

TEST(LowerBackground, NoBackgroundATransparentOneAndANestedOneDrawNothingExtra) {
  Lowered plain, transparent, nested, none;
  LowerSvg(Doc(""), plain);
  LowerSvg(Doc("style=\"background-color: transparent\""), transparent);
  LowerSvg(Doc("", "<svg width=\"5\" height=\"5\" style=\"background-color: red\"><rect width=\"1\" height=\"1\"/></svg>"), nested);
  LowerSvg(Doc("", "<g style=\"background-color: red\"><rect width=\"1\" height=\"1\"/></g>"), none);
  Lowered base_nested, base_none;
  LowerSvg(Doc("", "<svg width=\"5\" height=\"5\"><rect width=\"1\" height=\"1\"/></svg>"), base_nested);
  LowerSvg(Doc("", "<g><rect width=\"1\" height=\"1\"/></g>"), base_none);
  EXPECT_EQ(transparent.Items(), plain.Items());
  EXPECT_EQ(nested.Items(), base_nested.Items());
  EXPECT_EQ(none.Items(), base_none.Items());
}

TEST(LowerBackground, CurrentColorIsTheForeground) {
  Lowered l;
  LowerSvg(Doc("style=\"background-color: currentcolor\""), l);
  ASSERT_GE(l.Items(), (size_t) 2);
  EXPECT_EQ(l.Item(0).paint.kind, GSHSCENE_PAINT_FOREGROUND);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
