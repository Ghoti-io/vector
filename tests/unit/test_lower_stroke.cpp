/**
 * @file
 *
 * Strokes (K1 to K6): the `stroke-*` properties become the fields of the waist's stroke style, the stroke is an item beside the
 * fill in the order `paint-order` gives, and nothing is outlined, dashed or joined here.
 */

#include "../lower_helpers.h"

#include <ghoti.io/shape-scene/stroke.h>

using namespace gvec_test;

namespace {

/** A document of one open path with `attributes`, lowered both ways; the stroke's style is read into `style`. */
bool OneStroke(const std::string & attributes, Lowered & l, GSHSCENE_Stroke_Style & style, const std::string & open = "",
    const std::string & close = "") {
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               open + "<path d=\"M2 2L20 2L20 20\" fill=\"none\" stroke=\"#336699\" " + attributes + "/>" + close),
      l);
  if (l.Items() != 1 || l.Item(0).kind != GSHSCENE_ITEM_STROKE || !l.Item(0).style) {
    return false;
  }
  gshscene_stroke_style_init(&style);
  return gshscene_resource_stroke_style(l.Item(0).style, &style) == GSHSCENE_OK;
}

}  // namespace

TEST(LowerStroke, TheDefaultsAreSvgs) {
  Lowered l;
  GSHSCENE_Stroke_Style s;
  ASSERT_TRUE(OneStroke("", l, s));
  EXPECT_EQ(s.width, 1.0);
  EXPECT_EQ(s.width_space, (uint32_t) GSHSCENE_STROKE_WIDTH_LOCAL);
  EXPECT_EQ(s.join, (uint32_t) GSHSCENE_STROKE_JOIN_MITER);
  EXPECT_EQ(s.miter_limit, 4.0);
  EXPECT_EQ(s.cap, (uint32_t) GSHSCENE_STROKE_CAP_BUTT);
  EXPECT_EQ(s.dash_count, (size_t) 0);
  EXPECT_EQ(s.alignment, (uint32_t) GSHSCENE_STROKE_ALIGN_CENTER);
  const GSHSCENE_Paint & p = l.Item(0).paint;
  EXPECT_EQ(p.kind, GSHSCENE_PAINT_SOLID);
  EXPECT_EQ(p.rgba[0], 0x33 / 255.0);
  EXPECT_EQ(p.rgba[3], 1.0);
}

TEST(LowerStroke, WidthIsTheUsedLengthAndZeroDrawsNothing) {
  Lowered l;
  GSHSCENE_Stroke_Style s;
  ASSERT_TRUE(OneStroke("stroke-width=\"7.5\"", l, s));
  EXPECT_EQ(s.width, 7.5);
  Lowered percent;
  // 10% of the normalized diagonal of a 40 by 40 viewport.
  ASSERT_TRUE(OneStroke("stroke-width=\"10%\"", percent, s));
  EXPECT_EQ(s.width, 4.0);
  Lowered none;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<path d=\"M2 2L20 2\" stroke=\"red\" stroke-width=\"0\"/>"), none);
  EXPECT_EQ(none.Count(GSHSCENE_ITEM_STROKE), (size_t) 0);
}

TEST(LowerStroke, EveryCapAndEveryJoinIsNamedToTheWaist) {
  const char * caps[] = {"butt", "round", "square"};
  for (uint32_t i = 0; i < 3; i++) {
    Lowered l;
    GSHSCENE_Stroke_Style s;
    ASSERT_TRUE(OneStroke(std::string("stroke-linecap=\"") + caps[i] + "\"", l, s)) << caps[i];
    EXPECT_EQ(s.cap, i) << caps[i];
  }
  const char * joins[] = {"miter", "round", "bevel", "miter-clip", "arcs"};
  for (uint32_t i = 0; i < 5; i++) {
    Lowered l;
    GSHSCENE_Stroke_Style s;
    ASSERT_TRUE(OneStroke(std::string("stroke-linejoin=\"") + joins[i] + "\"", l, s)) << joins[i];
    EXPECT_EQ(s.join, i) << joins[i];
  }
}

TEST(LowerStroke, TheMiterLimitIsKeptAndTheOutOfRangeOnesAreHandledPerSvg2) {
  Lowered l;
  GSHSCENE_Stroke_Style s;
  ASSERT_TRUE(OneStroke("stroke-miterlimit=\"2.5\"", l, s));
  EXPECT_EQ(s.miter_limit, 2.5);
  Lowered big;
  ASSERT_TRUE(OneStroke("stroke-miterlimit=\"1e9\"", big, s));
  EXPECT_EQ(s.miter_limit, GSHSCENE_STROKE_MITER_LIMIT_MAX);
  Lowered low;  // below one is an error: the property is its initial value
  ASSERT_TRUE(OneStroke("stroke-miterlimit=\"0.5\"", low, s));
  EXPECT_EQ(s.miter_limit, 4.0);
}

TEST(LowerStroke, DashesAreTheWaistsCanonicalFormAndTheOffsetIsKept) {
  Lowered l;
  GSHSCENE_Stroke_Style s;
  ASSERT_TRUE(OneStroke("stroke-dasharray=\"5 3\" stroke-dashoffset=\"-2\"", l, s));
  ASSERT_EQ(s.dash_count, (size_t) 2);
  EXPECT_EQ(s.dashes[0], 5.0);
  EXPECT_EQ(s.dashes[1], 3.0);
  EXPECT_EQ(s.dash_offset, -2.0);
  EXPECT_EQ(s.dash_scope, (uint32_t) GSHSCENE_STROKE_DASH_PER_SUBPATH);
  Lowered odd;  // an odd count is repeated to an even one (SVG 2)
  ASSERT_TRUE(OneStroke("stroke-dasharray=\"5,3,2\"", odd, s));
  ASSERT_EQ(s.dash_count, (size_t) 6);
  EXPECT_EQ(s.dashes[3], 5.0);
  Lowered zero;  // all zero is a solid stroke
  ASSERT_TRUE(OneStroke("stroke-dasharray=\"0 0\" stroke-dashoffset=\"4\"", zero, s));
  EXPECT_EQ(s.dash_count, (size_t) 0);
  EXPECT_EQ(s.dash_offset, 0.0);
  Lowered percent;  // a percentage is of the normalized diagonal
  ASSERT_TRUE(OneStroke("stroke-dasharray=\"10%\"", percent, s));
  ASSERT_EQ(s.dash_count, (size_t) 2);
  EXPECT_EQ(s.dashes[0], 4.0);
}

TEST(LowerStroke, PaintIsAsAFillAndTheOpacityScalesTheAlphaWithoutAGroup) {
  Lowered l;
  GSHSCENE_Stroke_Style s;
  ASSERT_TRUE(OneStroke("stroke-opacity=\".25\"", l, s));
  EXPECT_EQ(l.Item(0).paint.rgba[3], 0.25);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_GROUP_PUSH), (size_t) 0);
  Lowered current;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<path d=\"M2 2L20 2\" stroke=\"currentColor\"/>"), current);
  ASSERT_EQ(current.Count(GSHSCENE_ITEM_STROKE), (size_t) 1);
  EXPECT_EQ(current.Item(1).paint.kind, GSHSCENE_PAINT_FOREGROUND);
}

TEST(LowerStroke, AGradientOrContextStrokeRefusesTheWholeElement) {
  Lowered g;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<defs><linearGradient id=\"g\" color-interpolation=\"linearRGB\"/></defs><path d=\"M2 2L20 2\" stroke=\"url(#g)\"/>"),
      g);
  EXPECT_NE(g.Refusal(GVEC_LOWER_GRADIENT), nullptr);
  EXPECT_EQ(g.Items(), (size_t) 0);  // not even its fill
  Lowered c;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<path d=\"M2 2L20 2\" stroke=\"context-stroke\"/>"), c);
  EXPECT_NE(c.Refusal(GVEC_LOWER_CONTEXT_PAINT), nullptr);
  EXPECT_EQ(c.Items(), (size_t) 0);
}

TEST(LowerStroke, NonScalingStrokeIsADeviceWidthAndTheOtherEffectsAreRefused) {
  Lowered l;
  GSHSCENE_Stroke_Style s;
  ASSERT_TRUE(OneStroke("style=\"vector-effect: non-scaling-stroke\" stroke-width=\"3\" transform=\"scale(10)\"", l, s));
  EXPECT_EQ(s.width_space, (uint32_t) GSHSCENE_STROKE_WIDTH_DEVICE);
  EXPECT_EQ(s.width, 3.0);
  Lowered other;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<path d=\"M2 2L20 2\" stroke=\"red\" style=\"vector-effect: non-rotation\"/>"), other);
  EXPECT_NE(other.Refusal(GVEC_LOWER_VECTOR_EFFECT), nullptr);
}

TEST(LowerStroke, TheFillComesFirstUnlessPaintOrderSaysOtherwise) {
  const struct {
    const char * order;
    bool stroke_first;
  } cases[] = {{"", false}, {"normal", false}, {"fill", false}, {"stroke", true}, {"stroke fill", true}, {"fill stroke", false},
      {"markers stroke", true}, {"markers fill", false}, {"fill markers", false}};
  for (const auto & c : cases) {
    Lowered l;
    LowerSvg(Svg("width=\"40\" height=\"40\"",
                 std::string("<path d=\"M2 2L20 2L20 20Z\" fill=\"red\" stroke=\"blue\" stroke-width=\"2\" paint-order=\"") + c.order +
                     "\"/>"),
        l);
    ASSERT_EQ(l.Items(), (size_t) 2) << c.order;
    EXPECT_EQ(l.Item(0).kind, c.stroke_first ? GSHSCENE_ITEM_STROKE : GSHSCENE_ITEM_FILL) << c.order;
    EXPECT_EQ(l.Item(1).kind, c.stroke_first ? GSHSCENE_ITEM_FILL : GSHSCENE_ITEM_STROKE) << c.order;
  }
}

TEST(LowerStroke, AnElementsOpacityGroupHoldsItsFillAndItsStrokeTogether) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<path d=\"M2 2L20 2L20 20Z\" fill=\"red\" stroke=\"blue\" stroke-width=\"2\" opacity=\".5\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 4);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(2).kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(l.Item(3).kind, GSHSCENE_ITEM_GROUP_POP);
}

TEST(LowerStroke, APolylineIsStrokedOpenAndAPolygonClosed) {
  Lowered open;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<polyline points=\"2,2 20,2 20,20\" fill=\"none\" stroke=\"red\"/>"), open);
  Lowered closed;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<polygon points=\"2,2 20,2 20,20\" fill=\"none\" stroke=\"red\"/>"), closed);
  ASSERT_EQ(open.Count(GSHSCENE_ITEM_STROKE), (size_t) 1);
  ASSERT_EQ(closed.Count(GSHSCENE_ITEM_STROKE), (size_t) 1);
  EXPECT_EQ(Describe(open.Item(0).resource), "M2,2 L20,2 L20,20");
  EXPECT_EQ(Describe(closed.Item(0).resource), "M2,2 L20,2 L20,20 Z");
}

TEST(LowerStroke, ALineIsAStrokeOfTwoPointsAndHasNoFill) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<line x1=\"1\" y1=\"2\" x2=\"30\" y2=\"4\" stroke=\"red\" fill=\"blue\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(Describe(l.Item(0).resource), "M1,2 L30,4");
  Lowered units;  // absolute units and percentages of the viewport; the attributes are lengths
  LowerSvg(Svg("width=\"40\" height=\"20\"", "<line x2=\"50%\" y2=\"1in\" stroke=\"red\"/>"), units);
  ASSERT_EQ(units.Items(), (size_t) 1);
  EXPECT_EQ(Describe(units.Item(0).resource), "M0,0 L20,96");
  Lowered em;  // a unit that needs the cascade is refused, not guessed
  LowerSvg(Svg("width=\"40\" height=\"20\"", "<line x2=\"2em\" stroke=\"red\"/>"), em);
  EXPECT_NE(em.Refusal(GVEC_LOWER_STYLE), nullptr);
}

TEST(LowerStroke, ASubpathOfNoLengthIsKeptForItsCaps) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<path d=\"M10 10L10 10\" stroke=\"red\" stroke-width=\"6\" stroke-linecap=\"round\"/>"),
      l);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_STROKE), (size_t) 1);
  Lowered lone;  // a move alone draws nothing
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<path d=\"M10 10\" stroke=\"red\" stroke-linecap=\"round\"/>"), lone);
  EXPECT_EQ(lone.Items(), (size_t) 0);
}

TEST(LowerStroke, AStrokeIsPlacedByTheElementsMapAndNotScaledHere) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<path d=\"M0 0L5 0\" fill=\"none\" stroke=\"red\" stroke-width=\"2\" transform=\"scale(3)\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  GSHSCENE_Stroke_Style s;
  gshscene_stroke_style_init(&s);
  ASSERT_EQ(gshscene_resource_stroke_style(l.Item(0).style, &s), GSHSCENE_OK);
  EXPECT_EQ(s.width, 2.0);  // the width stays in the path's own space; the map carries the scale
  EXPECT_EQ(l.Item(0).map.m[0][0], 3.0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
