/**
 * @file
 *
 * Arcs (X1): path data holds an arc in endpoint form and the lowering hands it, as written, to the geometry library, which owns the
 * radius correction and the degenerate cases. The expected centre forms come from a reference that shares no code with the
 * library (tools/lower/gen-arc-vectors.py, 80-digit decimal arithmetic).
 */

#include "../lower_helpers.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include <ghoti.io/shape-geometry/arc.h>

using namespace gvec_test;

namespace {

/** The only arc of the only fill of `d`; false when there is no fill or the path has no arc at `index`. */
bool ArcOf(const std::string & d, GSHGEOM_Arc & arc, size_t index = 1, const std::string & extra = "") {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"", "<path d=\"" + d + "\" " + extra + "/>"), l);
  if (l.Items() != 1 || l.Refusals() != 0) {
    return false;
  }
  const GSHGEOM_Path * path = gshscene_resource_path(l.Item(0).resource);
  return path && gshgeom_path_arc(path, index, &arc) == GSHGEOM_OK;
}

std::string Num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.17g", v);
  return b;
}

}  // namespace

TEST(LowerArc, EveryReferenceVectorIsTheCentreFormTheReferenceGives) {
  std::ifstream in("tests/data/arc-vectors.txt");
  ASSERT_TRUE(in.good());
  std::string line;
  size_t rows = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream s(line);
    double x0, y0, rx, ry, phi, x, y, cx, cy, ux, uy, vx, vy;
    int large, sweep;
    std::string bar;
    s >> x0 >> y0 >> rx >> ry >> phi >> large >> sweep >> x >> y >> bar >> cx >> cy >> ux >> uy >> vx >> vy;
    ASSERT_EQ(bar, "|") << line;
    std::string d = "M" + Num(x0) + " " + Num(y0) + "A" + Num(rx) + " " + Num(ry) + " " + Num(phi) + " " + std::to_string(large) + " " +
                    std::to_string(sweep) + " " + Num(x) + " " + Num(y);
    GSHGEOM_Arc a;
    ASSERT_TRUE(ArcOf(d, a, 1, "fill=\"red\"")) << line;
    double scale = 1 + std::fabs(cx) + std::fabs(cy) + std::fabs(ux) + std::fabs(vy);
    EXPECT_NEAR(a.center.x, cx, 1e-12 * scale) << line;
    EXPECT_NEAR(a.center.y, cy, 1e-12 * scale) << line;
    EXPECT_NEAR(a.u.x, ux, 1e-12 * scale) << line;
    EXPECT_NEAR(a.u.y, uy, 1e-12 * scale) << line;
    EXPECT_NEAR(a.v.x, vx, 1e-12 * scale) << line;
    EXPECT_NEAR(a.v.y, vy, 1e-12 * scale) << line;
    // The ends are the document's, to the bit; the sweep's sign is the sweep flag and a large arc turns more than half.
    EXPECT_EQ(a.start.x, x0) << line;
    EXPECT_EQ(a.start.y, y0) << line;
    EXPECT_EQ(a.end.x, x) << line;
    EXPECT_EQ(a.end.y, y) << line;
    EXPECT_EQ(a.sweep > 0, sweep == 1) << line;
    double half = a.unit == GSHGEOM_ANGLE_DEGREES ? 180.0 : a.unit == GSHGEOM_ANGLE_RADIANS ? 3.14159265358979323846
                : a.unit == GSHGEOM_ANGLE_GRADIANS ? 200.0 : 0.5;
    double turn = std::fabs(a.sweep);
    EXPECT_EQ(turn > half * (1 + 1e-12), large == 1 && std::fabs(turn - half) > 1e-12 * half) << line;
    rows++;
  }
  EXPECT_GE(rows, (size_t) 20);
}

TEST(LowerArc, ACommandAfterAnArcContinuesFromItsEnd) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<path d=\"M10 10A10 10 0 0 1 30 10L30 30Z\" fill=\"red\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource).substr(0, 5), "M10,1");
  const GSHGEOM_Path * path = gshscene_resource_path(l.Item(0).resource);
  ASSERT_EQ(gshgeom_path_element_count(path), (size_t) 4);
  GSHGEOM_Element line;
  ASSERT_EQ(gshgeom_path_element(path, 2, &line), GSHGEOM_OK);
  EXPECT_EQ(line.curve.p[0].x, 30);
  EXPECT_EQ(line.curve.p[0].y, 10);
  EXPECT_EQ(line.curve.p[1].y, 30);
}

TEST(LowerArc, ARelativeArcAndTheCompactFlagSpellingAreTheSameArc) {
  GSHGEOM_Arc absolute, relative, compact;
  ASSERT_TRUE(ArcOf("M10 10A5 5 0 1 1 20 10", absolute));
  ASSERT_TRUE(ArcOf("M10 10a5 5 0 1 1 10 0", relative));
  ASSERT_TRUE(ArcOf("M10 10A5 5 0 1120 10", compact));
  EXPECT_EQ(std::memcmp(&absolute, &relative, sizeof absolute), 0);
  EXPECT_EQ(std::memcmp(&absolute, &compact, sizeof absolute), 0);
}

TEST(LowerArc, AZeroRadiusIsALineAndEqualEndsDrawNothing) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<path d=\"M10 10A0 5 0 0 1 30 10L30 30Z\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource), "M10,10 L30,10 L30,30 Z");
  Lowered same;  // an arc to its own start adds nothing, and a path of nothing else has no area
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<path d=\"M10 10A5 5 0 1 1 10 10\"/>"), same);
  EXPECT_EQ(same.Items(), (size_t) 0);
  EXPECT_EQ(same.Refusals(), (size_t) 0);
}

TEST(LowerArc, ANegativeRadiusIsItsAbsoluteValueAndASmallOneIsScaledUp) {
  GSHGEOM_Arc positive, negative, scaled;
  ASSERT_TRUE(ArcOf("M0 0A4 4 0 0 1 6 0", positive));
  ASSERT_TRUE(ArcOf("M0 0A-4 -4 0 0 1 6 0", negative));
  EXPECT_EQ(std::memcmp(&positive, &negative, sizeof positive), 0);
  ASSERT_TRUE(ArcOf("M0 0A1 1 0 0 1 6 0", scaled));
  EXPECT_NEAR(scaled.center.x, 3.0, 1e-15);
  EXPECT_NEAR(scaled.center.y, 0.0, 1e-15);
  EXPECT_NEAR(std::fabs(scaled.u.x), 3.0, 1e-15);
}

TEST(LowerArc, AStrokedArcIsAStrokeOfTheArcPath) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<path d=\"M10 10A10 10 0 0 1 30 10\" fill=\"none\" stroke=\"red\" stroke-width=\"2\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_STROKE);
  const GSHGEOM_Path * path = gshscene_resource_path(l.Item(0).resource);
  GSHGEOM_Verb verb;
  ASSERT_EQ(gshgeom_path_verb(path, 1, &verb), GSHGEOM_OK);
  EXPECT_EQ(verb, GSHGEOM_VERB_ARC);
}

TEST(LowerArc, ARadiusPastTheWaistsRangeIsALimitNotACrash) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<path d=\"M0 0A1e300 1e300 0 0 1 1e300 0\"/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
}

TEST(LowerArc, ARotationOfAnyAmountIsPassedInDegrees) {
  GSHGEOM_Arc a, b;
  ASSERT_TRUE(ArcOf("M0 0A8 4 90 0 1 6 0", a));
  ASSERT_TRUE(ArcOf("M0 0A8 4 450 0 1 6 0", b));
  EXPECT_NEAR(a.u.x, 0.0, 1e-12);
  EXPECT_NEAR(a.u.y, 8.0, 1e-12);  // the first half-diameter turned a quarter: u = rx (cos 90, sin 90)
  EXPECT_NEAR(b.u.y, 8.0, 1e-12);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
