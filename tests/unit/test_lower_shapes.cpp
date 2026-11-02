/**
 * @file
 *
 * `rect`, `circle` and `ellipse` (X2, X3): each reaches the waist as the shape family's named primitive with the parameters SVG 2's
 * rules resolve, asserted through the tag; a zero or negative size draws nothing.
 */

#include "../lower_helpers.h"

#include <ghoti.io/shape-scene/primitive.h>

using namespace gvec_test;

namespace {

/** The tag of the only fill of `<svg width=100 height=100>body</svg>`; `NONE` when it draws nothing or draws an untagged path. */
GSHSCENE_Primitive Tag(const std::string & body, size_t * items = nullptr, const std::string & svg_attributes = "width=\"100\" height=\"100\"") {
  Lowered l;
  LowerSvg(Svg(svg_attributes, body), l);
  if (items) {
    *items = l.Items();
  }
  GSHSCENE_Primitive p;
  std::memset(&p, 0, sizeof p);
  if (l.Items() == 1 && l.Item(0).resource) {
    EXPECT_EQ(gshscene_resource_primitive(l.Item(0).resource, &p), GSHSCENE_OK);
  }
  return p;
}

void ExpectCorners(const GSHSCENE_Primitive & p, double rx, double ry) {
  ASSERT_EQ(p.kind, GSHSCENE_PRIMITIVE_ROUNDED_RECT);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(p.r[i].rx, rx) << i;
    EXPECT_EQ(p.r[i].ry, ry) << i;
  }
}

}  // namespace

TEST(LowerShapes, APlainRectIsTheRectPrimitive) {
  GSHSCENE_Primitive p = Tag("<rect x=\"1.5\" y=\"2\" width=\"20\" height=\"10\"/>");
  ASSERT_EQ(p.kind, GSHSCENE_PRIMITIVE_RECT);
  EXPECT_EQ(p.x, 1.5);
  EXPECT_EQ(p.y, 2.0);
  EXPECT_EQ(p.w, 20.0);
  EXPECT_EQ(p.h, 10.0);
  EXPECT_EQ(p.flip_x, 0);
  EXPECT_EQ(p.flip_y, 0);
}

TEST(LowerShapes, APositionMayBeNegativeAndASizeMayNot) {
  GSHSCENE_Primitive p = Tag("<rect x=\"-40\" y=\"-2.5\" width=\"20\" height=\"10\"/>");
  ASSERT_EQ(p.kind, GSHSCENE_PRIMITIVE_RECT);
  EXPECT_EQ(p.x, -40.0);
  EXPECT_EQ(p.y, -2.5);
  size_t items = 0;
  Tag("<circle cx=\"-3\" cy=\"-4\" r=\"5\"/>", &items);
  EXPECT_EQ(items, (size_t) 1);
  items = 9;
  Tag("<rect width=\"-20\" height=\"10\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
}

TEST(LowerShapes, RoundedCornersFollowTheAutoRule) {
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"3\"/>"), 3, 3);       // ry takes rx
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" ry=\"4\"/>"), 4, 4);       // rx takes ry
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"3\" ry=\"2\"/>"), 3, 2);
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"auto\" ry=\"2\"/>"), 2, 2);
  EXPECT_EQ(Tag("<rect width=\"20\" height=\"10\" rx=\"auto\" ry=\"auto\"/>").kind, GSHSCENE_PRIMITIVE_RECT);
}

TEST(LowerShapes, EachRadiusIsClampedToHalfItsSide) {
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"15\"/>"), 10, 5);     // ry takes 15 and is clamped to 5
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"30\" ry=\"2\"/>"), 10, 2);
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"4\" ry=\"99\"/>"), 4, 5);
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"4\" ry=\"7\"/>"), 4, 5);   // above half, below the whole side
}

TEST(LowerShapes, ANegativeRadiusIsAnErrorThatFallsBackToAuto) {
  EXPECT_EQ(Tag("<rect width=\"20\" height=\"10\" rx=\"-3\"/>").kind, GSHSCENE_PRIMITIVE_RECT);
  ExpectCorners(Tag("<rect width=\"20\" height=\"10\" rx=\"-3\" ry=\"2\"/>"), 2, 2);
}

TEST(LowerShapes, AZeroRadiusIsAPlainRectangle) {
  EXPECT_EQ(Tag("<rect width=\"20\" height=\"10\" rx=\"0\" ry=\"4\"/>").kind, GSHSCENE_PRIMITIVE_RECT);
  EXPECT_EQ(Tag("<rect width=\"20\" height=\"10\" rx=\"4\" ry=\"0\"/>").kind, GSHSCENE_PRIMITIVE_RECT);
}

TEST(LowerShapes, ARectOfNoSizeIsNotRendered) {
  size_t items = 9;
  Tag("<rect width=\"0\" height=\"10\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
  Tag("<rect width=\"10\" height=\"0\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
  Tag("<rect width=\"-5\" height=\"10\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
  Tag("<rect height=\"10\"/>", &items);  // width auto
  EXPECT_EQ(items, (size_t) 0);
}

TEST(LowerShapes, PercentagesAreOfTheViewport) {
  GSHSCENE_Primitive p = Tag("<rect x=\"10%\" y=\"20%\" width=\"50%\" height=\"25%\" rx=\"10%\"/>");
  ASSERT_EQ(p.kind, GSHSCENE_PRIMITIVE_ROUNDED_RECT);
  EXPECT_EQ(p.x, 10.0);
  EXPECT_EQ(p.y, 20.0);
  EXPECT_EQ(p.w, 50.0);
  EXPECT_EQ(p.h, 25.0);
  EXPECT_EQ(p.r[0].rx, 10.0);  // 10% of the width
  EXPECT_EQ(p.r[0].ry, 10.0);  // ry takes rx
}

TEST(LowerShapes, ACircleIsAnEllipseOfEqualRadii) {
  GSHSCENE_Primitive p = Tag("<circle cx=\"30\" cy=\"40\" r=\"7\"/>");
  ASSERT_EQ(p.kind, GSHSCENE_PRIMITIVE_ELLIPSE);
  EXPECT_EQ(p.cx, 30.0);
  EXPECT_EQ(p.cy, 40.0);
  EXPECT_EQ(p.rx, 7.0);
  EXPECT_EQ(p.ry, 7.0);
  size_t items = 9;
  Tag("<circle r=\"0\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
  Tag("<circle r=\"-1\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
  Tag("<circle cx=\"5\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
}

TEST(LowerShapes, ACircleRadiusInPercentIsOfTheDiagonal) {
  GSHSCENE_Primitive p = Tag("<circle cx=\"50\" cy=\"50\" r=\"10%\"/>");
  ASSERT_EQ(p.kind, GSHSCENE_PRIMITIVE_ELLIPSE);
  EXPECT_EQ(p.rx, 10.0);  // 10% of the normalized diagonal of a 100 by 100 viewport
}

TEST(LowerShapes, AnEllipseTakesTheOtherRadiusWhenOneIsAuto) {
  GSHSCENE_Primitive p = Tag("<ellipse cx=\"5\" cy=\"6\" rx=\"4\" ry=\"2\"/>");
  ASSERT_EQ(p.kind, GSHSCENE_PRIMITIVE_ELLIPSE);
  EXPECT_EQ(p.rx, 4.0);
  EXPECT_EQ(p.ry, 2.0);
  p = Tag("<ellipse rx=\"4\"/>");
  EXPECT_EQ(p.ry, 4.0);
  p = Tag("<ellipse ry=\"3\"/>");
  EXPECT_EQ(p.rx, 3.0);
  size_t items = 9;
  Tag("<ellipse/>", &items);
  EXPECT_EQ(items, (size_t) 0);
  Tag("<ellipse rx=\"0\" ry=\"3\"/>", &items);
  EXPECT_EQ(items, (size_t) 0);
}

TEST(LowerShapes, AStrokedShapeIsAStrokeOfTheTaggedPath) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<circle cx=\"30\" cy=\"30\" r=\"10\" fill=\"none\" stroke=\"red\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_STROKE);
  GSHSCENE_Primitive p;
  ASSERT_EQ(gshscene_resource_primitive(l.Item(0).resource, &p), GSHSCENE_OK);
  EXPECT_EQ(p.kind, GSHSCENE_PRIMITIVE_ELLIPSE);
}

TEST(LowerShapes, TheTagIsContentAndEqualShapesShareOneResource) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<circle cx=\"30\" cy=\"30\" r=\"10\"/><ellipse cx=\"30\" cy=\"30\" rx=\"10\" ry=\"10\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 2);
  EXPECT_EQ(l.Item(0).resource, l.Item(1).resource);
}

TEST(LowerShapes, AnEllipseStartsAtThreeOClockAndTurnsThePositiveWay) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<ellipse cx=\"30\" cy=\"30\" rx=\"10\" ry=\"6\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource).substr(0, 7), "M40,30 ");
  EXPECT_NE(Describe(l.Item(0).resource).find("A30,36"), std::string::npos);
}

TEST(LowerShapes, ARoundedRectStartsAfterItsTopLeftRadius) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<rect x=\"10\" y=\"10\" width=\"40\" height=\"20\" rx=\"4\" ry=\"3\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource).substr(0, 7), "M14,10 ");
}

TEST(LowerShapes, ARectOnTheEdgeOfTheRangeIsALimit) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<rect width=\"1e300\" height=\"1e300\"/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
