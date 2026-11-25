/**
 * @file
 *
 * The layers of `mask` (CSS Masking 1): a gradient, an image or a list of them, positioned, sized and repeated in a box of the element, and composited.
 */

#include <ghoti.io/shape-scene/mask.h>
#include <ghoti.io/shape-scene/paint.h>
#include <ghoti.io/shape-scene/resource.h>

#include <algorithm>
#include <array>
#include <cmath>
#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

const char * const kImages = "tests/data/images";

/** A mask lowered: the item list of the picture of its soft mask. */
struct Masked {
  Lowered l;
  GSHSCENE_Mask mask{};
  std::vector<GSHSCENE_Item> items;
  bool has_mask = false;
};

/** A rect, 40 by 20 at (20, 20) (more `shape` attributes may be given), masked by the CSS `css`; a `mask` element `mask1` is in the document. */
void Lower(const std::string & css, Masked & m, const std::string & shape = "", const std::string & body_extra = "") {
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.image_root = kImages;
  options.image_base = kImages;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<mask id=\"mask1\"><rect width=\"100\" height=\"100\" fill=\"white\"/></mask>" + body_extra + "<rect x=\"20\" y=\"20\" width=\"40\" height=\"20\" fill=\"green\" " + shape +
                   " style=\"" + css + "\"/>"),
      m.l, 0, 0, &options);
  m.has_mask = m.l.Items() >= 1 && m.l.Item(0).kind == GSHSCENE_ITEM_GROUP_PUSH && m.l.Item(0).group.soft_mask &&
      gshscene_resource_mask(m.l.Item(0).group.soft_mask, &m.mask) == GSHSCENE_OK;
  if (!m.has_mask) {
    return;
  }
  const GSHSCENE_List * list = gshscene_resource_picture(m.mask.picture);
  for (size_t i = 0; list && i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * item = nullptr;
    if (gshscene_list_item(list, i, &item) == GSHSCENE_OK) {
      m.items.push_back(*item);
    }
  }
}

size_t Fills(const Masked & m) {
  size_t n = 0;
  for (const GSHSCENE_Item & item : m.items) {
    n += item.kind == GSHSCENE_ITEM_FILL;
  }
  return n;
}

std::vector<GSHSCENE_Item> FillsOf(const Masked & m) {
  std::vector<GSHSCENE_Item> out;
  for (const GSHSCENE_Item & item : m.items) {
    if (item.kind == GSHSCENE_ITEM_FILL) {
      out.push_back(item);
    }
  }
  return out;
}

bool GradientOf(const GSHSCENE_Item & item, GSHSCENE_Gradient & g) {
  return item.paint.kind == GSHSCENE_PAINT_GRADIENT && gshscene_resource_gradient(item.paint.resource, &g) == GSHSCENE_OK;
}

GSHGEOM_Bounds BoundsOf(const GSHSCENE_Item & item) {
  GSHGEOM_Bounds box;
  box.empty = 1;
  const GSHGEOM_Path * path = gshscene_resource_path(item.resource);
  if (!path || gshgeom_path_bounds(path, &box) != GSHGEOM_OK) {
    box.empty = 1;
  }
  return box;
}

}  // namespace

TEST(LowerMaskLayers, ALinearGradientIsAnAlphaMaskOfTheElementsBox) {
  Masked m;
  Lower("mask: linear-gradient(to right, black, transparent)", m);
  ASSERT_TRUE(m.has_mask);
  EXPECT_EQ(m.l.Refusals(), (size_t) 0);
  EXPECT_EQ(m.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA) << "match-source: an image is its alpha";
  ASSERT_EQ(Fills(m), (size_t) 1);
  const GSHSCENE_Item fill = FillsOf(m)[0];
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(fill, g));
  EXPECT_EQ(g.kind, (uint32_t) GSHSCENE_GRADIENT_LINEAR);
  EXPECT_EQ(g.premultiplied, (uint32_t) 1) << "CSS gradients interpolate premultiplied";
  EXPECT_EQ(g.extend, (uint32_t) GSHSCENE_EXTEND_PAD);
  EXPECT_NEAR(g.p0[0], 0.0, 1e-9);
  EXPECT_NEAR(g.p1[0], 40.0, 1e-9) << "the line is the tile's width";
  EXPECT_NEAR(g.p0[1], 10.0, 1e-9);
  EXPECT_NEAR(g.p1[1], 10.0, 1e-9);
  EXPECT_EQ(g.map.t[0], 20.0) << "the tile is at the element's box";
  EXPECT_EQ(g.map.t[1], 20.0);
  EXPECT_EQ(g.stop_count, (size_t) 2);
  GSHGEOM_Bounds box = BoundsOf(fill);
  EXPECT_EQ(box.min.x, 20.0);
  EXPECT_EQ(box.max.x, 60.0);
  EXPECT_EQ(box.max.y, 40.0);
}

TEST(LowerMaskLayers, TheAngleAndTheCornerSetTheLineOfALinearGradient) {
  Masked angle, corner, diagonal;
  Lower("mask: linear-gradient(90deg, black, white)", angle);
  Lower("mask: linear-gradient(to top, black, white)", corner);
  Lower("mask: linear-gradient(to bottom right, black, white)", diagonal);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(angle.has_mask && GradientOf(FillsOf(angle)[0], g));
  EXPECT_NEAR(g.p1[0] - g.p0[0], 40.0, 1e-6);
  EXPECT_NEAR(g.p1[1] - g.p0[1], 0.0, 1e-6);
  ASSERT_TRUE(corner.has_mask && GradientOf(FillsOf(corner)[0], g));
  EXPECT_NEAR(g.p1[0] - g.p0[0], 0.0, 1e-6);
  EXPECT_NEAR(g.p1[1] - g.p0[1], -20.0, 1e-6) << "to top runs up the tile";
  ASSERT_TRUE(diagonal.has_mask && GradientOf(FillsOf(diagonal)[0], g));
  // the 50% line of `to bottom right` passes through the other two corners: the gradient line is perpendicular to the diagonal from top right to bottom left
  const double dx = g.p1[0] - g.p0[0], dy = g.p1[1] - g.p0[1];
  EXPECT_NEAR(dx * 40.0 - dy * 20.0, 0.0, 1e-6) << "perpendicular to (w, -h)... that is (20, 40) up to scale";
  EXPECT_GT(dx, 0.0);
  EXPECT_GT(dy, 0.0);
}

TEST(LowerMaskLayers, TheStopsAreFixedUpAsCssSays) {
  Masked m;
  Lower("mask: linear-gradient(black, black 30%, white, white 20px)", m);
  ASSERT_TRUE(m.has_mask);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(FillsOf(m)[0], g));
  ASSERT_EQ(g.stop_count, (size_t) 4);
  EXPECT_NEAR(g.stops[0].offset, 0.0, 1e-9) << "the first is 0 when it has no position";
  EXPECT_NEAR(g.stops[1].offset, 0.3, 1e-9);
  EXPECT_NEAR(g.stops[2].offset, 0.65, 1e-9) << "evenly between the stops with positions: 30% and 20px (the tile is 20 high and the line runs down it: 100%)";
  EXPECT_NEAR(g.stops[3].offset, 1.0, 1e-9);
  Masked two;
  Lower("mask: linear-gradient(red 10% 40%, blue)", two);
  ASSERT_TRUE(two.has_mask && GradientOf(FillsOf(two)[0], g));
  ASSERT_EQ(g.stop_count, (size_t) 3) << "a stop with two positions is two stops of one color";
  EXPECT_NEAR(g.stops[0].offset, 0.1, 1e-9);
  EXPECT_NEAR(g.stops[1].offset, 0.4, 1e-9);
  EXPECT_NEAR(g.stops[2].offset, 1.0, 1e-9);
  // a position before an earlier one is raised to it
  Masked back;
  Lower("mask: linear-gradient(black 50%, white 20%)", back);
  ASSERT_TRUE(back.has_mask && GradientOf(FillsOf(back)[0], g));
  ASSERT_EQ(g.stop_count, (size_t) 2);
  EXPECT_NEAR(g.stops[0].offset, 0.5, 1e-9);
  EXPECT_NEAR(g.stops[1].offset, 0.5, 1e-9) << "the scene sorts the stops by position, so only raised to 50% can the first stay the first";
}

TEST(LowerMaskLayers, ARepeatingGradientRepeatsItsStops) {
  Masked m;
  Lower("mask: repeating-linear-gradient(to right, black 0px, white 10px)", m);
  ASSERT_TRUE(m.has_mask);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(FillsOf(m)[0], g));
  EXPECT_EQ(g.extend, (uint32_t) GSHSCENE_EXTEND_REPEAT);
  EXPECT_NEAR(g.p1[0] - g.p0[0], 10.0, 1e-9) << "the period is the span of the stops";
}

TEST(LowerMaskLayers, ARadialGradientHasItsShapeSizeAndCenter) {
  Masked m;
  Lower("mask: radial-gradient(circle closest-side at 25% 50%, black, transparent)", m);
  ASSERT_TRUE(m.has_mask);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(FillsOf(m)[0], g));
  EXPECT_EQ(g.kind, (uint32_t) GSHSCENE_GRADIENT_RADIAL);
  EXPECT_NEAR(g.r1, 1.0, 1e-9);
  // the center is (10, 10) in the tile of 40 by 20 at (20, 20); closest-side is 10 (the sides at 10 and 10 vertically)
  EXPECT_NEAR(g.map.t[0], 20.0 + 10.0, 1e-9);
  EXPECT_NEAR(g.map.t[1], 20.0 + 10.0, 1e-9);
  EXPECT_NEAR(g.map.m[0][0], 10.0, 1e-9);
  EXPECT_NEAR(g.map.m[1][1], 10.0, 1e-9);
  Masked ellipse;
  Lower("mask: radial-gradient(farthest-side, black, transparent)", ellipse);
  ASSERT_TRUE(ellipse.has_mask && GradientOf(FillsOf(ellipse)[0], g));
  EXPECT_NEAR(g.map.m[0][0], 20.0, 1e-9) << "an ellipse reaching the far sides: half the width";
  EXPECT_NEAR(g.map.m[1][1], 10.0, 1e-9);
  Masked corner;
  Lower("mask: radial-gradient(black, transparent)", corner);
  ASSERT_TRUE(corner.has_mask && GradientOf(FillsOf(corner)[0], g));
  EXPECT_NEAR(g.map.m[0][0], 20.0 * 1.4142135623730951, 1e-6) << "the default is an ellipse to the farthest corner";
  Masked sized;
  Lower("mask: radial-gradient(circle 15px at left top, black, transparent)", sized);
  ASSERT_TRUE(sized.has_mask && GradientOf(FillsOf(sized)[0], g));
  EXPECT_NEAR(g.map.m[0][0], 15.0, 1e-9);
  EXPECT_NEAR(g.map.t[0], 20.0, 1e-9);
  EXPECT_NEAR(g.map.t[1], 20.0, 1e-9);
}

namespace {

/** sRGB to oklab, and back: an independent reading of CSS Color 4, 9, to check the samples of a gradient that interpolates in oklab. */
double Linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double Gamma(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
void ToOklab(const double rgb[3], double lab[3]) {
  const double r = Linear(rgb[0]), g = Linear(rgb[1]), b = Linear(rgb[2]);
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}
void FromOklab(const double lab[3], double rgb[3]) {
  const double l = std::pow(lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2], 3);
  const double m = std::pow(lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2], 3);
  const double s = std::pow(lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2], 3);
  rgb[0] = Gamma(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
  rgb[1] = Gamma(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
  rgb[2] = Gamma(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

/** The stops of a gradient fill as sRGB (the colors of the stops differ in the space they were written in; the waist resolves them to one). */
std::vector<GSHSCENE_Resolved_Stop> Resolved(const GSHSCENE_Item & item) {
  GSHSCENE_Gradient g;
  EXPECT_TRUE(GradientOf(item, g));
  GSHSCENE_Color_Space srgb;
  srgb.primaries = GCOL_PRIMARIES_SRGB;
  srgb.transfer = GCOL_TRANSFER_SRGB;
  srgb.gamma = 0;
  GSHSCENE_Color foreground;
  gshscene_color_init(&foreground);
  std::vector<GSHSCENE_Resolved_Stop> out(g.stop_count);
  EXPECT_EQ(gshscene_gradient_resolve(item.paint.resource, &foreground, nullptr, &srgb, nullptr, out.data(), out.size()), GSHSCENE_OK);
  return out;
}

/** The resolved stop at `offset`, or nullptr. */
const GSHSCENE_Resolved_Stop * StopAt(const std::vector<GSHSCENE_Resolved_Stop> & stops, double offset) {
  for (const auto & s : stops) {
    if (std::fabs(s.offset - offset) < 1e-9) {
      return &s;
    }
  }
  return nullptr;
}

}  // namespace

TEST(LowerMaskLayers, AConicGradientIsTheWaistsSweepFromTheTopClockwise) {
  Masked m;
  Lower("mask: conic-gradient(from 90deg at 25% 50%, black, white)", m);
  ASSERT_TRUE(m.has_mask);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(FillsOf(m)[0], g));
  EXPECT_EQ(g.kind, (uint32_t) GSHSCENE_GRADIENT_SWEEP);
  EXPECT_EQ(g.angle_unit, (uint32_t) GSHGEOM_ANGLE_DEGREES);
  EXPECT_NEAR(g.a0, 0.0, 1e-9) << "CSS starts at the top; the sweep at +x: from 90deg is the sweep's 0";
  EXPECT_NEAR(g.a1, 360.0, 1e-9);
  EXPECT_EQ(g.extend, (uint32_t) GSHSCENE_EXTEND_REPEAT) << "the turn wraps";
  EXPECT_NEAR(g.center[0], 10.0, 1e-9) << "25% of a 40 wide tile";
  EXPECT_NEAR(g.center[1], 10.0, 1e-9);
  Masked top;
  Lower("mask: conic-gradient(black, white)", top);
  ASSERT_TRUE(top.has_mask && GradientOf(FillsOf(top)[0], g));
  EXPECT_NEAR(g.a0, -90.0, 1e-9) << "from the top (-y)";
  EXPECT_NEAR(g.center[0], 20.0, 1e-9);
  // a stop at an angle: before the first and after the last the end colors, as stops at the ends of the turn
  Masked angled;
  Lower("mask: conic-gradient(black 90deg, white 180deg)", angled);
  ASSERT_TRUE(angled.has_mask && GradientOf(FillsOf(angled)[0], g));
  ASSERT_EQ(g.stop_count, (size_t) 4);
  EXPECT_NEAR(g.stops[0].offset, 0.0, 1e-9);
  EXPECT_NEAR(g.stops[1].offset, 0.25, 1e-9);
  EXPECT_NEAR(g.stops[2].offset, 0.5, 1e-9);
  EXPECT_NEAR(g.stops[3].offset, 1.0, 1e-9);
  EXPECT_NEAR(g.stops[0].color.rgba[0], 0.0, 1e-9) << "black before the first stop";
  EXPECT_NEAR(g.stops[3].color.rgba[0], 1.0, 1e-9) << "white after the last";
  // repeating: the period is the stops' span
  Masked repeating;
  Lower("mask: repeating-conic-gradient(black 0 10%, white 10% 20%)", repeating);
  ASSERT_TRUE(repeating.has_mask && GradientOf(FillsOf(repeating)[0], g));
  EXPECT_NEAR(g.a0, -90.0, 1e-9);
  EXPECT_NEAR(g.a1, -90.0 + 72.0, 1e-9) << "20% of a turn";
}

TEST(LowerMaskLayers, AnInterpolationHintBendsTheStretchItLiesIn) {
  Masked m;
  Lower("mask: linear-gradient(black, 25%, white)", m);
  ASSERT_TRUE(m.has_mask);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(FillsOf(m)[0], g));
  EXPECT_GT(g.stop_count, (size_t) 2) << "the curve is given as stops";
  const auto stops = Resolved(FillsOf(m)[0]);
  const GSHSCENE_Resolved_Stop * at_hint = StopAt(stops, 0.25);
  ASSERT_NE(at_hint, nullptr);
  EXPECT_NEAR(at_hint->rgba[0], 0.5, 1.0 / 256.0) << "the color halfway between the two is at the hint";
  for (size_t i = 1; i < stops.size(); i++) {
    EXPECT_GE(stops[i].rgba[0], stops[i - 1].rgba[0]) << "the curve rises";
  }
  // between two samples the waist draws a line: it is within 1/256 of the curve at the middle of every stretch
  for (size_t i = 1; i < stops.size(); i++) {
    const double o = (stops[i].offset + stops[i - 1].offset) / 2;
    const double w = std::pow(o, std::log(0.5) / std::log(0.25));
    const double line = (stops[i].rgba[0] + stops[i - 1].rgba[0]) / 2;
    EXPECT_NEAR(line, w, 1.0 / 256.0) << "at " << o;
  }
  // a hint at an end of its stretch is a jump
  Masked jump;
  Lower("mask: linear-gradient(black, 0%, white)", jump);
  ASSERT_TRUE(jump.has_mask && GradientOf(FillsOf(jump)[0], g));
  ASSERT_EQ(g.stop_count, (size_t) 3);
  EXPECT_NEAR(g.stops[0].color.rgba[0], 0.0, 1e-9);
  EXPECT_NEAR(g.stops[1].color.rgba[0], 1.0, 1e-9) << "white from the start of the stretch";
  EXPECT_NEAR(g.stops[1].offset, 0.0, 1e-9);
}

TEST(LowerMaskLayers, AColorInterpolationMethodIsSampledInThatSpace) {
  Masked m;
  Lower("mask: linear-gradient(in oklab, red, blue)", m);
  ASSERT_TRUE(m.has_mask);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(FillsOf(m)[0], g));
  EXPECT_GT(g.stop_count, (size_t) 2);
  const auto stops = Resolved(FillsOf(m)[0]);
  const GSHSCENE_Resolved_Stop * mid = StopAt(stops, 0.5);
  ASSERT_NE(mid, nullptr);
  const double red[3] = {1, 0, 0}, blue[3] = {0, 0, 1};
  double a[3], b[3], c[3], expected[3];
  ToOklab(red, a);
  ToOklab(blue, b);
  for (int k = 0; k < 3; k++) {
    c[k] = (a[k] + b[k]) / 2;
  }
  FromOklab(c, expected);
  for (int k = 0; k < 3; k++) {
    EXPECT_NEAR(mid->rgba[k], std::min(1.0, std::max(0.0, expected[k])), 1.0 / 128.0) << "channel " << k << ": the middle in oklab is not the middle in sRGB";
  }
  // a polar space with a hue method
  Masked hue;
  Lower("mask: linear-gradient(in hsl longer hue, red, blue)", hue);
  ASSERT_TRUE(hue.has_mask && GradientOf(FillsOf(hue)[0], g));
  EXPECT_GT(g.stop_count, (size_t) 2);
  // the method may come after the direction, and for a conic and a radial gradient
  Masked after;
  Lower("mask: linear-gradient(to right in oklab, red, blue)", after);
  EXPECT_TRUE(after.has_mask);
  Masked conic;
  Lower("mask: conic-gradient(from 10deg in oklab, red, blue)", conic);
  EXPECT_TRUE(conic.has_mask);
  Masked radial;
  Lower("mask: radial-gradient(circle in oklab, red, blue)", radial);
  EXPECT_TRUE(radial.has_mask);
}

TEST(LowerMaskLayers, TheSizeThePositionAndTheRepeatPlaceTheTiles) {
  Masked one;
  Lower("mask: linear-gradient(black, white) center / 50% auto no-repeat", one);
  ASSERT_TRUE(one.has_mask);
  ASSERT_EQ(Fills(one), (size_t) 1);
  GSHGEOM_Bounds box = BoundsOf(FillsOf(one)[0]);
  EXPECT_EQ(box.max.x - box.min.x, 20.0) << "50% of the 40 wide area";
  EXPECT_EQ(box.max.y - box.min.y, 20.0) << "auto: a gradient has no size of its own, so the area's";
  EXPECT_EQ(box.min.x, 30.0) << "centered: (40 - 20) / 2 into the area";
  Masked across;
  Lower("mask: linear-gradient(black, white) 0 0 / 10px 20px repeat-x", across);
  ASSERT_TRUE(across.has_mask);
  EXPECT_EQ(Fills(across), (size_t) 4);
  Masked both;
  Lower("mask: linear-gradient(black, white) 0 0 / 10px 10px repeat", both);
  ASSERT_TRUE(both.has_mask);
  EXPECT_EQ(Fills(both), (size_t) 8) << "4 across and 2 down";
  Masked down;
  Lower("mask: linear-gradient(black, white) 0 0 / 10px 10px repeat-y", down);
  ASSERT_TRUE(down.has_mask);
  EXPECT_EQ(Fills(down), (size_t) 2);
  Masked round;
  Lower("mask: linear-gradient(black, white) 0 0 / 15px 20px round no-repeat", round);
  ASSERT_TRUE(round.has_mask);
  Masked rounded;
  Lower("mask: linear-gradient(black, white) 0 0 / 15px 20px round", rounded);
  ASSERT_TRUE(rounded.has_mask);
  EXPECT_EQ(Fills(rounded), (size_t) 3) << "40 / 15 rounds to 3 tiles";
  box = BoundsOf(FillsOf(rounded)[0]);
  EXPECT_NEAR(box.max.x - box.min.x, 40.0 / 3.0, 1e-9) << "stretched to fit";
  Masked spaced;
  Lower("mask: linear-gradient(black, white) 0 0 / 15px 20px space", spaced);
  ASSERT_TRUE(spaced.has_mask);
  ASSERT_EQ(Fills(spaced), (size_t) 2) << "2 of 15 fit in 40";
  EXPECT_EQ(BoundsOf(FillsOf(spaced)[0]).min.x, 20.0);
  EXPECT_EQ(BoundsOf(FillsOf(spaced)[1]).min.x, 45.0) << "the 10 left over is the space between";
  Masked contain, cover;
  Lower("mask: linear-gradient(black, white) 0 0 / contain no-repeat", contain);
  Lower("mask: linear-gradient(black, white) 0 0 / cover no-repeat", cover);
  ASSERT_TRUE(contain.has_mask && cover.has_mask);
  EXPECT_EQ(BoundsOf(FillsOf(contain)[0]).max.x - BoundsOf(FillsOf(contain)[0]).min.x, 40.0) << "no ratio: the area";
}

TEST(LowerMaskLayers, AMaskPositionIsAnyCssPositionWithOffsetsFromItsEdges) {
  // a 10 by 10 tile once, in the 40 by 20 area at (20, 20): where its corner is for each form of position
  struct Case {
    const char * position;
    double x, y;
  };
  const Case cases[] = {
      {"left 5px top 3px", 25, 23},
      {"right 5px bottom 3px", 45, 27},   // 20 + (40 - 10) - 5, 20 + (20 - 10) - 3
      {"right 10% bottom 20%", 47, 28},   // 90% and 80% of the free space
      {"top 3px left 5px", 25, 23},        // the keywords in the other order
      {"right bottom", 50, 30},
      {"center", 35, 25},
      {"bottom", 35, 30},
      {"right", 50, 25},
      {"30% 70%", 29, 27},
      {"4px", 24, 25},
      {"left 25%", 20, 22.5},             // a keyword and a percentage: x at the left, y at 25%
  };
  for (const Case & c : cases) {
    Masked m;
    Lower(std::string("mask-image: linear-gradient(black, white); mask-repeat: no-repeat; mask-size: 10px 10px; mask-position: ") + c.position, m);
    ASSERT_TRUE(m.has_mask) << c.position;
    ASSERT_EQ(Fills(m), (size_t) 1) << c.position;
    const GSHGEOM_Bounds box = BoundsOf(FillsOf(m)[0]);
    EXPECT_NEAR(box.min.x, c.x, 1e-9) << c.position;
    EXPECT_NEAR(box.min.y, c.y, 1e-9) << c.position;
  }
  // not a position (browsers refuse the three-value forms too): the declaration is not valid, so there is the initial one (0% 0%)
  for (const char * bad : {"top 25%", "left right", "10px 5px 3px", "left 10px top", "center top 2px", "right 10px bottom 5px left"}) {
    Masked m;
    Lower(std::string("mask-image: linear-gradient(black, white); mask-repeat: no-repeat; mask-size: 10px 10px; mask-position: ") + bad, m);
    ASSERT_TRUE(m.has_mask) << bad;
    const GSHGEOM_Bounds box = BoundsOf(FillsOf(m)[0]);
    EXPECT_NEAR(box.min.x, 20, 1e-9) << bad;
    EXPECT_NEAR(box.min.y, 20, 1e-9) << bad;
  }
  // a gradient's center takes the same positions
  Masked radial;
  Lower("mask: radial-gradient(circle 5px at right 10px bottom 5px, black, white)", radial);
  ASSERT_TRUE(radial.has_mask);
  GSHSCENE_Gradient g;
  ASSERT_TRUE(GradientOf(FillsOf(radial)[0], g));
  EXPECT_NEAR(g.map.t[0], 20.0 + 40.0 - 10.0, 1e-9);
  EXPECT_NEAR(g.map.t[1], 20.0 + 20.0 - 5.0, 1e-9);
}

namespace {

/** The rectangles of every fill of the mask, as (x0, y0, x1, y1), sorted. */
std::vector<std::array<double, 4>> Cells(const Masked & m) {
  std::vector<std::array<double, 4>> out;
  for (const GSHSCENE_Item & fill : FillsOf(m)) {
    const GSHGEOM_Bounds box = BoundsOf(fill);
    out.push_back({box.min.x, box.min.y, box.max.x, box.max.y});
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST(LowerMaskLayers, AMaskBorderIsTheSourceCutIntoNineAndShownAroundTheBox) {
  // the box is 40 by 20 at (20, 20); a gradient has the size of the border image area, and the slices are 5 pixels, shown in borders of 5
  Masked m;
  Lower("mask-border-source: linear-gradient(black, white); mask-border-slice: 5; mask-border-width: 5px", m);
  ASSERT_TRUE(m.has_mask);
  auto cells = Cells(m);
  ASSERT_EQ(cells.size(), (size_t) 8) << "the middle is not drawn without `fill`";
  const std::vector<std::array<double, 4>> expected = {{20, 20, 25, 25}, {20, 25, 25, 35}, {20, 35, 25, 40}, {25, 20, 55, 25},
                                                       {25, 35, 55, 40}, {55, 20, 60, 25}, {55, 25, 60, 35}, {55, 35, 60, 40}};
  EXPECT_EQ(cells, expected);
  GSHSCENE_Gradient g;
  for (const GSHSCENE_Item & fill : FillsOf(m)) {
    ASSERT_TRUE(GradientOf(fill, g));
    EXPECT_NEAR(g.map.t[0] + g.p0[0] * 0, 20.0, 1e-9) << "slices of the gradient are where the gradient is: the area is at (20, 20)";
    EXPECT_NEAR(g.map.t[1], 20.0, 1e-9);
  }
  Masked filled;
  Lower("mask-border-source: linear-gradient(black, white); mask-border-slice: 5 fill; mask-border-width: 5px", filled);
  ASSERT_TRUE(filled.has_mask);
  EXPECT_EQ(Fills(filled), (size_t) 9) << "`fill` keeps the middle";
  // an auto width of a gradient is nothing (it has no size of its own, and an SVG element no border): there is no border to draw
  Masked automatic;
  Lower("mask-border-source: linear-gradient(black, white); mask-border-slice: 5", automatic);
  EXPECT_FALSE(automatic.has_mask && Fills(automatic) > 0);
  // borders that are together wider than the area are shrunk, together, to fit it: 30 and 30 across 40, and down 20, is a third of each (10)
  Masked shrunk;
  Lower("mask-border-source: linear-gradient(black, white); mask-border-slice: 5; mask-border-width: 30px", shrunk);
  ASSERT_TRUE(shrunk.has_mask);
  auto sc = Cells(shrunk);
  ASSERT_EQ(sc.size(), (size_t) 6) << "the corners and the top and the bottom: the sides are of no height";
  EXPECT_EQ(sc.front(), (std::array<double, 4>{20, 20, 30, 30}));
  // percentages: a width of 25% is of the area (10 of 40 across, 5 of 20 down); a slice of 25% is of the image
  Masked percent;
  Lower("mask-border-source: linear-gradient(black, white); mask-border-slice: 25%; mask-border-width: 25%", percent);
  ASSERT_TRUE(percent.has_mask);
  auto pc = Cells(percent);
  ASSERT_EQ(pc.size(), (size_t) 8);
  EXPECT_EQ(pc.front(), (std::array<double, 4>{20, 20, 30, 25}));
  // the outset pushes the area out
  Masked outset;
  Lower("mask-border-source: linear-gradient(black, white); mask-border-slice: 5; mask-border-width: 5px; mask-border-outset: 10px", outset);
  ASSERT_TRUE(outset.has_mask);
  EXPECT_EQ(Cells(outset).front(), (std::array<double, 4>{10, 10, 15, 15}));
  // it is over the layers of `mask`, added to them
  Masked layered;
  Lower("mask-image: linear-gradient(black, white); mask-border-source: linear-gradient(black, white); mask-border-slice: 5; mask-border-width: 5px", layered);
  ASSERT_TRUE(layered.has_mask);
  EXPECT_GE(Fills(layered), (size_t) 9) << "the layer and the eight pieces of the border";
}

TEST(LowerMaskLayers, AMaskBorderRepeatsStretchesRoundsOrSpacesItsEdges) {
  // slices of 5 down and 10 across, in borders of 5: the top edge is the 20 wide middle of the image at 1:1 (it is 5 high in both), in 30 of room
  const char * base = "mask-border-source: linear-gradient(black, white); mask-border-slice: 5 10; mask-border-width: 5px; mask-border-repeat: ";
  auto top_edge = [&](const char * repeat) {
    Masked m;
    Lower(std::string(base) + repeat, m);
    EXPECT_TRUE(m.has_mask) << repeat;
    std::vector<std::array<double, 4>> out;
    for (const auto & c : Cells(m)) {
      if (c[1] == 20 && c[3] == 25 && c[0] >= 25 && c[2] <= 55) {
        out.push_back(c);
      }
    }
    return out;
  };
  using Cell = std::array<double, 4>;
  EXPECT_EQ(top_edge("stretch"), (std::vector<Cell>{{25, 20, 55, 25}}));
  EXPECT_EQ(top_edge("repeat"), (std::vector<Cell>{{25, 20, 40, 25}, {40, 20, 55, 25}})) << "20 wide tiles centered in 30: cut off 5 at each end";
  EXPECT_EQ(top_edge("round"), (std::vector<Cell>{{25, 20, 40, 25}, {40, 20, 55, 25}})) << "two tiles of 15";
  EXPECT_EQ(top_edge("space"), (std::vector<Cell>{{30, 20, 50, 25}})) << "one tile of 20 with 5 of room each side";
}

TEST(LowerMaskLayers, AMaskBorderOfAnImageTakesAutoWidthsFromItsSlices) {
  Masked m;
  Lower("mask-border-source: url(red-blue-2x1.png); mask-border-slice: 0 1", m);
  ASSERT_TRUE(m.has_mask);
  EXPECT_EQ(Fills(m), (size_t) 2) << "the left and the right edge, each as wide as its pixel, as tall as the box";
  Masked mode;
  Lower("mask-border-source: url(red-blue-2x1.png); mask-border-slice: 0 1; mask-border-mode: luminance", mode);
  ASSERT_TRUE(mode.has_mask);
  EXPECT_EQ(mode.mask.mode, (uint32_t) GSHSCENE_MASK_LUMINANCE);
}

TEST(LowerMaskLayers, TheOriginAndTheClipAreBoxesOfTheElement) {
  Masked fill, stroke, view;
  Lower("mask: linear-gradient(black, white) border-box", fill, "stroke=\"black\" stroke-width=\"10\"");
  ASSERT_TRUE(fill.has_mask);
  GSHGEOM_Bounds box = BoundsOf(FillsOf(fill)[0]);
  EXPECT_EQ(box.min.x, 15.0) << "border-box is the stroke-box of an SVG element: 5 each side";
  EXPECT_EQ(box.max.x, 65.0);
  Lower("mask: linear-gradient(black, white) content-box", stroke, "stroke=\"black\" stroke-width=\"10\"");
  ASSERT_TRUE(stroke.has_mask);
  box = BoundsOf(FillsOf(stroke)[0]);
  EXPECT_EQ(box.min.x, 20.0) << "content-box is the fill-box";
  Lower("mask: linear-gradient(black, white) view-box", view);
  ASSERT_TRUE(view.has_mask);
  box = BoundsOf(FillsOf(view)[0]);
  EXPECT_EQ(box.min.x, 0.0);
  EXPECT_EQ(box.max.x, 100.0);
  EXPECT_EQ(box.max.y, 100.0);
  // a view box that is not square: its width and its height are not exchanged
  Masked wide;
  GVEC_Lower_Options wide_options;
  std::memset(&wide_options, 0, sizeof wide_options);
  LowerSvg(Svg("width=\"200\" height=\"100\"", "<rect x=\"20\" y=\"20\" width=\"40\" height=\"20\" style=\"mask: linear-gradient(black, white) view-box\"/>"), wide.l, 0, 0, &wide_options);
  ASSERT_GE(wide.l.Items(), 1u);
  ASSERT_EQ(wide.l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  ASSERT_NE(wide.l.Item(0).group.soft_mask, nullptr);
  ASSERT_EQ(gshscene_resource_mask(wide.l.Item(0).group.soft_mask, &wide.mask), GSHSCENE_OK);
  const GSHSCENE_List * wide_list = gshscene_resource_picture(wide.mask.picture);
  ASSERT_NE(wide_list, nullptr);
  for (size_t i = 0; i < gshscene_list_count(wide_list); i++) {
    const GSHSCENE_Item * item = nullptr;
    ASSERT_EQ(gshscene_list_item(wide_list, i, &item), GSHSCENE_OK);
    wide.items.push_back(*item);
  }
  ASSERT_GE(Fills(wide), 1u);
  box = BoundsOf(FillsOf(wide)[0]);
  EXPECT_EQ(box.max.x, 200.0) << "the view box is as wide as the viewport";
  EXPECT_EQ(box.max.y, 100.0);
  // the origin and the clip are separate: the picture is placed in one, painted in the other
  Masked split;
  Lower("mask-image: linear-gradient(black, white); mask-repeat: no-repeat; mask-origin: view-box; mask-clip: fill-box", split);
  ASSERT_TRUE(split.has_mask);
  box = BoundsOf(FillsOf(split)[0]);
  EXPECT_EQ(box.min.x, 0.0) << "the tile is the view box";
  EXPECT_EQ(box.max.x, 100.0);
  ASSERT_GE(split.items.size(), (size_t) 1);
  EXPECT_EQ(split.items[0].kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(BoundsOf(split.items[0]).min.x, 20.0) << "clipped to the fill-box";
}

TEST(LowerMaskLayers, AModeOfLuminanceIsTheMaskModeOfASingleLayer) {
  Masked luminance, alpha;
  Lower("mask: linear-gradient(black, white) luminance", luminance);
  Lower("mask: linear-gradient(black, white) alpha", alpha);
  ASSERT_TRUE(luminance.has_mask && alpha.has_mask);
  EXPECT_EQ(luminance.mask.mode, (uint32_t) GSHSCENE_MASK_LUMINANCE);
  EXPECT_EQ(alpha.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA);
}

TEST(LowerMaskLayers, AMaskElementIsItsOwnMaskTypeUntilALayerNamesAMode) {
  Masked implicit, alpha, luminance;
  Lower("mask: url(#mask1)", implicit);
  Lower("mask: url(#mask1) alpha", alpha);
  Lower("mask: url(#mask1) luminance", luminance);
  ASSERT_TRUE(implicit.has_mask && alpha.has_mask && luminance.has_mask);
  EXPECT_EQ(implicit.mask.mode, (uint32_t) GSHSCENE_MASK_LUMINANCE) << "match-source: the element's mask-type is luminance";
  EXPECT_EQ(alpha.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA) << "a layer that names alpha overrides the element's mask-type";
  EXPECT_EQ(luminance.mask.mode, (uint32_t) GSHSCENE_MASK_LUMINANCE);
  Masked typed;
  Lower("mask: url(#typed)", typed, "", "<mask id=\"typed\" style=\"mask-type: alpha\"><rect width=\"100\" height=\"100\" fill=\"white\"/></mask>");
  Masked typed_luminance;
  Lower("mask: url(#typed) luminance", typed_luminance, "", "<mask id=\"typed\" style=\"mask-type: alpha\"><rect width=\"100\" height=\"100\" fill=\"white\"/></mask>");
  ASSERT_TRUE(typed.has_mask && typed_luminance.has_mask);
  EXPECT_EQ(typed.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA) << "match-source reads mask-type: alpha";
  EXPECT_EQ(typed_luminance.mask.mode, (uint32_t) GSHSCENE_MASK_LUMINANCE) << "a layer that names luminance overrides mask-type: alpha";
}

TEST(LowerMaskLayers, AMaskElementInAListMatchesItsSourceAsLuminance) {
  Masked implicit, luminance, alpha;
  Lower("mask: url(#mask1), url(#mask1) intersect", implicit);
  Lower("mask: url(#mask1) luminance, url(#mask1) luminance intersect", luminance);
  Lower("mask: url(#mask1) alpha, url(#mask1) alpha intersect", alpha);
  ASSERT_TRUE(implicit.has_mask && luminance.has_mask && alpha.has_mask);
  EXPECT_EQ(gshscene_resource_hash(implicit.mask.picture), gshscene_resource_hash(luminance.mask.picture)) << "match-source: a mask element is its luminance";
  EXPECT_NE(gshscene_resource_hash(implicit.mask.picture), gshscene_resource_hash(alpha.mask.picture))
      << "a luminance layer is made into its alpha first; an alpha one is not";
}

TEST(LowerMaskLayers, TheLayersAreCompositedWithThoseUnderThem) {
  struct { const char * keyword; uint32_t composite; } cases[] = {
    {"add", GSHSCENE_COMPOSITE_SRC_OVER}, {"subtract", GSHSCENE_COMPOSITE_SRC_OUT}, {"intersect", GSHSCENE_COMPOSITE_SRC_IN}, {"exclude", GSHSCENE_COMPOSITE_XOR}};
  for (const auto & c : cases) {
    Masked m;
    Lower(std::string("mask: linear-gradient(black, white) ") + c.keyword + ", linear-gradient(white, black)", m);
    ASSERT_TRUE(m.has_mask) << c.keyword;
    EXPECT_EQ(m.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA) << c.keyword;
    EXPECT_EQ(Fills(m), (size_t) 2) << c.keyword;
    bool found = false;
    for (const GSHSCENE_Item & item : m.items) {
      found = found || (item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.composite == c.composite);
    }
    EXPECT_TRUE(found) << c.keyword << ": the upper layer is a group that composites it over the lower";
  }
  // the lower layer is first in the picture: the last layer of the list is the bottom
  Masked order;
  Lower("mask: linear-gradient(to right, black, white), linear-gradient(to bottom, black, white)", order);
  ASSERT_TRUE(order.has_mask);
  std::vector<GSHSCENE_Item> fills = FillsOf(order);
  ASSERT_EQ(fills.size(), (size_t) 2);
  GSHSCENE_Gradient first, second;
  ASSERT_TRUE(GradientOf(fills[0], first) && GradientOf(fills[1], second));
  EXPECT_NEAR(first.p1[1] - first.p0[1], 20.0, 1e-6) << "the second of the list, `to bottom`, is drawn first";
  EXPECT_NEAR(second.p1[0] - second.p0[0], 40.0, 1e-6);
}

TEST(LowerMaskLayers, AListOfMaskElementsIsComposited) {
  Masked m;
  Lower("mask: url(#mask1), url(#mask1) intersect", m);
  ASSERT_TRUE(m.has_mask) << "two element masks make a layered mask";
  EXPECT_EQ(m.l.Refusals(), (size_t) 0);
  EXPECT_EQ(m.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA) << "each layer is made into the alpha of its luminance first";
  bool found = false;
  for (const GSHSCENE_Item & item : m.items) {
    found = found || (item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.composite == GSHSCENE_COMPOSITE_SRC_IN);
    found = found || (item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.soft_mask);
  }
  EXPECT_TRUE(found);
}

TEST(LowerMaskLayers, ALayerThatNamesNothingIsSkippedAndNoneIsNoMask) {
  Masked none, one;
  Lower("mask: none", none);
  EXPECT_FALSE(none.has_mask);
  EXPECT_EQ(none.l.Items(), (size_t) 1) << "the element is drawn unmasked";
  Lower("mask: none, linear-gradient(black, white)", one);
  ASSERT_TRUE(one.has_mask);
  EXPECT_EQ(Fills(one), (size_t) 1);
}

TEST(LowerMaskLayers, AnImageIsTiledAtItsSizeOrTheOneGiven) {
  Masked m;
  Lower("mask: url(red-blue-2x1.png) 0 0 / 40px 20px no-repeat", m);
  ASSERT_TRUE(m.has_mask);
  EXPECT_EQ(m.l.Refusals(), (size_t) 0);
  EXPECT_EQ(m.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA);
  ASSERT_EQ(Fills(m), (size_t) 1);
  EXPECT_EQ(FillsOf(m)[0].paint.kind, GSHSCENE_PAINT_IMAGE);
  GSHGEOM_Bounds box = BoundsOf(FillsOf(m)[0]);
  EXPECT_EQ(box.max.x - box.min.x, 40.0);
  // at its own size the 2 by 1 image repeats across the 40 by 20 area: 20 across and 20 down
  Masked own;
  Lower("mask: url(red-blue-2x1.png)", own);
  ASSERT_TRUE(own.has_mask);
  EXPECT_EQ(Fills(own), (size_t) 400);
  // one that is not there covers nothing: the element is not seen
  Masked missing;
  Lower("mask: url(no-such-image.png)", missing);
  EXPECT_FALSE(missing.has_mask);
  EXPECT_EQ(missing.l.Items(), (size_t) 0) << "a mask that covers nothing hides the element";
}

TEST(LowerMaskLayers, WhatThisDoesNotReadIsRefusedNotHalfDrawn) {
  // more tiles than the lowering draws
  Masked many;
  Lower("mask: linear-gradient(black, white) 0 0 / 1px 1px repeat", many, "", "");
  EXPECT_EQ(many.l.Refusal(GVEC_LOWER_LIMIT), nullptr) << "800 tiles are drawn";
  Masked huge;
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  LowerSvg(Svg("width=\"1000\" height=\"1000\"", "<rect width=\"1000\" height=\"1000\" style=\"mask: linear-gradient(black, white) 0 0 / 1px 1px repeat\"/>"), huge.l, 0, 0, &options);
  ASSERT_NE(huge.l.Refusal(GVEC_LOWER_LIMIT), nullptr) << "a million tiles are not";
  EXPECT_NE(std::string(huge.l.Refusal(GVEC_LOWER_LIMIT)->message).find("repeated"), std::string::npos) << "the refusal is the one of the tiles, not some other limit";
  // seventeen layers are one more than are kept; sixteen are kept
  std::string sixteen = "linear-gradient(black, white)", seventeen;
  for (int i = 1; i < 16; i++) {
    sixteen += ", linear-gradient(black, white)";
  }
  seventeen = sixteen + ", linear-gradient(black, white)";
  Masked kept, over;
  Lower("mask-image: " + sixteen, kept);
  EXPECT_EQ(kept.l.Refusal(GVEC_LOWER_LIMIT), nullptr);
  Lower("mask-image: " + seventeen, over);
  EXPECT_NE(over.l.Refusal(GVEC_LOWER_LIMIT), nullptr) << "more layers than the lowering keeps are refused, not dropped";
  // a mask element that names an element nothing supplies for a mask of its own is refused
  Masked unresolved;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<mask id=\"m\" style=\"mask: url(elsewhere.svg#x)\"><rect width=\"9\" height=\"9\" fill=\"white\"/></mask><rect width=\"9\" height=\"9\" mask=\"url(#m)\"/>"),
      unresolved.l);
  EXPECT_NE(unresolved.l.Refusal(GVEC_LOWER_MASK), nullptr);
}

TEST(LowerMaskLayers, AMaskElementCanBeMaskedByLayersOfItsOwn) {
  Masked m;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<mask id=\"m\" style=\"mask: linear-gradient(black, white)\"><rect width=\"9\" height=\"9\" fill=\"white\"/></mask><rect width=\"9\" height=\"9\" mask=\"url(#m)\"/>"),
      m.l);
  EXPECT_EQ(m.l.Refusals(), (size_t) 0);
  ASSERT_GE(m.l.Items(), (size_t) 1);
  ASSERT_EQ(m.l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  ASSERT_NE(m.l.Item(0).group.soft_mask, nullptr);
  GSHSCENE_Mask outer;
  ASSERT_EQ(gshscene_resource_mask(m.l.Item(0).group.soft_mask, &outer), GSHSCENE_OK);
  // the picture of the outer mask holds the content (the rect) under a group that the mask element's own mask masks
  const GSHSCENE_List * list = gshscene_resource_picture(outer.picture);
  ASSERT_NE(list, nullptr);
  size_t masked_groups = 0;
  for (size_t i = 0; i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * item = nullptr;
    ASSERT_EQ(gshscene_list_item(list, i, &item), GSHSCENE_OK);
    masked_groups += item->kind == GSHSCENE_ITEM_GROUP_PUSH && item->group.soft_mask != nullptr;
  }
  EXPECT_EQ(masked_groups, (size_t) 1) << "the mask element's content is masked by its own gradient";
}

TEST(LowerMaskLayers, AnSvgDocumentIsAMaskImageToo) {
  Masked m;
  Lower("mask: url(document.svg) 0 0 / 40px 20px no-repeat", m);
  ASSERT_TRUE(m.has_mask);
  EXPECT_EQ(m.l.Refusals(), (size_t) 0);
  bool instance = false;
  for (const GSHSCENE_Item & item : m.items) {
    instance = instance || item.kind == GSHSCENE_ITEM_INSTANCE || item.kind == GSHSCENE_ITEM_FILL;
  }
  EXPECT_TRUE(instance) << "the document is drawn in the tile";
}

TEST(LowerMaskLayers, AMaskElementOfAnotherDocumentInAListIsRefused) {
  Masked m;
  Lower("mask: url(other.svg#m), linear-gradient(black, white)", m);
  EXPECT_NE(m.l.Refusal(GVEC_LOWER_MASK), nullptr);
  EXPECT_EQ(m.l.Items(), (size_t) 0);
}

TEST(LowerMaskLayers, AnElementMaskThatNamesNothingIsNotALayer) {
  Masked m;
  Lower("mask: url(#nothing), linear-gradient(black, white)", m);
  ASSERT_TRUE(m.has_mask);
  EXPECT_EQ(Fills(m), (size_t) 1);
}

TEST(LowerMaskLayers, TheListsOfTheLonghandsAreCycledToTheLayers) {
  Masked m;
  // three layers, one composite and two repeats given: the composites and repeats are used again from the first
  Lower("mask-image: linear-gradient(black, white), linear-gradient(black, white), linear-gradient(black, white); mask-size: 20px 20px; mask-repeat: no-repeat, repeat-x", m);
  ASSERT_TRUE(m.has_mask);
  // layer 1 no-repeat (1 tile), layer 2 repeat-x (2 tiles across 40), layer 3 no-repeat (1 tile)
  EXPECT_EQ(Fills(m), (size_t) 4);
}

TEST(LowerMaskLayers, ContainAndCoverFitAnImageByItsRatio) {
  Masked contain, cover, auto_height, auto_width;
  // the image is 3 by 2, the area 40 by 20: contain scales by min(40 / 3, 20 / 2) = 10, cover by 40 / 3
  Lower("mask: url(rgb-3x2.png) 0 0 / contain no-repeat", contain);
  Lower("mask: url(rgb-3x2.png) 0 0 / cover no-repeat", cover);
  Lower("mask: url(rgb-3x2.png) 0 0 / 15px auto no-repeat", auto_height);
  Lower("mask: url(rgb-3x2.png) 0 0 / auto 8px no-repeat", auto_width);
  ASSERT_TRUE(contain.has_mask && cover.has_mask && auto_height.has_mask && auto_width.has_mask);
  GSHGEOM_Bounds box = BoundsOf(FillsOf(contain)[0]);
  EXPECT_NEAR(box.max.x - box.min.x, 30.0, 1e-9);
  EXPECT_NEAR(box.max.y - box.min.y, 20.0, 1e-9);
  box = BoundsOf(FillsOf(cover)[0]);
  EXPECT_NEAR(box.max.x - box.min.x, 40.0, 1e-9);
  EXPECT_NEAR(box.max.y - box.min.y, 40.0 * 2.0 / 3.0, 1e-9);
  box = BoundsOf(FillsOf(auto_height)[0]);
  EXPECT_NEAR(box.max.y - box.min.y, 10.0, 1e-9) << "the other side keeps the ratio";
  box = BoundsOf(FillsOf(auto_width)[0]);
  EXPECT_NEAR(box.max.x - box.min.x, 12.0, 1e-9);
}

TEST(LowerMaskLayers, ALuminanceLayerBesideAnotherIsMadeIntoItsAlpha) {
  Masked m;
  Lower("mask: linear-gradient(black, white) luminance, linear-gradient(black, white)", m);
  ASSERT_TRUE(m.has_mask);
  EXPECT_EQ(m.mask.mode, (uint32_t) GSHSCENE_MASK_ALPHA);
  bool conversion = false;
  for (const GSHSCENE_Item & item : m.items) {
    conversion = conversion || (item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.soft_mask);
  }
  EXPECT_TRUE(conversion) << "the luminance layer is white under a luminance mask of its own";
  Masked alpha;
  Lower("mask: linear-gradient(black, white), linear-gradient(black, white)", alpha);
  ASSERT_TRUE(alpha.has_mask);
  for (const GSHSCENE_Item & item : alpha.items) {
    EXPECT_FALSE(item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.soft_mask) << "two alpha layers need no conversion";
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
