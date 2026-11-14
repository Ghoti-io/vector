/**
 * @file
 *
 * `clip-path` (H9, H10): a `clipPath` of one shape is one clip push around what the element draws, with the map the element's own
 * space, the clipPath's transform, the box when the units say so and the shape's transform make; a clip that is clipped is a nested
 * push; several shapes are one union push; a `use` child is the shape it names; a clip on one of several shapes is that member's inner clips.
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

TEST(LowerClip, WhatThisDoesNotLowerIsRefusedWhole) {
  const struct {
    const char * clip;
    GVEC_Lower_Reason reason;
  } cases[] = {
      {"<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" style=\"clip-path: inset(1px)\"/></clipPath>", GVEC_LOWER_CLIP_PATH},
      {"<clipPath id=\"c\"><text>x</text></clipPath>", GVEC_LOWER_TEXT},
      {"<rect id=\"r\" width=\"5\" height=\"5\"/><clipPath id=\"c\"><use href=\"#r\" style=\"clip-path: inset(1px)\"/></clipPath>", GVEC_LOWER_CLIP_PATH},
      {"<text id=\"t\">x</text><clipPath id=\"c\"><use href=\"#t\"/></clipPath>", GVEC_LOWER_TEXT},
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

namespace {

/** Every member of the union an element's clip pushed, read back. */
std::vector<GSHSCENE_Clip_Member> Members(const Lowered & l) {
  std::vector<GSHSCENE_Clip_Member> out;
  const GSHSCENE_Item push = l.Item(0);
  const size_t n = gshscene_resource_clip_union_count(push.resource);
  for (size_t i = 0; i < n; i++) {
    GSHSCENE_Clip_Member m;
    EXPECT_EQ(gshscene_resource_clip_union_member(push.resource, i, &m), GSHSCENE_OK);
    out.push_back(m);
  }
  return out;
}

size_t WithInner(const std::vector<GSHSCENE_Clip_Member> & ms, size_t inner) {
  size_t n = 0;
  for (const auto & m : ms) {
    n += m.inner_count == inner;
  }
  return n;
}

}  // namespace

TEST(LowerClip, AShapeOfSeveralThatIsClippedIsAMemberWithAnInnerClip) {
  Lowered l;
  Lower("<defs><clipPath id=\"o\"><rect width=\"4\" height=\"6\" transform=\"translate(1 2)\"/></clipPath>"
        "<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" transform=\"translate(10 0)\" clip-path=\"url(#o)\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  const auto ms = Members(l);
  ASSERT_EQ(ms.size(), (size_t) 2);
  EXPECT_EQ(WithInner(ms, 0), (size_t) 1);
  EXPECT_EQ(WithInner(ms, 1), (size_t) 1);
  for (const auto & m : ms) {
    if (m.inner_count == 1) {
      EXPECT_EQ(m.map.t[0], 10.0) << "the member's own transform";
      // the inner clip is in the clipped shape's own space, its transform included: the clipPath's rect is translated (1 2) in it
      EXPECT_EQ(m.inner[0].map.t[0], 11.0);
      EXPECT_EQ(m.inner[0].map.t[1], 2.0);
    }
  }
}

TEST(LowerClip, AClipThatIsAUnionOfShapesMakesAMemberForEachOfThem) {
  Lowered l;
  Lower("<defs><clipPath id=\"o\"><rect width=\"4\" height=\"4\"/><rect x=\"6\" width=\"4\" height=\"4\"/></clipPath>"
        "<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" clip-path=\"url(#o)\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  const auto ms = Members(l);
  EXPECT_EQ(ms.size(), (size_t) 3) << "the first shape, and the second cut by each of the two shapes of its clip";
  EXPECT_EQ(WithInner(ms, 0), (size_t) 1);
  EXPECT_EQ(WithInner(ms, 1), (size_t) 2);
}

TEST(LowerClip, AClipThatIsClippedAndAClipThatIsEmptyOnAShapeOfSeveral) {
  Lowered l;  // the member's clip is clipped in turn: two inner clips
  Lower("<defs><clipPath id=\"p\"><rect width=\"3\" height=\"3\"/></clipPath>"
        "<clipPath id=\"o\" clip-path=\"url(#p)\"><rect width=\"4\" height=\"4\"/></clipPath>"
        "<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" clip-path=\"url(#o)\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>",
      l);
  auto ms = Members(l);
  ASSERT_EQ(ms.size(), (size_t) 2);
  EXPECT_EQ(WithInner(ms, 2), (size_t) 1);
  Lowered empty;  // a clip of nothing lets nothing of that shape through: it is not a member
  Lower("<defs><clipPath id=\"o\"></clipPath>"
        "<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/><rect x=\"9\" width=\"5\" height=\"5\" clip-path=\"url(#o)\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>",
      empty);
  ASSERT_EQ(Kinds(empty), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(gshscene_resource_kind(empty.Item(0).resource), GSHSCENE_RESOURCE_PATH) << "one member left: the union is that member's own clip push (CU10)";
}

TEST(LowerClip, AUseChildWithAClipOfItsOwnIsAMemberWithAnInnerClipInTheUsesSpace) {
  Lowered l;
  Lower("<defs><clipPath id=\"o\"><rect width=\"4\" height=\"4\"/></clipPath><rect id=\"r\" width=\"5\" height=\"5\"/>"
        "<clipPath id=\"c\"><rect x=\"30\" width=\"5\" height=\"5\"/><use href=\"#r\" x=\"20\" transform=\"translate(2 3)\" clip-path=\"url(#o)\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  const auto ms = Members(l);
  ASSERT_EQ(ms.size(), (size_t) 2);
  EXPECT_EQ(WithInner(ms, 1), (size_t) 1) << "the use's own clip-path is the member's inner clip";
  for (const auto & m : ms) {
    if (m.inner_count == 1) {
      EXPECT_EQ(m.map.t[0], 22.0) << "the use's transform and its x";
      EXPECT_EQ(m.inner[0].map.t[0], 2.0) << "the use's clip is in the use's space: its transform, not its x";
      EXPECT_EQ(m.inner[0].map.t[1], 3.0);
    }
  }
}

TEST(LowerClip, AUseChildThatNamesAShapeIsThatShapePlacedByTheUse) {
  Lowered l;
  Lower("<defs><rect id=\"r\" width=\"5\" height=\"5\" transform=\"translate(0 2)\"/>"
        "<clipPath id=\"c\"><use href=\"#r\" x=\"7\" transform=\"translate(1 3)\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>",
      l);
  ASSERT_EQ(Kinds(l), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Item(0).map.t[0], 8.0) << "the use's transform and its x";
  EXPECT_EQ(l.Item(0).map.t[1], 5.0) << "the use's transform and the shape's own";
  Lowered several;  // a use and a shape are one union
  Lower("<defs><rect id=\"r\" width=\"5\" height=\"5\"/><clipPath id=\"c\"><use href=\"#r\"/><rect x=\"9\" width=\"5\" height=\"5\"/></clipPath></defs>"
        "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>",
      several);
  EXPECT_EQ(Kinds(several), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
  EXPECT_EQ(several.Refusals(), (size_t) 0);
}

TEST(LowerClip, AUseChildThatNamesAnythingElseOrIsNotDrawnAddsNothing) {
  const char * const clips[] = {
      "<g id=\"g\"><rect width=\"5\" height=\"5\"/></g><clipPath id=\"c\"><use href=\"#g\"/></clipPath>",
      "<symbol id=\"s\"><rect width=\"5\" height=\"5\"/></symbol><clipPath id=\"c\"><use href=\"#s\"/></clipPath>",
      "<clipPath id=\"c\"><use href=\"#nothing\"/></clipPath>",
      "<rect id=\"r\" width=\"5\" height=\"5\" display=\"none\"/><clipPath id=\"c\"><use href=\"#r\"/></clipPath>",
      "<rect id=\"r\" width=\"5\" height=\"5\"/><clipPath id=\"c\"><use href=\"#r\" visibility=\"hidden\"/></clipPath>",
  };
  for (const char * clip : clips) {
    Lowered l;
    Lower(std::string("<defs>") + clip + "</defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>", l);
    EXPECT_EQ(l.Count(FILL), (size_t) 0) << clip << ": a clip of nothing lets nothing through";
    EXPECT_EQ(l.Refusals(), (size_t) 0) << clip;
  }
  Lowered valid;  // the invalid use is skipped and the shape beside it clips
  Lower("<defs><g id=\"g\"/><clipPath id=\"c\"><use href=\"#g\"/><rect width=\"5\" height=\"5\"/></clipPath></defs><rect width=\"9\" height=\"9\" clip-path=\"url(#c)\"/>",
      valid);
  EXPECT_EQ(Kinds(valid), (std::vector<GSHSCENE_Item_Kind>{PUSH, FILL, POP}));
}

TEST(LowerClip, AGroupInTheUnitsOfTheBoundingBoxIsPlacedInTheBoxOfWhatItDraws) {
  Lowered l;  // the hidden rectangle and the opacity-0 one are in the box; the one with display none is not
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath></defs>"
        "<g clip-path=\"url(#c)\"><rect x=\"20\" y=\"20\" width=\"60\" height=\"60\" visibility=\"hidden\"/>"
        "<g transform=\"translate(100 0)\"><rect x=\"0\" y=\"40\" width=\"20\" height=\"60\" opacity=\"0\"/></g>"
        "<rect x=\"30\" y=\"30\" width=\"10\" height=\"10\"/>"
        "<rect x=\"0\" y=\"0\" width=\"500\" height=\"500\" display=\"none\"/></g>",
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_GT(l.Items(), (size_t) 0);
  ASSERT_EQ(Kinds(l).front(), PUSH);
  EXPECT_EQ(l.Item(0).map.m[0][0], 100.0) << "x from 20 to 120";
  EXPECT_EQ(l.Item(0).map.m[1][1], 80.0) << "y from 20 to 100";
  EXPECT_EQ(l.Item(0).map.t[0], 20.0);
  EXPECT_EQ(l.Item(0).map.t[1], 20.0);
  Lowered rotated;  // a rotated child is boxed by where its corners go
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath></defs>"
        "<g clip-path=\"url(#c)\"><rect width=\"10\" height=\"10\" transform=\"rotate(45)\"/></g>",
      rotated);
  ASSERT_GT(rotated.Items(), (size_t) 0);
  ASSERT_EQ(Kinds(rotated).front(), PUSH);
  EXPECT_NEAR(rotated.Item(0).map.m[0][0], 14.142135623730951, 1e-9);
  EXPECT_NEAR(rotated.Item(0).map.t[0], -7.0710678118654755, 1e-9);
  Lowered round;  // a rotated circle is boxed by where its arcs go (a circle of radius 5 at (5,5), turned 45 degrees about the origin)
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath></defs>"
        "<g clip-path=\"url(#c)\"><circle cx=\"5\" cy=\"5\" r=\"5\" transform=\"rotate(45)\"/></g>",
      round);
  EXPECT_EQ(round.Refusals(), (size_t) 0);
  ASSERT_GT(round.Items(), (size_t) 0);
  ASSERT_EQ(Kinds(round).front(), PUSH);
  EXPECT_NEAR(round.Item(0).map.m[0][0], 10.0, 1e-6);
  EXPECT_NEAR(round.Item(0).map.m[1][1], 10.0, 1e-6);
  EXPECT_NEAR(round.Item(0).map.t[0], -5.0, 1e-6);
  Lowered nothing;  // a group whose only shape has display none has no box, so nothing is drawn
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath></defs>"
        "<g clip-path=\"url(#c)\"><rect width=\"10\" height=\"10\" display=\"none\"/></g>",
      nothing);
  EXPECT_EQ(nothing.Count(FILL), (size_t) 0);
  EXPECT_EQ(nothing.Refusals(), (size_t) 0);
  Lowered flat;  // a group whose box has no height cannot place the clip: it is not rendered
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath></defs>"
        "<g clip-path=\"url(#c)\"><line x1=\"0\" y1=\"5\" x2=\"10\" y2=\"5\" stroke=\"black\"/></g>",
      flat);
  EXPECT_EQ(flat.Items(), (size_t) 0);
  EXPECT_EQ(flat.Refusals(), (size_t) 0);
  Lowered unboxed;  // a group that holds text, an image or a nested use is still refused
  Lower("<defs><clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath><rect id=\"r\" width=\"5\" height=\"5\"/></defs>"
        "<g clip-path=\"url(#c)\"><use href=\"#r\"/></g>",
      unboxed);
  EXPECT_NE(unboxed.Refusal(GVEC_LOWER_CLIP_PATH), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
