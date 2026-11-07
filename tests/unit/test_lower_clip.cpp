/**
 * @file
 *
 * `clip-path` (H9, H10): a `clipPath` of one shape is one clip push around what the element draws, with the map the element's own
 * space, the clipPath's transform, the box when the units say so and the shape's transform make; a clip that is clipped is a nested
 * push; several shapes are one union push; a `use` child and a clip on one of several shapes are refused.
 */

#include "../lower_helpers.h"

#include <vector>

using namespace gvec_test;

namespace {

bool Lower(const std::string & body, Lowered & l) {
  LowerSvg(Svg("width=\"200\" height=\"200\"", body), l);
  return true;
}

std::vector<GSHSCENE_Item_Kind> Kinds(const Lowered & l) {
  std::vector<GSHSCENE_Item_Kind> kinds;
  for (size_t i = 0; i < l.Items(); i++) {
    kinds.push_back(l.Item(i).kind);
  }
  return kinds;
}

const GSHSCENE_Item_Kind PUSH = GSHSCENE_ITEM_CLIP_PUSH, POP = GSHSCENE_ITEM_CLIP_POP, FILL = GSHSCENE_ITEM_FILL;

}  // namespace

TEST(LowerClip, AClipPathOfOneShapeIsOnePushAroundTheElement) {
  Lowered l;
  Lower("<defs><clipPath id=\"c\"><rect x=\"10\" y=\"10\" width=\"50\" height=\"50\"/></clipPath></defs>"
      "<rect width=\"100\" height=\"100\" fill=\"red\" clip-path=\"url(#c)\"/>",
      l);
  EXPECT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Item(0).fill_rule, GSHGEOM_FILL_NONZERO);
  EXPECT_EQ(l.Item(0).map.m[0][0], 1.0);
  EXPECT_EQ(l.Item(0).map.t[0], 0.0);
}

TEST(LowerClip, TheClipIsInTheElementsOwnSpaceAndTakesTheClipPathsAndTheShapesTransforms) {
  Lowered l;
  Lower("<defs><clipPath id=\"c\" transform=\"translate(10 0)\"><rect width=\"5\" height=\"5\" transform=\"translate(0 7)\"/></clipPath></defs>"
      "<rect width=\"100\" height=\"100\" transform=\"translate(3 4)\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Item(0).map.t[0], 13.0) << "the element's 3 and the clipPath's 10";
  EXPECT_EQ(l.Item(0).map.t[1], 11.0) << "the element's 4 and the shape's 7";
  EXPECT_EQ(l.Item(1).map.t[0], 3.0);
}

TEST(LowerClip, TheUnitsOfTheBoundingBoxPlaceTheClipInTheBoxOfTheElement) {
  Lowered l;
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"0.5\" height=\"1\"/></clipPath></defs>"
      "<rect x=\"20\" y=\"20\" width=\"100\" height=\"40\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Item(0).map.m[0][0], 100.0);
  EXPECT_EQ(l.Item(0).map.m[1][1], 40.0);
  EXPECT_EQ(l.Item(0).map.t[0], 20.0);
  EXPECT_EQ(l.Item(0).map.t[1], 20.0);
  Lowered moved;  // the clipPath's own transform is applied to the clip once it is in the box, so it is in user units
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\" transform=\"translate(5 0)\"><rect width=\"0.5\" height=\"1\"/></clipPath></defs>"
        "<rect x=\"20\" y=\"20\" width=\"100\" height=\"40\" clip-path=\"url(#c)\"/>",
      moved);
  ASSERT_EQ(Kinds(moved), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(moved.Item(0).map.m[0][0], 100.0);
  EXPECT_EQ(moved.Item(0).map.t[0], 25.0) << "the box's 20 and the clipPath's 5, not 20 + 100 * 5";
}

TEST(LowerClip, TheClipRuleIsTheShapesAndInheritsFromTheClipPath) {
  Lowered own;
  Lower("<defs><clipPath id=\"c\"><path d=\"M0 0H5V5H0Z\" clip-rule=\"evenodd\"/></clipPath></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", own);
  EXPECT_EQ(own.Item(0).fill_rule, GSHGEOM_FILL_EVEN_ODD);
  Lowered inherited;
  Lower("<defs><clipPath id=\"c\" clip-rule=\"evenodd\"><path d=\"M0 0H5V5H0Z\"/></clipPath></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", inherited);
  EXPECT_EQ(inherited.Item(0).fill_rule, GSHGEOM_FILL_EVEN_ODD);
  Lowered fill_rule;  // `fill-rule` is not `clip-rule`
  Lower("<defs><clipPath id=\"c\"><path d=\"M0 0H5V5H0Z\" fill-rule=\"evenodd\"/></clipPath></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", fill_rule);
  EXPECT_EQ(fill_rule.Item(0).fill_rule, GSHGEOM_FILL_NONZERO);
}

TEST(LowerClip, SeveralShapesAreOneUnionPushWithEachShapesOwnTransformAndRule) {
  Lowered l;
  Lower("<defs><clipPath id=\"c\" transform=\"translate(10 0)\"><rect width=\"5\" height=\"5\" transform=\"translate(0 7)\"/>"
        "<path d=\"M0 0H5V5H0Z\" clip-rule=\"evenodd\" transform=\"translate(20 0)\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" transform=\"translate(3 4)\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  const GSHSCENE_Item push = l.Item(0);
  ASSERT_EQ(gshscene_resource_clip_union_count(push.resource), (size_t) 2);
  EXPECT_EQ(push.map.t[0], 13.0) << "the element's 3 and the clipPath's 10: the shapes' own transforms are the members'";
  bool shifted_up = false, shifted_right = false, even_odd = false;
  for (size_t i = 0; i < 2; i++) {
    GSHSCENE_Clip_Member m;
    ASSERT_EQ(gshscene_resource_clip_union_member(push.resource, i, &m), GSHSCENE_OK);
    shifted_up = shifted_up || m.map.t[1] == 7.0;
    shifted_right = shifted_right || m.map.t[0] == 20.0;
    even_odd = even_odd || m.rule == GSHGEOM_FILL_EVEN_ODD;
  }
  EXPECT_TRUE(shifted_up);
  EXPECT_TRUE(shifted_right);
  EXPECT_TRUE(even_odd);
}

TEST(LowerClip, TheOrderOfTheShapesDoesNotChangeTheUnion) {
  Lowered ab, ba;
  Lower("<defs><clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><circle cx=\"20\" cy=\"20\" r=\"4\"/></clipPath></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", ab);
  Lower("<defs><clipPath id=\"c\"><circle cx=\"20\" cy=\"20\" r=\"4\"/><rect width=\"5\" height=\"5\"/></clipPath></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", ba);
  EXPECT_EQ(gshscene_resource_hash(ab.Item(0).resource), gshscene_resource_hash(ba.Item(0).resource));
}

TEST(LowerClip, AShapeThatIsHiddenOrHasNoAreaAddsNothingToTheUnion) {
  Lowered l;
  Lower("<defs><clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" display=\"none\"/>"
        "<rect x=\"20\" width=\"0\" height=\"5\"/><circle r=\"3\" visibility=\"hidden\"/></clipPath></defs>"
        "<rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_NE(gshscene_resource_path(l.Item(0).resource), nullptr) << "one member left: the waist records it as the single clip of that path";
  Lowered two;
  Lower("<defs><clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" display=\"none\"/>"
        "<rect x=\"20\" width=\"0\" height=\"5\"/><rect x=\"30\" width=\"5\" height=\"5\"/></clipPath></defs>"
        "<rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>",
      two);
  ASSERT_EQ(Kinds(two), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(gshscene_resource_clip_union_count(two.Item(0).resource), (size_t) 2);
}

TEST(LowerClip, AUnionOfShapesWithNoAreaLetsNothingThrough) {
  Lowered l;
  Lower("<defs><clipPath id=\"c\"><rect width=\"0\" height=\"5\"/><rect width=\"5\" height=\"0\"/></clipPath></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerClip, AUnionInTheUnitsOfTheBoundingBoxIsPlacedAsOneAndAClippedClipPathNestsAroundIt) {
  Lowered l;
  Lower("<defs><clipPath id=\"o\"><rect width=\"500\" height=\"500\"/></clipPath>"
        "<clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\" clip-path=\"url(#o)\"><rect width=\"0.5\" height=\"1\"/><rect x=\"0.75\" width=\"0.25\" height=\"1\"/></clipPath></defs>"
        "<rect x=\"20\" y=\"20\" width=\"100\" height=\"40\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, PUSH, FILL, POP, POP}));
  EXPECT_EQ(gshscene_resource_clip_union_count(l.Item(1).resource), (size_t) 2);
  EXPECT_EQ(l.Item(1).map.m[0][0], 100.0);
  EXPECT_EQ(l.Item(1).map.t[0], 20.0);
}

TEST(LowerClip, AGroupIsClippedAsOneAndItsOpacityGroupIsInsideTheClip) {
  Lowered g;
  Lower("<defs><clipPath id=\"c\"><rect width=\"50\" height=\"50\"/></clipPath></defs>"
      "<g clip-path=\"url(#c)\"><rect width=\"9\" height=\"9\"/><rect x=\"20\" width=\"9\" height=\"9\"/></g>",
      g);
  EXPECT_EQ(Kinds(g), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, FILL, POP}));
  Lowered o;
  Lower("<defs><clipPath id=\"c\"><rect width=\"50\" height=\"50\"/></clipPath></defs><rect width=\"9\" height=\"9\" opacity=\"0.5\" clip-path=\"url(#c)\"/>", o);
  EXPECT_EQ(Kinds(o), (std::vector<GSHSCENE_Item_Kind>{PUSH, GSHSCENE_ITEM_GROUP_PUSH, FILL, GSHSCENE_ITEM_GROUP_POP, POP}));
}

TEST(LowerClip, AClipThatIsClippedIsANestedPushWhetherItIsOnTheClipPathOrOnItsShape) {
  Lowered on_clip;
  Lower("<defs><clipPath id=\"a\" clip-path=\"url(#b)\"><rect width=\"50\" height=\"50\"/></clipPath><clipPath id=\"b\"><rect x=\"9\" width=\"50\" height=\"50\"/></clipPath></defs>"
      "<rect width=\"90\" height=\"90\" clip-path=\"url(#a)\"/>",
      on_clip);
  EXPECT_EQ(Kinds(on_clip), (std::vector<GSHSCENE_Item_Kind>{PUSH, PUSH, FILL, POP, POP}));
  Lowered on_shape;
  Lower("<defs><clipPath id=\"a\"><rect width=\"50\" height=\"50\" clip-path=\"url(#b)\"/></clipPath><clipPath id=\"b\"><rect x=\"9\" width=\"50\" height=\"50\"/></clipPath></defs>"
      "<rect width=\"90\" height=\"90\" clip-path=\"url(#a)\"/>",
      on_shape);
  EXPECT_EQ(Kinds(on_shape), (std::vector<GSHSCENE_Item_Kind>{PUSH, PUSH, FILL, POP, POP}));
}

TEST(LowerClip, AClipThatComesBackToItselfIsFollowedUntilItRepeats) {
  Lowered self;
  Lower("<defs><clipPath id=\"a\" clip-path=\"url(#a)\"><rect width=\"50\" height=\"50\"/></clipPath></defs><rect width=\"90\" height=\"90\" clip-path=\"url(#a)\"/>", self);
  EXPECT_EQ(Kinds(self), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  Lowered two;
  Lower("<defs><clipPath id=\"a\" clip-path=\"url(#b)\"><rect width=\"50\" height=\"50\"/></clipPath>"
      "<clipPath id=\"b\" clip-path=\"url(#a)\"><rect x=\"9\" width=\"50\" height=\"50\"/></clipPath></defs><rect width=\"90\" height=\"90\" clip-path=\"url(#a)\"/>",
      two);
  EXPECT_EQ(Kinds(two), (std::vector<GSHSCENE_Item_Kind>{PUSH, PUSH, FILL, POP, POP}));
}

TEST(LowerClip, AClipOfNothingLetsNothingThroughAndAReferenceToNothingClipsNothing) {
  const char * nothing[] = {"<clipPath id=\"c\"/>", "<clipPath id=\"c\"><rect width=\"5\" height=\"5\" display=\"none\"/></clipPath>",
      "<clipPath id=\"c\"><rect width=\"0\" height=\"5\"/></clipPath>", "<clipPath id=\"c\"><rect width=\"5\" height=\"5\" visibility=\"hidden\"/></clipPath>",
      "<clipPath id=\"c\"><title/><g><rect width=\"5\" height=\"5\"/></g></clipPath>"};
  for (const char * clip : nothing) {
    Lowered l;
    Lower(std::string("<defs>") + clip + "</defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", l);
    EXPECT_EQ(l.Items(), (size_t) 0) << clip;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << clip << ": nothing is not a refusal";
  }
  Lowered missing;
  Lower("<rect width=\"9\" height=\"9\" clip-path=\"url(#nowhere)\"/>", missing);
  EXPECT_EQ(Kinds(missing), (std::vector<GSHSCENE_Item_Kind>{FILL}));
  Lowered not_a_clip;
  Lower("<defs><rect id=\"r\" width=\"5\" height=\"5\"/></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#r)\"/>", not_a_clip);
  EXPECT_EQ(Kinds(not_a_clip), (std::vector<GSHSCENE_Item_Kind>{FILL}));
}

TEST(LowerClip, ABoundingBoxOfNoAreaGivesNoClipToPlaceAndTheElementIsNotRendered) {
  Lowered l;
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath></defs>"
      "<line x1=\"0\" y1=\"5\" x2=\"20\" y2=\"5\" stroke=\"red\" clip-path=\"url(#c)\"/>",
      l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerClip, WhatIsNotAShapeOrOneOfSeveralWithItsOwnClipIsRefusedWhole) {
  const struct {
    const char * clip;
    GVEC_Lower_Reason reason;
  } cases[] = {
      {"<clipPath id=\"o\"><rect width=\"1\" height=\"1\"/></clipPath><clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" clip-path=\"url(#o)\"/></clipPath>", GVEC_LOWER_CLIP_PATH},
      {"<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" style=\"clip-path: inset(1px)\"/></clipPath>", GVEC_LOWER_CLIP_PATH},
      {"<clipPath id=\"c\"><text>x</text></clipPath>", GVEC_LOWER_TEXT},
      {"<clipPath id=\"c\"><rect id=\"r\" width=\"5\" height=\"5\"/><use href=\"#r\"/></clipPath>", GVEC_LOWER_CLIP_PATH},
  };
  for (const auto & c : cases) {
    Lowered l;
    Lower(std::string("<defs>") + c.clip + "</defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", l);
    EXPECT_EQ(l.Items(), (size_t) 0) << c.clip;
    EXPECT_NE(l.Refusal(c.reason), nullptr) << c.clip;
  }
  Lowered shape;  // a basic shape on the element itself is lowered (test_lower_basic_shape.cpp); on a clip's shape it is not
  Lower("<rect width=\"9\" height=\"9\" style=\"clip-path: inset(1px)\"/>", shape);
  EXPECT_EQ(shape.Items(), (size_t) 3);
  EXPECT_EQ(shape.Refusals(), (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
