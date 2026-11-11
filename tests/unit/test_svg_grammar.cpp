/**
 * @file
 *
 * The attribute grammars of the typed document: numbers, lengths, angles, lists, points, path data,
 * viewBox, preserveAspectRatio and the rest (svg_grammar.h). Each row is a requirement of the SVG 2
 * attribute chapters; the number vectors come from Python's correctly rounded float().
 */

#include "test_helpers.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "../../src/codec/svg/svg_grammar.h"

namespace {

struct Ctx {
  gvec_arena arena;
  gvec_svg_parse_context ctx;
  Ctx(size_t items = 1000, size_t segments = 1000) {
    gvec_arena_init(&arena, nullptr);
    ctx.arena = &arena;
    ctx.allocator = nullptr;
    ctx.max_list_items = items;
    ctx.max_path_segments = segments;
  }
  ~Ctx() { gvec_arena_destroy(&arena); }
};

uint64_t Bits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, sizeof b);
  return b;
}

gvec_svg_parse_status Num(const std::string & s, double * v, size_t * offset = nullptr) {
  gvec_svg_parse_error err{0, nullptr};
  auto st = gvec_svg_parse_number(s.data(), s.size(), v, &err);
  if (offset) {
    *offset = err.offset;
  }
  return st;
}

}  // namespace

TEST(Number, TheKnownAnswersAreCorrectlyRoundedOnEveryTarget) {
  std::ifstream f("tests/data/svg-grammar/numbers.tsv");
  ASSERT_TRUE(f.good());
  std::string line;
  size_t rows = 0, invalid = 0;
  while (std::getline(f, line)) {
    size_t tab = line.find('\t');
    ASSERT_NE(tab, std::string::npos) << line;
    std::string text = line.substr(0, tab), want = line.substr(tab + 1);
    double v = 12345;
    auto st = Num(text, &v);
    if (want == "INVALID") {
      EXPECT_EQ(st, GVEC_SVG_PARSE_INVALID) << text;
      invalid++;
    } else {
      ASSERT_EQ(st, GVEC_SVG_PARSE_OK) << text;
      EXPECT_EQ(Bits(v), std::stoull(want, nullptr, 16)) << text;
    }
    rows++;
  }
  EXPECT_GT(rows, 3000u);
  EXPECT_GT(invalid, 10u);
}

TEST(Number, TheGrammarIsSvgs) {
  double v;
  EXPECT_EQ(Num("  1.5  ", &v), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v, 1.5);
  EXPECT_EQ(Num("\t\n\f\r 2", &v), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v, 2.0);
  size_t at;
  for (const char * bad : {"", " ", "abc", "0x10", "nan", "inf", "-inf", "1,5", "1 2", "--1", "+-1", "1e", "1e+", ".", "-", "+", "1.2.3", "١٢"}) {
    EXPECT_EQ(Num(bad, &v, &at), GVEC_SVG_PARSE_INVALID) << "'" << bad << "'";
  }
  EXPECT_EQ(Num("1x", &v, &at), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(at, 1u);
  EXPECT_EQ(Num("1e999", &v, &at), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(at, 0u);
  EXPECT_EQ(Num("1e-999", &v), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(Bits(v), (uint64_t) 0);
  EXPECT_EQ(Num("-0", &v), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(Bits(v), UINT64_C(0x8000000000000000));
}

static GVEC_Svg_Length Len(const std::string & s, gvec_svg_parse_status * st = nullptr) {
  GVEC_Svg_Length l{-1, GVEC_SVG_UNIT_COUNT};
  gvec_svg_parse_error err{0, nullptr};
  auto r = gvec_svg_parse_length(s.data(), s.size(), &l, &err);
  if (st) {
    *st = r;
  }
  return l;
}

TEST(Length, EveryUnitIsRecognisedAsCssSpellsItWithoutRegardToCase) {
  struct Row {
    const char * text;
    GVEC_Svg_Unit unit;
  };
  const Row rows[] = {{"5px", GVEC_SVG_UNIT_PX}, {"5em", GVEC_SVG_UNIT_EM}, {"5ex", GVEC_SVG_UNIT_EX},
      {"5ch", GVEC_SVG_UNIT_CH}, {"5rem", GVEC_SVG_UNIT_REM}, {"5vw", GVEC_SVG_UNIT_VW}, {"5vh", GVEC_SVG_UNIT_VH},
      {"5vmin", GVEC_SVG_UNIT_VMIN}, {"5vmax", GVEC_SVG_UNIT_VMAX}, {"5cm", GVEC_SVG_UNIT_CM},
      {"5mm", GVEC_SVG_UNIT_MM}, {"5Q", GVEC_SVG_UNIT_Q}, {"5q", GVEC_SVG_UNIT_Q}, {"5in", GVEC_SVG_UNIT_IN},
      {"5pt", GVEC_SVG_UNIT_PT}, {"5pc", GVEC_SVG_UNIT_PC}, {"5%", GVEC_SVG_UNIT_PERCENT}, {"5PX", GVEC_SVG_UNIT_PX},
      {"5Em", GVEC_SVG_UNIT_EM}, {"5", GVEC_SVG_UNIT_NONE}, {" 5 ", GVEC_SVG_UNIT_NONE}, {"-.5e1em", GVEC_SVG_UNIT_EM}};
  for (const Row & r : rows) {
    gvec_svg_parse_status st;
    GVEC_Svg_Length l = Len(r.text, &st);
    EXPECT_EQ(st, GVEC_SVG_PARSE_OK) << r.text;
    EXPECT_EQ(l.unit, r.unit) << r.text;
  }
  EXPECT_EQ(Len("1e3px").value, 1000.0);
  EXPECT_EQ(Len("1em").value, 1.0) << "1e not followed by a digit is the number 1 and the unit em";
  for (const char * bad : {"5 px", "5xx", "px", "5deg", "5%%", "5 %", "", "5p"}) {
    gvec_svg_parse_status st;
    Len(bad, &st);
    EXPECT_EQ(st, GVEC_SVG_PARSE_INVALID) << bad;
  }
}

TEST(Angle, UnitsAndConversionToDegrees) {
  GVEC_Svg_Length a;
  gvec_svg_parse_error err;
  struct Row {
    const char * text;
    double degrees;
  };
  for (const Row & r : {Row{"90", 90}, Row{"90deg", 90}, Row{"100grad", 90}, Row{"0.25turn", 90}, Row{"-1turn", -360}}) {
    ASSERT_EQ(gvec_svg_parse_angle(r.text, std::strlen(r.text), &a, &err), GVEC_SVG_PARSE_OK) << r.text;
    EXPECT_DOUBLE_EQ(gvec_svg_angle_degrees(&a), r.degrees) << r.text;
  }
  ASSERT_EQ(gvec_svg_parse_angle("3.141592653589793rad", 20, &a, &err), GVEC_SVG_PARSE_OK);
  EXPECT_NEAR(gvec_svg_angle_degrees(&a), 180.0, 1e-12);
  EXPECT_EQ(gvec_svg_parse_angle("5px", 3, &a, &err), GVEC_SVG_PARSE_INVALID) << "a length unit is not an angle unit";
}

static std::string Join(const gvec_svg_numbers & n) {
  std::ostringstream o;
  for (size_t i = 0; i < n.count; i++) {
    o << (i ? "," : "") << n.items[i];
  }
  return o.str();
}

TEST(NumberList, SeparatorsAreWhiteSpaceAndOneOptionalComma) {
  struct Row {
    const char * text;
    gvec_svg_parse_status status;
    const char * values;
    size_t offset;
  };
  const Row rows[] = {
      {"", GVEC_SVG_PARSE_OK, "", 0},
      {"  ", GVEC_SVG_PARSE_OK, "", 0},
      {"1", GVEC_SVG_PARSE_OK, "1", 0},
      {"1 2 3", GVEC_SVG_PARSE_OK, "1,2,3", 0},
      {"1,2,3", GVEC_SVG_PARSE_OK, "1,2,3", 0},
      {" 1 , 2 ,3 ", GVEC_SVG_PARSE_OK, "1,2,3", 0},
      {"1\n2\t3", GVEC_SVG_PARSE_OK, "1,2,3", 0},
      {"1,,2", GVEC_SVG_PARSE_INVALID, "", 2},
      {"1,", GVEC_SVG_PARSE_INVALID, "", 1},
      {"1 2,", GVEC_SVG_PARSE_INVALID, "", 3},
      {",1", GVEC_SVG_PARSE_INVALID, "", 0},
      {"1 x", GVEC_SVG_PARSE_INVALID, "", 2},
      {"1-2", GVEC_SVG_PARSE_INVALID, "", 1},
      {"1 2 e", GVEC_SVG_PARSE_INVALID, "", 4},
  };
  for (const Row & r : rows) {
    Ctx c;
    gvec_svg_numbers out{nullptr, 0};
    gvec_svg_parse_error err{0, nullptr};
    auto st = gvec_svg_parse_number_list(r.text, std::strlen(r.text), &c.ctx, &out, &err);
    EXPECT_EQ(st, r.status) << "'" << r.text << "'";
    if (r.status == GVEC_SVG_PARSE_OK) {
      EXPECT_EQ(Join(out), r.values) << r.text;
    } else {
      EXPECT_EQ(err.offset, r.offset) << r.text;
    }
  }
}

TEST(NumberList, TheLimitKeepsTheFirstItemsAndSaysSo) {
  Ctx c(3);
  gvec_svg_numbers out{nullptr, 0};
  gvec_svg_parse_error err{0, nullptr};
  EXPECT_EQ(gvec_svg_parse_number_list("1 2 3", 5, &c.ctx, &out, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(out.count, 3u);
  EXPECT_EQ(gvec_svg_parse_number_list("1 2 3 4", 7, &c.ctx, &out, &err), GVEC_SVG_PARSE_LIMIT);
  EXPECT_EQ(out.count, 3u);
  EXPECT_EQ(Join(out), "1,2,3");
  EXPECT_EQ(err.offset, 6u);
}

TEST(LengthList, LengthsWithUnitsSeparatedLikeNumbers) {
  Ctx c;
  gvec_svg_lengths out{nullptr, 0};
  gvec_svg_parse_error err{0, nullptr};
  ASSERT_EQ(gvec_svg_parse_length_list("1px, 2em 3%", 11, &c.ctx, &out, &err), GVEC_SVG_PARSE_OK);
  ASSERT_EQ(out.count, 3u);
  EXPECT_EQ(out.items[0].unit, GVEC_SVG_UNIT_PX);
  EXPECT_EQ(out.items[1].unit, GVEC_SVG_UNIT_EM);
  EXPECT_EQ(out.items[2].unit, GVEC_SVG_UNIT_PERCENT);
  EXPECT_EQ(gvec_svg_parse_length_list("1px,", 4, &c.ctx, &out, &err), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(gvec_svg_parse_length_list("1foo", 4, &c.ctx, &out, &err), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(err.offset, 1u);
}

TEST(Points, PairsOfNumbersAndAnOddCountKeepsTheCompletePairs) {
  Ctx c;
  gvec_svg_numbers out{nullptr, 0};
  gvec_svg_parse_error err{0, nullptr};
  ASSERT_EQ(gvec_svg_parse_points("0,0 10,0 10,10", 14, &c.ctx, &out, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(Join(out), "0,0,10,0,10,10");
  ASSERT_EQ(gvec_svg_parse_points("0,0 10,0 10", 11, &c.ctx, &out, &err), GVEC_SVG_PARSE_PARTIAL);
  EXPECT_EQ(Join(out), "0,0,10,0");
  ASSERT_EQ(gvec_svg_parse_points("10-5", 4, &c.ctx, &out, &err), GVEC_SVG_PARSE_OK) << "SVG 2: coordinate negative-coordinate";
  EXPECT_EQ(Join(out), "10,-5");
  EXPECT_EQ(gvec_svg_parse_points("1 x", 3, &c.ctx, &out, &err), GVEC_SVG_PARSE_INVALID) << "no complete pair before the error";
  ASSERT_EQ(gvec_svg_parse_points("0 1 2 3 text", 12, &c.ctx, &out, &err), GVEC_SVG_PARSE_PARTIAL) << "SVG 2: render up to the error";
  EXPECT_EQ(Join(out), "0,1,2,3");
  ASSERT_EQ(gvec_svg_parse_points("0 1 2 x", 7, &c.ctx, &out, &err), GVEC_SVG_PARSE_PARTIAL);
  EXPECT_EQ(Join(out), "0,1");
  EXPECT_EQ(gvec_svg_parse_number_list("0 1 x", 5, &c.ctx, &out, &err), GVEC_SVG_PARSE_INVALID) << "another list is dropped whole";
  ASSERT_EQ(gvec_svg_parse_points("", 0, &c.ctx, &out, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(out.count, 0u);
}

TEST(ViewBox, FourNumbersTheSizesNotNegative) {
  GVEC_Svg_View_Box v;
  gvec_svg_parse_error err{0, nullptr};
  auto parse = [&](const char * s) { return gvec_svg_parse_view_box(s, std::strlen(s), &v, &err); };
  ASSERT_EQ(parse("0 0 10 20"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v.width, 10.0);
  EXPECT_EQ(v.height, 20.0);
  ASSERT_EQ(parse(" -1,-2, 3 ,4 "), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v.x, -1.0);
  EXPECT_EQ(v.y, -2.0);
  ASSERT_EQ(parse("0 0 0 5"), GVEC_SVG_PARSE_OK) << "a zero size is kept; it disables rendering at lowering";
  for (const char * bad : {"", "0 0 10", "0 0 -1 5", "0 0 5 -1", "0 0 10 10 10", "a b c d", "0 0 10,", "0 0 1010"}) {
    EXPECT_EQ(parse(bad), GVEC_SVG_PARSE_INVALID) << bad;
  }
}

TEST(PreserveAspectRatio, AlignAndMeetOrSliceWithTheDeferKeyword) {
  GVEC_Svg_Preserve_Aspect_Ratio p;
  gvec_svg_parse_error err{0, nullptr};
  auto parse = [&](const char * s) { return gvec_svg_parse_par(s, std::strlen(s), &p, &err); };
  static const char * const aligns[] = {"none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid",
      "xMinYMax", "xMidYMax", "xMaxYMax"};
  for (size_t i = 0; i < 10; i++) {
    std::string meet = std::string(aligns[i]) + " meet", slice = std::string(aligns[i]) + " slice";
    ASSERT_EQ(parse(aligns[i]), GVEC_SVG_PARSE_OK) << aligns[i];
    EXPECT_EQ((size_t) p.align, i);
    EXPECT_EQ(p.slice, 0);
    ASSERT_EQ(parse(meet.c_str()), GVEC_SVG_PARSE_OK);
    EXPECT_EQ(p.slice, 0);
    ASSERT_EQ(parse(slice.c_str()), GVEC_SVG_PARSE_OK);
    EXPECT_EQ(p.slice, 1);
  }
  ASSERT_EQ(parse("defer xMinYMin slice"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(p.defer, 1);
  EXPECT_EQ(p.align, GVEC_SVG_ALIGN_X_MIN_Y_MIN);
  for (const char * bad : {"", "meet", "xMidYMid meet slice", "xmidymid", "xMidYMid  foo", "defer", "slice xMidYMid", "none none"}) {
    EXPECT_EQ(parse(bad), GVEC_SVG_PARSE_INVALID) << bad;
  }
}

TEST(Orient, AutoAutoStartReverseOrAnAngle) {
  GVEC_Svg_Orient o;
  gvec_svg_parse_error err{0, nullptr};
  auto parse = [&](const char * s) { return gvec_svg_parse_orient(s, std::strlen(s), &o, &err); };
  ASSERT_EQ(parse("auto"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(o.mode, GVEC_SVG_ORIENT_AUTO);
  ASSERT_EQ(parse(" auto-start-reverse "), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(o.mode, GVEC_SVG_ORIENT_AUTO_START_REVERSE);
  ASSERT_EQ(parse("45"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(o.mode, GVEC_SVG_ORIENT_ANGLE);
  EXPECT_EQ(o.angle, 45.0);
  ASSERT_EQ(parse("0.5turn"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(o.angle, 180.0);
  EXPECT_EQ(parse("Auto"), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(parse("auto-start"), GVEC_SVG_PARSE_INVALID);
}

TEST(Offset, ANumberOrAPercentageNotClamped) {
  double v;
  gvec_svg_parse_error err{0, nullptr};
  auto parse = [&](const char * s) { return gvec_svg_parse_offset(s, std::strlen(s), &v, &err); };
  ASSERT_EQ(parse("0.5"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v, 0.5);
  ASSERT_EQ(parse("25%"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v, 0.25);
  ASSERT_EQ(parse("150%"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v, 1.5);
  ASSERT_EQ(parse("-1"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(parse("5px"), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(parse("%"), GVEC_SVG_PARSE_INVALID);
}

TEST(Integer, SignedDigitsWithNoOverflow) {
  long v;
  gvec_svg_parse_error err{0, nullptr};
  auto parse = [&](const char * s) { return gvec_svg_parse_integer(s, std::strlen(s), &v, &err); };
  ASSERT_EQ(parse(" -12 "), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v, -12);
  ASSERT_EQ(parse("+7"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(v, 7);
  EXPECT_EQ(parse(""), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(parse("1.5"), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(parse("99999999999999999999999"), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(parse("-"), GVEC_SVG_PARSE_INVALID);
}

TEST(Keyword, AnEnumerationValueIsCaseSensitive) {
  static const char * const words[] = {"pad", "reflect", "repeat"};
  size_t index;
  gvec_svg_parse_error err{0, nullptr};
  ASSERT_EQ(gvec_svg_parse_keyword(" reflect ", 9, words, 3, &index, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(index, 1u);
  EXPECT_EQ(gvec_svg_parse_keyword("Pad", 3, words, 3, &index, &err), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(gvec_svg_parse_keyword("pad pad", 7, words, 3, &index, &err), GVEC_SVG_PARSE_INVALID);
  EXPECT_EQ(gvec_svg_parse_keyword("", 0, words, 3, &index, &err), GVEC_SVG_PARSE_INVALID);
}

TEST(RefCoord, ALengthOrAKeywordAsAPercentage) {
  GVEC_Svg_Length l;
  gvec_svg_parse_error err{0, nullptr};
  ASSERT_EQ(gvec_svg_parse_ref_coord("left", 4, true, &l, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(l.value, 0.0);
  EXPECT_EQ(l.unit, GVEC_SVG_UNIT_PERCENT);
  ASSERT_EQ(gvec_svg_parse_ref_coord("right", 5, true, &l, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(l.value, 100.0);
  ASSERT_EQ(gvec_svg_parse_ref_coord("center", 6, false, &l, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(l.value, 50.0);
  ASSERT_EQ(gvec_svg_parse_ref_coord("bottom", 6, false, &l, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(l.value, 100.0);
  EXPECT_EQ(gvec_svg_parse_ref_coord("top", 3, true, &l, &err), GVEC_SVG_PARSE_INVALID) << "top is not a horizontal keyword";
  ASSERT_EQ(gvec_svg_parse_ref_coord("2.5em", 5, true, &l, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(l.unit, GVEC_SVG_UNIT_EM);
}

TEST(Reference, InternalDataExternalAndEmpty) {
  Ctx c;
  GVEC_Svg_Reference r;
  gvec_svg_parse_error err{0, nullptr};
  auto parse = [&](const std::string & s) { return gvec_svg_parse_reference(s.data(), s.size(), &c.ctx, &r, &err); };
  ASSERT_EQ(parse(""), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(r.kind, GVEC_SVG_REF_EMPTY);
  ASSERT_EQ(parse("  #a%20b  "), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(r.kind, GVEC_SVG_REF_INTERNAL);
  EXPECT_STREQ(r.fragment, "a b") << "the fragment is percent-decoded";
  ASSERT_EQ(parse("#a%2"), GVEC_SVG_PARSE_OK);
  EXPECT_STREQ(r.fragment, "a%2") << "an incomplete escape is kept as written";
  ASSERT_EQ(parse("http://example.org/x.svg#a"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(r.kind, GVEC_SVG_REF_EXTERNAL);
  EXPECT_STREQ(r.url, "http://example.org/x.svg#a");
  ASSERT_EQ(parse("other.svg"), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(r.kind, GVEC_SVG_REF_EXTERNAL);
}

TEST(DataUri, TheHeaderAndThePayloadAreSplitPerRfc2397) {
  Ctx c;
  GVEC_Svg_Reference r;
  gvec_svg_parse_error err{0, nullptr};
  auto parse = [&](const std::string & s) { return gvec_svg_parse_reference(s.data(), s.size(), &c.ctx, &r, &err); };
  ASSERT_EQ(parse("data:image/PNG;base64,iVBORw0KGgo="), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(r.kind, GVEC_SVG_REF_DATA);
  EXPECT_STREQ(r.data.media_type, "image/png");
  EXPECT_STREQ(r.data.parameters, "");
  EXPECT_EQ(r.data.base64, 1);
  EXPECT_EQ(r.data.payload_length, (size_t) 12);
  ASSERT_EQ(parse("DATA:image/svg+xml;charset=utf-8,%3Csvg%2F%3E"), GVEC_SVG_PARSE_OK);
  EXPECT_STREQ(r.data.media_type, "image/svg+xml");
  EXPECT_STREQ(r.data.parameters, "charset=utf-8");
  EXPECT_EQ(r.data.base64, 0);
  EXPECT_STREQ(r.data.payload, "%3Csvg%2F%3E");
  ASSERT_EQ(parse("data:,hello"), GVEC_SVG_PARSE_OK);
  EXPECT_STREQ(r.data.media_type, "");
  EXPECT_STREQ(r.data.payload, "hello");
  ASSERT_EQ(parse("data:;charset=x;base64,AAAA"), GVEC_SVG_PARSE_OK);
  EXPECT_STREQ(r.data.media_type, "");
  EXPECT_STREQ(r.data.parameters, "charset=x");
  EXPECT_EQ(r.data.base64, 1);
  EXPECT_EQ(parse("data:image/png;base64"), GVEC_SVG_PARSE_INVALID) << "no comma";
}

TEST(Language, ACommaSeparatedListOfTags) {
  Ctx c;
  const char ** tags = nullptr;
  size_t n = 0;
  gvec_svg_parse_error err{0, nullptr};
  std::string s = " en-US ,fr,, de ";
  ASSERT_EQ(gvec_svg_parse_language_list(s.data(), s.size(), &c.ctx, &tags, &n, &err), GVEC_SVG_PARSE_OK);
  ASSERT_EQ(n, 3u);
  EXPECT_STREQ(tags[0], "en-US");
  EXPECT_STREQ(tags[1], "fr");
  EXPECT_STREQ(tags[2], "de");
  ASSERT_EQ(gvec_svg_parse_language_list("", 0, &c.ctx, &tags, &n, &err), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(n, 0u);
  size_t count;
  ASSERT_EQ(gvec_svg_parse_extension_list("  a b\tc ", 8, &count), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(count, 3u);
  ASSERT_EQ(gvec_svg_parse_extension_list("", 0, &count), GVEC_SVG_PARSE_OK);
  EXPECT_EQ(count, 0u);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
