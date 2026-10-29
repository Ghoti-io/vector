/**
 * @file
 *
 * Markers (K7 to K10): the vertices and the direction at each come from the geometry library, and a marker's content is a picture
 * placed once for each vertex under the map of the vertex, the direction, the scale and the reference point.
 */

#include "../lower_helpers.h"

#include <cmath>

using namespace gvec_test;

namespace {

/** A square marker of `size` user units with no viewBox and the reference point at its corner, with `attributes`. */
std::string Marker(const std::string & attributes, const std::string & body = "<path d=\"M0 0H4V4H0Z\"/>", const std::string & id = "m") {
  return "<marker id=\"" + id + "\" markerWidth=\"4\" markerHeight=\"4\" markerUnits=\"userSpaceOnUse\" " + attributes + ">" + body +
         "</marker>";
}

/** The instance items of a lowering, in order. */
std::vector<GSHSCENE_Item> Instances(const Lowered & l) {
  std::vector<GSHSCENE_Item> out;
  for (size_t i = 0; i < l.Items(); i++) {
    if (l.Item(i).kind == GSHSCENE_ITEM_INSTANCE) {
      out.push_back(l.Item(i));
    }
  }
  return out;
}

/** The items of the picture an instance draws. */
std::vector<GSHSCENE_Item> Picture(const GSHSCENE_Item & instance) {
  std::vector<GSHSCENE_Item> out;
  const GSHSCENE_List * picture = gshscene_resource_picture(instance.resource);
  for (size_t i = 0; picture && i < gshscene_list_count(picture); i++) {
    const GSHSCENE_Item * item = nullptr;
    EXPECT_EQ(gshscene_list_item(picture, i, &item), GSHSCENE_OK);
    if (item) {
      out.push_back(*item);
    }
  }
  return out;
}

/** The document: the defs, then one shape that draws only its markers. */
std::string OnlyMarkers(const std::string & defs, const std::string & shape) {
  return Svg("width=\"60\" height=\"60\"", "<defs>" + defs + "</defs>" + shape);
}

const char * kPath =
    "<path d=\"M10 10L30 10L30 30\" fill=\"none\" stroke=\"none\" marker-start=\"url(#m)\" marker-mid=\"url(#m)\" marker-end=\"url(#m)\"/>";

}  // namespace

TEST(LowerMarker, AnOpenPathHasAStartAMidAndAnEndAtItsVertices) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), kPath), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 3);
  EXPECT_EQ(in[0].map.t[0], 10.0);
  EXPECT_EQ(in[0].map.t[1], 10.0);
  EXPECT_EQ(in[1].map.t[0], 30.0);
  EXPECT_EQ(in[1].map.t[1], 10.0);
  EXPECT_EQ(in[2].map.t[0], 30.0);
  EXPECT_EQ(in[2].map.t[1], 30.0);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 0);  // the shape itself draws nothing
}

TEST(LowerMarker, TheStartMidAndEndMarkersAreTheOnesTheirPropertiesName) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\"") + Marker("orient=\"0\"", "<path d=\"M0 0H3V3H0Z\"/>", "n"),
               "<path d=\"M10 10L30 10L30 30\" fill=\"none\" marker-start=\"url(#m)\" marker-end=\"url(#n)\"/>"),
      l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 2);  // no mid marker
  EXPECT_EQ(in[0].map.t[0], 10.0);
  EXPECT_EQ(in[1].map.t[0], 30.0);
  EXPECT_NE(in[0].resource, in[1].resource);  // two markers, two pictures
}

TEST(LowerMarker, TheMarkerShorthandNamesAllThree) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), "<path d=\"M10 10L30 10L30 30\" fill=\"none\" style=\"marker: url(#m)\"/>"), l);
  EXPECT_EQ(Instances(l).size(), (size_t) 3);
}

TEST(LowerMarker, EverySubpathVertexIsAMidExceptTheFirstAndLastOfThePath) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""),
               "<path d=\"M10 10L20 10M30 30L40 30\" fill=\"none\" marker-start=\"url(#m)\" marker-mid=\"url(#m)\" marker-end=\"url(#m)\"/>"),
      l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 4);  // the start, the end of the first subpath, the start of the second, the end
  EXPECT_EQ(in[1].map.t[0], 20.0);
  EXPECT_EQ(in[2].map.t[0], 30.0);
  EXPECT_EQ(in[3].map.t[0], 40.0);
}

TEST(LowerMarker, AClosedSubpathHasAVertexForTheCloseAtItsStart) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""),
               "<path d=\"M10 10L30 10L30 30Z\" fill=\"none\" marker-start=\"url(#m)\" marker-mid=\"url(#m)\" marker-end=\"url(#m)\"/>"),
      l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 4);
  EXPECT_EQ(in[3].map.t[0], 10.0);  // the end marker is where the close ends
  EXPECT_EQ(in[3].map.t[1], 10.0);
}

TEST(LowerMarker, LinePolylineAndPolygonHaveMarkersAndRectHasNone) {
  const char * shapes[] = {"<line x1=\"10\" y1=\"10\" x2=\"30\" y2=\"10\" stroke=\"none\"",
      "<polyline points=\"10,10 30,10 30,30\" fill=\"none\"", "<polygon points=\"10,10 30,10 30,30\" fill=\"none\""};
  const size_t expected[] = {2, 3, 4};  // a polygon is closed
  for (int i = 0; i < 3; i++) {
    Lowered l;
    LowerSvg(OnlyMarkers(Marker("orient=\"0\""),
                 std::string(shapes[i]) + " marker-start=\"url(#m)\" marker-mid=\"url(#m)\" marker-end=\"url(#m)\"/>"),
        l);
    EXPECT_EQ(Instances(l).size(), expected[i]) << shapes[i];
  }
  Lowered rect;
  LowerSvg(OnlyMarkers(Marker(""), "<rect width=\"4\" height=\"4\" fill=\"none\" marker-start=\"url(#m)\"/>"), rect);
  EXPECT_EQ(Instances(rect).size(), (size_t) 0);
  EXPECT_EQ(rect.Refusals(), (size_t) 0);
}

TEST(LowerMarker, AutoTakesTheDirectionOfTheVertexFromTheGeometry) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"auto\""), kPath), l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 3);
  // The start looks along +x: no turn. The end looks along +y: a quarter turn, whose entries are exact.
  EXPECT_EQ(in[0].map.m[0][0], 1.0);
  EXPECT_EQ(in[0].map.m[1][0], 0.0);
  EXPECT_EQ(in[2].map.m[0][0], 0.0);
  EXPECT_EQ(in[2].map.m[0][1], -1.0);
  EXPECT_EQ(in[2].map.m[1][0], 1.0);
  EXPECT_EQ(in[2].map.m[1][1], 0.0);
  // The mid vertex is a corner: the bisector of +x and +y.
  EXPECT_NEAR(in[1].map.m[0][0], std::sqrt(0.5), 1e-15);
  EXPECT_NEAR(in[1].map.m[1][0], std::sqrt(0.5), 1e-15);
}

TEST(LowerMarker, ANumberIsAnAngleInDegreesAndAutoStartReverseTurnsTheStart) {
  Lowered number;
  LowerSvg(OnlyMarkers(Marker("orient=\"90\""), kPath), number);
  std::vector<GSHSCENE_Item> n = Instances(number);
  ASSERT_EQ(n.size(), (size_t) 3);
  for (const GSHSCENE_Item & item : n) {
    EXPECT_EQ(item.map.m[0][1], -1.0);
    EXPECT_EQ(item.map.m[1][0], 1.0);
  }
  Lowered reverse;
  LowerSvg(OnlyMarkers(Marker("orient=\"auto-start-reverse\""), kPath), reverse);
  std::vector<GSHSCENE_Item> r = Instances(reverse);
  ASSERT_EQ(r.size(), (size_t) 3);
  EXPECT_EQ(r[0].map.m[0][0], -1.0);  // the start is turned half way round
  EXPECT_EQ(r[2].map.m[1][0], 1.0);   // the end is as auto has it
}

TEST(LowerMarker, MarkerUnitsScaleByTheStrokeWidthByDefaultAndNotWithUserSpaceOnUse) {
  const std::string shape =
      "<path d=\"M10 10L30 10\" fill=\"none\" stroke=\"#000\" stroke-width=\"3\" marker-start=\"url(#m)\"/>";
  Lowered units;
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"4\" markerHeight=\"4\" orient=\"0\"><path d=\"M0 0H4V4H0Z\"/></marker>", shape), units);
  std::vector<GSHSCENE_Item> u = Instances(units);
  ASSERT_EQ(u.size(), (size_t) 1);
  EXPECT_EQ(u[0].map.m[0][0], 3.0);
  EXPECT_EQ(u[0].map.m[1][1], 3.0);
  Lowered user;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), shape), user);
  std::vector<GSHSCENE_Item> w = Instances(user);
  ASSERT_EQ(w.size(), (size_t) 1);
  EXPECT_EQ(w[0].map.m[0][0], 1.0);
  Lowered zero;  // a stroke of no width scales the marker to nothing, which is not drawn
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"4\" markerHeight=\"4\"><path d=\"M0 0H4V4H0Z\"/></marker>",
               "<path d=\"M10 10L30 10\" fill=\"none\" stroke=\"#000\" stroke-width=\"0\" marker-start=\"url(#m)\"/>"),
      zero);
  EXPECT_EQ(Instances(zero).size(), (size_t) 0);
}

TEST(LowerMarker, TheReferencePointMovesTheMarkerAndItsKeywordsAreOfTheViewBox) {
  // refX and refY are in the content's coordinates: the point (2, 1) of the viewBox is put on the vertex, scaled by 2.
  Lowered l;
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"20\" markerHeight=\"20\" markerUnits=\"userSpaceOnUse\" viewBox=\"0 0 10 10\" "
                       "refX=\"2\" refY=\"1\" orient=\"0\"><path d=\"M0 0H4V4H0Z\"/></marker>",
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 1);
  EXPECT_EQ(in[0].map.t[0], 10.0 - 4.0);  // the vertex, less the reference point's place in the viewport (2 * 2)
  EXPECT_EQ(in[0].map.t[1], 10.0 - 2.0);
  Lowered right;  // `right` and `bottom` are the viewBox's far edges
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"20\" markerHeight=\"20\" markerUnits=\"userSpaceOnUse\" viewBox=\"5 5 10 10\" "
                       "refX=\"right\" refY=\"bottom\" orient=\"0\"><path d=\"M0 0H4V4H0Z\"/></marker>",
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      right);
  std::vector<GSHSCENE_Item> rin = Instances(right);
  ASSERT_EQ(rin.size(), (size_t) 1);
  EXPECT_EQ(rin[0].map.t[0], 10.0 - 20.0);  // the viewBox's right edge is x = 15, which the viewport puts at 20
  EXPECT_EQ(rin[0].map.t[1], 10.0 - 20.0);
}

TEST(LowerMarker, PreserveAspectRatioMapsTheViewBoxToTheViewport) {
  const std::string body = "<path d=\"M0 0H4V4H0Z\"/>";
  Lowered none;
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"20\" markerHeight=\"20\" markerUnits=\"userSpaceOnUse\" viewBox=\"0 0 10 5\" "
                       "preserveAspectRatio=\"none\" overflow=\"visible\" orient=\"0\">" + body + "</marker>",
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      none);
  std::vector<GSHSCENE_Item> n = Instances(none);
  ASSERT_EQ(n.size(), (size_t) 1);
  std::vector<GSHSCENE_Item> c = Picture(n[0]);
  ASSERT_EQ(c.size(), (size_t) 1);
  EXPECT_EQ(c[0].map.m[0][0], 2.0);  // the box's 10 by 5 stretched to 20 by 20
  EXPECT_EQ(c[0].map.m[1][1], 4.0);
  Lowered meet;
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"20\" markerHeight=\"20\" markerUnits=\"userSpaceOnUse\" viewBox=\"0 0 10 5\" "
                       "overflow=\"visible\" orient=\"0\">" + body + "</marker>",
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      meet);
  std::vector<GSHSCENE_Item> m = Instances(meet);
  ASSERT_EQ(m.size(), (size_t) 1);
  std::vector<GSHSCENE_Item> mc = Picture(m[0]);
  ASSERT_EQ(mc.size(), (size_t) 1);
  EXPECT_EQ(mc[0].map.m[0][0], 2.0);  // meet: the smaller scale on both axes, centered
  EXPECT_EQ(mc[0].map.m[1][1], 2.0);
  EXPECT_EQ(mc[0].map.t[1], 5.0);
}

TEST(LowerMarker, TheContentIsClippedToTheViewportUnlessOverflowIsVisible) {
  Lowered clipped;
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"6\" markerHeight=\"4\" markerUnits=\"userSpaceOnUse\" orient=\"0\">"
                       "<path d=\"M0 0H40V40H0Z\"/></marker>",
               kPath),
      clipped);
  std::vector<GSHSCENE_Item> in = Instances(clipped);
  ASSERT_EQ(in.size(), (size_t) 3);
  std::vector<GSHSCENE_Item> picture = Picture(in[0]);
  ASSERT_GE(picture.size(), (size_t) 3);
  EXPECT_EQ(picture[0].kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(Describe(picture[0].resource), "M0,0 L6,0 L6,4 L0,4 Z");  // the marker's viewport, in its own coordinates
  EXPECT_EQ(picture[2].kind, GSHSCENE_ITEM_CLIP_POP);
  Lowered open;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\" overflow=\"visible\"", "<path d=\"M0 0H40V40H0Z\"/>"), kPath), open);
  std::vector<GSHSCENE_Item> o = Instances(open);
  ASSERT_EQ(o.size(), (size_t) 3);
  std::vector<GSHSCENE_Item> content = Picture(o[0]);
  ASSERT_EQ(content.size(), (size_t) 1);
  EXPECT_EQ(content[0].kind, GSHSCENE_ITEM_FILL);
}

TEST(LowerMarker, TheContentInheritsFromWhereTheMarkerIsDefinedNotFromTheShape) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"",
               "<g fill=\"#0000ff\"><defs>" + Marker("orient=\"0\" overflow=\"visible\"") +
                   "</defs></g><path d=\"M10 10L30 10\" fill=\"#ff0000\" stroke=\"none\" marker-start=\"url(#m)\"/>"),
      l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 1);
  std::vector<GSHSCENE_Item> content = Picture(in[0]);
  ASSERT_EQ(content.size(), (size_t) 1);
  EXPECT_EQ(content[0].paint.rgba[2], 1.0);  // blue
  EXPECT_EQ(content[0].paint.rgba[0], 0.0);
}

TEST(LowerMarker, ContextPaintIsTheReferencingElementsAndMakesAPicturePerPair) {
  const std::string marker = Marker("orient=\"0\" overflow=\"visible\"", "<path d=\"M0 0H4V4H0Z\" fill=\"context-fill\" stroke=\"context-stroke\"/>");
  Lowered l;
  LowerSvg(OnlyMarkers(marker,
               "<path d=\"M10 10L30 10\" fill=\"#ff0000\" stroke=\"#00ff00\" marker-start=\"url(#m)\"/>"
               "<path d=\"M10 40L30 40\" fill=\"#0000ff\" stroke=\"#00ff00\" marker-start=\"url(#m)\"/>"
               "<path d=\"M10 50L30 50\" fill=\"#ff0000\" stroke=\"#00ff00\" marker-start=\"url(#m)\"/>"),
      l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 3);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_NE(in[0].resource, in[1].resource);  // a different fill, a different picture
  EXPECT_EQ(in[0].resource, in[2].resource);  // the same pair, the same picture
  std::vector<GSHSCENE_Item> first = Picture(in[0]);
  ASSERT_GE(first.size(), (size_t) 2);
  EXPECT_EQ(first[0].kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(first[0].paint.rgba[0], 1.0);
  EXPECT_EQ(first[1].kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(first[1].paint.rgba[1], 1.0);
}

TEST(LowerMarker, ContextPaintInAUseIsTheUsesPaint) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"",
               "<defs><path id=\"p\" d=\"M0 0H4V4H0Z\" fill=\"context-fill\" stroke=\"context-stroke\"/></defs>"
               "<use href=\"#p\" fill=\"#ff0000\" stroke=\"#00ff00\"/><use href=\"#p\" fill=\"#0000ff\" stroke=\"#00ff00\"/>"),
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 2);
  EXPECT_NE(in[0].resource, in[1].resource);
  std::vector<GSHSCENE_Item> first = Picture(in[0]);
  ASSERT_GE(first.size(), (size_t) 2);
  EXPECT_EQ(first[0].paint.rgba[0], 1.0);
  EXPECT_EQ(first[1].paint.rgba[1], 1.0);
}

TEST(LowerMarker, ContextPaintOutsideAMarkerIsStillRefused) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"", "<path d=\"M10 10L30 10\" fill=\"context-fill\"/>"), l);
  EXPECT_NE(l.Refusal(GVEC_LOWER_CONTEXT_PAINT), nullptr);
}

TEST(LowerMarker, AMissingMarkerAndAnElementThatIsNoMarkerDrawNoneAndTheSecondIsSaid) {
  Lowered missing;
  LowerSvg(OnlyMarkers("", "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#nothing)\"/>"), missing);
  EXPECT_EQ(Instances(missing).size(), (size_t) 0);
  EXPECT_EQ(missing.Refusals(), (size_t) 0);
  Lowered wrong;
  LowerSvg(OnlyMarkers("<g id=\"g\"/>", "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#g)\"/>"), wrong);
  EXPECT_EQ(Instances(wrong).size(), (size_t) 0);
  EXPECT_EQ(gvec_diagnostics_count_code(&wrong.by_list, GVEC_DIAG_REFERENCE_MISSING), (size_t) 1);
}

TEST(LowerMarker, AMarkerOfNoSizeDisablesItAndIsSaid) {
  Lowered l;
  LowerSvg(OnlyMarkers("<marker id=\"m\" markerWidth=\"0\" markerHeight=\"4\"><path d=\"M0 0H4V4H0Z\"/></marker>",
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      l);
  EXPECT_EQ(Instances(l).size(), (size_t) 0);
  EXPECT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_RENDERING_DISABLED), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerMarker, AMarkerThatIsInItsOwnContentIsCutNotLoopedOn) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\"", "<path d=\"M0 0H4V4H0Z\" marker-start=\"url(#m)\"/>"),
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      l);
  EXPECT_EQ(Instances(l).size(), (size_t) 1);  // the outer one only
  Lowered two;  // through a second marker
  LowerSvg(OnlyMarkers(Marker("orient=\"0\"", "<path d=\"M0 0H4V4H0Z\" marker-start=\"url(#n)\"/>") +
                           Marker("orient=\"0\"", "<path d=\"M0 0H3V3H0Z\" marker-start=\"url(#m)\"/>", "n"),
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      two);
  EXPECT_LE(Instances(two).size(), (size_t) 2);
  // Through a `use` of a shape that has the marker: a cycle of two kinds of reference.
  Lowered through_use;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\"", "<use href=\"#p\"/>") +
                           "<path id=\"p\" d=\"M0 0L2 0\" fill=\"none\" marker-start=\"url(#m)\"/>",
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      through_use);
  // The document's graph cuts it and says so when it loads; nothing recurses.
  EXPECT_GE(gvec_diagnostics_count_code(&through_use.by_list, GVEC_DIAG_REFERENCE_CYCLE), (size_t) 1);
  EXPECT_LE(Instances(through_use).size(), (size_t) 1);
}

TEST(LowerMarker, TheShapesTransformMovesItsMarkers) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), "<path d=\"M10 10L30 10\" fill=\"none\" transform=\"translate(5 7)\" marker-start=\"url(#m)\"/>"), l);
  std::vector<GSHSCENE_Item> in = Instances(l);
  ASSERT_EQ(in.size(), (size_t) 1);
  EXPECT_EQ(in[0].map.t[0], 15.0);
  EXPECT_EQ(in[0].map.t[1], 17.0);
}

TEST(LowerMarker, MarkersInMarkerContentAreDrawnWhenNothingCloses) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\" overflow=\"visible\"", "<path d=\"M0 0L4 0\" fill=\"none\" marker-end=\"url(#n)\"/>") +
                           Marker("orient=\"0\"", "<path d=\"M0 0H1V1H0Z\"/>", "n"),
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      l);
  std::vector<GSHSCENE_Item> outer = Instances(l);
  ASSERT_EQ(outer.size(), (size_t) 1);
  std::vector<GSHSCENE_Item> content = Picture(outer[0]);
  size_t nested = 0;
  for (const GSHSCENE_Item & item : content) {
    nested += item.kind == GSHSCENE_ITEM_INSTANCE;
  }
  EXPECT_EQ(nested, (size_t) 1);
}

TEST(LowerMarker, MarkersNestedPastThePictureDepthAreRefusedWhereTheyAre) {
  GSHSCENE_Limits limits;
  gshscene_limits_default(&limits);
  limits.max_picture_depth = 1;
  Lowered l;
  l.scene_limits = &limits;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\" overflow=\"visible\"", "<path d=\"M0 0L4 0\" fill=\"none\" marker-end=\"url(#n)\"/>") +
                           Marker("orient=\"0\"", "<path d=\"M0 0H1V1H0Z\"/>", "n"),
               "<path d=\"M10 10L30 10\" fill=\"none\" marker-start=\"url(#m)\"/>"),
      l);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
  EXPECT_EQ(Instances(l).size(), (size_t) 0);  // the outer marker has no content left, so it draws nothing
}

TEST(LowerMarker, TheLimitRefusesTheWholeElementAndCountsTheDocument) {
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.max_marker_instances = 2;
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""),
               "<path d=\"M10 10L30 10L30 30\" fill=\"#ff0000\" stroke=\"#00ff00\" marker-start=\"url(#m)\" marker-mid=\"url(#m)\" "
               "marker-end=\"url(#m)\"/>"),
      l, 0, 0, &options);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
  EXPECT_EQ(l.Items(), (size_t) 0);  // not even its fill and stroke
  Lowered fits;
  options.max_marker_instances = 3;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), kPath), fits, 0, 0, &options);
  EXPECT_EQ(Instances(fits).size(), (size_t) 3);
  Lowered twice;  // the count is the document's: a second shape past the bound is refused
  options.max_marker_instances = 4;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), std::string(kPath) + kPath), twice, 0, 0, &options);
  EXPECT_EQ(Instances(twice).size(), (size_t) 3);
  EXPECT_NE(twice.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerMarker, PaintOrderPlacesTheMarkersAmongTheFillAndTheStroke) {
  const std::string shape =
      "<path d=\"M10 10L30 10L30 30\" fill=\"#ff0000\" stroke=\"#00ff00\" marker-start=\"url(#m)\" style=\"paint-order: ";
  Lowered normal;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), shape + "normal\"/>"), normal);
  ASSERT_EQ(normal.Items(), (size_t) 3);
  EXPECT_EQ(normal.Item(0).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(normal.Item(1).kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(normal.Item(2).kind, GSHSCENE_ITEM_INSTANCE);
  Lowered first;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), shape + "markers\"/>"), first);
  ASSERT_EQ(first.Items(), (size_t) 3);
  EXPECT_EQ(first.Item(0).kind, GSHSCENE_ITEM_INSTANCE);
  EXPECT_EQ(first.Item(1).kind, GSHSCENE_ITEM_FILL);
  Lowered middle;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), shape + "stroke markers fill\"/>"), middle);
  ASSERT_EQ(middle.Items(), (size_t) 3);
  EXPECT_EQ(middle.Item(0).kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(middle.Item(1).kind, GSHSCENE_ITEM_INSTANCE);
  EXPECT_EQ(middle.Item(2).kind, GSHSCENE_ITEM_FILL);
}

TEST(LowerMarker, TheElementsOpacityIsOneGroupOverItsFillStrokeAndMarkers) {
  Lowered l;
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""),
               "<path d=\"M10 10L30 10\" fill=\"#ff0000\" stroke=\"#00ff00\" opacity=\".5\" marker-start=\"url(#m)\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 5);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(3).kind, GSHSCENE_ITEM_INSTANCE);
  EXPECT_EQ(l.Item(4).kind, GSHSCENE_ITEM_GROUP_POP);
}

TEST(LowerMarker, ADegenerateAndAnArcPathAreHandledByTheirRules) {
  Lowered arc;  // an arc cannot be placed until the shape family has its exact primitive
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), "<path d=\"M10 10A5 5 0 0 1 20 10\" fill=\"none\" marker-start=\"url(#m)\"/>"), arc);
  EXPECT_NE(arc.Refusal(GVEC_LOWER_ARC), nullptr);
  EXPECT_EQ(Instances(arc).size(), (size_t) 0);
  Lowered point;  // a path of one point has no marker position to speak of
  LowerSvg(OnlyMarkers(Marker("orient=\"0\""), "<path d=\"M10 10\" fill=\"none\" marker-start=\"url(#m)\"/>"), point);
  EXPECT_EQ(Instances(point).size(), (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
