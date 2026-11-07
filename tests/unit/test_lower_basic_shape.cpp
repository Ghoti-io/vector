/**
 * @file
 *
 * `clip-path` written as a basic shape (H11): each of `circle()`, `ellipse()`, `inset()`, `polygon()` and `path()` is one clip push
 * whose region is the shape resolved against the reference box, in the element's own space; what is not a shape is as if it were not
 * specified, and what is a shape this does not read is refused.
 */

#include "../lower_helpers.h"

#include <cmath>
#include <vector>

using namespace gvec_test;

namespace {

const GSHSCENE_Item_Kind PUSH = GSHSCENE_ITEM_CLIP_PUSH, POP = GSHSCENE_ITEM_CLIP_POP, FILL = GSHSCENE_ITEM_FILL;

std::vector<GSHSCENE_Item_Kind> Kinds(const Lowered & l) {
  std::vector<GSHSCENE_Item_Kind> kinds;
  for (size_t i = 0; i < l.Items(); i++) {
    kinds.push_back(l.Item(i).kind);
  }
  return kinds;
}

std::string g_last;

/** `clip-path` as the style attribute of a rect at (20, 20) of 160 by 160, in a viewBox of 200 by 200. */
void Lower(const std::string & clip, Lowered & l, const std::string & extra = "", const std::string & element = "rect") {
  g_last = clip;
  std::string geometry = element == "rect" ? "x=\"20\" y=\"20\" width=\"160\" height=\"160\"" : "";
  LowerSvg(Svg("viewBox=\"0 0 200 200\" width=\"200\" height=\"200\"", "<" + element + " " + geometry + " " + extra + " style=\"clip-path: " + clip + "\"/>"), l);
}

/** The bounds of the pushed region, placed by the push's map. */
bool Bounds(const Lowered & l, double b[4]) {
  for (size_t i = 0; i < l.Items(); i++) {
    const GSHSCENE_Item & item = l.Item(i);
    if (item.kind != GSHSCENE_ITEM_CLIP_PUSH) {
      continue;
    }
    const GSHGEOM_Path * path = gshscene_resource_path(item.resource);
    GSHGEOM_Bounds box;
    if (!path || gshgeom_path_bounds(path, &box) != GSHGEOM_OK || box.empty) {
      return false;
    }
    b[0] = box.min.x + item.map.t[0];
    b[1] = box.min.y + item.map.t[1];
    b[2] = box.max.x + item.map.t[0];
    b[3] = box.max.y + item.map.t[1];
    return true;
  }
  return false;
}

void ExpectBounds(const Lowered & l, double x0, double y0, double x1, double y1) {
  double b[4];
  ASSERT_TRUE(Bounds(l, b)) << g_last << " items " << l.Items() << " refusals " << l.Refusals();
  EXPECT_NEAR(b[0], x0, 1e-9);
  EXPECT_NEAR(b[1], y0, 1e-9);
  EXPECT_NEAR(b[2], x1, 1e-9);
  EXPECT_NEAR(b[3], y1, 1e-9);
}

}  // namespace

TEST(LowerBasicShape, ACircleWithNoRadiusIsTheClosestSideOfTheFillBox) {
  Lowered l;
  Lower("circle() fill-box", l);
  EXPECT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ExpectBounds(l, 20, 20, 180, 180);
}

TEST(LowerBasicShape, TheDefaultBoxIsTheStrokeBoxAndTheStrokeWidensIt) {
  Lowered none, stroked;
  Lower("circle()", none);
  ExpectBounds(none, 20, 20, 180, 180);
  Lower("circle()", stroked, "stroke=\"blue\" stroke-width=\"20\"");
  ExpectBounds(stroked, 10, 10, 190, 190);
  Lowered fill;
  Lower("circle() fill-box", fill, "stroke=\"blue\" stroke-width=\"20\"");
  ExpectBounds(fill, 20, 20, 180, 180);
}

TEST(LowerBasicShape, TheViewBoxIsTheViewport) {
  Lowered l;
  Lower("circle() view-box", l);
  ExpectBounds(l, 0, 0, 200, 200);
}

TEST(LowerBasicShape, ACircleTakesALengthOrAPercentageOfTheNormalizedDiagonalAndAPosition) {
  Lowered l;
  Lower("circle(30px at 10px 20px) fill-box", l);
  ExpectBounds(l, 20 - 20, 20 + 20 - 30, 20 + 40, 20 + 20 + 30);  // the center is at the box's corner plus (10, 20)
  Lowered percent;
  Lower("circle(50% at 25% 75%) fill-box", percent);
  double r = 0.5 * 160;  // the diagonal of a square of 160 normalized by sqrt 2 is 160
  ExpectBounds(percent, 20 + 40 - r, 20 + 120 - r, 20 + 40 + r, 20 + 120 + r);
  Lowered farthest;
  Lower("circle(farthest-side at left top) fill-box", farthest);
  ExpectBounds(farthest, 20 - 160, 20 - 160, 20 + 160, 20 + 160);
  Lowered keyword;
  Lower("circle(closest-side at right 10px bottom 20px) fill-box", keyword);
  ExpectBounds(keyword, 170 - 10, 160 - 10, 170 + 10, 160 + 10);
}

TEST(LowerBasicShape, AnEllipseTakesTwoRadiiOrTheSidesOfItsBox) {
  Lowered l;
  Lower("ellipse(40px 20px) fill-box", l);
  ExpectBounds(l, 100 - 40, 100 - 20, 100 + 40, 100 + 20);
  Lowered sides;
  Lower("ellipse(closest-side farthest-side at 25% 50%) fill-box", sides);
  ExpectBounds(sides, 60 - 40, 100 - 80, 60 + 40, 100 + 80);
  Lowered half;  // one radius is not a form of ellipse()
  Lower("ellipse(40px) fill-box", half);
  EXPECT_EQ(Kinds(half), (std::vector<GSHSCENE_Item_Kind>{FILL}));
}

TEST(LowerBasicShape, AnInsetIsTheBoxMovedInByEachSideWithRoundedCorners) {
  Lowered l;
  Lower("inset(10px 20px 30px 40px) fill-box", l);
  ExpectBounds(l, 20 + 40, 20 + 10, 180 - 20, 180 - 30);
  Lowered one;
  Lower("inset(10%) fill-box", one);
  ExpectBounds(one, 20 + 16, 20 + 16, 180 - 16, 180 - 16);
  Lowered round;
  Lower("inset(0 round 10px) fill-box", round);
  ExpectBounds(round, 20, 20, 180, 180);
  const GSHSCENE_Item & push = round.Item(0);
  GSHSCENE_Primitive p;
  ASSERT_EQ(gshscene_resource_primitive(push.resource, &p), GSHSCENE_OK);
  EXPECT_EQ(p.kind, GSHSCENE_PRIMITIVE_ROUNDED_RECT);
  EXPECT_EQ(p.r[0].rx, 10.0);
  EXPECT_EQ(p.r[2].ry, 10.0);
}

TEST(LowerBasicShape, CornersThatWouldOverlapAreScaledByOneFactor) {
  Lowered l;
  Lower("inset(0 round 160px 0 0 0) fill-box", l);  // a lone radius is as wide as the side: nothing to scale
  GSHSCENE_Primitive p;
  ASSERT_EQ(gshscene_resource_primitive(l.Item(0).resource, &p), GSHSCENE_OK);
  EXPECT_EQ(p.r[0].rx, 160.0);
  Lowered scaled;
  Lower("inset(0 round 160px) fill-box", scaled);  // two radii of 160 on a side of 160: each is halved
  ASSERT_EQ(gshscene_resource_primitive(scaled.Item(0).resource, &p), GSHSCENE_OK);
  EXPECT_EQ(p.r[0].rx, 80.0);
  EXPECT_EQ(p.r[0].ry, 80.0);
  Lowered one_side;  // only the top overlaps (200 on 160), so only it sets the factor
  Lower("inset(0 round 100px 100px 0 0) fill-box", one_side);
  ASSERT_EQ(gshscene_resource_primitive(one_side.Item(0).resource, &p), GSHSCENE_OK);
  EXPECT_EQ(p.r[0].rx, 80.0);
  EXPECT_EQ(p.r[1].rx, 80.0);
  Lowered elliptical;  // `/` gives the vertical radii
  Lower("inset(0 round 10px / 20px) fill-box", elliptical);
  ASSERT_EQ(gshscene_resource_primitive(elliptical.Item(0).resource, &p), GSHSCENE_OK);
  EXPECT_EQ(p.r[1].rx, 10.0);
  EXPECT_EQ(p.r[1].ry, 20.0);
}

TEST(LowerBasicShape, APolygonIsPlacedInTheBoxAndKeepsItsFillRule) {
  Lowered l;
  Lower("polygon(evenodd, 0 0, 100% 0, 50% 100%) fill-box", l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  ExpectBounds(l, 20, 20, 180, 180);
  EXPECT_EQ(l.Item(0).fill_rule, GSHGEOM_FILL_EVEN_ODD);
  Lowered plain;
  Lower("polygon(10px 10px, 50px 10px, 10px 50px) fill-box", plain);
  ExpectBounds(plain, 30, 30, 70, 70);
  EXPECT_EQ(plain.Item(0).fill_rule, GSHGEOM_FILL_NONZERO);
}

TEST(LowerBasicShape, APathIsRelativeToTheBoxOrigin) {
  Lowered l;
  Lower("path('M 0 0 H 50 V 40 Z') fill-box", l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  ExpectBounds(l, 20, 20, 70, 60);
  Lowered rule;
  Lower("path(evenodd, 'M 0 0 H 50 V 40 Z') fill-box", rule);
  EXPECT_EQ(rule.Item(0).fill_rule, GSHGEOM_FILL_EVEN_ODD);
}

TEST(LowerBasicShape, TheShapeIsInTheElementsSpaceSoItsTransformMovesIt) {
  Lowered l;
  Lower("circle(10px at 0 0) fill-box", l, "transform=\"translate(5 7)\"");
  ExpectBounds(l, 25 - 10, 27 - 10, 25 + 10, 27 + 10);
}

TEST(LowerBasicShape, AShapeWithNoAreaLetsNothingThrough) {
  Lowered circle;
  Lower("circle(0px) fill-box", circle);
  EXPECT_EQ(Kinds(circle), (std::vector<GSHSCENE_Item_Kind>{}));
  Lowered inset;
  Lower("inset(100px) fill-box", inset);  // the sides meet past the middle
  EXPECT_EQ(Kinds(inset), (std::vector<GSHSCENE_Item_Kind>{}));
  EXPECT_EQ(circle.Refusals(), (size_t) 0);
}

TEST(LowerBasicShape, WhatIsNotAShapeIsAsIfNotSpecifiedAndAGroupHasNoBoxToNameAndALineWithAStrokeIsRefused) {
  Lowered junk;
  Lower("circle(banana)", junk);
  EXPECT_EQ(Kinds(junk), (std::vector<GSHSCENE_Item_Kind>{FILL}));
  EXPECT_EQ(junk.Refusals(), (size_t) 0);
  Lowered group;  // a group has no box of its own
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<g style=\"clip-path: circle(10px)\"><rect width=\"5\" height=\"5\"/></g>"), group);
  EXPECT_NE(group.Refusal(GVEC_LOWER_CLIP_PATH), nullptr);
  Lowered line;  // a diagonal line's stroke reaches by its caps and its slope, which the half width does not
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<line x2=\"50\" y2=\"30\" stroke=\"red\" stroke-width=\"6\" style=\"clip-path: circle()\"/>"), line);
  EXPECT_NE(line.Refusal(GVEC_LOWER_CLIP_PATH), nullptr);
}

TEST(LowerBasicShape, TheViewBoxIsRefusedWhenTheViewportDoesNotStartAtTheOrigin) {
  Lowered l;
  LowerSvg(Svg("viewBox=\"10 10 200 200\" width=\"200\" height=\"200\"", "<rect width=\"5\" height=\"5\" style=\"clip-path: circle() view-box\"/>"), l);
  EXPECT_NE(l.Refusal(GVEC_LOWER_CLIP_PATH), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
