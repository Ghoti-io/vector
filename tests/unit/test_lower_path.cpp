/**
 * @file
 *
 * The geometry of `path`, `polygon`, `polyline` and `line` as a model path (P7): every command of path data, the fill
 * rule, degenerate paths, and the arcs that are refused.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** The path of the only fill of a document with one shape (`attributes` on a `path`), or a note why not. */
std::string PathOf(const std::string & d, const std::string & extra = "") {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<path d=\"" + d + "\" " + extra + "/>"), l);
  if (l.Items() != 1) {
    return "(" + std::to_string(l.Items()) + " items)";
  }
  return Describe(l.Item(0).resource);
}

}  // namespace

TEST(LowerPath, AbsoluteLinesAndTheClose) {
  EXPECT_EQ(PathOf("M10 10 L20 10 L20 20 Z"), "M10,10 L20,10 L20,20 Z");
}

TEST(LowerPath, RelativeCommandsAndTheHorizontalAndVerticalLines) {
  EXPECT_EQ(PathOf("M10 10 h10 v10 H10 z"), "M10,10 L20,10 L20,20 L10,20 Z");
  EXPECT_EQ(PathOf("m10 10 l5 0 l0 5 l-5 0 z"), "M10,10 L15,10 L15,15 L10,15 Z");
}

TEST(LowerPath, ExtraCoordinatePairsAfterAMoveAreLines) {
  EXPECT_EQ(PathOf("M0 0 10 0 10 10"), "M0,0 L10,0 L10,10");
}

TEST(LowerPath, CubicAndSmoothCubicWithTheReflectedControlPointWrittenOut) {
  EXPECT_EQ(PathOf("M0 0 C 1 2 3 4 5 6 S 9 10 11 12"), "M0,0 C1,2 3,4 5,6 C7,8 9,10 11,12");
}

TEST(LowerPath, QuadraticAndSmoothQuadratic) {
  EXPECT_EQ(PathOf("M0 0 Q 2 4 4 0 T 8 0"), "M0,0 Q2,4 4,0 Q6,-4 8,0");
}

TEST(LowerPath, AClosedSubpathIsFollowedByANewOneAtItsStart) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<path d=\"M0 0 L10 0 L10 10 Z L5 5 L0 5\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  const std::string text = Describe(l.Item(0).resource);
  EXPECT_NE(text.find("Z"), std::string::npos);
  EXPECT_NE(text.find("L5,5 L0,5"), std::string::npos) << text;
}

TEST(LowerPath, TheFillRuleIsTheItemsRule) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<path d=\"M0 0H4V4H0Z\"/><path d=\"M0 0H4V4H0Z\" fill-rule=\"evenodd\"/>"
               "<g fill-rule=\"evenodd\"><path d=\"M0 0H4V4H0Z\"/><path d=\"M0 0H4V4H0Z\" style=\"fill-rule: nonzero\"/></g>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 4);
  EXPECT_EQ(l.Item(0).fill_rule, GSHGEOM_FILL_NONZERO);
  EXPECT_EQ(l.Item(1).fill_rule, GSHGEOM_FILL_EVEN_ODD);
  EXPECT_EQ(l.Item(2).fill_rule, GSHGEOM_FILL_EVEN_ODD);
  EXPECT_EQ(l.Item(3).fill_rule, GSHGEOM_FILL_NONZERO);
}

TEST(LowerPath, APolygonAndAPolylineFillAsTheClosedPolygon) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<polygon points=\"0,0 10,0 10,10\"/><polyline points=\"0 0 10 0 10 10\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 2);
  EXPECT_EQ(Describe(l.Item(0).resource), "M0,0 L10,0 L10,10 Z");
  EXPECT_EQ(Describe(l.Item(1).resource), "M0,0 L10,0 L10,10 Z");
}

TEST(LowerPath, ALineHasNoAreaAndDrawsNothing) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<line x1=\"0\" y1=\"0\" x2=\"10\" y2=\"10\"/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerPath, DegenerateShapesDrawNothing) {
  for (const char * d : {"M 10 10", "M 0 0 L 0 0", "M 0 0 Z", "M 3 3 L 3 3 L 3 3 Z", "M 1 1 Q 1 1 1 1", "M 1 1 C 1 1 1 1 1 1"}) {
    EXPECT_EQ(PathOf(d), "(0 items)") << d;
  }
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<polygon points=\"5 5\"/><polygon points=\"5 5 5 5\"/><polyline points=\"\"/><polygon/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerPath, ATrailingOddCoordinateEndsThePointList) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<polygon points=\"0 0 10 0 10 10 7\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource), "M0,0 L10,0 L10,10 Z");
}

TEST(LowerPath, PathDataThatEndsInAnErrorDrawsItsValidPrefix) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<path d=\"M0 0 L10 0 L10 10 X 5 5\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource), "M0,0 L10,0 L10,10");
  EXPECT_EQ(l.loaded.Count(GVEC_DIAG_ATTRIBUTE_INVALID), (size_t) 1);
}

TEST(LowerPath, PathDataWithNoValidPrefixOrNoDataDrawsNothing) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<path d=\"X\"/><path d=\"\"/><path/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerPath, ACoordinatePastTheWaistsRangeIsALimitNotAClamp) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<path d=\"M0 0 L1e200 0 L0 1e200 Z\"/><polygon points=\"0 0 1e200 0 0 1e200\"/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 2);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerPath, CoordinatesPastTheWaistsRangeAreALimitNotAnInternalError) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<path d=\"M 1e308 0 L 1e308 1e308 L 0 1e308 z\"/><path d=\"M -1e308 0 L -1e308 -1e308 L 0 -1e308 z\"/>"
               "<svg x=\"1e308\" y=\"0\" width=\"1e308\" height=\"10\"><path d=\"M0 0H4V4Z\"/></svg>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 3);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerPath, TwoElementsWithTheSamePathShareOneResource) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<path d=\"M0 0H4V4H0Z\"/><path d=\"M0 0H4V4H0Z\" fill=\"red\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 2);
  EXPECT_EQ(l.Item(0).resource, l.Item(1).resource);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
