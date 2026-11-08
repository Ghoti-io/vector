/**
 * @file
 *
 * Gradients (H1 to H8): `linearGradient` and `radialGradient` as the waist's gradient resource: the geometry in the units the
 * document gives, the map from the element's bounding box and `gradientTransform`, the spread, the stops with their clamping and
 * order, what an `href` chain gives, and what a gradient that cannot be drawn amounts to.
 */

#include "../lower_helpers.h"

#include <vector>

using namespace gvec_test;

namespace {

struct Gradient {
  GSHSCENE_Gradient g;
  std::vector<GSHSCENE_Gradient_Stop> stops;
};

const char * kStops = "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/>";

/** The rect at (20, 20) of 100 by 40, filled with `url(#g)` over the gradient `defs` (which names itself `g`). */
bool Lower(const std::string & defs, Lowered & l, const std::string & shape_attributes = "", const std::string & root = "") {
  LowerSvg(Svg("width=\"200\" height=\"200\" " + root,
               "<defs>" + defs + "</defs><rect x=\"20\" y=\"20\" width=\"100\" height=\"40\" fill=\"url(#g)\" " + shape_attributes + "/>"),
      l);
  return l.Items() == 1;
}

bool Read(const Lowered & l, Gradient & out) {
  if (l.Items() != 1 || l.Item(0).paint.kind != GSHSCENE_PAINT_GRADIENT) {
    return false;
  }
  if (gshscene_resource_gradient(l.Item(0).paint.resource, &out.g) != GSHSCENE_OK) {
    return false;
  }
  out.stops.assign(out.g.stops, out.g.stops + out.g.stop_count);
  return true;
}

}  // namespace

TEST(LowerGradient, ALinearGradientIsPlacedInTheBoundingBoxByDefault) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\">") + kStops + "</linearGradient>", l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.g.kind, (uint32_t) GSHSCENE_GRADIENT_LINEAR);
  EXPECT_EQ(g.g.p0[0], 0.0);
  EXPECT_EQ(g.g.p0[1], 0.0);
  EXPECT_EQ(g.g.p1[0], 1.0);
  EXPECT_EQ(g.g.p1[1], 0.0);
  EXPECT_EQ(g.g.map.m[0][0], 100.0);  // the box's width and height, at its corner
  EXPECT_EQ(g.g.map.m[1][1], 40.0);
  EXPECT_EQ(g.g.map.t[0], 20.0);
  EXPECT_EQ(g.g.map.t[1], 20.0);
  EXPECT_EQ(g.g.extend, (uint32_t) GSHSCENE_EXTEND_PAD);
  EXPECT_EQ(g.stops.size(), (size_t) 2);
}

TEST(LowerGradient, UserSpaceUnitsAreTheCoordinatesAndPercentagesAreOfTheViewport) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x1=\"10\" y1=\"5\" x2=\"50%\">") + kStops +
                        "</linearGradient>",
      l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.g.p0[0], 10.0);
  EXPECT_EQ(g.g.p0[1], 5.0);
  EXPECT_EQ(g.g.p1[0], 100.0);  // half of the 200 wide viewport
  EXPECT_EQ(g.g.p1[1], 0.0);    // the default y2 is 0%
  EXPECT_EQ(g.g.map.m[0][0], 1.0);
  EXPECT_EQ(g.g.map.t[0], 0.0);
}

TEST(LowerGradient, TheGradientTransformIsAppliedBeforeTheBoundingBox) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\" gradientTransform=\"translate(0.5 0)\">") + kStops + "</linearGradient>", l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.g.map.m[0][0], 100.0);
  EXPECT_EQ(g.g.map.t[0], 70.0) << "20 + 100 * 0.5: the transform is in the box's units";
  EXPECT_EQ(g.g.map.t[1], 20.0);
}

TEST(LowerGradient, TheSpreadMethodIsTheExtendMode) {
  const struct {
    const char * word;
    uint32_t extend;
  } cases[] = {{"pad", GSHSCENE_EXTEND_PAD}, {"reflect", GSHSCENE_EXTEND_REFLECT}, {"repeat", GSHSCENE_EXTEND_REPEAT}};
  for (const auto & c : cases) {
    Lowered l;
    ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\" spreadMethod=\"") + c.word + "\">" + kStops + "</linearGradient>", l));
    Gradient g;
    ASSERT_TRUE(Read(l, g));
    EXPECT_EQ(g.g.extend, c.extend) << c.word;
  }
}

TEST(LowerGradient, StopOffsetsAreClampedAndNeverGoBackwards) {
  Lowered l;
  ASSERT_TRUE(Lower("<linearGradient id=\"g\"><stop offset=\"-0.5\"/><stop offset=\"80%\"/><stop offset=\"0.2\"/><stop offset=\"2\"/></linearGradient>", l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  ASSERT_EQ(g.stops.size(), (size_t) 4);
  EXPECT_EQ(g.stops[0].offset, 0.0);
  EXPECT_EQ(g.stops[1].offset, 0.8);
  EXPECT_EQ(g.stops[2].offset, 0.8) << "a stop never comes before the one before it (SVG 2, 13.2.3)";
  EXPECT_EQ(g.stops[3].offset, 1.0);
}

TEST(LowerGradient, StopColorsAreTheColorTimesTheStopOpacity) {
  Lowered l;
  ASSERT_TRUE(Lower("<linearGradient id=\"g\" color=\"green\"><stop offset=\"0\" stop-color=\"#336699\" stop-opacity=\"0.5\"/>"
                    "<stop offset=\"1\" stop-color=\"currentColor\"/></linearGradient>",
      l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.stops[0].color.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(g.stops[0].color.rgba[0], 0x33 / 255.0);
  EXPECT_EQ(g.stops[0].color.rgba[3], 0.5);
  EXPECT_EQ(g.stops[1].color.kind, GSHSCENE_PAINT_SOLID) << "currentColor is the color of the gradient, which is green";
  EXPECT_EQ(g.stops[1].color.rgba[1], 128 / 255.0);
}

TEST(LowerGradient, CurrentColorWithNoColorIsTheForeground) {
  Lowered l;
  ASSERT_TRUE(Lower("<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"currentColor\"/><stop offset=\"1\"/></linearGradient>", l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.stops[0].color.kind, GSHSCENE_PAINT_FOREGROUND);
}

TEST(LowerGradient, NoStopsIsNoPaintAndOneStopIsASolidFill) {
  Lowered none;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               "<defs><linearGradient id=\"g\"/></defs><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"),
      none);
  EXPECT_EQ(none.Items(), (size_t) 0);
  EXPECT_EQ(none.Refusals(), (size_t) 0);
  Lowered one;
  ASSERT_TRUE(Lower("<linearGradient id=\"g\"><stop offset=\"0.3\" stop-color=\"#336699\" stop-opacity=\"0.5\"/></linearGradient>", one));
  EXPECT_EQ(one.Item(0).paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(one.Item(0).paint.rgba[0], 0x33 / 255.0);
  EXPECT_EQ(one.Item(0).paint.rgba[3], 0.5);
}

TEST(LowerGradient, TheOpacityOfTheFillMultipliesTheGradient) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\">") + kStops + "</linearGradient>", l, "fill-opacity=\"0.25\""));
  EXPECT_EQ(l.Item(0).paint.kind, GSHSCENE_PAINT_GRADIENT);
  EXPECT_EQ(l.Item(0).paint.rgba[3], 0.25);
  Lowered solid;  // a gradient that is a solid color is multiplied the same way, with the stop's own opacity
  ASSERT_TRUE(Lower("<linearGradient id=\"g\"><stop offset=\"0\" stop-opacity=\"0.5\"/></linearGradient>", solid, "fill-opacity=\"0.5\""));
  EXPECT_EQ(solid.Item(0).paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(solid.Item(0).paint.rgba[3], 0.25);
}

TEST(LowerGradient, AReferenceChainGivesAttributesAndStopsFromTheFirstThatHasThem) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\" href=\"#b\" x2=\"0.5\"/>") +
                        "<linearGradient id=\"b\" href=\"#c\" x1=\"0.25\" x2=\"0.9\" spreadMethod=\"repeat\"/>"
                        "<linearGradient id=\"c\" y2=\"1\">" +
                        kStops + "</linearGradient>",
      l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.g.p0[0], 0.25) << "x1 from b";
  EXPECT_EQ(g.g.p1[0], 0.5) << "x2 from g itself, which wins over b's";
  EXPECT_EQ(g.g.p1[1], 1.0) << "y2 from c";
  EXPECT_EQ(g.g.extend, (uint32_t) GSHSCENE_EXTEND_REPEAT);
  EXPECT_EQ(g.stops.size(), (size_t) 2) << "the stops of c";
}

TEST(LowerGradient, ARadialGradientTakesOnlyTheStopsAndSharedAttributesOfALinearOne) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<radialGradient id=\"g\" href=\"#c\"/><linearGradient id=\"c\" x2=\"0.1\" spreadMethod=\"reflect\">") + kStops +
                        "</linearGradient>",
      l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.g.kind, (uint32_t) GSHSCENE_GRADIENT_RADIAL);
  EXPECT_EQ(g.g.c1[0], 0.5);
  EXPECT_EQ(g.g.r1, 0.5);
  EXPECT_EQ(g.g.extend, (uint32_t) GSHSCENE_EXTEND_REFLECT);
  EXPECT_EQ(g.stops.size(), (size_t) 2);
}

TEST(LowerGradient, AChainThatComesBackToItselfUsesWhatItHasGivenBeforeThat) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\" href=\"#a\"/><linearGradient id=\"a\" href=\"#b\">") + kStops +
                        "</linearGradient><linearGradient id=\"b\" href=\"#g\"/>",
      l));
  Gradient g;
  ASSERT_TRUE(Read(l, g));
  EXPECT_EQ(g.stops.size(), (size_t) 2);
}

TEST(LowerGradient, ARadialGradientHasTwoCirclesWhoseFocalPointDefaultsToTheCenter) {
  Lowered plain;
  ASSERT_TRUE(Lower(std::string("<radialGradient id=\"g\">") + kStops + "</radialGradient>", plain));
  Gradient g;
  ASSERT_TRUE(Read(plain, g));
  EXPECT_EQ(g.g.c0[0], 0.5);
  EXPECT_EQ(g.g.c0[1], 0.5);
  EXPECT_EQ(g.g.r0, 0.0);
  EXPECT_EQ(g.g.c1[0], 0.5);
  EXPECT_EQ(g.g.r1, 0.5);
  Lowered focal;
  ASSERT_TRUE(Lower(std::string("<radialGradient id=\"g\" cx=\"0.6\" fx=\"0.2\" fr=\"0.1\" r=\"0.4\">") + kStops + "</radialGradient>", focal));
  Gradient f;
  ASSERT_TRUE(Read(focal, f));
  EXPECT_EQ(f.g.c0[0], 0.2);
  EXPECT_EQ(f.g.c0[1], 0.5) << "fy follows cy";
  EXPECT_EQ(f.g.r0, 0.1);
  EXPECT_EQ(f.g.c1[0], 0.6);
  EXPECT_EQ(f.g.r1, 0.4);
}

TEST(LowerGradient, AZeroRadiusIsTheLastStopAndANegativeOneIsNoPaint) {
  Lowered zero;
  ASSERT_TRUE(Lower(std::string("<radialGradient id=\"g\" r=\"0\">") + kStops + "</radialGradient>", zero));
  EXPECT_EQ(zero.Item(0).paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(zero.Item(0).paint.rgba[2], 1.0) << "the last stop is blue";
  Lowered negative;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("<defs><radialGradient id=\"g\" r=\"-1\">") + kStops +
                   "</radialGradient></defs><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"),
      negative);
  EXPECT_EQ(negative.Items(), (size_t) 0);
  EXPECT_EQ(negative.Refusals(), (size_t) 0);
}

TEST(LowerGradient, AGradientOfNoExtentIsTheLastStop) {
  Lowered l;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\" x1=\"0.3\" x2=\"0.3\">") + kStops + "</linearGradient>", l));
  EXPECT_EQ(l.Item(0).paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(l.Item(0).paint.rgba[2], 1.0);
}

TEST(LowerGradient, ABoundingBoxOfNoAreaAndATransformWithNoInverseDisableThePaint) {
  Lowered line;  // a horizontal line has no height: nothing to place an objectBoundingBox gradient in
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("<defs><linearGradient id=\"g\">") + kStops +
                   "</linearGradient></defs><line x1=\"0\" y1=\"5\" x2=\"20\" y2=\"5\" stroke=\"url(#g)\"/>"),
      line);
  EXPECT_EQ(line.Items(), (size_t) 0);
  EXPECT_EQ(line.Refusals(), (size_t) 0);
  Lowered user;  // user-space units do not need a box
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x2=\"20\">") + kStops +
                   "</linearGradient></defs><line x1=\"0\" y1=\"5\" x2=\"20\" y2=\"5\" stroke=\"url(#g)\"/>"),
      user);
  ASSERT_EQ(user.Items(), (size_t) 1);
  EXPECT_EQ(user.Item(0).paint.kind, GSHSCENE_PAINT_GRADIENT);
  Lowered singular;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("<defs><linearGradient id=\"g\" gradientTransform=\"scale(0)\">") + kStops +
                   "</linearGradient></defs><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"),
      singular);
  EXPECT_EQ(singular.Items(), (size_t) 0);
  EXPECT_EQ(singular.Refusals(), (size_t) 0);
}

TEST(LowerGradient, ABoundingBoxOfNoAreaTakesTheFallbackPaintWhenThereIsOne) {
  const std::string defs = std::string("<defs><linearGradient id=\"g\">") + kStops + "</linearGradient></defs>";
  Lowered colored;
  LowerSvg(Svg("width=\"200\" height=\"200\"", defs + "<line x1=\"0\" y1=\"5\" x2=\"20\" y2=\"5\" stroke=\"url(#g) lime\"/>"), colored);
  ASSERT_EQ(colored.Items(), (size_t) 1);
  EXPECT_EQ(colored.Item(0).paint.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(colored.Item(0).paint.rgba[1], 1.0) << "lime, not the gradient's red or blue";
  EXPECT_EQ(colored.Item(0).paint.rgba[0], 0.0);
  Lowered none;  // a fallback of none is still none
  LowerSvg(Svg("width=\"200\" height=\"200\"", defs + "<line x1=\"0\" y1=\"5\" x2=\"20\" y2=\"5\" stroke=\"url(#g) none\"/>"), none);
  EXPECT_EQ(none.Items(), (size_t) 0);
  Lowered box;  // a box that has an area takes the gradient, fallback or not
  LowerSvg(Svg("width=\"200\" height=\"200\"", defs + "<rect width=\"10\" height=\"10\" fill=\"url(#g) lime\"/>"), box);
  ASSERT_EQ(box.Items(), (size_t) 1);
  EXPECT_EQ(box.Item(0).paint.kind, GSHSCENE_PAINT_GRADIENT);
}

TEST(LowerGradient, LinearRgbInterpolationNamesLinearLightAsTheInterpolationSpace) {
  Lowered srgb, linear;
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\">") + kStops + "</linearGradient>", srgb));
  ASSERT_TRUE(Lower(std::string("<linearGradient id=\"g\" color-interpolation=\"linearRGB\">") + kStops + "</linearGradient>", linear));
  Gradient a, b;
  ASSERT_TRUE(Read(srgb, a));
  ASSERT_TRUE(Read(linear, b));
  EXPECT_EQ(a.g.space.transfer, GCOL_TRANSFER_UNKNOWN);  // the target's own space
  EXPECT_EQ(b.g.space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(b.g.space.transfer, GCOL_TRANSFER_LINEAR);
  EXPECT_EQ(b.g.premultiplied, (uint32_t) 0);
  // The stops are the document's colors, in sRGB, in both; only the ramp between them differs.
  ASSERT_EQ(b.stops.size(), (size_t) 2);
  EXPECT_EQ(b.stops[0].color.space.transfer, a.stops[0].color.space.transfer);
  EXPECT_EQ(b.stops[1].color.rgba[2], a.stops[1].color.rgba[2]);
  EXPECT_NE(srgb.Item(0).paint.resource, linear.Item(0).paint.resource);
}

TEST(LowerGradient, AGradientWhoseStopColorCannotBeCarriedRefusesTheElement) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               "<defs><linearGradient id=\"g\"><stop stop-color=\"lab(50% 40 59)\"/></linearGradient></defs><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_NE(l.Refusal(GVEC_LOWER_GRADIENT), nullptr);
}

TEST(LowerGradient, TheStopLimitIsAssertedAtTheBoundAndOnePast) {
  for (size_t extra = 0; extra < 2; extra++) {
    GVEC_Limits limits;
    gvec_limits_default(&limits);
    limits.max_gradient_stops = 4;
    std::string stops;
    for (size_t i = 0; i < 4 + extra; i++) {
      stops += "<stop offset=\"" + std::to_string(i) + "\" stop-color=\"red\"/>";
    }
    Lowered l;
    LoadSvg(Svg("width=\"200\" height=\"200\"",
                "<defs><linearGradient id=\"g\">" + stops + "</linearGradient></defs><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"),
        l.loaded, 0, 0, &limits);
    ASSERT_EQ(l.loaded.result, GVEC_OK);
    LowerBoth(l.loaded, l);
    if (extra == 0) {
      EXPECT_EQ(l.Items(), (size_t) 1);
      EXPECT_EQ(l.Refusals(), (size_t) 0);
    }
    else {
      EXPECT_EQ(l.Items(), (size_t) 0);
      EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
    }
  }
}

TEST(LowerGradient, TheSameGradientIsOneResourceHoweverOftenItPaints) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"200\"",
               std::string("<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\">") + kStops +
                   "</linearGradient></defs><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/><rect x=\"20\" width=\"10\" height=\"10\" fill=\"url(#g)\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 2);
  EXPECT_EQ(l.Item(0).paint.resource, l.Item(1).paint.resource);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
