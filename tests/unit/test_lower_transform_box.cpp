/**
 * @file
 *
 * A `transform` whose percentages are of a bounding box (`transform-box` of `fill-box`, `stroke-box`, `content-box` or `border-box`, with a
 * `transform-origin` in percent or `center`, or a `translate()` in percent) is lowered: the box is that of what the element draws, in its own
 * space, and the list is the one the same transform written with the box's numbers gives.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

std::string Doc(const std::string & body) {
  return Svg("width=\"200\" height=\"100\"", body);
}

void ExpectSame(const std::string & boxed, const std::string & explicit_numbers, const char * what) {
  SCOPED_TRACE(what);
  Lowered a, b;
  LowerSvg(Doc(boxed), a);
  LowerSvg(Doc(explicit_numbers), b);
  EXPECT_EQ(a.Refusals(), (size_t) 0) << "the box is made";
  EXPECT_EQ(b.Refusals(), (size_t) 0);
  ASSERT_NE(a.list, nullptr);
  ASSERT_NE(b.list, nullptr);
  EXPECT_GE(a.Items(), (size_t) 1);
  EXPECT_EQ(gshscene_list_hash(a.list), gshscene_list_hash(b.list)) << "the same list as the transform with the box's numbers";
}

}  // namespace

TEST(LowerTransformBox, ARotationAboutTheCentreOfTheFillBoxIsTheRotationAboutThatPoint) {
  // The rectangle's fill box is (10, 20) to (50, 30): its centre is (30, 25).
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" style=\"transform-box:fill-box;transform-origin:center;transform:rotate(90deg)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" transform=\"rotate(90 30 25)\"/>", "center");
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" style=\"transform-box:fill-box;transform-origin:25% 75%;transform:rotate(30deg)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" transform=\"rotate(30 20 27.5)\"/>", "percentages");
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" style=\"transform-box:content-box;transform-origin:100% 0%;transform:scale(2)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" transform=\"translate(50 20) scale(2) translate(-50 -20)\"/>", "content-box is the fill box");
}

TEST(LowerTransformBox, TheStrokeBoxIsTheFillBoxGrownByTheStroke) {
  // A stroke of 10 reaches 5 past the rectangle on each side: the stroke box is (5, 15) to (55, 35).
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" fill=\"none\" stroke=\"black\" stroke-width=\"10\" "
             "style=\"transform-box:stroke-box;transform-origin:10% 20%;transform:rotate(20deg)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" fill=\"none\" stroke=\"black\" stroke-width=\"10\" transform=\"rotate(20 10 19)\"/>", "stroke-box");
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" fill=\"none\" stroke=\"black\" stroke-width=\"10\" "
             "style=\"transform-box:border-box;transform-origin:100% 100%;transform:rotate(20deg)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" fill=\"none\" stroke=\"black\" stroke-width=\"10\" transform=\"rotate(20 55 35)\"/>", "border-box is the stroke box");
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" fill=\"none\" stroke=\"black\" stroke-width=\"10\" "
             "style=\"transform-box:fill-box;transform-origin:10% 20%;transform:rotate(20deg)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" fill=\"none\" stroke=\"black\" stroke-width=\"10\" transform=\"rotate(20 14 22)\"/>", "the fill box ignores it");
}

TEST(LowerTransformBox, AnOriginOfZeroPercentIsTheBoxsCornerAndNotTheOriginOfTheUserSpace) {
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" style=\"transform-box:fill-box;transform-origin:0% 0%;transform:rotate(20deg)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" transform=\"rotate(20 10 20)\"/>", "0% 0%");
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" style=\"transform-box:fill-box;transform-origin:left top;transform:rotate(20deg)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" transform=\"rotate(20 10 20)\"/>", "left top");
}

TEST(LowerTransformBox, APercentageTranslationIsAFractionOfTheBoxsWidthAndHeight) {
  ExpectSame("<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" style=\"transform-box:fill-box;transform:translate(50%, 100%)\"/>",
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" transform=\"translate(20 10)\"/>", "translate");
}

TEST(LowerTransformBox, TheBoxOfAGroupIsTheUnionOfWhatItsChildrenDrawEachUnderItsOwnTransform) {
  ExpectSame("<g style=\"transform-box:fill-box;transform-origin:center;transform:rotate(90deg)\">"
             "<rect x=\"10\" y=\"10\" width=\"20\" height=\"10\"/><rect x=\"60\" y=\"30\" width=\"10\" height=\"10\" transform=\"translate(10 20)\"/></g>",
      "<g transform=\"rotate(90 45 35)\"><rect x=\"10\" y=\"10\" width=\"20\" height=\"10\"/><rect x=\"60\" y=\"30\" width=\"10\" height=\"10\" transform=\"translate(10 20)\"/></g>",
      "a group: (10, 10) to (80, 60), centre (45, 35)");
}

TEST(LowerTransformBox, TheChildOfAClipPathAndAnElementWithAFilterTakeTheirBoxToo) {
  const std::string boxed = "style=\"transform-box:fill-box;transform-origin:center;transform:rotate(45deg)\"";
  ExpectSame("<defs><clipPath id=\"c\"><rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" " + boxed + "/></clipPath></defs>"
             "<rect width=\"200\" height=\"100\" clip-path=\"url(#c)\"/>",
      "<defs><clipPath id=\"c\"><rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" transform=\"rotate(45 30 25)\"/></clipPath></defs>"
      "<rect width=\"200\" height=\"100\" clip-path=\"url(#c)\"/>", "a clipPath's child");
  ExpectSame("<defs><filter id=\"f\"><feOffset dx=\"3\"/></filter></defs>"
             "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" filter=\"url(#f)\" " + boxed + "/>",
      "<defs><filter id=\"f\"><feOffset dx=\"3\"/></filter></defs>"
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" filter=\"url(#f)\" transform=\"rotate(45 30 25)\"/>", "an element with a filter");
  ExpectSame("<defs><mask id=\"m\"><rect width=\"200\" height=\"100\" fill=\"white\"/></mask></defs>"
             "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" mask=\"url(#m)\" " + boxed + "/>",
      "<defs><mask id=\"m\"><rect width=\"200\" height=\"100\" fill=\"white\"/></mask></defs>"
      "<rect x=\"10\" y=\"20\" width=\"40\" height=\"10\" mask=\"url(#m)\" transform=\"rotate(45 30 25)\"/>", "an element with a mask");
}

TEST(LowerTransformBox, AClipsChildWhoseBoxCannotBeMadeRefusesTheClippedElementAndNamesTheBox) {
  Lowered l;
  LowerSvg(Doc("<defs><clipPath id=\"c\"><path d=\"M0 0L1e300 1e300\" style=\"transform-box:fill-box;transform:rotate(10deg)\"/></clipPath></defs>"
               "<rect width=\"200\" height=\"100\" clip-path=\"url(#c)\"/>"),
      l);
  EXPECT_NE(l.Refusal(GVEC_LOWER_TRANSFORM_BOX), nullptr);
}

TEST(LowerTransformBox, TheBoxOfTextIsMadeAndItsTransformIsNotRefusedForIt) {
  Lowered l;
  LowerSvg(Doc("<text x=\"10\" y=\"50\" style=\"transform-box:fill-box;transform-origin:center;transform:rotate(10deg)\">A</text>"), l);
  EXPECT_EQ(l.Refusal(GVEC_LOWER_TRANSFORM_BOX), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
