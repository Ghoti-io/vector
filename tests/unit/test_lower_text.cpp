/**
 * @file
 *
 * Text: the character data of a `text` element as glyph runs of the waist, with the fonts the caller gives. The tests use two faces of a
 * font the generator writes (tests/data/fonts: every glyph a box, 600 units wide, 700 in the bold face, a space 300) so that the
 * layout is arithmetic: a font size of 100 makes the advance 60 (70 bold), a space 30.
 */

#include <ghoti.io/shape-geometry/path.h>
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
  EXPECT_EQ(run.glyphs[0].x, 10.0);    // the glyphs carry their positions in the element's space
  EXPECT_EQ(run.glyphs[1].x, 70.0);
  EXPECT_EQ(run.glyphs[2].x, 130.0);   // the space is 30 wide
  EXPECT_EQ(run.glyphs[3].x, 160.0);
  EXPECT_EQ(run.glyphs[0].y, 50.0);
  EXPECT_EQ(run.glyphs[0].id, run.glyphs[3].id);
  EXPECT_NE(run.glyphs[0].id, run.glyphs[1].id);
  EXPECT_EQ(l.Item(0).map.t[0], 0.0);
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

TEST(LowerText, AStrokeIsAnItemOfTheRunAndPaintOrderSaysWhichComesFirst) {
  Fonts f;
  Lowered l, stroke_first;
  ASSERT_TRUE(LowerText(f, "fill=\"green\" stroke=\"red\" stroke-width=\"3\"", "A", l));
  ASSERT_EQ(l.Items(), (size_t) 2);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(l.Item(0).resource, l.Item(1).resource);  // one run, filled and stroked
  LowerText(f, "fill=\"green\" stroke=\"red\" paint-order=\"stroke\"", "A", stroke_first);
  ASSERT_EQ(stroke_first.Items(), (size_t) 2);
  EXPECT_EQ(stroke_first.Item(0).kind, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(stroke_first.Item(1).kind, GSHSCENE_ITEM_FILL);
}

TEST(LowerText, AGradientIsPlacedInTheBoxOfTheWholeText) {
  Fonts f;
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  Lowered l;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\"",
               "<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>"
               "<text x=\"10\" y=\"100\" fill=\"url(#g)\">AA</text>"),
      l, 0, 0, &options);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Item(0).paint.kind, GSHSCENE_PAINT_GRADIENT);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerText, TheOpacityOfASpanIsAGroupAroundItsRuns) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "", "A<tspan opacity=\"0.5\">B</tspan>", l));
  ASSERT_EQ(l.Items(), (size_t) 4);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(1).group.opacity, 0.5);
  EXPECT_EQ(l.Item(2).kind, GSHSCENE_ITEM_FILL);
}

TEST(LowerText, OptimizeSpeedAsksForNoAntialiasing) {
  Fonts f;
  Lowered crisp, plain;
  ASSERT_TRUE(LowerText(f, "text-rendering=\"optimizeSpeed\"", "A", crisp));
  EXPECT_EQ(crisp.Item(0).antialias, (uint32_t) GSHSCENE_ANTIALIAS_NONE);
  ASSERT_TRUE(LowerText(f, "text-rendering=\"geometricPrecision\"", "A", plain));
  EXPECT_EQ(plain.Item(0).antialias, (uint32_t) GSHSCENE_ANTIALIAS_AREA);
}

TEST(LowerText, ASpanWithAnotherStyleIsItsOwnRunAndTheLineContinuesAcrossIt) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"50\"", "A<tspan fill=\"green\" font-weight=\"bold\">BB</tspan>A", l));
  ASSERT_EQ(l.Items(), (size_t) 3);
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.glyph_count, (size_t) 1);
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_EQ(run.glyph_count, (size_t) 2);
  EXPECT_EQ(run.glyphs[0].x, 60.0);    // after the first A
  EXPECT_EQ(run.glyphs[1].x, 130.0);   // the bold advance is 70
  EXPECT_GT(l.Item(1).paint.rgba[1], 0.4);
  EXPECT_EQ(l.Item(0).paint.rgba[1], 0.0);
  ASSERT_TRUE(ReadRun(l, 2, run));
  EXPECT_EQ(run.glyphs[0].x, 200.0);
}

TEST(LowerText, AnAbsoluteXOnASpanStartsAChunkAndTheAnchorIsOfEachChunk) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "x=\"100\" y=\"20\" text-anchor=\"middle\"", "AA<tspan x=\"300\" y=\"60\">AAAA</tspan>", l));
  ASSERT_EQ(l.Items(), (size_t) 2);
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.glyphs[0].x, 100.0 - 60.0);
  EXPECT_EQ(run.glyphs[0].y, 20.0);
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_EQ(run.glyphs[0].x, 300.0 - 120.0);
  EXPECT_EQ(run.glyphs[0].y, 60.0);
}

TEST(LowerText, ListsOfPositionsApplyToTheCharactersInOrderAndAnInnerListWins) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "x=\"10 100 200\" y=\"30\"", "AAAA", l));
  // A chunk begins at each x: three runs' worth of glyphs, the last chunk holding the fourth character after the third.
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(ReadRun(l, 0, run));
  ASSERT_EQ(run.glyph_count, (size_t) 4);
  EXPECT_EQ(run.glyphs[0].x, 10.0);
  EXPECT_EQ(run.glyphs[1].x, 100.0);
  EXPECT_EQ(run.glyphs[2].x, 200.0);
  EXPECT_EQ(run.glyphs[3].x, 260.0);
  Lowered inner;
  ASSERT_TRUE(LowerText(f, "x=\"0 50\"", "A<tspan x=\"400\">AA</tspan>", inner));
  ASSERT_TRUE(ReadRun(inner, 0, run));
  EXPECT_EQ(run.glyphs[0].x, 0.0);
  ASSERT_TRUE(ReadRun(inner, 1, run));
  EXPECT_EQ(run.glyphs[0].x, 400.0);  // the span's list gives the second character x 400, not the text's 50
}

TEST(LowerText, DxAndDyMoveThePenForTheCharactersAfterThem) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"40\" dx=\"0 10\" dy=\"0 5\"", "AAA", l));
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.glyphs[0].x, 0.0);
  EXPECT_EQ(run.glyphs[1].x, 70.0);    // 60 and the 10 of dx
  EXPECT_EQ(run.glyphs[1].y, 45.0);
  EXPECT_EQ(run.glyphs[2].x, 130.0);
  EXPECT_EQ(run.glyphs[2].y, 45.0);    // dy stays
}

TEST(LowerText, WhiteSpaceRunsAcrossSpansAndASpanThatIsNotDisplayedTakesNoRoom) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "", "A <tspan> B</tspan><tspan display=\"none\">XXXX</tspan> A", l));
  uint64_t glyphs = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    if (ReadRun(l, i, run)) {
      glyphs += run.glyph_count;
    }
  }
  EXPECT_EQ(glyphs, (uint64_t) 5);  // "A B A": the white space collapses across the spans, and the span that is not displayed adds nothing
}

TEST(LowerText, AHiddenSpanKeepsItsRoomAndDrawsNothing) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "x=\"0\"", "A<tspan visibility=\"hidden\">BB</tspan>A", l));
  ASSERT_EQ(l.Items(), (size_t) 2);
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_EQ(run.glyphs[0].x, 180.0);
}

TEST(LowerText, TextPathIsRefusedUntilItIsLowered) {
  Fonts f;
  Lowered path;
  LowerText(f, "", "A<textPath href=\"#p\">B</textPath>", path);
  EXPECT_NE(path.Refusal(GVEC_LOWER_TEXT), nullptr);
}

TEST(LowerText, EachGlyphHasItsRotateAndTheLastValueRepeats) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"50\" rotate=\"0 90\"", "AAA", l));
  ASSERT_EQ(l.Items(), (size_t) 3);  // the first glyph, then one run for each rotated glyph
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_EQ(run.glyph_count, (size_t) 1);
  EXPECT_EQ(run.glyphs[0].x, 60.0);
  // The map turns the glyph a quarter about its own origin (60, 50): the point (60, 50) stays, (61, 50) goes to (60, 51).
  const GSHGEOM_Map & m = l.Item(1).map;
  EXPECT_DOUBLE_EQ(m.m[0][0], 0.0);
  EXPECT_DOUBLE_EQ(m.m[1][0], 1.0);
  EXPECT_DOUBLE_EQ(m.m[0][0] * 60 + m.m[0][1] * 50 + m.t[0], 60.0);
  EXPECT_DOUBLE_EQ(m.m[1][0] * 60 + m.m[1][1] * 50 + m.t[1], 50.0);
  EXPECT_DOUBLE_EQ(l.Item(2).map.m[1][0], 1.0);
}

TEST(LowerText, UnderlineAndLineThroughAreRectanglesAcrossTheGlyphsInTheFacesMetrics) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "x=\"10\" y=\"100\" text-decoration=\"underline line-through\"", "AA", l));
  // The underline under the text, the glyph run, the line-through over it.
  ASSERT_EQ(l.Items(), (size_t) 3);
  GSHGEOM_Bounds under, strike;
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(l.Item(0).resource), &under), GSHGEOM_OK);
  EXPECT_DOUBLE_EQ(under.min.x, 10.0);
  EXPECT_DOUBLE_EQ(under.max.x, 130.0);
  EXPECT_LT(under.min.y, under.max.y);
  EXPECT_GT(under.min.y, 100.0);          // below the baseline
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(l.Item(2).resource), &strike), GSHGEOM_OK);
  EXPECT_LT(strike.max.y, 100.0);         // above it
}

TEST(LowerText, ADecorationHasThePaintOfTheElementThatDeclaredItAndAnAncestorsTheTextsOwn) {
  Fonts f;
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  Lowered own, outside;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\"",
               "<text x=\"0\" y=\"100\" fill=\"black\">A<tspan fill=\"green\" text-decoration=\"underline\">A</tspan></text>"),
      own, 0, 0, &options);
  ASSERT_EQ(own.Items(), (size_t) 3);   // the span's underline (under the text), then the two glyph runs
  EXPECT_GT(own.Item(0).paint.rgba[1], 0.4);  // the underline is the span's green
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\"",
               "<g fill=\"green\" text-decoration=\"underline\"><text x=\"0\" y=\"100\" fill=\"red\">A</text></g>"),
      outside, 0, 0, &options);
  ASSERT_EQ(outside.Items(), (size_t) 2);
  EXPECT_EQ(outside.Item(0).paint.rgba[0], 1.0);   // the underline is the text's red, not the g's green
  EXPECT_EQ(outside.Item(1).paint.rgba[0], 1.0);
}

TEST(LowerText, ADecorationOverGlyphsThatRotateOrChangeBaselineIsRefused) {
  Fonts f;
  Lowered rotated, shifted;
  LowerText(f, "rotate=\"10\" text-decoration=\"underline\"", "AA", rotated);
  EXPECT_NE(rotated.Refusal(GVEC_LOWER_TEXT), nullptr);
  LowerText(f, "dy=\"0 10\" text-decoration=\"underline\"", "AA", shifted);
  EXPECT_NE(shifted.Refusal(GVEC_LOWER_TEXT), nullptr);
}

TEST(LowerText, LetterSpacingFollowsEveryCharacterAndWordSpacingEverySpace) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "x=\"0\" letter-spacing=\"10\" word-spacing=\"5\"", "A A", l));
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.glyphs[0].x, 0.0);
  EXPECT_EQ(run.glyphs[1].x, 70.0);    // 60 and the letter-spacing
  EXPECT_EQ(run.glyphs[2].x, 70.0 + 30.0 + 10.0 + 5.0);  // the space's 30, its letter-spacing and its word-spacing
}

TEST(LowerText, ASpanHasItsOwnLetterSpacingAndAPercentageIsOfTheViewport) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "x=\"0\"", "A<tspan letter-spacing=\"20\">A</tspan>A", l));
  ASSERT_EQ(l.Items(), (size_t) 3);
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_EQ(run.glyphs[0].x, 60.0);
  Lowered p;
  ASSERT_TRUE(LowerText(f, "x=\"0\" letter-spacing=\"5%\"", "AA", p));
  ASSERT_TRUE(ReadRun(p, 0, run));
  EXPECT_EQ(run.glyphs[1].x, 60.0 + 20.0);  // 5% of the 400 wide viewport
}

TEST(LowerText, TextLengthSpreadsTheGlyphsAndSpacingAndGlyphsStretchesThem) {
  Fonts f;
  Lowered spaced, stretched;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "x=\"0\" textLength=\"300\"", "AAA", spaced));
  ASSERT_TRUE(ReadRun(spaced, 0, run));
  EXPECT_EQ(run.glyphs[0].x, 0.0);
  EXPECT_EQ(run.glyphs[1].x, 60.0 + 60.0);  // 180 wide, 300 asked: 120 more over the two gaps
  EXPECT_EQ(run.glyphs[2].x, 240.0);
  ASSERT_TRUE(LowerText(f, "x=\"10\" textLength=\"360\" lengthAdjust=\"spacingAndGlyphs\"", "AAA", stretched));
  ASSERT_TRUE(ReadRun(stretched, 0, run));
  EXPECT_EQ(run.glyphs[1].x, 70.0);   // the glyphs keep their positions; the map stretches the run about the start of the text
  EXPECT_EQ(stretched.Item(0).map.m[0][0], 2.0);
  EXPECT_EQ(stretched.Item(0).map.t[0], -10.0);
}

TEST(LowerText, KerningCanBeSwitchedOffSmallCapsAskForTheFacesOwnAndAnInvalidLengthIsIgnored) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "style=\"font-kerning: none\"", "AA", l));
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.glyph_count, (size_t) 2);
  Lowered caps, negative;
  ASSERT_TRUE(LowerText(f, "font-variant=\"small-caps\"", "aA", caps));
  EXPECT_EQ(caps.Refusals(), (size_t) 0);
  ASSERT_TRUE(LowerText(f, "textLength=\"-50\"", "AA", negative));
  ASSERT_TRUE(ReadRun(negative, 0, run));
  EXPECT_EQ(run.glyphs[1].x, 60.0);
}

TEST(LowerText, DecorationOnVerticalTextAndFontSizeAdjustAreRefusedUntilTheyAreLowered) {
  Fonts f;
  Lowered tb, adjust;
  LowerText(f, "writing-mode=\"tb\" text-decoration=\"underline\"", "A", tb);
  EXPECT_NE(tb.Refusal(GVEC_LOWER_TEXT), nullptr);
  LowerText(f, "font-size-adjust=\"0.5\"", "A", adjust);
  EXPECT_NE(adjust.Refusal(GVEC_LOWER_TEXT), nullptr);
}

TEST(LowerText, TheDominantBaselineIsPutAtTheTextPosition) {
  // The test font: ascent 800, descent -200 and x-height 500 in 1000 units, at a size of 100.
  static const struct {
    const char * value;
    double down;
  } cases[] = {{"alphabetic", 0}, {"central", 30}, {"middle", 25}, {"hanging", 64}, {"mathematical", 40}, {"ideographic", -20},
      {"text-before-edge", 80}, {"text-after-edge", -20}};
  Fonts f;
  for (const auto & c : cases) {
    Lowered l;
    GSHSCENE_Glyph_Run run;
    ASSERT_TRUE(LowerText(f, std::string("y=\"50\" dominant-baseline=\"") + c.value + "\"", "A", l)) << c.value;
    ASSERT_TRUE(ReadRun(l, 0, run)) << c.value;
    EXPECT_DOUBLE_EQ(run.glyphs[0].y, 50.0 + c.down) << c.value;
  }
}

TEST(LowerText, AnAlignmentBaselineOfASpanAndABaselineShiftLiftOrLowerIt) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "y=\"50\"", "A<tspan alignment-baseline=\"hanging\">A</tspan><tspan baseline-shift=\"10\">A</tspan><tspan baseline-shift=\"super\">A</tspan>"
                                          "<tspan baseline-shift=\"sub\">A</tspan><tspan baseline-shift=\"50%\">A</tspan>",
      l));
  ASSERT_EQ(l.Items(), (size_t) 6);
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].y, 50.0 + 64.0);
  ASSERT_TRUE(ReadRun(l, 2, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].y, 40.0);
  ASSERT_TRUE(ReadRun(l, 3, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].y, 50.0 - 100.0 / 3.0);
  ASSERT_TRUE(ReadRun(l, 4, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].y, 50.0 + 20.0);
  ASSERT_TRUE(ReadRun(l, 5, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].y, 0.0);  // 50% of the font size
}

TEST(LowerText, ShiftsOfNestedSpansAddUpAndTheShiftOfTheTextItselfIsIgnored) {
  Fonts f;
  Lowered nested, own;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "y=\"50\"", "<tspan baseline-shift=\"5\"><tspan baseline-shift=\"5\">A</tspan></tspan>", nested));
  ASSERT_TRUE(ReadRun(nested, 0, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].y, 40.0);
  ASSERT_TRUE(LowerText(f, "y=\"50\" baseline-shift=\"super\"", "A", own));
  ASSERT_TRUE(ReadRun(own, 0, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].y, 50.0);
}

TEST(LowerText, RightToLeftTextStartsAtTheRightEdgeOfItsAnchor) {
  Fonts f;
  Lowered ltr, rtl;
  GSHSCENE_Glyph_Run a, b;
  ASSERT_TRUE(LowerText(f, "x=\"100\" y=\"50\"", "AB", ltr));
  ASSERT_TRUE(LowerText(f, "x=\"100\" y=\"50\" direction=\"rtl\"", "AB", rtl));
  ASSERT_TRUE(ReadRun(ltr, 0, a));
  ASSERT_TRUE(ReadRun(rtl, 0, b));
  ASSERT_GT(a.glyph_count, (size_t) 0);
  EXPECT_DOUBLE_EQ(a.glyphs[0].x, 100.0);       // start is the left edge of left-to-right text
  EXPECT_LT(b.glyphs[b.glyph_count - 1].x, 100.0); // and the right edge of right-to-left text: the text lies to the left of x
}

TEST(LowerText, ABidiOverrideSetsTheCharactersInTheDirectionsOrder) {
  Fonts f;
  Lowered plain, over;
  GSHSCENE_Glyph_Run a, b;
  ASSERT_TRUE(LowerText(f, "x=\"10\" y=\"50\"", "AB", plain));
  ASSERT_TRUE(LowerText(f, "x=\"10\" y=\"50\" direction=\"rtl\" unicode-bidi=\"bidi-override\" text-anchor=\"end\"", "AB", over));
  ASSERT_TRUE(ReadRun(plain, 0, a));
  ASSERT_TRUE(ReadRun(over, 0, b));
  ASSERT_EQ(a.glyph_count, (size_t) 2);
  ASSERT_EQ(b.glyph_count, (size_t) 2);
  EXPECT_EQ(a.glyphs[0].id, b.glyphs[1].id);  // the glyphs come in the opposite order
  EXPECT_EQ(a.glyphs[1].id, b.glyphs[0].id);
}

TEST(LowerText, VerticalTextRunsDownTheLineAndItsLettersLieOnTheirSide) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run a, b;
  ASSERT_TRUE(LowerText(f, "x=\"50\" y=\"10\" writing-mode=\"tb\"", "AB", l));
  ASSERT_EQ(l.Items(), (size_t) 2);  // each turned glyph is a run of its own
  ASSERT_TRUE(ReadRun(l, 0, a));
  ASSERT_TRUE(ReadRun(l, 1, b));
  EXPECT_DOUBLE_EQ(a.glyphs[0].y, 10.0);          // the first glyph is at the position of the text
  EXPECT_GT(b.glyphs[0].y, a.glyphs[0].y);        // the next one is below it
  EXPECT_DOUBLE_EQ(a.glyphs[0].x, b.glyphs[0].x); // on the same line
  EXPECT_LT(a.glyphs[0].x, 50.0);                 // whose middle is at x: the baseline is left of it
  Lowered h;
  ASSERT_TRUE(LowerText(f, "x=\"50\" y=\"10\" writing-mode=\"lr-tb\"", "AB", h));
  ASSERT_EQ(h.Items(), (size_t) 1);               // the legacy horizontal values change nothing
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
