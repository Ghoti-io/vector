/**
 * @file
 *
 * The walk (P5, P6): transforms compose into the maps of the items and make no group, `opacity` makes a group only over
 * painted content, `display`, `visibility` and `switch` decide what is drawn, and what is never rendered is no refusal.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {
const char * const kSquare = "<path d=\"M0 0H4V4H0Z\"/>";
}

TEST(LowerWalk, ASolidFillIsOneItemWithItsPathAndPaint) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M2 2H10V10H2Z\" fill=\"#ff0000\"/>"), l, 20, 20);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(Describe(l.Item(0).resource), "M2,2 L10,2 L10,10 L2,10 Z");
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerWalk, TransformsComposeIntoTheItemsMapAndMakeNoGroup) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               std::string("<g transform=\"translate(10 0)\"><g transform=\"scale(2)\">") + kSquare + "</g></g>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  const GSHGEOM_Map & m = l.Item(0).map;
  EXPECT_EQ(m.m[0][0], 2.0);
  EXPECT_EQ(m.m[1][1], 2.0);
  EXPECT_EQ(m.t[0], 10.0);
  EXPECT_EQ(m.t[1], 0.0);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_GROUP_PUSH) + l.Count(GSHSCENE_ITEM_CLIP_PUSH), (size_t) 0);
}

TEST(LowerWalk, AElementsOwnTransformComesAfterItsParents) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<g transform=\"translate(10 0)\"><path d=\"M0 0H4V4H0Z\" transform=\"scale(2)\"/></g>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  // point p -> translate(scale(p)): the scale is applied first.
  EXPECT_EQ(l.Item(0).map.m[0][0], 2.0);
  EXPECT_EQ(l.Item(0).map.t[0], 10.0);
}

TEST(LowerWalk, APlainGTreeCostsNoGroupAtAnyDepth) {
  std::string body;
  for (int i = 0; i < 20; i++) {
    body += "<g transform=\"translate(1 1)\" fill=\"red\">";
  }
  body += kSquare;
  for (int i = 0; i < 20; i++) {
    body += "</g>";
  }
  Lowered l;
  LowerSvg(Svg("width=\"50\" height=\"50\"", body), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).map.t[0], 20.0);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_GROUP_PUSH), (size_t) 0);
}

TEST(LowerWalk, OpacityIsOneGroupAroundTheContent) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", std::string("<g opacity=\".5\">") + kSquare + kSquare + "</g>"), l);
  ASSERT_EQ(l.Items(), (size_t) 4);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(0).group.opacity, 0.5);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(2).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(3).kind, GSHSCENE_ITEM_GROUP_POP);
}

TEST(LowerWalk, OpacityOverNothingPaintedMakesNoGroup) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<g opacity=\".5\"></g><g opacity=\".5\"><path d=\"M0 0\"/><path d=\"M0 0H4V4H0Z\" fill=\"none\"/></g>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
}

TEST(LowerWalk, OpacityZeroDrawsNothingAndOpacityOnAShapeIsAGroup) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4H0Z\" opacity=\"0\"/><path d=\"M0 0H4V4H0Z\" opacity=\".25\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(0).group.opacity, 0.25);
}

TEST(LowerWalk, NestedOpacitiesAreNestedGroupsNotAProduct) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", std::string("<g opacity=\".5\"><g opacity=\".5\">") + kSquare + "</g></g>"), l);
  ASSERT_EQ(l.Items(), (size_t) 5);
  EXPECT_EQ(l.Item(0).group.opacity, 0.5);
  EXPECT_EQ(l.Item(1).group.opacity, 0.5);
}

TEST(LowerWalk, FillOpacityScalesTheAlphaAndIsNotAGroup) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4H0Z\" fill=\"#ff0000\" fill-opacity=\".5\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).paint.rgba[3], 0.5);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_GROUP_PUSH), (size_t) 0);
}

TEST(LowerWalk, DisplayNoneRemovesAnElementAndItsSubtree) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               std::string("<path d=\"M0 0H4V4H0Z\" display=\"none\"/><g display=\"none\">") + kSquare + "</g><g style=\"display: none\"><g style=\"display: inline\">" +
                   kSquare + "</g></g>" + kSquare),
      l);
  EXPECT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerWalk, DisplayNoneOfAReferentRemovesItFromTheUseToo) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<defs><path id=\"p\" d=\"M0 0H4V4H0Z\" display=\"none\"/></defs><use href=\"#p\"/>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
}

TEST(LowerWalk, VisibilityHiddenRemovesOnlyTheElementsOwnPainting) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<g visibility=\"hidden\"><path d=\"M0 0H4V4H0Z\"/><path d=\"M5 5H9V9H5Z\" visibility=\"visible\"/></g>"
               "<path d=\"M0 0H4V4H0Z\" visibility=\"collapse\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource), "M5,5 L9,5 L9,9 L5,9 Z");
}

TEST(LowerWalk, ASwitchDrawsItsSelectedChildOnly) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<switch><path d=\"M0 0H1V1Z\" systemLanguage=\"xx\"/><path d=\"M0 0H2V2Z\"/><path d=\"M0 0H3V3Z\"/></switch>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(Describe(l.Item(0).resource), "M0,0 L2,0 L2,2 Z");
}

TEST(LowerWalk, ASwitchWithNoTrueChildDrawsNothing) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<switch><path d=\"M0 0H1V1Z\" systemLanguage=\"xx\"/></switch>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
}

TEST(LowerWalk, ElementsWhoseConditionsAreFalseAreNotDrawn) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H1V1Z\" systemLanguage=\"xx\"/><g requiredExtensions=\"urn:nothing\">" + std::string(kSquare) + "</g>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerWalk, WhatIsNeverRenderedIsNotARefusal) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<defs><path id=\"d\" d=\"M0 0H1V1Z\"/></defs>"
               "<symbol id=\"s\"><path d=\"M0 0H1V1Z\"/></symbol>"
               "<marker id=\"m\"><path d=\"M0 0H1V1Z\"/></marker>"
               "<clipPath id=\"c\"><path d=\"M0 0H1V1Z\"/></clipPath>"
               "<mask id=\"k\"><path d=\"M0 0H1V1Z\"/></mask>"
               "<pattern id=\"p\"><path d=\"M0 0H1V1Z\"/></pattern>"
               "<linearGradient id=\"g\"><stop offset=\"0\"/></linearGradient><radialGradient id=\"r\"/>"
               "<filter id=\"f\"><feFlood/></filter>"
               "<style type=\"text/x-other\">path { fill: red }</style><style> \n</style><script>1</script><title>t</title><desc>d</desc><metadata/>"
               "<animate attributeName=\"x\"/><set attributeName=\"x\"/><view id=\"v\"/>"
               "<unknown-thing><path d=\"M0 0H1V1Z\"/></unknown-thing><x:thing xmlns:x=\"urn:other\"><path d=\"M0 0H1V1Z\"/></x:thing>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerWalk, ATransformOfABoundingBoxIsRefused) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<path d=\"M0 0H4V4H0Z\" style=\"transform: translate(10%, 0); transform-box: fill-box\"/>"
               "<path d=\"M0 0H4V4H0Z\" style=\"transform: translate(10px, 0); transform-box: fill-box\"/>"),
      l);
  ASSERT_NE(l.Refusal(GVEC_LOWER_TRANSFORM_BOX), nullptr);
  EXPECT_EQ(l.Refusals(), (size_t) 1);
  EXPECT_EQ(l.Items(), (size_t) 1);
}

TEST(LowerWalk, ATransformPastTheWaistsRangeRefusesTheElement) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<g transform=\"scale(1e20)\"><g transform=\"scale(1e20)\"><g transform=\"scale(1e20)\">" +
                                                  std::string(kSquare) + "</g></g></g>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  ASSERT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerWalk, ADrawingNestedPastTheDepthLimitThroughAUseIsRefusedAsALimit) {
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_depth = 8;
  Lowered l;
  LoadSvg(Svg("width=\"20\" height=\"20\"",
              "<defs><g id=\"d\"><g><g><g><path d=\"M0 0H1V1Z\"/></g></g></g></g></defs><g><g><g><use href=\"#d\"/></g></g></g>"),
      l.loaded, 0, 0, &limits);
  ASSERT_EQ(l.loaded.result, GVEC_OK);
  LowerBoth(l.loaded, l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  const GVEC_Diagnostic * d = l.Refusal(GVEC_LOWER_LIMIT);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 0);
}

TEST(LowerWalk, APyramidOfUsesIsLoweredWithTheStylesOfOneBranchAtATime) {
  // 2^k instances of a shape through k levels of two uses each: every instance is read once and its style given back when its
  // subtree is done, so the styles held at once are a branch's, not the document's. (A million of them held at once is gigabytes.)
  std::string body = "<defs><path id=\"a0\" d=\"M0 0H1V1Z\"/>";
  for (int i = 1; i <= 9; i++) {
    body += "<g id=\"a" + std::to_string(i) + "\"><use href=\"#a" + std::to_string(i - 1) + "\"/><use href=\"#a" + std::to_string(i - 1) + "\" x=\"1\"/></g>";
  }
  body += "</defs><use href=\"#a9\"/>";
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_reference_expansion = 100000;
  Lowered l;
  LoadSvg(Svg("width=\"20\" height=\"20\"", body), l.loaded, 0, 0, &limits);
  LowerBoth(l.loaded, l);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_INSTANCE), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerWalk, AStyleGivenBackIsComputedAgainAndReadsTheSame) {
  Loaded loaded;
  LoadSvg(Svg("fill=\"red\"", "<g id=\"g\" fill=\"blue\"><path id=\"p\" d=\"M0 0\"/></g>"), loaded);
  ASSERT_EQ(loaded.result, GVEC_OK);
  GVEC_Svg_Style_Context * context = nullptr;
  ASSERT_EQ(gvec_svg_style_context_new(gvec_svg_document(loaded.doc), nullptr, nullptr, &context), GVEC_OK);
  const GVEC_Svg_Instance * root = gvec_svg_instance_root(context);
  const GVEC_Svg_Instance *g = nullptr, *p = nullptr;
  ASSERT_EQ(gvec_svg_instance_child(context, root, gvec_svg_find_by_id(gvec_svg_document(loaded.doc), "g"), &g), GVEC_OK);
  ASSERT_EQ(gvec_svg_instance_child(context, g, gvec_svg_find_by_id(gvec_svg_document(loaded.doc), "p"), &p), GVEC_OK);
  GVEC_Svg_Style_Value before, after;
  ASSERT_EQ(gvec_svg_get(context, p, GVEC_SVG_PROP_FILL, &before), GVEC_OK);
  gvec_svg_instance_release(context, p);
  gvec_svg_instance_release(context, g);
  gvec_svg_instance_release(context, root);
  gvec_svg_instance_release(context, root);  // twice is nothing
  gvec_svg_instance_release(context, nullptr);
  gvec_svg_instance_release(nullptr, root);
  ASSERT_EQ(gvec_svg_get(context, p, GVEC_SVG_PROP_FILL, &after), GVEC_OK);
  EXPECT_EQ(before.paint.color.color.rgba[2], 1.0);
  EXPECT_EQ(after.paint.color.color.rgba[2], 1.0);
  gvec_svg_style_context_free(context);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
