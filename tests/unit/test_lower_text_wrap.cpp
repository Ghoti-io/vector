/**
 * @file
 *
 * Wrapped text (SVG 2, 11.4.1 and 11.7): a `text` with an `inline-size` is broken into lines. The tests use the font of tests/data/fonts, every glyph a box 600
 * units wide and a space 300, with an ascent of 800 and a descent of 200, so that at a font size of 100 a letter is 60 wide, a space 30 and a line `normal`
 * 100 tall: the lines and their places are arithmetic.
 */

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/glyph.h>
#include <ghoti.io/shape-scene/glyph_provider.h>

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

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

struct Glyph {
  double x, y;
};

/** Lower `<text attributes>content</text>` in a 400 by 200 viewport at font size 100, and the glyphs of every run in the order they are drawn. */
std::vector<Glyph> Wrapped(Fonts & f, const std::string & attributes, const std::string & content, const std::string & root = "", Lowered * keep = nullptr) {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  Lowered own;
  Lowered & l = keep ? *keep : own;
  // `inline-size` is a property (SVG 2, 11.4.1) with no presentation attribute: the tests spell it as one for brevity, and it is put in a rule
  std::string attrs = attributes, rule;
  const size_t at = attrs.find("inline-size=\"");
  if (at != std::string::npos) {
    const size_t end = attrs.find('"', at + 13);
    std::string value = attrs.substr(at + 13, end - at - 13);
    rule = "<style>text { inline-size: " + value + (value.find_first_not_of("0123456789.") == std::string::npos ? "px" : "") + " }</style>";
    attrs.erase(at, end - at + 1);
  }
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\" " + root, rule + "<text " + attrs + ">" + content + "</text>"), l, 0, 0, &options);
  std::vector<Glyph> out;
  for (size_t i = 0; i < l.Items(); i++) {
    GSHSCENE_Glyph_Run run;
    if (gshscene_resource_glyph_run(l.Item(i).resource, &run) != GSHSCENE_OK) {
      continue;
    }
    for (size_t g = 0; g < run.glyph_count; g++) {
      out.push_back({run.glyphs[g].x, run.glyphs[g].y});
    }
  }
  return out;
}

/** The distinct baselines of the glyphs, in the order they come. */
std::vector<double> Baselines(const std::vector<Glyph> & glyphs) {
  std::vector<double> out;
  for (const Glyph & g : glyphs) {
    if (out.empty() || out.back() != g.y) {
      out.push_back(g.y);
    }
  }
  return out;
}

}  // namespace

TEST(LowerTextWrap, ALineBreaksAtTheSpaceThatLetsTheWordsFitAndTheSpaceAtItsEndIsDropped) {
  Fonts f;
  // "AA BB" is 120 + 30 + 120 = 270 wide, and " CC" would make 420: CC is on a line of its own, the space before it gone
  auto g = Wrapped(f, "inline-size=\"300\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 7);
  EXPECT_EQ(g[0].x, 0.0);
  EXPECT_EQ(g[1].x, 60.0);
  EXPECT_EQ(g[2].x, 120.0);  // the space between
  EXPECT_EQ(g[3].x, 150.0);
  EXPECT_EQ(g[4].x, 210.0);
  EXPECT_EQ(g[0].y, 0.0);
  EXPECT_EQ(g[4].y, 0.0);
  EXPECT_EQ(g[5].x, 0.0);
  EXPECT_EQ(g[6].x, 60.0);
  EXPECT_EQ(g[5].y, 100.0);  // 20 below the first baseline, 80 above the second
  EXPECT_EQ(g[6].y, 100.0);
}

TEST(LowerTextWrap, TextIndentTakesRoomFromTheFirstLine) {
  Fonts f;
  // 40 of the 300 are the indent: "AA BB" (270) no longer fits the first line, "AA" does
  auto g = Wrapped(f, "inline-size=\"300\" style=\"text-indent: 40px\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 7);
  EXPECT_EQ(g[0].x, 40.0);
  EXPECT_EQ(g[1].x, 100.0);
  EXPECT_EQ(g[2].x, 0.0) << "the second line is not indented";
  EXPECT_EQ(g[2].y, 100.0);
  EXPECT_EQ(g[5].x, 150.0);
  // a percentage is of the inline-size
  auto percent = Wrapped(f, "inline-size=\"300\" style=\"text-indent: 10%\"", "AA BB CC");
  EXPECT_EQ(percent[0].x, 30.0);
  // hanging: every line but the first
  auto hanging = Wrapped(f, "inline-size=\"300\" style=\"text-indent: 40px hanging\"", "AA BB CC");
  ASSERT_EQ(hanging.size(), (size_t) 7);
  EXPECT_EQ(hanging[0].x, 0.0);
  EXPECT_EQ(hanging[5].x, 40.0) << "CC on the second line, indented";
  EXPECT_EQ(hanging[6].x, 100.0);
  // a negative indent pulls the first line out, and gives it the room
  auto outdent = Wrapped(f, "inline-size=\"300\" style=\"text-indent: -30px\"", "AA BB CC");
  EXPECT_EQ(outdent[0].x, -30.0);
  EXPECT_EQ(Baselines(outdent), (std::vector<double>{0.0, 100.0})) << "AA BB CC is 420 wide, 330 fit the first line: CC goes on";
  // each-line: a line after a forced break is a first line too
  auto each = Wrapped(f, "inline-size=\"300\" style=\"text-indent: 40px each-line; white-space: pre-line\"", "AA\nBB");
  ASSERT_EQ(each.size(), (size_t) 4);
  EXPECT_EQ(each[0].x, 40.0);
  EXPECT_EQ(each[2].x, 40.0);
  auto without = Wrapped(f, "inline-size=\"300\" style=\"text-indent: 40px; white-space: pre-line\"", "AA\nBB");
  EXPECT_EQ(without[2].x, 0.0);
}

TEST(LowerTextWrap, TextAlignPutsALineInItsStretch) {
  Fonts f;
  const struct {
    const char * align;
    double x;
  } cases[] = {{"center", 90.0}, {"end", 180.0}, {"right", 180.0}, {"left", 0.0}, {"start", 0.0}};
  for (const auto & c : cases) {
    auto g = Wrapped(f, std::string("inline-size=\"300\" style=\"text-align: ") + c.align + "\"", "AA");
    ASSERT_EQ(g.size(), (size_t) 2) << c.align;
    EXPECT_EQ(g[0].x, c.x) << c.align;
  }
  // text-align is stronger than text-anchor only when it says more than start
  auto anchored = Wrapped(f, "inline-size=\"300\" text-anchor=\"end\"", "AA");
  EXPECT_EQ(anchored[0].x, 180.0);
  auto both = Wrapped(f, "inline-size=\"300\" text-anchor=\"end\" style=\"text-align: left\"", "AA");
  EXPECT_EQ(both[0].x, 0.0);
}

TEST(LowerTextWrap, JustifyFillsEveryLineButTheLast) {
  Fonts f;
  auto g = Wrapped(f, "inline-size=\"300\" style=\"text-align: justify\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 7);
  EXPECT_EQ(g[0].x, 0.0);
  EXPECT_EQ(g[2].x, 120.0) << "the space begins where AA ends";
  EXPECT_EQ(g[3].x, 180.0) << "the 30 left over are in the space: BB ends at the end of the 300";
  EXPECT_EQ(g[4].x, 240.0);
  EXPECT_EQ(g[5].x, 0.0) << "the last line is not justified";
  // the last line as well, when text-align-last says so: a line of one word has no place for the room, and is left
  auto last = Wrapped(f, "inline-size=\"300\" style=\"text-align: justify; text-align-last: justify\"", "AA BB CC");
  EXPECT_EQ(last[5].x, 0.0);
  auto centered = Wrapped(f, "inline-size=\"300\" style=\"text-align: justify; text-align-last: center\"", "AA BB CC");
  EXPECT_EQ(centered[5].x, 90.0);
  // text-justify: none leaves the line, inter-character shares the room among the characters
  auto none = Wrapped(f, "inline-size=\"300\" style=\"text-align: justify; text-justify: none\"", "AA BB CC");
  EXPECT_EQ(none[3].x, 150.0);
  auto each = Wrapped(f, "inline-size=\"300\" style=\"text-align: justify; text-justify: inter-character\"", "AA BB CC");
  EXPECT_NEAR(each[1].x, 67.5, 1e-9) << "30 shared among the 4 gaps of AA BB";
  EXPECT_NEAR(each[3].x, 172.5, 1e-9);
  EXPECT_NEAR(each[4].x, 240.0, 1e-9);
  // a forced break ends a paragraph: the line before it is a last line
  auto pre = Wrapped(f, "inline-size=\"300\" style=\"text-align: justify; white-space: pre-line\"", "AA BB\nCC DD");
  EXPECT_EQ(pre[3].x, 150.0) << "AA BB ends at a line feed: not justified";
}

TEST(LowerTextWrap, AWordThatFitsExactlyStaysOnItsLine) {
  Fonts f;
  auto g = Wrapped(f, "inline-size=\"270\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 7);
  EXPECT_EQ(Baselines(g), (std::vector<double>{0.0, 100.0}));
  auto narrower = Wrapped(f, "inline-size=\"269\"", "AA BB CC");
  EXPECT_EQ(Baselines(narrower), (std::vector<double>{0.0, 100.0, 200.0}));
}

TEST(LowerTextWrap, TheAreaIsPutByTheFirstCharactersPosition) {
  Fonts f;
  auto g = Wrapped(f, "x=\"10\" y=\"50\" inline-size=\"300\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 7);
  EXPECT_EQ(g[0].x, 10.0);
  EXPECT_EQ(g[0].y, 50.0);
  EXPECT_EQ(g[5].x, 10.0);
  EXPECT_EQ(g[5].y, 150.0);
}

TEST(LowerTextWrap, TextAnchorPutsEachLineWithinTheRectangle) {
  Fonts f;
  auto middle = Wrapped(f, "inline-size=\"300\" text-anchor=\"middle\"", "AA BB CC");
  ASSERT_EQ(middle.size(), (size_t) 7);
  EXPECT_EQ(middle[0].x, 15.0);  // 270 wide, in 300: 15 from the left
  EXPECT_EQ(middle[5].x, 90.0);  // CC is 120 wide: 90 from the left
  auto end = Wrapped(f, "inline-size=\"300\" text-anchor=\"end\"", "AA BB CC");
  EXPECT_EQ(end[0].x, 30.0);
  EXPECT_EQ(end[5].x, 180.0);
  auto start_anchor = Wrapped(f, "x=\"10\" inline-size=\"300\" text-anchor=\"end\"", "AA BB CC");
  EXPECT_EQ(start_anchor[5].x, 190.0);  // the rectangle begins at x
}

TEST(LowerTextWrap, TheLineHeightMakesTheLineBoxesAndTheHalfLeadingIsEven) {
  Fonts f;
  auto g = Wrapped(f, "inline-size=\"300\" style=\"line-height: 2\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 7);
  EXPECT_EQ(g[5].y - g[0].y, 200.0);
  auto fixed = Wrapped(f, "inline-size=\"300\" style=\"line-height: 150px\"", "AA BB CC");
  EXPECT_EQ(fixed[5].y - fixed[0].y, 150.0);
  auto percent = Wrapped(f, "inline-size=\"300\" style=\"line-height: 50%\"", "AA BB CC");
  EXPECT_EQ(percent[5].y - percent[0].y, 50.0);
}

TEST(LowerTextWrap, ALargerSpanMakesItsLineTaller) {
  Fonts f;
  // the second line has a span of size 200: its box is 200 tall (160 above, 40 below), the first's 100 (80, 20)
  auto g = Wrapped(f, "inline-size=\"300\"", "AA BB <tspan font-size=\"200\">C</tspan>");
  ASSERT_EQ(g.size(), (size_t) 6);
  EXPECT_EQ(g[5].y - g[0].y, 20.0 + 160.0);
}

TEST(LowerTextWrap, AnInlineSizeOfZeroOrAutoDoesNotWrap) {
  Fonts f;
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"0\"", "AA BB CC")).size(), (size_t) 1);
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"auto\"", "AA BB CC")).size(), (size_t) 1);
  EXPECT_EQ(Baselines(Wrapped(f, "", "AA BB CC")).size(), (size_t) 1);
}

TEST(LowerTextWrap, APercentageIsOfTheViewportsWidth) {
  Fonts f;
  // 75% of 400 is 300
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"75%\"", "AA BB CC")), (std::vector<double>{0.0, 100.0}));
  // 50% is 200: "AA BB" does not fit
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"50%\"", "AA BB CC")), (std::vector<double>{0.0, 100.0, 200.0}));
}

TEST(LowerTextWrap, AWordTooLongForItsLineOverflowsUnlessTheWordMayBeBroken) {
  Fonts f;
  auto overflow = Wrapped(f, "inline-size=\"100\"", "AAAAA");
  ASSERT_EQ(overflow.size(), (size_t) 5);
  EXPECT_EQ(Baselines(overflow).size(), (size_t) 1);
  auto broken = Wrapped(f, "inline-size=\"130\" style=\"overflow-wrap: break-word\"", "AAAAA");
  ASSERT_EQ(broken.size(), (size_t) 5);
  EXPECT_EQ(Baselines(broken), (std::vector<double>{0.0, 100.0, 200.0}));  // AA AA A
  EXPECT_EQ(broken[2].x, 0.0);
  auto old = Wrapped(f, "inline-size=\"130\" style=\"word-wrap: break-word\"", "AAAAA");
  EXPECT_EQ(Baselines(old).size(), (size_t) 3);
  auto narrow = Wrapped(f, "inline-size=\"10\" style=\"overflow-wrap: anywhere\"", "AAA");
  EXPECT_EQ(Baselines(narrow).size(), (size_t) 3) << "a line takes at least one character, or the text would never end";
}

TEST(LowerTextWrap, WordBreakBreakAllBreaksBetweenTheLettersOfAWord) {
  Fonts f;
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"130\"", "AAAA")).size(), (size_t) 1);
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"130\" style=\"word-break: break-all\"", "AAAA")), (std::vector<double>{0.0, 100.0}));
}

TEST(LowerTextWrap, AForcedBreakOfThePreservedWhiteSpaceEndsALineWhateverItsWidth) {
  Fonts f;
  Lowered l;
  auto g = Wrapped(f, "inline-size=\"400\" style=\"white-space: pre\"", "AA\nBB", "", &l);
  ASSERT_EQ(g.size(), (size_t) 4);
  EXPECT_EQ(Baselines(g), (std::vector<double>{0.0, 100.0}));
  EXPECT_EQ(g[2].x, 0.0);
  // `pre-line` collapses the spaces and keeps the line feeds; the spaces about a break go
  auto line = Wrapped(f, "inline-size=\"400\" style=\"white-space: pre-line\"", "AA  \n  BB   CC");
  ASSERT_EQ(line.size(), (size_t) 7);  // AA / BB CC
  EXPECT_EQ(Baselines(line), (std::vector<double>{0.0, 100.0}));
  // `pre-wrap` keeps the spaces, and wraps
  auto wrap = Wrapped(f, "inline-size=\"200\" style=\"white-space: pre-wrap\"", "AA  BB");
  EXPECT_EQ(Baselines(wrap), (std::vector<double>{0.0, 100.0}));
  // a line feed is a space where the white space is `xml:space="preserve"`, which is not a break
  auto xml = Wrapped(f, "inline-size=\"400\" xml:space=\"preserve\"", "AA\nBB", "xmlns:xml=\"http://www.w3.org/XML/1998/namespace\"");
  EXPECT_EQ(Baselines(xml).size(), (size_t) 1);
}

TEST(LowerTextWrap, ABlankLineTakesAStrut) {
  Fonts f;
  auto g = Wrapped(f, "inline-size=\"400\" style=\"white-space: pre\"", "AA\n\nBB");
  ASSERT_EQ(g.size(), (size_t) 4);
  EXPECT_EQ(Baselines(g), (std::vector<double>{0.0, 200.0}));
}

TEST(LowerTextWrap, NoWrapEndsALineOnlyAtAForcedBreak) {
  Fonts f;
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"100\" style=\"text-wrap-mode: nowrap\"", "AA BB CC")).size(), (size_t) 1);
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"100\" style=\"white-space: nowrap\"", "AA BB CC")).size(), (size_t) 1);
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"100\" style=\"white-space: pre\"", "AA BB CC")).size(), (size_t) 1);
}

TEST(LowerTextWrap, BalancedTextTakesTheSameLinesInTheNarrowestWidth) {
  Fonts f;
  // greedy at 500: "AA BB CC" and "DD"; balanced: "AA BB" and "CC DD" (270 each is the narrowest that takes two lines)
  auto greedy = Wrapped(f, "inline-size=\"500\"", "AA BB CC DD");
  ASSERT_EQ(greedy.size(), (size_t) 10);
  EXPECT_EQ(greedy[7].y, 0.0);
  EXPECT_EQ(greedy[8].y, 100.0);
  auto balanced = Wrapped(f, "inline-size=\"500\" style=\"text-wrap: balance\"", "AA BB CC DD");
  ASSERT_EQ(balanced.size(), (size_t) 10);
  EXPECT_EQ(balanced[4].y, 0.0);
  EXPECT_EQ(balanced[5].y, 100.0);
}

TEST(LowerTextWrap, ThePositionsOfTheCharactersAndTheirOffsetsAreIgnored) {
  Fonts f;
  auto g = Wrapped(f, "inline-size=\"300\" x=\"0 7 9 11 13\" y=\"0 40 50\" dx=\"5 5 5\" dy=\"9\" rotate=\"30\" textLength=\"1000\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 7);
  EXPECT_EQ(g[1].x, 60.0);
  EXPECT_EQ(g[1].y, 0.0);
  EXPECT_EQ(g[5].y, 100.0);
}

TEST(LowerTextWrap, TextOnAPathIsNotWrapped) {
  Fonts f;
  Lowered l;
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  LowerSvg(Svg("width=\"800\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\"",
               "<path id=\"p\" d=\"M0 100 L800 100\"/><text inline-size=\"100\"><textPath href=\"#p\">AA BB CC</textPath></text>"),
      l, 0, 0, &options);
  size_t glyphs = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    GSHSCENE_Glyph_Run run;
    if (gshscene_resource_glyph_run(l.Item(i).resource, &run) == GSHSCENE_OK) {
      glyphs += run.glyph_count;
    }
  }
  EXPECT_EQ(glyphs, (size_t) 8);  // one line along the path: no character is dropped
}

TEST(LowerTextWrap, EachLineIsDecoratedOnItsOwn) {
  Fonts f;
  Lowered l;
  Wrapped(f, "inline-size=\"300\" style=\"text-decoration: underline\"", "AA BB CC", "", &l);
  size_t decorations = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    GSHSCENE_Glyph_Run run;
    if (gshscene_resource_glyph_run(l.Item(i).resource, &run) != GSHSCENE_OK) {
      decorations++;
    }
  }
  EXPECT_EQ(decorations, (size_t) 2);
}

TEST(LowerTextWrap, AVerticalTextIsWrappedByItsHeightAndTheLinesGoToTheLeft) {
  Fonts f;
  Lowered l;
  // upright, a letter is the font size tall: inline-size 250 takes two letters of 100 a line
  auto g = Wrapped(f, "x=\"300\" y=\"10\" inline-size=\"250\" style=\"writing-mode: vertical-rl; text-orientation: upright\"", "AA AA", "", &l);
  ASSERT_EQ(g.size(), (size_t) 4);
  EXPECT_EQ(g[0].x, g[1].x);
  EXPECT_EQ(g[2].x, g[3].x);
  EXPECT_LT(g[2].x, g[0].x) << "the next column is to the left";
  EXPECT_GT(g[1].y, g[0].y);
  EXPECT_EQ(g[2].y, g[0].y) << "each column begins at the same height";
}

TEST(LowerTextWrap, AUnicodeLineBreakingRuleKeepsWhatItForbidsTogether) {
  Fonts f;
  // a no-break space is no opportunity: "A\xC2\xA0" then "B" cannot be separated, but the hyphen allows a break after it
  auto glue = Wrapped(f, "inline-size=\"130\"", "A\xC2\xA0" "A B");
  EXPECT_EQ(Baselines(glue).size(), (size_t) 2);
  auto hyphen = Wrapped(f, "inline-size=\"130\"", "AA-AA");
  EXPECT_EQ(Baselines(hyphen).size(), (size_t) 2);
}


namespace {

/** `<defs>` and a `text` in a 400 by 200 viewport: the glyphs of the text. */
std::vector<Glyph> Flowed(Fonts & f, const std::string & defs, const std::string & attributes, const std::string & content, const std::string & root = "") {
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.fonts = f.fonts;
  Lowered l;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\" font-family=\"sans-serif\" " + root, "<defs>" + defs + "</defs><text " + attributes + ">" + content + "</text>"), l, 0, 0, &options);
  std::vector<Glyph> out;
  for (size_t i = 0; i < l.Items(); i++) {
    GSHSCENE_Glyph_Run run;
    if (gshscene_resource_glyph_run(l.Item(i).resource, &run) != GSHSCENE_OK) {
      continue;
    }
    for (size_t g = 0; g < run.glyph_count; g++) {
      out.push_back({run.glyphs[g].x, run.glyphs[g].y});
    }
  }
  return out;
}

/** Whether the box of the glyph (18 wide at the font size 30 the shapes' tests use, from the baseline's 24 above to 6 below) is inside the circle at (cx, cy) of radius r. */
bool InCircle(const Glyph & g, double cx, double cy, double r) {
  for (double x : {g.x, g.x + 18}) {
    for (double y : {g.y - 24, g.y + 6}) {
      // the shape is flattened (to a tenth of a unit at most): a corner that is at the polygon's edge can be a hair outside the circle
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > (r + 0.05) * (r + 0.05)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

TEST(LowerTextWrap, ATextFlowsIntoTheShapeItIsGivenFromItsTop) {
  Fonts f;
  // the rectangle (10, 20) to (210, 320): the first baseline is 80 below its top, and a line holds one word of the three (270 > 200)
  auto g = Flowed(f, "<rect id=\"r\" x=\"10\" y=\"20\" width=\"200\" height=\"300\"/>", "style=\"shape-inside: url(#r)\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 6);
  EXPECT_EQ(g[0].x, 10.0);
  EXPECT_EQ(g[0].y, 100.0);
  EXPECT_EQ(g[2].y, 200.0);
  EXPECT_EQ(g[4].y, 300.0);
  EXPECT_EQ(g[4].x, 10.0);
}

TEST(LowerTextWrap, TheTextThatDoesNotFitTheShapeIsNotDrawn) {
  Fonts f;
  // the fourth line would be from 320 to 420: below the rectangle
  auto g = Flowed(f, "<rect id=\"r\" x=\"10\" y=\"20\" width=\"200\" height=\"300\"/>", "style=\"shape-inside: url(#r)\"", "AA BB CC DD");
  EXPECT_EQ(g.size(), (size_t) 6);
}

TEST(LowerTextWrap, AShapeOutweighsTheInlineSize) {
  Fonts f;
  auto g = Flowed(f, "<rect id=\"r\" width=\"400\" height=\"200\"/><style>text { inline-size: 100px }</style>", "style=\"shape-inside: url(#r)\"", "AA BB");
  EXPECT_EQ(Baselines(g), (std::vector<double>{80.0}));  // 400 wide: one line
  auto none = Flowed(f, "<style>text { inline-size: 100px }</style>", "style=\"shape-inside: none\"", "AA BB CC");
  EXPECT_EQ(Baselines(none).size(), (size_t) 3);  // the inline-size: AA / BB / CC
}

TEST(LowerTextWrap, TextAnchorPutsALineInItsStretchOfTheShape) {
  Fonts f;
  auto g = Flowed(f, "<rect id=\"r\" x=\"50\" width=\"300\" height=\"200\"/>", "style=\"shape-inside: url(#r)\" text-anchor=\"middle\"", "AA");
  ASSERT_EQ(g.size(), (size_t) 2);
  EXPECT_EQ(g[0].x, 140.0);  // 120 wide in 300 from 50: 90 from the left
  auto end = Flowed(f, "<rect id=\"r\" x=\"50\" width=\"300\" height=\"200\"/>", "style=\"shape-inside: url(#r)\" text-anchor=\"end\"", "AA");
  EXPECT_EQ(end[0].x, 230.0);
}

TEST(LowerTextWrap, EveryGlyphOfATextInACircleIsInsideTheCircle) {
  Fonts f;
  auto g = Flowed(f, "", "style=\"font-size: 30px; shape-inside: circle(95px at 200px 100px)\"", "AA AA AA AA AA AA AA AA AA AA AA AA AA AA AA AA", "");
  ASSERT_GT(g.size(), (size_t) 4);
  for (const Glyph & glyph : g) {
    EXPECT_TRUE(InCircle(glyph, 200, 100, 95)) << glyph.x << ", " << glyph.y;
  }
  EXPECT_GE(Baselines(g).size(), (size_t) 2);
}

TEST(LowerTextWrap, ATextInATriangleIsNarrowestAtItsApex) {
  Fonts f;
  // the apex at the top: the lines get longer going down
  auto g = Flowed(f, "", "style=\"font-size: 30px; shape-inside: polygon(200px 0, 400px 200px, 0 200px)\"", "A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A");
  ASSERT_GT(g.size(), (size_t) 3);
  std::vector<double> widths;
  double last_y = g[0].y, first_x = g[0].x, last_x = g[0].x;
  for (const Glyph & glyph : g) {
    if (glyph.y != last_y) {
      widths.push_back(last_x + 18 - first_x);
      last_y = glyph.y;
      first_x = glyph.x;
    }
    last_x = glyph.x;
  }
  widths.push_back(last_x + 18 - first_x);
  ASSERT_GE(widths.size(), (size_t) 2);
  EXPECT_LE(widths.front(), widths.back());
  // and every glyph lies inside the triangle: left of the right edge and right of the left edge, at the bottom of its box
  for (const Glyph & glyph : g) {
    const double bottom = glyph.y + 6;
    EXPECT_GE(glyph.x, 200.0 - bottom - 1e-6);   // the edges are 45 degrees: half the width is the depth
    EXPECT_LE(glyph.x + 18, 200.0 + bottom + 1e-6);
  }
}

TEST(LowerTextWrap, AShapeSubtractedLeavesTwoStretchesThatTheTextFillsInTurn) {
  Fonts f;
  // the region is 400 by 200 and a column x 100..200 is taken out: [0, 100] and [200, 400] in each band
  auto g = Flowed(f, "<rect id=\"r\" width=\"400\" height=\"200\"/><rect id=\"s\" x=\"100\" width=\"100\" height=\"200\"/>",
      "style=\"shape-inside: url(#r); shape-subtract: url(#s)\"", "A BB CCC");
  ASSERT_EQ(g.size(), (size_t) 6);
  EXPECT_EQ(g[0].x, 0.0);    // "A" in the first stretch
  EXPECT_EQ(g[0].y, 80.0);
  EXPECT_EQ(g[1].x, 200.0);  // "BB" in the second
  EXPECT_EQ(g[1].y, 80.0);
  EXPECT_EQ(g[3].x, 200.0);  // "CCC" (180) does not fit the first stretch (100): the second takes it, below
  EXPECT_EQ(g[3].y, 180.0);
}

TEST(LowerTextWrap, TheMarginKeepsTheTextAwayFromASubtractedShape) {
  Fonts f;
  // the region is 400 wide and the subtracted shape is x 250..400: 250 are left, 210 with a margin of 40
  const std::string defs = "<rect id=\"r\" width=\"400\" height=\"200\"/><rect id=\"s\" x=\"250\" width=\"150\" height=\"200\"/>";
  auto plain = Flowed(f, defs, "style=\"shape-inside: url(#r); shape-subtract: url(#s)\"", "AAAA");
  EXPECT_EQ(plain.size(), (size_t) 4);  // 240 wide: it fits in 250
  auto margin = Flowed(f, defs, "style=\"shape-inside: url(#r); shape-subtract: url(#s); shape-margin: 40px\"", "AAAA");
  EXPECT_EQ(margin.size(), (size_t) 0) << "240 does not fit in 210";
  auto fits = Flowed(f, defs, "style=\"shape-inside: url(#r); shape-subtract: url(#s); shape-margin: 40px\"", "AAA");
  ASSERT_EQ(fits.size(), (size_t) 3);  // 180 does
  EXPECT_LE(fits[2].x + 60, 210.0 + 1e-6);
}

TEST(LowerTextWrap, ThePaddingInsetsTheShape) {
  Fonts f;
  const std::string defs = "<rect id=\"r\" width=\"400\" height=\"200\"/>";
  auto g = Flowed(f, defs, "style=\"shape-inside: url(#r); shape-padding: 20px\"", "AA BB");
  ASSERT_EQ(g.size(), (size_t) 5);
  EXPECT_EQ(g[0].x, 20.0);
  EXPECT_EQ(g[0].y, 100.0);  // the padded top is at 20; the baseline 80 below it
  // 360 wide: AA BB is 270 and fits
  auto narrow = Flowed(f, "<rect id=\"r\" width=\"260\" height=\"400\"/>", "style=\"shape-inside: url(#r); shape-padding: 20px\"", "AA BB");
  EXPECT_EQ(Baselines(narrow).size(), (size_t) 2);  // 220 wide: AA BB (270) does not
}

TEST(LowerTextWrap, APathOfItsOwnTransformIsTheShapeWhereTheTransformPutsIt) {
  Fonts f;
  auto g = Flowed(f, "<path id=\"p\" d=\"M0 0 H200 V200 H0 Z\" transform=\"translate(150 0)\"/>", "style=\"shape-inside: url(#p)\"", "AA");
  ASSERT_EQ(g.size(), (size_t) 2);
  EXPECT_EQ(g[0].x, 150.0);
}

TEST(LowerTextWrap, AnEvenOddShapeHasAHole) {
  Fonts f;
  // a square with a square cut out of its middle: the text goes in the ring, not across the hole
  auto g = Flowed(f, "<path id=\"p\" fill-rule=\"evenodd\" d=\"M0 0H400V200H0Z M100 0H300V200H100Z\"/>", "style=\"shape-inside: url(#p)\"", "A A A");
  ASSERT_EQ(g.size(), (size_t) 3);  // the spaces at the ends of lines are dropped
  for (const Glyph & glyph : g) {
    EXPECT_TRUE(glyph.x + 60 <= 100.0 + 1e-6 || glyph.x >= 300.0 - 1e-6) << glyph.x;
  }
}

TEST(LowerTextWrap, ATextInAVerticalShapeIsInColumnsFromTheRight) {
  Fonts f;
  auto g = Flowed(f, "<rect id=\"r\" x=\"100\" width=\"200\" height=\"200\"/>",
      "style=\"shape-inside: url(#r); writing-mode: vertical-rl; text-orientation: upright\"", "AA AA");
  ASSERT_EQ(g.size(), (size_t) 4);
  EXPECT_EQ(g[0].x, g[1].x);
  EXPECT_EQ(g[2].x, g[3].x);
  EXPECT_EQ(g[0].x - g[2].x, 100.0) << "the second column is a line box to the left of the first";
  EXPECT_GT(g[0].x, g[2].x);
  EXPECT_GE(g[0].x, 100.0);
  EXPECT_LE(g[0].x, 300.0);
}

TEST(LowerTextWrap, AShapeThatIsNothingIsNoShape) {
  Fonts f;
  // a reference to nothing: the text is laid out as it would be without the property
  auto g = Flowed(f, "", "x=\"5\" y=\"60\" style=\"shape-inside: url(#nowhere)\"", "AA BB");
  ASSERT_EQ(g.size(), (size_t) 5);
  EXPECT_EQ(g[0].x, 5.0);
  EXPECT_EQ(g[0].y, 60.0);
  EXPECT_EQ(Baselines(g).size(), (size_t) 1);
}

TEST(LowerTextWrap, ATextWiderThanAnyStretchOfTheShapeIsNotDrawnAndTheLoweringEnds) {
  Fonts f;
  auto g = Flowed(f, "<rect id=\"r\" width=\"100\" height=\"200\"/>", "style=\"shape-inside: url(#r)\"", "AAAAAA");
  EXPECT_EQ(g.size(), (size_t) 0);
  auto broken = Flowed(f, "<rect id=\"r\" width=\"100\" height=\"700\"/>", "style=\"shape-inside: url(#r); overflow-wrap: break-word\"", "AAAAAA");
  EXPECT_EQ(broken.size(), (size_t) 6) << "a word that may be broken is broken to fit";
}


TEST(LowerTextWrap, KeepAllForbidsTheBreaksBetweenLetters) {
  Fonts f;
  // U+4EBA is a letter of the ideographic class, which the test font has one glyph for (as wide as a letter): A, then two ideographs
  const std::string text = "A\xE4\xBA\xBA\xE4\xBA\xBA";
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"140\"", text)).size(), (size_t) 2);
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"140\" style=\"word-break: keep-all\"", text)).size(), (size_t) 1);
}

TEST(LowerTextWrap, ASoftHyphenIsAPlaceToBreakUnlessHyphensIsNone) {
  Fonts f;
  const std::string text = "AA\xC2\xAD" "AA";
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"200\"", text)).size(), (size_t) 2);
  EXPECT_EQ(Baselines(Wrapped(f, "inline-size=\"200\" style=\"hyphens: none\"", text)).size(), (size_t) 1);
}

TEST(LowerTextWrap, ALineThatEndsAtASoftHyphenShowsAHyphenAndOneThatDoesNotHidesIt) {
  Fonts f;
  const std::string text = "AA\xC2\xAD" "AA";
  // broken: "AA" and a hyphen (60 wide in this font, which lacks U+2010: the hyphen-minus), then "AA"
  auto broken = Wrapped(f, "inline-size=\"200\"", text);
  ASSERT_EQ(broken.size(), (size_t) 5);
  EXPECT_EQ(broken[2].x, 120.0);   // the hyphen, where the soft hyphen was
  EXPECT_EQ(broken[3].y, 100.0);
  EXPECT_EQ(broken[3].x, 0.0);
  // the hyphen takes room: "AA-" is 180, so a line of 170 cannot end there, and the word overflows whole
  auto tight = Wrapped(f, "inline-size=\"170\"", text);
  EXPECT_EQ(Baselines(tight).size(), (size_t) 1);
  // not broken, the soft hyphen is not seen and takes no room: "AA" then "AA" are 240 wide
  auto whole = Wrapped(f, "inline-size=\"300\"", text);
  ASSERT_GE(whole.size(), (size_t) 4);  // the face may draw the soft hyphen as a glyph of no width
  EXPECT_EQ(whole.back().x, 180.0);
}

TEST(LowerTextWrap, AForcedBreakLeavesTheRestOfTheLineBoxEmpty) {
  Fonts f;
  // the stretches are [0, 100] and [200, 400]: "A" ends its line by a line feed, so the second stretch is not used for "B"
  auto g = Flowed(f, "<rect id=\"r\" width=\"400\" height=\"200\"/><rect id=\"s\" x=\"100\" width=\"100\" height=\"200\"/>",
      "style=\"shape-inside: url(#r); shape-subtract: url(#s); white-space: pre-line\"", "A\nB C");
  ASSERT_EQ(g.size(), (size_t) 3);
  EXPECT_EQ(g[0].y, 80.0);
  EXPECT_EQ(g[1].y, 180.0);  // B is on the second line, in the first stretch
  EXPECT_EQ(g[1].x, 0.0);
  EXPECT_EQ(g[2].y, 180.0);  // and C is in the second stretch of that line
  EXPECT_EQ(g[2].x, 200.0);
}

TEST(LowerTextWrap, AShapeNarrowerThanALetterDrawsNothingThatItCannotContain) {
  Fonts f;
  EXPECT_EQ(Flowed(f, "<rect id=\"r\" width=\"40\" height=\"700\"/>", "style=\"shape-inside: url(#r); overflow-wrap: break-word\"", "AAA").size(), (size_t) 0);
}

TEST(LowerTextWrap, VerticalLeftToRightTextHasItsColumnsGoToTheRight) {
  Fonts f;
  // the shape is the column x 100..200: the first column of vertical-lr text is at its left, and there is room for one
  auto g = Flowed(f, "<rect id=\"r\" x=\"100\" width=\"100\" height=\"200\"/>",
      "style=\"shape-inside: url(#r); writing-mode: vertical-lr; text-orientation: upright\"", "AA AA");
  ASSERT_EQ(g.size(), (size_t) 2);
  EXPECT_GE(g[0].x, 100.0);
  EXPECT_LE(g[0].x, 200.0);
  auto plain = Wrapped(f, "x=\"100\" y=\"10\" inline-size=\"250\" style=\"writing-mode: vertical-lr; text-orientation: upright\"", "AA AA");
  ASSERT_EQ(plain.size(), (size_t) 4);
  EXPECT_EQ(plain[2].x - plain[0].x, 100.0);
}

TEST(LowerTextWrap, ASubtractedShapeThatLeansIsTakenOutWhereverTheLineBoxReachesIt) {
  Fonts f;
  // a parallelogram: x 100..200 at the top of the box and 200..300 at its bottom, so the box meets x 100..300; "AAA" (180) fits neither [0,100] nor [300,400]
  auto g = Flowed(f, "<rect id=\"r\" width=\"400\" height=\"100\"/><polygon id=\"s\" points=\"100,0 200,0 300,100 200,100\"/>",
      "style=\"shape-inside: url(#r); shape-subtract: url(#s)\"", "AAA");
  EXPECT_EQ(g.size(), (size_t) 0);
}

TEST(LowerTextWrap, ARightToLeftTextBeginsAtTheRightEdgeOfItsRectangle) {
  Fonts f;
  // the rectangle is 200 wide and ends at x = 300; each line is 120 wide, and `start` is its right edge
  auto g = Wrapped(f, "x=\"300\" inline-size=\"200\" style=\"direction: rtl\"", "AA BB CC");
  ASSERT_EQ(g.size(), (size_t) 6);
  EXPECT_EQ(Baselines(g).size(), (size_t) 3);
  for (size_t line = 0; line < 3; line++) {
    EXPECT_EQ(std::max(g[2 * line].x, g[2 * line + 1].x) + 60, 300.0);
    EXPECT_EQ(std::min(g[2 * line].x, g[2 * line + 1].x), 180.0);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
