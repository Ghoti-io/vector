/**
 * @file
 *
 * Text: the character data of a `text` element as glyph runs of the waist, with the fonts the caller gives. The tests use two faces of a
 * font the generator writes (tests/data/fonts: every glyph a box, 600 units wide, 700 in the bold face, a space 300) so that the
 * layout is arithmetic: a font size of 100 makes the advance 60 (70 bold), a space 30.
 */

#include <ghoti.io/shape-scene/glyph.h>
#include <ghoti.io/shape-scene/glyph_provider.h>

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** The font set of tests/data/fonts, with `sans-serif` and `serif` for the family of the test font. */
struct Fonts {
  GFNT_FontSet * set = nullptr;
  GVEC_Fonts * fonts = nullptr;
  Fonts() {
    const char * dirs[] = {"tests/data/fonts"};
    static const GFNT_FontAlias aliases[] = {{"sans-serif", "Vector Test"}, {"serif", "Vector Test"}};
    EXPECT_EQ(gfnt_fontset_scan(dirs, 1, aliases, 2, nullptr, &set, nullptr), GFNT_OK);
    EXPECT_EQ(gvec_fonts_new(set, nullptr, &fonts), GVEC_OK);
  }
  ~Fonts() {
    gvec_fonts_free(fonts);
    gfnt_fontset_free(set);
  }
};

bool LowerText(Fonts & f, const std::string & attributes, const std::string & content, Lowered & l, const std::string & root = "") {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\" " + root, "<text " + attributes + ">" + content + "</text>"), l, 0, 0, &options);
  return l.Items() >= 1 && l.Item(0).kind == GSHSCENE_ITEM_FILL;
}

bool ReadRun(const Lowered & l, size_t index, GSHSCENE_Glyph_Run & out) {
  return l.Items() > index && gshscene_resource_glyph_run(l.Item(index).resource, &out) == GSHSCENE_OK;
}

}  // namespace

TEST(LowerText, ACharacterIsAGlyphOfTheFaceThatMapsItAtTheFontSize) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "x=\"10\" y=\"50\"", "AB A", l));
  ASSERT_EQ(l.Items(), (size_t) 1);
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.source, GSHSCENE_GLYPHS_FROM_FONT);
  EXPECT_EQ(run.size, 100.0);
  EXPECT_EQ(run.hint_mode, (uint32_t) GSHSCENE_HINT_NONE);
  ASSERT_EQ(run.glyph_count, (size_t) 4);
  EXPECT_EQ(run.glyphs[0].x, 0.0);
  EXPECT_EQ(run.glyphs[1].x, 60.0);
  EXPECT_EQ(run.glyphs[2].x, 120.0);   // the space is 30 wide
  EXPECT_EQ(run.glyphs[3].x, 150.0);
  EXPECT_EQ(run.glyphs[0].id, run.glyphs[3].id);
  EXPECT_NE(run.glyphs[0].id, run.glyphs[1].id);
  EXPECT_EQ(l.Item(0).map.t[0], 10.0);
  EXPECT_EQ(l.Item(0).map.t[1], 50.0);
  EXPECT_EQ(l.Item(0).paint.rgba[3], 1.0);
}

TEST(LowerText, TextAnchorMovesTheLineByItsWholeAdvance) {
  Fonts f;
  Lowered start, middle, end;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "text-anchor=\"start\"", "AA", start));
  ASSERT_TRUE(ReadRun(start, 0, run));
  EXPECT_EQ(run.glyphs[0].x, 0.0);
  ASSERT_TRUE(LowerText(f, "text-anchor=\"middle\"", "AA", middle));
  ASSERT_TRUE(ReadRun(middle, 0, run));
  EXPECT_EQ(run.glyphs[0].x, -60.0);
  ASSERT_TRUE(LowerText(f, "text-anchor=\"end\"", "AA", end));
  ASSERT_TRUE(ReadRun(end, 0, run));
  EXPECT_EQ(run.glyphs[0].x, -120.0);
}

TEST(LowerText, WhiteSpaceIsCollapsedUnlessItIsPreserved) {
  Fonts f;
  Lowered collapsed, preserved;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "", "  A \n\t  B  ", collapsed));
  ASSERT_TRUE(ReadRun(collapsed, 0, run));
  EXPECT_EQ(run.glyph_count, (size_t) 3);  // "A B": the ends are stripped, the newline is removed, the rest is one space
  ASSERT_TRUE(LowerText(f, "xml:space=\"preserve\"", " A  B ", preserved, "xmlns:xml=\"http://www.w3.org/XML/1998/namespace\""));
  ASSERT_TRUE(ReadRun(preserved, 0, run));
  EXPECT_EQ(run.glyph_count, (size_t) 6);
}

TEST(LowerText, TheWeightPicksTheFaceAndSoTheAdvance) {
  Fonts f;
  Lowered regular, bold;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "font-weight=\"400\"", "AA", regular));
  ASSERT_TRUE(ReadRun(regular, 0, run));
  EXPECT_EQ(run.glyphs[1].x, 60.0);
  ASSERT_TRUE(LowerText(f, "font-weight=\"bold\"", "AA", bold));
  ASSERT_TRUE(ReadRun(bold, 0, run));
  EXPECT_EQ(run.glyphs[1].x, 70.0);
  EXPECT_NE(regular.Item(0).resource, bold.Item(0).resource);
}

TEST(LowerText, ANameInTheFamilyListThatTheFontsDoNotHaveIsSkipped) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "font-family=\"Nothing, 'Vector Test'\"", "AA", l));
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.glyphs[1].x, 60.0);
}

TEST(LowerText, TheFontSizeIsTheSizeOfTheRun) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "font-size=\"50\"", "AA", l));
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.size, 50.0);
  EXPECT_EQ(run.glyphs[1].x, 30.0);
}

TEST(LowerText, TheFillIsThePaintOfTheItemAndNoneDrawsNothing) {
  Fonts f;
  Lowered green, none;
  ASSERT_TRUE(LowerText(f, "fill=\"green\"", "A", green));
  EXPECT_GT(green.Item(0).paint.rgba[1], 0.4);
  LowerText(f, "fill=\"none\"", "A", none);
  EXPECT_EQ(none.Items(), (size_t) 0);
  EXPECT_EQ(none.Refusals(), (size_t) 0);
}

TEST(LowerText, WithoutFontsTheElementIsRefusedWhole) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<text>A</text>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  EXPECT_NE(l.Refusal(GVEC_LOWER_TEXT), nullptr);
}

TEST(LowerText, SpansAStrokeAndAGradientAreRefusedUntilTheyAreLowered) {
  Fonts f;
  Lowered span, stroke, gradient;
  LowerText(f, "", "A<tspan>B</tspan>", span);
  EXPECT_NE(span.Refusal(GVEC_LOWER_TEXT), nullptr);
  LowerText(f, "stroke=\"red\"", "A", stroke);
  EXPECT_NE(stroke.Refusal(GVEC_LOWER_TEXT), nullptr);
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LowerSvg(Svg("width=\"20\" height=\"20\" font-family=\"sans-serif\"",
               "<linearGradient id=\"g\"><stop/></linearGradient><text fill=\"url(#g)\">A</text>"),
      gradient, 0, 0, &options);
  EXPECT_NE(gradient.Refusal(GVEC_LOWER_TEXT), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
