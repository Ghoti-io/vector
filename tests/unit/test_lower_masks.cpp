/**
 * @file
 *
 * Masks: `mask` naming a `mask` element as the soft mask of a group of the waist. The mode (luminance, alpha, and the space of
 * `color-interpolation`), the region in the units the document gives, the content's units, a mask on the mask and on its children, a
 * mask that comes back to itself, and what a mask that cannot be placed amounts to.
 */

#include <ghoti.io/shape-scene/mask.h>

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

const char * kContent = "<rect x=\"10\" y=\"10\" width=\"60\" height=\"60\" fill=\"white\"/>";

/** A document of one rect, 20 by 20 at (20, 20), masked by `mask1` over `defs`; the mask's own attributes are `mask_attributes`. */
bool Lower(const std::string & mask_attributes, const std::string & content, Lowered & l, const std::string & target = "mask=\"url(#mask1)\"") {
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<mask id=\"mask1\" " + mask_attributes + ">" + content + "</mask><rect x=\"20\" y=\"20\" width=\"40\" height=\"20\" fill=\"green\" " + target + "/>"),
      l);
  return l.Items() >= 3 && l.Item(0).kind == GSHSCENE_ITEM_GROUP_PUSH && l.Item(0).group.soft_mask != nullptr;
}

bool Read(const Lowered & l, GSHSCENE_Mask & out) {
  return l.Items() >= 1 && l.Item(0).group.soft_mask && gshscene_resource_mask(l.Item(0).group.soft_mask, &out) == GSHSCENE_OK;
}

const GSHSCENE_Item & Inner(const GSHSCENE_Resource * picture, size_t index) {
  const GSHSCENE_Item * item = nullptr;
  EXPECT_EQ(gshscene_list_item(gshscene_resource_picture(picture), index, &item), GSHSCENE_OK);
  return *item;
}

}  // namespace

TEST(LowerMask, AMaskIsAGroupWhoseSoftMaskIsTheContentAsLuminance) {
  Lowered l;
  ASSERT_TRUE(Lower("", kContent, l));
  EXPECT_EQ(l.Item(0).group.opacity, 1.0);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(2).kind, GSHSCENE_ITEM_GROUP_POP);
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  EXPECT_EQ(m.mode, (uint32_t) GSHSCENE_MASK_LUMINANCE);
  EXPECT_EQ(m.coefficients, (uint32_t) GSHSCENE_MASK_COEFF_W3C);
  EXPECT_EQ(m.space.primaries, (GCOL_Primaries) 0);
  EXPECT_EQ(m.acts_on, (uint32_t) GSHSCENE_MASK_ACTS_ON_OPACITY);
}

TEST(LowerMask, MaskTypeAlphaReadsTheAlpha) {
  Lowered attribute;
  ASSERT_TRUE(Lower("mask-type=\"alpha\"", kContent, attribute));
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(attribute, m));
  EXPECT_EQ(m.mode, (uint32_t) GSHSCENE_MASK_ALPHA);
  Lowered style;
  ASSERT_TRUE(Lower("style=\"mask-type: alpha\"", kContent, style));
  ASSERT_TRUE(Read(style, m));
  EXPECT_EQ(m.mode, (uint32_t) GSHSCENE_MASK_ALPHA);
  Lowered invalid;
  ASSERT_TRUE(Lower("mask-type=\"nonsense\"", kContent, invalid));
  ASSERT_TRUE(Read(invalid, m));
  EXPECT_EQ(m.mode, (uint32_t) GSHSCENE_MASK_LUMINANCE);
}

TEST(LowerMask, LinearRGBIsTheSpaceOfTheLuminance) {
  Lowered l;
  ASSERT_TRUE(Lower("color-interpolation=\"linearRGB\"", kContent, l));
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  EXPECT_EQ(m.space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(m.space.transfer, GCOL_TRANSFER_LINEAR);
}

TEST(LowerMask, TheRegionIsTenPercentOutsideTheBoxByDefaultAndClipsTheContent) {
  Lowered l;
  ASSERT_TRUE(Lower("", kContent, l));
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  const GSHSCENE_Item & clip = Inner(m.picture, 0);
  ASSERT_EQ(clip.kind, GSHSCENE_ITEM_CLIP_PUSH);
  GSHGEOM_Bounds b;
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(clip.resource), &b), GSHGEOM_OK);
  EXPECT_DOUBLE_EQ(b.min.x, 20 - 4.0);   // the rect is 40 wide at x = 20: -10% of 40
  EXPECT_DOUBLE_EQ(b.min.y, 20 - 2.0);
  EXPECT_DOUBLE_EQ(b.max.x, 20 - 4.0 + 48.0);  // 120% of 40
  EXPECT_DOUBLE_EQ(b.max.y, 20 - 2.0 + 24.0);
}

TEST(LowerMask, TheRegionInUserUnitsIsTheLengthsTheDocumentGives) {
  Lowered l;
  ASSERT_TRUE(Lower("maskUnits=\"userSpaceOnUse\" x=\"5\" y=\"6\" width=\"30\" height=\"40\"", kContent, l));
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  GSHGEOM_Bounds b;
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(Inner(m.picture, 0).resource), &b), GSHGEOM_OK);
  EXPECT_EQ(b.min.x, 5.0);
  EXPECT_EQ(b.min.y, 6.0);
  EXPECT_EQ(b.max.x, 35.0);
  EXPECT_EQ(b.max.y, 46.0);
}

TEST(LowerMask, ContentInTheUnitsOfTheBoxIsPlacedAndScaledByIt) {
  Lowered l;
  ASSERT_TRUE(Lower("maskContentUnits=\"objectBoundingBox\"", "<rect width=\"0.5\" height=\"1\" fill=\"white\"/>", l));
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  const GSHSCENE_Item & fill = Inner(m.picture, 1);
  ASSERT_EQ(fill.kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(fill.map.m[0][0], 40.0);
  EXPECT_EQ(fill.map.m[1][1], 20.0);
  EXPECT_EQ(fill.map.t[0], 20.0);
  EXPECT_EQ(fill.map.t[1], 20.0);
}

TEST(LowerMask, AMaskIsInTheSpaceOfTheElementsOwnTransform) {
  Lowered l;
  ASSERT_TRUE(Lower("maskUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"50\" height=\"50\"", kContent, l, "mask=\"url(#mask1)\" transform=\"translate(7 9)\""));
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  const GSHSCENE_Item & clip = Inner(m.picture, 0);
  EXPECT_EQ(clip.map.t[0], 7.0);
  EXPECT_EQ(clip.map.t[1], 9.0);
}

TEST(LowerMask, AMaskOnTheMaskIsAMaskOfItsPicture) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               std::string("<mask id=\"inner\">") + kContent + "</mask><mask id=\"outer\" mask=\"url(#inner)\">" + kContent +
                   "</mask><rect x=\"20\" y=\"20\" width=\"40\" height=\"20\" fill=\"green\" mask=\"url(#outer)\"/>"),
      l);
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  const GSHSCENE_Item & group = Inner(m.picture, 0);
  ASSERT_EQ(group.kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_NE(group.group.soft_mask, nullptr);
}

TEST(LowerMask, MasksThatNameEachOtherDropTheMaskOfTheMask) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               std::string("<mask id=\"a\" mask=\"url(#b)\">") + kContent + "</mask><mask id=\"b\" mask=\"url(#a)\">" + kContent +
                   "</mask><rect x=\"20\" y=\"20\" width=\"40\" height=\"20\" fill=\"green\" mask=\"url(#b)\"/>"),
      l);
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  EXPECT_EQ(Inner(m.picture, 0).kind, GSHSCENE_ITEM_CLIP_PUSH);  // no group of the other mask in it
  Lowered self;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               std::string("<mask id=\"a\" mask=\"url(#a)\">") + kContent + "</mask><rect x=\"20\" y=\"20\" width=\"40\" height=\"20\" fill=\"green\" mask=\"url(#a)\"/>"),
      self);
  ASSERT_TRUE(Read(self, m));
  EXPECT_EQ(Inner(m.picture, 0).kind, GSHSCENE_ITEM_CLIP_PUSH);
}

TEST(LowerMask, AChildThatNamesItsOwnMaskIsDrawnWithoutIt) {
  Lowered l;
  ASSERT_TRUE(Lower("", "<rect width=\"60\" height=\"60\" fill=\"white\" mask=\"url(#mask1)\"/>", l));
  GSHSCENE_Mask m;
  ASSERT_TRUE(Read(l, m));
  EXPECT_EQ(Inner(m.picture, 1).kind, GSHSCENE_ITEM_FILL);
}

TEST(LowerMask, AReferenceToNothingDrawsTheElementUnmasked) {
  Lowered missing;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<rect width=\"40\" height=\"20\" mask=\"url(#nothing)\"/>"), missing);
  ASSERT_EQ(missing.Items(), (size_t) 1);
  EXPECT_EQ(missing.Item(0).kind, GSHSCENE_ITEM_FILL);
  Lowered not_a_mask;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<g id=\"g\"/><rect width=\"40\" height=\"20\" mask=\"url(#g)\"/>"), not_a_mask);
  ASSERT_EQ(not_a_mask.Items(), (size_t) 1);
}

TEST(LowerMask, AMaskThatCannotBePlacedHidesTheElement) {
  Lowered empty;
  LowerSvg(Svg("width=\"100\" height=\"100\"", "<mask id=\"mask1\"/><rect width=\"40\" height=\"20\" mask=\"url(#mask1)\"/>"), empty);
  EXPECT_EQ(empty.Items(), (size_t) 0);
  EXPECT_EQ(empty.Refusals(), (size_t) 0);
  Lowered no_region;
  LowerSvg(Svg("width=\"100\" height=\"100\"", std::string("<mask id=\"mask1\" width=\"0\">") + kContent + "</mask><rect width=\"40\" height=\"20\" mask=\"url(#mask1)\"/>"), no_region);
  EXPECT_EQ(no_region.Items(), (size_t) 0);
  Lowered no_box;
  LowerSvg(Svg("width=\"100\" height=\"100\"", std::string("<mask id=\"mask1\">") + kContent + "</mask><path d=\"M0 5H40\" stroke=\"red\" mask=\"url(#mask1)\"/>"), no_box);
  EXPECT_EQ(no_box.Items(), (size_t) 0);
}

TEST(LowerMask, AMaskOnAGroupMasksTheGroupAsOne) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               std::string("<mask id=\"mask1\">") + kContent + "</mask><g mask=\"url(#mask1)\"><rect width=\"10\" height=\"10\"/><rect x=\"20\" width=\"10\" height=\"10\"/></g>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 4);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_NE(l.Item(0).group.soft_mask, nullptr);
  EXPECT_EQ(l.Item(3).kind, GSHSCENE_ITEM_GROUP_POP);
}

TEST(LowerMask, TheSameMaskOnTwoElementsOfOneBoxIsOneResource) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               std::string("<mask id=\"mask1\" maskUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"50\" height=\"50\">") + kContent +
                   "</mask><rect width=\"10\" height=\"10\" mask=\"url(#mask1)\"/><rect y=\"20\" width=\"10\" height=\"10\" mask=\"url(#mask1)\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 6);
  EXPECT_EQ(l.Item(0).group.soft_mask, l.Item(3).group.soft_mask);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
