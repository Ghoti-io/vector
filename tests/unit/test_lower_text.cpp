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

TEST(LowerText, APositionListIsIndexedByUtf16CodeUnitsSoACharacterBeyondPlaneZeroTakesTwoValues) {
  Fonts f;
  Lowered l;
  // U+1F601 is two code units: `A` is the third, whose x is 30; with one value for each code point it would be 20
  ASSERT_TRUE(LowerText(f, "x=\"10 20 30\" y=\"50\"", "\xF0\x9F\x98\x81" "A", l));
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(ReadRun(l, 0, run));
  ASSERT_GE(run.glyph_count, (size_t) 1);
  EXPECT_EQ(run.glyphs[run.glyph_count - 1].x, 30.0);
  Lowered rotated;
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"50\" rotate=\"0 0 45\"", "\xF0\x9F\x98\x81" "A", rotated));
  bool turned = false;
  for (size_t i = 0; i < rotated.Items(); i++) {
    turned = turned || rotated.Item(i).map.m[0][1] != 0.0;
  }
  EXPECT_TRUE(turned) << "the angle of the third code unit is the A's";
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

TEST(LowerText, ADecorationOnVerticalTextIsASetOfRectsTurnedWithTheLine) {
  Fonts f;
  Lowered tb;
  LowerText(f, "writing-mode=\"tb\" text-decoration=\"underline\"", "A", tb);
  EXPECT_EQ(tb.Refusal(GVEC_LOWER_TEXT), nullptr);
  EXPECT_GE(tb.Items(), (size_t) 2);  // the underline, and the turned glyph
}

TEST(LowerText, FontSizeAdjustKeepsTheXHeightToTheRatioAskedForAndAKerningInAPercentageIsIgnored) {
  // The test font's x-height is half its em, so an adjust of 0.25 draws it at half the size.
  Fonts f;
  Lowered plain, adjust, exheight, none, kerning;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "", "A", plain));
  ASSERT_TRUE(ReadRun(plain, 0, run));
  EXPECT_DOUBLE_EQ(run.size, 100.0);
  ASSERT_TRUE(LowerText(f, "font-size-adjust=\"0.25\"", "AA", adjust));
  EXPECT_EQ(adjust.Refusals(), (size_t) 0);
  ASSERT_TRUE(ReadRun(adjust, 0, run));
  EXPECT_DOUBLE_EQ(run.size, 50.0);
  EXPECT_DOUBLE_EQ(run.glyphs[1].x, 30.0);  // and the advance is that of the smaller size
  ASSERT_TRUE(LowerText(f, "style=\"font-size-adjust: ex-height 0.25\"", "A", exheight));
  ASSERT_TRUE(ReadRun(exheight, 0, run));
  EXPECT_DOUBLE_EQ(run.size, 50.0);
  ASSERT_TRUE(LowerText(f, "font-size-adjust=\"none\"", "A", none));
  ASSERT_TRUE(ReadRun(none, 0, run));
  EXPECT_DOUBLE_EQ(run.size, 100.0);
  ASSERT_TRUE(LowerText(f, "kerning=\"10%\"", "AA", kerning));
  EXPECT_EQ(kerning.Refusals(), (size_t) 0);
  ASSERT_TRUE(ReadRun(kerning, 0, run));
  EXPECT_DOUBLE_EQ(run.glyphs[1].x, 60.0);
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

namespace {
// A text whose content has a textPath on a straight path along x from 10 to 110 at y 50.
bool LowerOnPath(Fonts & f, const std::string & path_attributes, const std::string & text_path, const std::string & content, Lowered & l) {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\"",
               "<path id=\"p\" d=\"M 10 50 L 110 50\" " + path_attributes + "/><text><textPath href=\"#p\" " + text_path + ">" + content + "</textPath></text>"),
      l, 0, 0, &options);
  return true;
}
}  // namespace

TEST(LowerText, TextOnAPathIsSetAlongItWithEachGlyphCentredOnItsDistance) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerOnPath(f, "", "", "A", l));
  ASSERT_GE(l.Items(), (size_t) 2);  // the path's own stroke, then the text
  ASSERT_TRUE(ReadRun(l, 1, run));
  ASSERT_EQ(run.glyph_count, (size_t) 1);
  EXPECT_NEAR(run.glyphs[0].y, 50.0, 1e-9);  // on the path: the glyph origin is on the line
  EXPECT_NEAR(run.glyphs[0].x, 10.0, 1e-9);  // a glyph that starts at the path's start has its origin there
}

TEST(LowerText, StartOffsetMovesTheTextAlongThePathAndAGlyphPastTheEndIsNotDrawn) {
  Fonts f;
  Lowered shifted, past;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerOnPath(f, "", "startOffset=\"20\"", "A", shifted));
  ASSERT_TRUE(ReadRun(shifted, 1, run));
  EXPECT_NEAR(run.glyphs[0].x, 30.0, 1e-9);
  ASSERT_TRUE(LowerOnPath(f, "", "startOffset=\"500\"", "A", past));
  EXPECT_EQ(past.Items(), (size_t) 1);  // only the path
}

TEST(LowerText, ThePathATextPathNamesKeepsItsTransformWhosePercentagesAreOfItsOwnBox) {
  Fonts f;
  Lowered moved;
  GSHSCENE_Glyph_Run run;
  // The path is 100 wide, so half its width is 50: the text starts at 10 + 50 along the moved path.
  ASSERT_TRUE(LowerOnPath(f, "style=\"transform-box:fill-box;transform:translate(50%, 0)\"", "", "A", moved));
  EXPECT_EQ(moved.Refusals(), (size_t) 0);
  ASSERT_TRUE(ReadRun(moved, 1, run));
  EXPECT_NEAR(run.glyphs[0].x, 60.0, 1e-9);
}

namespace {

/** The font set of tests/data/fonts-features: a face whose GSUB has the `f i` ligature (`liga`), the `o o` one (`dlig`) and single
 * substitutions (`ss01`), so that a feature the text asks for changes the glyphs it is shaped to. */
struct FeatureFonts {
  GFNT_FontSet * set = nullptr;
  GVEC_Fonts * fonts = nullptr;
  FeatureFonts() {
    const char * dirs[] = {"tests/data/fonts-features"};
    EXPECT_EQ(gfnt_fontset_scan(dirs, 1, nullptr, 0, nullptr, &set, nullptr), GFNT_OK);
    EXPECT_EQ(gvec_fonts_new(set, nullptr, &fonts), GVEC_OK);
  }
  ~FeatureFonts() {
    gvec_fonts_free(fonts);
    gfnt_fontset_free(set);
  }
};

/** The glyphs the first run of `content` (set at the font size 100 in the face of tests/data/fonts-features) has; `head` is put before the text. */
size_t GlyphsOf(FeatureFonts & f, const std::string & head, const std::string & text_attributes, const std::string & content, Lowered * keep = nullptr,
    const char * family = "Ghoti Fixture Layout GSUB", double * second_x = nullptr) {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  Lowered local;
  Lowered & l = keep ? *keep : local;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"" + std::string(family) + "\"",
               head + "<text x=\"10\" y=\"100\" " + text_attributes + ">" + content + "</text>"), l, 0, 0, &options);
  size_t glyphs = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    GSHSCENE_Glyph_Run run;
    if (l.Item(i).kind == GSHSCENE_ITEM_FILL && gshscene_resource_glyph_run(l.Item(i).resource, &run) == GSHSCENE_OK) {
      glyphs += run.glyph_count;
      if (second_x && run.glyph_count > 1) {
        *second_x = run.glyphs[1].x;
      }
    }
  }
  return glyphs;
}

}  // namespace

TEST(LowerText, TheFontFeaturesOfTheCascadeReachTheShaper) {
  FeatureFonts f;
  EXPECT_EQ(GlyphsOf(f, "", "", "fi"), (size_t) 1) << "the face's ligature, as the default";
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-variant-ligatures: no-common-ligatures\"", "fi"), (size_t) 2);
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-variant-ligatures: none\"", "fi"), (size_t) 2);
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-variant: no-common-ligatures\"", "fi"), (size_t) 2) << "the shorthand";
  EXPECT_EQ(GlyphsOf(f, "<style>.n { font-variant-ligatures: none }</style>", "class=\"n\"", "fi"), (size_t) 2) << "a style sheet";
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-feature-settings: 'liga' 0\"", "fi"), (size_t) 2) << "font-feature-settings";
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-feature-settings: 'liga' off\"", "fi"), (size_t) 2) << "off is a zero";
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-variant-ligatures: none; font-feature-settings: 'liga'\"", "fi"), (size_t) 1)
      << "the settings have the last word over the variant, and a tag alone is on";
  EXPECT_EQ(GlyphsOf(f, "", "", "oo"), (size_t) 2);
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-variant-ligatures: discretionary-ligatures\"", "oo"), (size_t) 1) << "dlig is off until it is asked for";
  // the same ligature has a historical form too, which stops at a mark: an acute between the two o's is skipped only by dlig
  EXPECT_LT(GlyphsOf(f, "", "style=\"font-variant-ligatures: discretionary-ligatures\"", "o&#x301;o"), GlyphsOf(f, "", "style=\"font-variant-ligatures: historical-ligatures\"", "o&#x301;o"))
      << "discretionary ligatures are not the historical ones";
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-feature-settings: 'dlig'\"", "oo"), (size_t) 1);
}

TEST(LowerText, FontKerningAndTheKernFeatureMoveTheSecondGlyphOfAPair) {
  FeatureFonts f;
  double kerned = 0, plain = 0, settings = 0, again = 0;
  GlyphsOf(f, "", "", "AV", nullptr, "Ghoti Fixture Layout GPOS", &kerned);
  GlyphsOf(f, "", "style=\"font-kerning: none\"", "AV", nullptr, "Ghoti Fixture Layout GPOS", &plain);
  GlyphsOf(f, "", "style=\"font-feature-settings: 'kern' 0\"", "AV", nullptr, "Ghoti Fixture Layout GPOS", &settings);
  GlyphsOf(f, "", "style=\"font-kerning: none; font-feature-settings: 'kern' 1\"", "AV", nullptr, "Ghoti Fixture Layout GPOS", &again);
  EXPECT_NE(kerned, plain) << "the pair is kerned unless font-kerning is none";
  EXPECT_EQ(settings, plain) << "font-feature-settings reaches kern too";
  EXPECT_EQ(again, kerned) << "and has the last word over font-kerning";
}

TEST(LowerText, TheFeaturesAreInheritedAndASpanWithOtherFeaturesIsShapedApart) {
  FeatureFonts f;
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-variant-ligatures: none\"", "<tspan>f</tspan><tspan>i</tspan>"), (size_t) 2) << "inherited by the spans";
  EXPECT_EQ(GlyphsOf(f, "", "", "<tspan>f</tspan><tspan>i</tspan>"), (size_t) 1) << "spans of one font are shaped together";
  EXPECT_EQ(GlyphsOf(f, "", "", "<tspan>f</tspan><tspan style=\"font-variant-ligatures: none\">i</tspan>"), (size_t) 2)
      << "a span with other features is its own run, and the ligature does not run across";
  EXPECT_EQ(GlyphsOf(f, "", "", "<tspan style=\"font-variant-ligatures: none\">f</tspan><tspan>i</tspan>"), (size_t) 2) << "in either order";
  EXPECT_EQ(GlyphsOf(f, "", "style=\"font-variant-ligatures: none\"", "<tspan style=\"font-variant-ligatures: normal\">fi</tspan>"), (size_t) 1)
      << "normal puts the default back";
}

TEST(LowerText, LetterSpacingSwitchesLigaturesOffUnlessTheTextAsksForThem) {
  FeatureFonts f;
  EXPECT_EQ(GlyphsOf(f, "", "letter-spacing=\"5\"", "fi"), (size_t) 2);
  EXPECT_EQ(GlyphsOf(f, "", "letter-spacing=\"5\" style=\"font-feature-settings: 'liga' 1, 'clig' 1\"", "fi"), (size_t) 1);
}

TEST(LowerText, TheAlternatesOfFontVariantAreRefusedNotDrawnWithoutThem) {
  FeatureFonts f;
  Lowered l;
  GlyphsOf(f, "", "style=\"font-variant: styleset(1)\"", "fi", &l);
  EXPECT_NE(l.Refusal(GVEC_LOWER_TEXT), nullptr);
}

TEST(LowerText, ATextLengthAndAStartOffsetInFontRelativeUnitsAreTheSameLengthInPixels) {
  Fonts f;
  GSHSCENE_Glyph_Run px, other;
  // The font size is 100: an em is 100 pixels, an ex and a ch fifty, and a rem the root's.
  Lowered base;
  ASSERT_TRUE(LowerText(f, "x=\"0\" textLength=\"300\"", "AAA", base));
  ASSERT_TRUE(ReadRun(base, 0, px));
  for (const char * length : {"3em", "6ex", "6ch", "3rem"}) {
    Lowered l;
    ASSERT_TRUE(LowerText(f, std::string("x=\"0\" textLength=\"") + length + "\"", "AAA", l)) << length;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << length;
    ASSERT_TRUE(ReadRun(l, 0, other)) << length;
    ASSERT_EQ(other.glyph_count, px.glyph_count);
    for (size_t i = 0; i < px.glyph_count; i++) {
      EXPECT_EQ(other.glyphs[i].x, px.glyphs[i].x) << length << " glyph " << i;
    }
  }
  for (const char * position : {"1ch", "0.5rem"}) {
    Lowered l;
    ASSERT_TRUE(LowerText(f, std::string("x=\"") + position + "\" y=\"100\"", "A", l)) << position;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << position;
    ASSERT_TRUE(ReadRun(l, 0, other)) << position;
    EXPECT_EQ(other.glyphs[0].x, 50.0) << position << ": the position of a character in a unit that is not an em or an ex";
  }
  Lowered own;
  ASSERT_TRUE(LowerOnPath(f, "", "font-size=\"50\" startOffset=\"0.2em\"", "A", own));
  ASSERT_TRUE(ReadRun(own, 1, px));
  EXPECT_NEAR(px.glyphs[0].x, 20.0, 1e-9) << "an em of the textPath's own font size (50), not the text's (100)";
  Lowered shifted;
  ASSERT_TRUE(LowerOnPath(f, "", "startOffset=\"0.2em\"", "A", shifted));
  EXPECT_EQ(shifted.Refusals(), (size_t) 0);
  ASSERT_TRUE(ReadRun(shifted, 1, px));
  EXPECT_NEAR(px.glyphs[0].x, 30.0, 1e-9) << "a fifth of an em of 100 pixels along the path";
}

TEST(LowerText, TextOnAPathWithAMethodASideOrATransformIsLowered) {
  Fonts f;
  Lowered stretch, right, moved;
  LowerOnPath(f, "", "method=\"stretch\"", "A", stretch);
  LowerOnPath(f, "", "side=\"right\"", "A", right);
  LowerOnPath(f, "transform=\"translate(5)\"", "", "A", moved);
  EXPECT_EQ(stretch.Refusals() + right.Refusals() + moved.Refusals(), (size_t) 0);
  GSHSCENE_Glyph_Run a, b, c, d;
  Lowered plain;
  ASSERT_TRUE(LowerOnPath(f, "", "", "A", plain));
  ASSERT_TRUE(ReadRun(plain, 1, a));
  ASSERT_TRUE(ReadRun(stretch, 1, b));
  ASSERT_TRUE(ReadRun(moved, 1, c));
  ASSERT_TRUE(ReadRun(right, 1, d));
  EXPECT_DOUBLE_EQ(a.glyphs[0].x, b.glyphs[0].x);          // stretch is read as align
  EXPECT_NEAR(c.glyphs[0].x, a.glyphs[0].x + 5.0, 1e-9);   // the path's own transform moves it
  EXPECT_GT(d.glyphs[0].x, a.glyphs[0].x + 50.0);          // side right starts at the path's far end
}

namespace {
bool LowerBody(Fonts & f, const std::string & body, Lowered & l) {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\"", body), l, 0, 0, &options);
  return true;
}
}  // namespace

TEST(LowerText, ATextLengthOverSeveralChunksIsSharedOutOverAllOfThem) {
  // Two chunks of two glyphs (60 wide each, 240 in all) asked to be 300: 20 more after every glyph but the last.
  Fonts f;
  Lowered spaced, stretched;
  GSHSCENE_Glyph_Run first, second;
  LowerBody(f, "<text textLength=\"300\"><tspan x=\"0\" y=\"50\">AA</tspan><tspan x=\"0\" y=\"150\">AA</tspan></text>", spaced);
  EXPECT_EQ(spaced.Refusals(), (size_t) 0);
  ASSERT_TRUE(ReadRun(spaced, 0, first));
  ASSERT_TRUE(ReadRun(spaced, 1, second));
  EXPECT_DOUBLE_EQ(first.glyphs[0].x, 0.0);
  EXPECT_DOUBLE_EQ(first.glyphs[1].x, 80.0);
  EXPECT_DOUBLE_EQ(second.glyphs[0].x, 0.0);  // an absolute x starts the chunk where it says, whatever came before
  EXPECT_DOUBLE_EQ(second.glyphs[1].x, 80.0);
  LowerBody(f, "<text textLength=\"480\" lengthAdjust=\"spacingAndGlyphs\"><tspan x=\"0\" y=\"50\">AA</tspan><tspan x=\"0\" y=\"150\">AA</tspan></text>", stretched);
  EXPECT_EQ(stretched.Refusals(), (size_t) 0);
  EXPECT_DOUBLE_EQ(stretched.Item(0).map.m[0][0], 2.0);
  EXPECT_DOUBLE_EQ(stretched.Item(1).map.m[0][0], 2.0);
}

TEST(LowerText, TextLengthOnVerticalTextSpacesAlongTheLineAndStretchesItsGlyphsAlongIt) {
  Fonts f;
  Lowered spaced, stretched, turned;
  GSHSCENE_Glyph_Run a, b;
  ASSERT_TRUE(LowerText(f, "x=\"50\" y=\"10\" writing-mode=\"tb\" textLength=\"300\"", "AA", spaced));
  EXPECT_EQ(spaced.Refusals(), (size_t) 0);
  ASSERT_TRUE(ReadRun(spaced, 0, a));
  ASSERT_TRUE(ReadRun(spaced, 1, b));
  EXPECT_DOUBLE_EQ(b.glyphs[0].y - a.glyphs[0].y, 60.0 + 180.0);  // 300 asked of 120: the gap between the two takes the 180
  ASSERT_TRUE(LowerText(f, "x=\"50\" y=\"10\" writing-mode=\"tb\" textLength=\"300\" lengthAdjust=\"spacingAndGlyphs\"", "AA", stretched));
  EXPECT_EQ(stretched.Refusals(), (size_t) 0);
  EXPECT_EQ(stretched.Items(), (size_t) 2);
  {
    // 300 asked of 120: the stretch is 2.5 along the line, which is down: the second glyph's origin, 60 below the first, goes to 150 below it.
    GSHSCENE_Glyph_Run second;
    ASSERT_TRUE(ReadRun(stretched, 1, second));
    GSHGEOM_Point origin = {second.glyphs[0].x, second.glyphs[0].y, 0}, moved;
    ASSERT_EQ(gshgeom_map_apply_point(&stretched.Item(1).map, origin, &moved), GSHGEOM_OK);
    EXPECT_NEAR(moved.x, origin.x, 1e-9);
    EXPECT_NEAR(moved.y, 10.0 + (origin.y - 10.0) * 2.5, 1e-9);
  }
  ASSERT_TRUE(LowerText(f, "x=\"10\" y=\"50\" rotate=\"10\" textLength=\"300\" lengthAdjust=\"spacingAndGlyphs\"", "AA", turned));
  EXPECT_EQ(turned.Refusals(), (size_t) 0);  // a glyph that is turned and stretched is both
  EXPECT_EQ(turned.Items(), (size_t) 2);
}

TEST(LowerText, AUseOfAGroupThatHoldsTextHasABoxForItsContextGradient) {
  Fonts f;
  Lowered l;
  LowerBody(f,
      "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>"
      "<g id=\"c\"><text x=\"10\" y=\"100\" fill=\"context-fill\">AA</text></g></defs><use href=\"#c\" fill=\"url(#g)\"/>",
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  // The use is an instance of a picture, and the text's fill is in the picture.
  ASSERT_GE(l.Items(), (size_t) 1);
  const GSHSCENE_List * picture = gshscene_resource_picture(l.Item(0).resource);
  ASSERT_NE(picture, nullptr);
  bool gradient = false;
  for (size_t i = 0; i < gshscene_list_count(picture); i++) {
    const GSHSCENE_Item * item = nullptr;
    ASSERT_EQ(gshscene_list_item(picture, i, &item), GSHSCENE_OK);
    gradient = gradient || (item->kind == GSHSCENE_ITEM_FILL && item->paint.kind == GSHSCENE_PAINT_GRADIENT);
  }
  EXPECT_TRUE(gradient);
}

TEST(LowerText, VerticalTextOnAPathIsPutAlongItWithItsLettersUprightOnAHorizontalOne) {
  Fonts f;
  Lowered l, stretched;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerOnPath(f, "", "", "A", l));
  (void) l;
  Lowered tb, tbstretched;
  LowerBody(f, "<path id=\"p\" d=\"M 10 50 L 110 50\"/><text writing-mode=\"tb\"><textPath href=\"#p\">A</textPath></text>", tb);
  EXPECT_EQ(tb.Refusals(), (size_t) 0);
  ASSERT_GE(tb.Items(), (size_t) 2);
  ASSERT_TRUE(ReadRun(tb, 1, run));
  EXPECT_NEAR(run.glyphs[0].x, 10.0, 1e-9);  // the glyph's middle is at half its advance along the path
  EXPECT_EQ(tb.Item(1).map.kind, tb.Item(0).map.kind);        // a letter on its side, on a path that runs along the line, is not turned at all
  LowerBody(f, "<path id=\"p\" d=\"M 10 50 L 110 50\"/><text writing-mode=\"tb\" textLength=\"90\" lengthAdjust=\"spacingAndGlyphs\"><textPath href=\"#p\">AA</textPath></text>", tbstretched);
  EXPECT_EQ(tbstretched.Refusals(), (size_t) 0);
  (void) stretched;
}


TEST(LowerText, ATrefIsTheCharacterDataOfTheElementItNamesInTheTrefsOwnStyle) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerBody(f, "<defs><text id=\"t\">AB<tspan>A</tspan></text></defs><text y=\"50\"><tref href=\"#t\" x=\"7\" fill=\"#ff0000\"/></text>", l));
  ASSERT_EQ(l.Items(), (size_t) 1);
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_EQ(run.glyph_count, (size_t) 3);
  EXPECT_DOUBLE_EQ(run.glyphs[0].x, 7.0);       // the tref's own x is read
  EXPECT_GT(l.Item(0).paint.rgba[0], 0.9);      // and its own fill
}

TEST(LowerText, ATrefToATrefOrToNothingDrawsNothing) {
  Fonts f;
  Lowered nested, missing;
  ASSERT_TRUE(LowerBody(f, "<defs><text id=\"a\"><tref href=\"#b\"/></text><text id=\"b\">A</text></defs><text y=\"50\"><tref href=\"#a\"/></text>", nested));
  EXPECT_EQ(nested.Items(), (size_t) 0);
  ASSERT_TRUE(LowerBody(f, "<text y=\"50\"><tref href=\"#nothing\"/></text>", missing));
  EXPECT_EQ(missing.Items(), (size_t) 0);
}

TEST(LowerText, TextInAClipPathIsTheClipOfItsGlyphsOutlines) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerBody(f, "<clipPath id=\"c\"><text x=\"0\" y=\"80\">AB</text></clipPath><rect width=\"300\" height=\"100\" clip-path=\"url(#c)\"/>", l));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_GE(l.Items(), (size_t) 3);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(2).kind, GSHSCENE_ITEM_CLIP_POP);
}

TEST(LowerText, EmAndExInAListOfPositionsAreTheFontSizeAndHalfOfIt) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerText(f, "x=\"0.5em\" y=\"50\" dx=\"1ex\"", "A", l));
  ASSERT_TRUE(ReadRun(l, 0, run));
  EXPECT_DOUBLE_EQ(run.glyphs[0].x, 100.0);  // half of 100, then half of 100
}

TEST(LowerText, ATextPathThatNamesWhatIsNoShapeHasNoCharacters) {
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerBody(f, "<text id=\"t\">A</text><text y=\"50\">B<textPath href=\"#t\">C</textPath></text>", l));
  ASSERT_GE(l.Items(), (size_t) 2);
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_EQ(run.glyph_count, (size_t) 1);  // only the B
}

TEST(LowerText, TheDataOfATextPathsPathAttributeIsThePathAndIsPreferredToItsHref) {
  Fonts f;
  Lowered l;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerBody(f, "<path id=\"p\" d=\"M 0 10 L 100 10\"/><text><textPath href=\"#p\" path=\"M 10 50 L 110 50\">A</textPath></text>", l));
  ASSERT_GE(l.Items(), (size_t) 2);
  ASSERT_TRUE(ReadRun(l, 1, run));
  EXPECT_NEAR(run.glyphs[0].y, 50.0, 1e-9);  // the path attribute's line, not the href's
  EXPECT_NEAR(run.glyphs[0].x, 10.0, 1e-9);
}

TEST(LowerText, TextOnARectIsSetAlongItsOutlineAndADecorationOnAPathIsCutAtEachGlyph) {
  Fonts f;
  Lowered rect, line, plain;
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(LowerBody(f, "<rect id=\"r\" x=\"10\" y=\"50\" width=\"100\" height=\"80\"/><text><textPath href=\"#r\">A</textPath></text>", rect));
  EXPECT_EQ(rect.Refusals(), (size_t) 0);
  ASSERT_GE(rect.Items(), (size_t) 2);
  ASSERT_TRUE(ReadRun(rect, 1, run));
  EXPECT_NEAR(run.glyphs[0].y, 50.0, 1e-9);  // the outline starts at the top left corner and runs right
  ASSERT_TRUE(LowerBody(f, "<path id=\"p\" d=\"M 10 50 L 310 50\"/><text text-decoration=\"underline\"><textPath href=\"#p\">AB</textPath></text>", line));
  EXPECT_EQ(line.Refusals(), (size_t) 0);
  ASSERT_TRUE(LowerBody(f, "<path id=\"p\" d=\"M 10 50 L 310 50\"/><text><textPath href=\"#p\">AB</textPath></text>", plain));
  EXPECT_GT(line.Items(), plain.Items());  // a line for each glyph on the path
}

TEST(LowerText, ADecorationOverGlyphsOnDifferentBaselinesOrTurnedIsAPieceForEachGlyph) {
  Fonts f;
  Lowered flat, dy, turned;
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"50\" text-decoration=\"underline\"", "AB", flat));
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"50\" dy=\"0 10\" text-decoration=\"underline\"", "AB", dy));
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"50\" rotate=\"0 20\" text-decoration=\"underline\"", "AB", turned));
  EXPECT_EQ(flat.Refusals() + dy.Refusals() + turned.Refusals(), (size_t) 0);
  EXPECT_EQ(dy.Items(), flat.Items() + 1);      // one line across both glyphs, or one under each
  EXPECT_GT(turned.Items(), flat.Items() + 1);  // a piece for each glyph, and the turned glyph a run of its own
}

namespace {

/** The font set of tests/data/fonts-color: one face with no outlines whose strike of PNG pictures (CBDT, as Noto Color Emoji is) carries the
 * glyphs of U+0041 to U+0049, and not U+004A (glyph 10), which is in the face and in no strike. */
struct ColorFonts {
  GFNT_FontSet * set = nullptr;
  GVEC_Fonts * fonts = nullptr;
  ColorFonts() {
    const char * dirs[] = {"tests/data/fonts-color"};
    EXPECT_EQ(gfnt_fontset_scan(dirs, 1, nullptr, 0, nullptr, &set, nullptr), GFNT_OK);
    EXPECT_EQ(gvec_fonts_new(set, nullptr, &fonts), GVEC_OK);
  }
  ~ColorFonts() {
    gvec_fonts_free(fonts);
    gfnt_fontset_free(set);
  }
};

size_t ImageFillsOf(const Lowered & l) {
  size_t n = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    n += l.Item(i).kind == GSHSCENE_ITEM_FILL && l.Item(i).paint.kind == GSHSCENE_PAINT_IMAGE;
  }
  return n;
}

void LowerColor(ColorFonts & f, const std::string & content, Lowered & l, const GVEC_Limits * limits = nullptr) {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LoadSvg(Svg("width=\"400\" height=\"200\" font-size=\"24\" font-family=\"Ghoti Fixture Color Strikes\"", "<text x=\"10\" y=\"50\">" + content + "</text>"),
      l.loaded, 0, 0, limits);
  LowerBoth(l.loaded, l, &options);
}

}  // namespace

TEST(LowerText, AGlyphOfAColourStrikeIsAnImageFillAndAGlyphTheStrikeLacksIsBlankNotMissing) {
  ColorFonts f;
  Lowered l;
  LowerColor(f, "AB", l);
  EXPECT_EQ(ImageFillsOf(l), (size_t) 2) << "one picture for each glyph";
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  Lowered blank;
  LowerColor(f, "J", blank);
  EXPECT_EQ(blank.Items(), (size_t) 0) << "glyph 10 is in no strike: nothing to draw";
  EXPECT_EQ(blank.Refusals(), (size_t) 0) << "and not a missing glyph";
  Lowered mixed;
  LowerColor(f, "AJA", mixed);
  EXPECT_EQ(ImageFillsOf(mixed), (size_t) 2) << "the blank glyph between is skipped, the pictures around it drawn";
  EXPECT_EQ(mixed.Refusals(), (size_t) 0);
}

TEST(LowerText, ThePicturesOfColourGlyphsCountAgainstTheDecodedBytesBudget) {
  ColorFonts f;
  Lowered whole;
  LowerColor(f, "A", whole);
  ASSERT_EQ(ImageFillsOf(whole), (size_t) 1);
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_image_decoded_bytes = 1;
  Lowered tiny;
  LowerColor(f, "A", tiny, &limits);
  EXPECT_NE(tiny.Refusal(GVEC_LOWER_LIMIT), nullptr);
  EXPECT_EQ(ImageFillsOf(tiny), (size_t) 0);
}

namespace {

struct ColorPlace {
  uint32_t width = 0, height = 0;
  double scale_x = 0, scale_y = 0, tx = 0, ty = 0;
};

/** The picture of the first glyph of "A" at @p size (the origin is (10, 50)): its pixels and the map of them into the text's space. */
ColorPlace PlaceOfA(ColorFonts & f, const char * size, const char * family = "Ghoti Fixture Color Strikes") {
  ColorPlace out;
  Lowered l;
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LoadSvg(Svg(std::string("width=\"400\" height=\"200\" font-size=\"") + size + "\" font-family=\"" + family + "\"",
                "<text x=\"10\" y=\"50\">A</text>"), l.loaded, 0, 0, nullptr);
  LowerBoth(l.loaded, l, &options);
  GSHSCENE_Image_Paint ip;
  GSHSCENE_Image_Info info;
  if (l.Items() >= 1 && gshscene_resource_image_paint(l.Item(0).paint.resource, &ip) == GSHSCENE_OK &&
      gshscene_resource_image(ip.image, &info) == GSHSCENE_OK) {
    out.width = info.width;
    out.height = info.height;
    out.scale_x = ip.map.m[0][0];
    out.scale_y = ip.map.m[1][1];
    out.tx = ip.map.t[0];
    out.ty = ip.map.t[1];
  }
  return out;
}

}  // namespace

TEST(LowerText, AColourGlyphsPictureIsScaledByTheSizeOverThePpemOfItsStrikeAndPlacedByItsBearings) {
  ColorFonts f;
  // The strikes are 16 and 24 ppem; A's bearings are (1, 11) in the first and (2, 17) in the second.
  ColorPlace at16 = PlaceOfA(f, "16");
  EXPECT_EQ(at16.width, 10u);
  EXPECT_EQ(at16.height, 12u);
  EXPECT_EQ(at16.scale_x, 1.0);
  EXPECT_EQ(at16.scale_y, 1.0);
  EXPECT_EQ(at16.tx, 11.0) << "x plus the bearing";
  EXPECT_EQ(at16.ty, 39.0) << "y minus the bearing above the baseline";
  ColorPlace at24 = PlaceOfA(f, "24");
  EXPECT_EQ(at24.width, 16u) << "the strike nearest the size";
  EXPECT_EQ(at24.scale_x, 1.0);
  EXPECT_EQ(at24.tx, 12.0);
  EXPECT_EQ(at24.ty, 33.0);
  ColorPlace at32 = PlaceOfA(f, "32");
  EXPECT_EQ(at32.width, 16u) << "past the largest strike, the largest";
  EXPECT_NEAR(at32.scale_x, 32.0 / 24.0, 1e-12);
  EXPECT_NEAR(at32.scale_y, 32.0 / 24.0, 1e-12);
  EXPECT_NEAR(at32.tx, 10.0 + 2.0 * 32.0 / 24.0, 1e-12);
  EXPECT_NEAR(at32.ty, 50.0 - 17.0 * 32.0 / 24.0, 1e-12);
  ColorPlace odd = PlaceOfA(f, "19.9");
  EXPECT_EQ(odd.width, 16u) << "19.9 rounds to a ppem of 20, nearer the 24 ppem strike (a tie goes to the larger) than 19 is the 16";
  // A strike whose ppem differs by axis (24 by 12 at a size of 12): each axis is scaled by the size over its own ppem.
  ColorPlace wide = PlaceOfA(f, "12", "Ghoti Fixture CBDT Sizes");
  EXPECT_EQ(wide.width, 8u);
  EXPECT_NEAR(wide.scale_x, 0.5, 1e-12);
  EXPECT_NEAR(wide.scale_y, 1.0, 1e-12);
  ColorPlace at12 = PlaceOfA(f, "12");
  EXPECT_EQ(at12.width, 10u) << "under the smallest strike, the smallest";
  EXPECT_NEAR(at12.scale_x, 0.75, 1e-12);
  EXPECT_NEAR(at12.tx, 10.0 + 0.75, 1e-12);
  EXPECT_NEAR(at12.ty, 50.0 - 11.0 * 0.75, 1e-12);
}

TEST(LowerText, TheDecodedBytesBudgetIsExactAndEveryColourGlyphDrawnTakesFromIt) {
  ColorFonts f;
  // The smallest budget a lone glyph lowers under is the bytes of its RGBA8 picture.
  size_t one = 0;
  for (size_t b = 1; b < ((size_t) 1 << 20) && one == 0; b++) {
    GVEC_Limits limits;
    gvec_limits_default(&limits);
    limits.max_image_decoded_bytes = b;
    Lowered l;
    LowerColor(f, "A", l, &limits);
    if (ImageFillsOf(l) == 1u) {
      one = b;
    }
  }
  ASSERT_NE(one, (size_t) 0);
  const ColorPlace place = PlaceOfA(f, "24");
  EXPECT_EQ(one, (size_t) place.width * place.height * 4u) << "a picture of exactly the budget is drawn";
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_image_decoded_bytes = one;
  Lowered twice;
  LowerColor(f, "AA", twice, &limits);
  EXPECT_EQ(ImageFillsOf(twice), (size_t) 1) << "the second picture is past the budget the first used";
  EXPECT_NE(twice.Refusal(GVEC_LOWER_LIMIT), nullptr);
  limits.max_image_decoded_bytes = 2 * one;
  Lowered both;
  LowerColor(f, "AA", both, &limits);
  EXPECT_EQ(ImageFillsOf(both), (size_t) 2);
  EXPECT_EQ(both.Refusals(), (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

/** The region of the filter of the first filtered group of the list. */
bool RegionOf(const Lowered & l, double region[4]) {
  for (size_t i = 0; i < l.Items(); i++) {
    const GSHSCENE_Item & it = l.Item(i);
    GSHSCENE_Filter f;
    if (it.kind == GSHSCENE_ITEM_GROUP_PUSH && it.group.filter && gshscene_resource_filter(it.group.filter, &f) == GSHSCENE_OK) {
      for (int k = 0; k < 4; k++) {
        region[k] = f.region[k];
      }
      return true;
    }
  }
  return false;
}

bool LowerFiltered(Fonts & f, const std::string & text_attributes, Lowered & l) {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\"",
               "<defs><filter id=\"f\" x=\"0\" y=\"0\" width=\"1\" height=\"1\"><feFlood flood-color=\"green\"/></filter></defs>"
               "<text x=\"10\" y=\"150\" filter=\"url(#f)\" " + text_attributes + ">AB</text>"),
           l, 0, 0, &options);
  return l.Refusals() == 0;
}

}  // namespace

TEST(LowerText, AFilterInTheUnitsOfTheBoxIsPlacedInTheBoxOfTheTextsGlyphs) {
  // The box of a `text` is its glyphs' cells; a filter at x=0 y=0 width=1 height=1 of it is that box. The text used to have no inner box, so
  // the region had no size and the text, filter and all, was not rendered (text/text/filter-bbox: every engine draws the flood over the text).
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerFiltered(f, "", l));
  double region[4];
  ASSERT_TRUE(RegionOf(l, region)) << "the text is drawn through its filter";
  EXPECT_EQ(region[0], 10.0);
  EXPECT_EQ(region[2], 120.0) << "two glyphs of 60";
}

TEST(LowerText, TheBoxOfTextHoldsTheSpacingBetweenGlyphsAndNotAfterTheLastOne) {
  // CSS Text: letter-spacing adds no space before or after the text, only between (text/letter-spacing/filter-bbox).
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerFiltered(f, "letter-spacing=\"20\"", l));
  double region[4];
  ASSERT_TRUE(RegionOf(l, region));
  EXPECT_EQ(region[0], 10.0);
  EXPECT_EQ(region[2], 140.0) << "120 of glyphs and 20 between them, not 160";
}

TEST(LowerText, TheBoxOfVerticalTextHoldsTheSpacingBetweenGlyphsAndNotAfterTheLastOne) {
  Fonts f;
  Lowered plain, spaced;
  ASSERT_TRUE(LowerFiltered(f, "writing-mode=\"tb\"", plain));
  ASSERT_TRUE(LowerFiltered(f, "writing-mode=\"tb\" letter-spacing=\"20\"", spaced));
  double a[4], b[4];
  ASSERT_TRUE(RegionOf(plain, a));
  ASSERT_TRUE(RegionOf(spaced, b));
  EXPECT_EQ(b[3] - a[3], 20.0) << "one gap between two glyphs, not two";
  EXPECT_EQ(b[2], a[2]);
}

TEST(LowerText, AMarkShapedAsAGlyphOfItsOwnTurnsAboutTheOriginOfItsBase) {
  // The face shapes "A" and U+0301 as two glyphs of one cluster; the rotate list gives the cluster one angle, and both turn about the base's
  // origin, so they are one run under one map (text/text/rotate-with-multiple-values-and-complex-text: the mark used to turn about its own origin).
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "x=\"10\" y=\"50\" rotate=\"30\"", "A\xCC\x81", l));
  size_t fills = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    fills += l.Item(i).kind == GSHSCENE_ITEM_FILL ? 1 : 0;
  }
  GSHSCENE_Glyph_Run run;
  ASSERT_TRUE(ReadRun(l, 0, run));
  ASSERT_EQ(run.glyph_count, (size_t) 2) << "the face has a glyph for the mark";
  EXPECT_EQ(fills, (size_t) 1) << "the base and its mark are one run, turned about one origin";
}

TEST(LowerText, TheSpacingATextLengthSpreadsIsTakenByTheLastGlyphOfTheSpanToo) {
  // "BB" is 120 wide; textLength 300 spreads 180 between the two (the second B starts 180 on) and the last B's advance takes it too, so the C after
  // the span starts 180 further than the span's own end, as Chromium, resvg and the corpus's reference place it (textLength/on-a-single-tspan).
  Fonts f;
  Lowered l;
  ASSERT_TRUE(LowerText(f, "x=\"0\" y=\"50\"", "A<tspan textLength=\"300\">BB</tspan>C", l));
  std::vector<double> xs;
  for (size_t i = 0; i < l.Items(); i++) {
    GSHSCENE_Glyph_Run run;
    if (ReadRun(l, i, run)) {
      for (size_t g = 0; g < run.glyph_count; g++) {
        xs.push_back(run.glyphs[g].x);
      }
    }
  }
  ASSERT_EQ(xs.size(), (size_t) 4);
  EXPECT_EQ(xs[0], 0.0);
  EXPECT_EQ(xs[1], 60.0);
  EXPECT_EQ(xs[2], 300.0);
  EXPECT_EQ(xs[3], 540.0) << "60 + 120 of glyphs + 180 between + 180 after the last";
}

