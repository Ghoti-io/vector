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

TEST(LowerBasicShape, RectIsTheEdgesFromTheBoxsTopAndLeftAndAutoIsTheBoxsOwnEdge) {
  Lowered l;
  Lower("rect(10px 150px 120px 30px) fill-box", l);
  ExpectBounds(l, 20 + 30, 20 + 10, 20 + 150, 20 + 120);
  Lowered autos;
  Lower("rect(auto 100px auto auto) fill-box", autos);
  ExpectBounds(autos, 20, 20, 20 + 100, 180);
  Lowered percent;
  Lower("rect(25% 75% 75% 25%) fill-box", percent);
  ExpectBounds(percent, 20 + 40, 20 + 40, 20 + 120, 20 + 120);
  Lowered reversed;  // the right edge before the left: no area, nothing gets through
  Lower("rect(10px 5px 20px 30px) fill-box", reversed);
  EXPECT_EQ(Kinds(reversed), (std::vector<GSHSCENE_Item_Kind>{}));
  EXPECT_EQ(reversed.Refusals(), (size_t) 0);
  Lowered round;
  Lower("rect(0 100% 100% 0 round 8px) fill-box", round);
  GSHSCENE_Primitive p;
  ASSERT_EQ(gshscene_resource_primitive(round.Item(0).resource, &p), GSHSCENE_OK);
  EXPECT_EQ(p.kind, GSHSCENE_PRIMITIVE_ROUNDED_RECT);
  EXPECT_EQ(p.r[1].rx, 8.0);
}

TEST(LowerBasicShape, XywhIsTheCornerAndTheSizeAndANegativeSizeIsAsIfNotSpecified) {
  Lowered l;
  Lower("xywh(10px 20px 50% 25%) fill-box", l);
  ExpectBounds(l, 20 + 10, 20 + 20, 20 + 10 + 80, 20 + 20 + 40);
  Lowered negative;
  Lower("xywh(0 0 -5px 5px) fill-box", negative);
  EXPECT_EQ(Kinds(negative), (std::vector<GSHSCENE_Item_Kind>{FILL}));
  Lowered round;
  Lower("xywh(0 0 100% 100% round 10px) fill-box", round);
  ExpectBounds(round, 20, 20, 180, 180);
}

TEST(LowerBasicShape, ShapeIsTheCommandsFromTheBoxsCornerWhoseByIsRelativeAndWhoseCurveHasControlPoints) {
  Lowered lines;
  Lower("shape(from 0px 0px, line to 100px 0px, vline to 50px, close) fill-box", lines);
  EXPECT_EQ(lines.Refusals(), (size_t) 0);
  ExpectBounds(lines, 20, 20, 120, 70);
  Lowered relative;
  Lower("shape(from 10px 10px, hline by 50px, vline by 40px, close) fill-box", relative);
  ExpectBounds(relative, 30, 30, 80, 70);
  Lowered keyword;
  Lower("shape(from 0% 0%, hline to right, vline to bottom, hline to left, close) fill-box", keyword);
  ExpectBounds(keyword, 20, 20, 180, 180);
  Lowered curve;
  Lower("shape(from 0% 0%, curve to 100% 100% with 100% 0%, close) fill-box", curve);
  EXPECT_EQ(curve.Refusals(), (size_t) 0);
  ExpectBounds(curve, 20, 20, 180, 180);
  Lowered cubic;
  Lower("shape(from 0px 0px, curve to 100px 0px with 0px 50px / 100px 50px, close) fill-box", cubic);
  EXPECT_EQ(cubic.Refusals(), (size_t) 0);
  Lowered arc;
  Lower("shape(evenodd from 10px 10px, hline to 100px, arc to 10px 90px of 40px cw large rotate 10deg, close) fill-box", arc);
  EXPECT_EQ(arc.Refusals(), (size_t) 0);
  EXPECT_EQ(arc.Item(0).fill_rule, GSHGEOM_FILL_EVEN_ODD);
  Lowered bad;  // a command that is no command: as if the property were not specified
  Lower("shape(from 0px 0px, jump to 5px 5px) fill-box", bad);
  EXPECT_EQ(Kinds(bad), (std::vector<GSHSCENE_Item_Kind>{FILL}));
}

TEST(LowerBasicShape, AGroupAUseAndAnInnerSvgAreMeasuredByWhatTheyDraw) {
  Lowered group;
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<g style=\"clip-path: inset(0) fill-box\"><rect x=\"10\" y=\"10\" width=\"50\" height=\"40\"/></g>"), group);
  EXPECT_EQ(group.Refusals(), (size_t) 0);
  ExpectBounds(group, 10, 10, 60, 50);
  Lowered stroked;  // the stroke box takes in the strokes of what the group draws
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               "<g style=\"clip-path: inset(0) stroke-box\"><rect x=\"10\" y=\"10\" width=\"50\" height=\"40\" stroke=\"red\" stroke-width=\"4\"/></g>"),
      stroked);
  ExpectBounds(stroked, 8, 8, 62, 52);
  Lowered use;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               "<defs><rect id=\"r\" width=\"30\" height=\"20\"/></defs><use href=\"#r\" x=\"5\" y=\"7\" style=\"clip-path: inset(0) fill-box\"/>"),
      use);
  EXPECT_EQ(use.Refusals(), (size_t) 0);
  ExpectBounds(use, 5, 7, 35, 27);  // the box of a use takes in its x and y
  Lowered foreign;  // what holds a foreignObject has a box that is not measured, and is refused whole
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<g style=\"clip-path: circle(50%)\"><foreignObject width=\"10\" height=\"10\"/></g>"), foreign);
  EXPECT_NE(foreign.Refusal(GVEC_LOWER_CLIP_PATH), nullptr);
}

TEST(LowerBasicShape, RectAndXywhTakeTheirPercentagesOfTheWidthAndOfTheHeight) {
  // a box of 100 by 40, which is not square, so that a percentage taken of the wrong side is seen
  Lowered r;
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<rect width=\"100\" height=\"40\" style=\"clip-path: rect(25% 75% 75% 25%) fill-box\"/>"), r);
  ExpectBounds(r, 25, 10, 75, 30);
  Lowered autos;
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<rect width=\"100\" height=\"40\" style=\"clip-path: rect(10px auto auto 20px) fill-box\"/>"), autos);
  ExpectBounds(autos, 20, 10, 100, 40);
  Lowered x;
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<rect width=\"100\" height=\"40\" style=\"clip-path: xywh(10% 10% 50% 50%) fill-box\"/>"), x);
  ExpectBounds(x, 10, 4, 60, 24);
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

TEST(LowerBasicShape, WhatIsNotAShapeIsAsIfNotSpecifiedAGroupIsMeasuredByWhatItDrawsAndALineWithAStrokeIsRefused) {
  Lowered junk;
  Lower("circle(banana)", junk);
  EXPECT_EQ(Kinds(junk), (std::vector<GSHSCENE_Item_Kind>{FILL}));
  EXPECT_EQ(junk.Refusals(), (size_t) 0);
  Lowered group;  // a group is measured by the box of what it draws: the circle of 50% is the one of the box of the rect
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<g style=\"clip-path: circle(50%)\"><rect width=\"6\" height=\"6\"/></g>"), group);
  EXPECT_EQ(group.Refusals(), (size_t) 0);
  EXPECT_GT(group.Count(GSHSCENE_ITEM_FILL), (size_t) 0);
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
