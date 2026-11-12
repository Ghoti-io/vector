/**
 * @file
 *
 * Style sheets: the rules of `style` elements applied through the cascade the document already has. The selectors (type, class, id,
 * attribute, universal, combinators), the order the cascade gives (a rule over a presentation attribute, the `style` attribute over a
 * rule, `!important`, specificity, the order of appearance), a sheet that comes after what it styles, the `style` elements that are not
 * CSS, what is reported when a rule is dropped, and the sheets that cannot be applied (`@import`, `@media`).
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** The green or red channel of the first fill, to say which rule won. */
bool FirstFill(const std::string & body, Lowered & l) {
  LowerSvg(Svg("width=\"20\" height=\"20\"", body), l);
  return l.Items() >= 1 && l.Item(0).kind == GSHSCENE_ITEM_FILL && l.Item(0).paint.kind == GSHSCENE_PAINT_SOLID;
}

bool IsGreen(const Lowered & l) {
  const GSHSCENE_Paint & p = l.Item(0).paint;
  return p.rgba[0] == 0.0 && p.rgba[1] > 0.4 && p.rgba[2] == 0.0;
}

bool IsRed(const Lowered & l) {
  const GSHSCENE_Paint & p = l.Item(0).paint;
  return p.rgba[0] == 1.0 && p.rgba[1] == 0.0 && p.rgba[2] == 0.0;
}

std::string Shape(const std::string & attributes = "") {
  return "<path id=\"a\" d=\"M0 0H4V4H0Z\" " + attributes + "/>";
}

std::string Style(const std::string & rules) {
  return "<style>" + rules + "</style>";
}

}  // namespace

TEST(LowerSheets, ATypeSelectorStylesEveryElementOfThatName) {
  Lowered l;
  ASSERT_TRUE(FirstFill(Style("path { fill: green }") + Shape(), l));
  EXPECT_TRUE(IsGreen(l));
}

TEST(LowerSheets, AClassSelectorMatchesAnyWordOfTheClassAttribute) {
  Lowered l;
  ASSERT_TRUE(FirstFill(Style(".b { fill: green }") + Shape("class=\"a  b\tc\""), l));
  EXPECT_TRUE(IsGreen(l));
  Lowered none;
  ASSERT_TRUE(FirstFill(Style(".b { fill: green }") + Shape("class=\"ab\""), none));
  EXPECT_FALSE(IsGreen(none));
}

TEST(LowerSheets, AnIdSelectorAndAnAttributeSelectorMatch) {
  Lowered id;
  ASSERT_TRUE(FirstFill(Style("#a { fill: green }") + Shape(), id));
  EXPECT_TRUE(IsGreen(id));
  Lowered attr;
  ASSERT_TRUE(FirstFill(Style("[d] { fill: green }") + Shape(), attr));
  EXPECT_TRUE(IsGreen(attr));
}

TEST(LowerSheets, TheUniversalSelectorAndTheCombinatorsMatchTheDocumentsTree) {
  Lowered child;
  ASSERT_TRUE(FirstFill(Style("svg > path { fill: green }") + Shape(), child));
  EXPECT_TRUE(IsGreen(child));
  Lowered grand;
  ASSERT_TRUE(FirstFill(Style("svg > path { fill: green }") + "<g>" + Shape() + "</g>", grand));
  EXPECT_FALSE(IsGreen(grand));
  Lowered descendant;
  ASSERT_TRUE(FirstFill(Style("svg path { fill: green }") + "<g>" + Shape() + "</g>", descendant));
  EXPECT_TRUE(IsGreen(descendant));
  Lowered sibling;
  ASSERT_TRUE(FirstFill(Style("g + path { fill: green }") + "<g/>" + Shape(), sibling));
  EXPECT_TRUE(IsGreen(sibling));
  Lowered nth;
  ASSERT_TRUE(FirstFill(Style("path:first-child { fill: green }") + "<g/>" + Shape(), nth));
  EXPECT_FALSE(IsGreen(nth));
}

TEST(LowerSheets, ARuleBeatsAPresentationAttributeAndTheStyleAttributeBeatsARule) {
  Lowered rule;
  ASSERT_TRUE(FirstFill(Style("path { fill: green }") + Shape("fill=\"red\""), rule));
  EXPECT_TRUE(IsGreen(rule));
  Lowered attribute;
  ASSERT_TRUE(FirstFill(Style("path { fill: red }") + Shape("style=\"fill: green\""), attribute));
  EXPECT_TRUE(IsGreen(attribute));
}

TEST(LowerSheets, ImportantBeatsTheLaterRuleAndSpecificityBeatsOrder) {
  Lowered important;
  ASSERT_TRUE(FirstFill(Style("#a { fill: green !important } #a { fill: red }") + Shape(), important));
  EXPECT_TRUE(IsGreen(important));
  Lowered specific;
  ASSERT_TRUE(FirstFill(Style("#a { fill: green } path { fill: red }") + Shape(), specific));
  EXPECT_TRUE(IsGreen(specific));
  Lowered order;
  ASSERT_TRUE(FirstFill(Style("path { fill: red } path { fill: green }") + Shape(), order));
  EXPECT_TRUE(IsGreen(order));
}

TEST(LowerSheets, ASheetAfterWhatItStylesAndInsideCDataStillApplies) {
  Lowered after;
  ASSERT_TRUE(FirstFill(Shape() + Style("path { fill: green }"), after));
  EXPECT_TRUE(IsGreen(after));
  Lowered cdata;
  ASSERT_TRUE(FirstFill("<style><![CDATA[\n path { fill: green }\n]]></style>" + Shape(), cdata));
  EXPECT_TRUE(IsGreen(cdata));
}

TEST(LowerSheets, SeveralStyleElementsApplyInDocumentOrder) {
  Lowered l;
  ASSERT_TRUE(FirstFill(Style("path { fill: red }") + Shape() + Style("path { fill: green }"), l));
  EXPECT_TRUE(IsGreen(l));
}

TEST(LowerSheets, AStyleElementOfAnotherTypeIsNotASheet) {
  Lowered l;
  ASSERT_TRUE(FirstFill("<style type=\"text/plain\">path { fill: green }</style>" + Shape("fill=\"red\""), l));
  EXPECT_TRUE(IsRed(l));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  Lowered css;
  ASSERT_TRUE(FirstFill("<style type=\"text/css\">path { fill: green }</style>" + Shape("fill=\"red\""), css));
  EXPECT_TRUE(IsGreen(css));
}

TEST(LowerSheets, ACloneMadeByUseIsStyledAsTheElementItClones) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", Style("defs > path { fill: green }") + "<defs>" + Shape() + "</defs><use href=\"#a\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  const GSHSCENE_List * picture = gshscene_resource_picture(l.Item(0).resource);
  ASSERT_NE(picture, nullptr);
  ASSERT_EQ(gshscene_list_count(picture), (size_t) 1);
  const GSHSCENE_Item * fill = nullptr;
  ASSERT_EQ(gshscene_list_item(picture, 0, &fill), GSHSCENE_OK);
  EXPECT_EQ(fill->paint.rgba[0], 0.0);
  EXPECT_GT(fill->paint.rgba[1], 0.4);
}

TEST(LowerSheets, ARuleThatIsNotValidIsDroppedAndReportedTheRestApply) {
  Lowered l;
  ASSERT_TRUE(FirstFill(Style("path:nonsense { fill: red } path { fill: green; no-such-property: 1 }") + Shape(), l));
  EXPECT_TRUE(IsGreen(l));
  EXPECT_GE(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_STYLE_INVALID), (size_t) 1);
}

TEST(LowerSheets, AnImportIsNeverFetchedAndRefusesTheSheetsElement) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", Style("@import \"a.css\";") + Shape("fill=\"red\"")), l);
  EXPECT_EQ(l.Refusals(), (size_t) 1);
  EXPECT_EQ(l.Items(), (size_t) 1);  // the shape is drawn as the document has it; the refusal says its sheet was not applied
}

TEST(LowerSheets, AConditionalRuleRefusesTheSheetsElement) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", Style("@media screen { path { fill: green } }") + Shape("fill=\"red\"")), l);
  EXPECT_EQ(l.Refusals(), (size_t) 1);
  EXPECT_EQ(l.Items(), (size_t) 1);  // the shape is drawn as the document has it; the refusal says its sheet was not applied
}

TEST(LowerSheets, ASheetOfOnlyWhiteSpaceOrCommentsChangesNothing) {
  Lowered l;
  ASSERT_TRUE(FirstFill(Style("  \n ") + Style("/* nothing */") + Shape("fill=\"green\""), l));
  EXPECT_TRUE(IsGreen(l));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
