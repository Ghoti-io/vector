/**
 * @file
 *
 * Blend modes and isolation: `mix-blend-mode` is the composite operator of the group an element is drawn in, `isolation: isolate` an
 * isolated group of its own, and a mask, which is itself an isolated group, carries the blend of its element.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

const char * kSquare = "<path d=\"M0 0H4V4H0Z\"";

}  // namespace

TEST(LowerBlend, EveryModeIsItsOperatorOfTheWaist) {
  static const struct {
    const char * word;
    GSHSCENE_Composite op;
  } modes[] = {{"multiply", GSHSCENE_COMPOSITE_MULTIPLY}, {"screen", GSHSCENE_COMPOSITE_SCREEN}, {"overlay", GSHSCENE_COMPOSITE_OVERLAY},
      {"darken", GSHSCENE_COMPOSITE_DARKEN}, {"lighten", GSHSCENE_COMPOSITE_LIGHTEN}, {"color-dodge", GSHSCENE_COMPOSITE_COLOR_DODGE},
      {"color-burn", GSHSCENE_COMPOSITE_COLOR_BURN}, {"hard-light", GSHSCENE_COMPOSITE_HARD_LIGHT},
      {"soft-light", GSHSCENE_COMPOSITE_SOFT_LIGHT}, {"difference", GSHSCENE_COMPOSITE_DIFFERENCE},
      {"exclusion", GSHSCENE_COMPOSITE_EXCLUSION}, {"hue", GSHSCENE_COMPOSITE_HUE}, {"saturation", GSHSCENE_COMPOSITE_SATURATION},
      {"color", GSHSCENE_COMPOSITE_COLOR}, {"luminosity", GSHSCENE_COMPOSITE_LUMINOSITY}};
  for (const auto & m : modes) {
    Lowered l;
    LowerSvg(Svg("width=\"20\" height=\"20\"", std::string(kSquare) + " style=\"mix-blend-mode: " + m.word + "\"/>"), l);
    ASSERT_EQ(l.Items(), (size_t) 3) << m.word;
    EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH) << m.word;
    EXPECT_EQ(l.Item(0).group.composite, (uint32_t) m.op) << m.word;
    EXPECT_EQ(l.Item(0).group.opacity, 1.0) << m.word;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << m.word;
  }
}

TEST(LowerBlend, NormalAnInvalidValueAndAPresentationAttributeMakeNoGroup) {
  for (const char * attributes : {"style=\"mix-blend-mode: normal\"", "style=\"mix-blend-mode: nonsense\"", "mix-blend-mode=\"multiply\""}) {
    Lowered l;
    LowerSvg(Svg("width=\"20\" height=\"20\"", std::string(kSquare) + " " + attributes + "/>"), l);
    ASSERT_EQ(l.Items(), (size_t) 1) << attributes;
    EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_FILL) << attributes;
  }
}

TEST(LowerBlend, OpacityAndTheBlendAreOneGroup) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", std::string(kSquare) + " opacity=\".5\" style=\"mix-blend-mode: multiply\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_EQ(l.Item(0).group.opacity, 0.5);
  EXPECT_EQ(l.Item(0).group.composite, (uint32_t) GSHSCENE_COMPOSITE_MULTIPLY);
}

TEST(LowerBlend, IsolationIsAGroupOfItsOwnAroundTheChildren) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", std::string("<g style=\"isolation: isolate\">") + kSquare + " style=\"mix-blend-mode: screen\"/></g>"), l);
  ASSERT_EQ(l.Items(), (size_t) 5);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(0).group.composite, (uint32_t) GSHSCENE_COMPOSITE_SRC_OVER);
  EXPECT_EQ(l.Item(0).group.isolation, (uint32_t) GSHSCENE_ISOLATION_ISOLATED);
  // The waist drops a group that is all defaults, so the isolated group names the space it blends in (sRGB, as CSS blends).
  EXPECT_EQ(l.Item(0).group.blend_space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(l.Item(0).group.blend_space.transfer, GCOL_TRANSFER_SRGB);
  EXPECT_EQ(l.Item(1).group.composite, (uint32_t) GSHSCENE_COMPOSITE_SCREEN);
  Lowered auto_;
  LowerSvg(Svg("width=\"20\" height=\"20\"", std::string("<g style=\"isolation: auto\">") + kSquare + "/></g>"), auto_);
  ASSERT_EQ(auto_.Items(), (size_t) 1);
}

TEST(LowerBlend, AGroupWithoutAStackingContextBlendsItsChildrenWithTheBackdrop) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", std::string("<g>") + kSquare + " style=\"mix-blend-mode: multiply\"/></g>"), l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_EQ(l.Item(0).group.composite, (uint32_t) GSHSCENE_COMPOSITE_MULTIPLY);
}

TEST(LowerBlend, AMaskGroupCarriesTheBlendOfItsElement) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               std::string("<mask id=\"m\"><rect width=\"100\" height=\"100\" fill=\"white\"/></mask>") + kSquare +
                   " mask=\"url(#m)\" style=\"mix-blend-mode: multiply\"/>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_NE(l.Item(0).group.soft_mask, nullptr);
  EXPECT_EQ(l.Item(0).group.composite, (uint32_t) GSHSCENE_COMPOSITE_MULTIPLY);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_FILL);  // no second group inside it
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
