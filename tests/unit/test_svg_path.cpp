/**
 * @file
 *
 * Path data (SVG 2, section 9.3): the commands as written and the absolute form, the error rule
 * (render up to the last complete segment), and the bound. The expected values are worked out by hand from
 * the specification's rules for relative commands, smooth curves and closepath.
 */

#include "test_helpers.h"

#include <cstring>
#include <sstream>
#include <string>

#include "../../src/codec/svg/svg_grammar.h"

namespace {

struct Parsed {
  gvec_arena arena;
  gvec_svg_path path{nullptr, 0, nullptr, 0};
  gvec_svg_parse_status status = GVEC_SVG_PARSE_OOM;
  gvec_svg_parse_error err{0, nullptr};
  Parsed(const std::string & s, size_t max_segments = 1000) {
    gvec_arena_init(&arena, nullptr);
    gvec_svg_parse_context ctx{&arena, nullptr, 1000, max_segments};
    status = gvec_svg_parse_path(s.data(), s.size(), &ctx, &path, &err);
  }
  ~Parsed() { gvec_arena_destroy(&arena); }

  /** Each segment as KIND(x0,y0>...end) with the controls or arc parameters in between. */
  std::string Abs() const {
    static const char kinds[] = "MLQCAZ";
    std::ostringstream o;
    for (size_t i = 0; i < path.segment_count; i++) {
      const GVEC_Svg_Path_Segment & s = path.segments[i];
      if (i) {
        o << " ";
      }
      o << kinds[s.kind] << "(" << s.x0 << "," << s.y0;
      if (s.kind == GVEC_SVG_SEGMENT_QUAD) {
        o << ";" << s.x1 << "," << s.y1;
      } else if (s.kind == GVEC_SVG_SEGMENT_CUBIC) {
        o << ";" << s.x1 << "," << s.y1 << ";" << s.x2 << "," << s.y2;
      } else if (s.kind == GVEC_SVG_SEGMENT_ARC) {
        o << ";" << s.rx << "," << s.ry << "," << s.rotation << "," << (int) s.large_arc << "," << (int) s.sweep;
      }
      o << ">" << s.x << "," << s.y << ")";
    }
    return o.str();
  }

  /** The commands as written: letters (a leading + marks an implicit repeat) with their arguments. */
  std::string Cmds() const {
    std::ostringstream o;
    for (size_t i = 0; i < path.command_count; i++) {
      const GVEC_Svg_Path_Command & c = path.commands[i];
      if (i) {
        o << ";";
      }
      o << (c.implicit ? "+" : "") << c.letter;
      for (int k = 0; k < c.arg_count; k++) {
        o << (k ? "," : " ") << c.args[k];
      }
    }
    return o.str();
  }
};

}  // namespace

TEST(Path, AbsoluteCommandsAreKeptAsWrittenAndInAbsoluteForm) {
  Parsed p("M 10 20 L 30 40 Z");
  ASSERT_EQ(p.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(p.Cmds(), "M 10,20;L 30,40;Z");
  EXPECT_EQ(p.Abs(), "M(0,0>10,20) L(10,20>30,40) Z(30,40>10,20)");
}

TEST(Path, RelativeCommandsAreRelativeToTheCurrentPointAndTheFirstMovetoIsAbsolute) {
  Parsed p("m 10 20 l 5 5 h 10 v -5 z");
  ASSERT_EQ(p.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(p.Cmds(), "m 10,20;l 5,5;h 10;v -5;z");
  EXPECT_EQ(p.Abs(), "M(0,0>10,20) L(10,20>15,25) L(15,25>25,25) L(25,25>25,20) Z(25,20>10,20)");
}

TEST(Path, AnImplicitRepeatAfterAMovetoIsALineto) {
  Parsed p("M 0 0 10 10 20 20");
  ASSERT_EQ(p.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(p.Cmds(), "M 0,0;+L 10,10;+L 20,20");
  EXPECT_EQ(p.Abs(), "M(0,0>0,0) L(0,0>10,10) L(10,10>20,20)");
  Parsed r("m 1 1 2 2 3 3");
  ASSERT_EQ(r.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(r.Cmds(), "m 1,1;+l 2,2;+l 3,3");
  EXPECT_EQ(r.Abs(), "M(0,0>1,1) L(1,1>3,3) L(3,3>6,6)");
  Parsed c("M0 0 L 1 1 2 2, 3 3");
  ASSERT_EQ(c.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(c.Cmds(), "M 0,0;L 1,1;+L 2,2;+L 3,3") << "the same command repeats, with or without a comma";
}

TEST(Path, NumbersNeedNoSeparatorWhereTheyCannotRunTogether) {
  Parsed p("M10-5.5L.5.5");
  ASSERT_EQ(p.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(p.Abs(), "M(0,0>10,-5.5) L(10,-5.5>0.5,0.5)");
  Parsed e("M1e2-1e-1l1,1");
  ASSERT_EQ(e.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(e.Abs(), "M(0,0>100,-0.1) L(100,-0.1>101,0.9)");
}

TEST(Path, ArcFlagsAreSingleCharactersAndNeedNoSeparator) {
  Parsed a("M0 0 A1 1 0 11 5 5");
  ASSERT_EQ(a.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(a.Cmds(), "M 0,0;A 1,1,0,1,1,5,5");
  EXPECT_EQ(a.Abs(), "M(0,0>0,0) A(0,0;1,1,0,1,1>5,5)");
  Parsed b("M 5 5 a 2 3 45 0 1 10 -4");
  ASSERT_EQ(b.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(b.Abs(), "M(0,0>5,5) A(5,5;2,3,45,0,1>15,1)") << "an arc stays an arc, in endpoint form, relative end made absolute";
  Parsed c("M0 0 A1 1 0 0,1,5,5 A1 1 0 1 0 6 6");
  ASSERT_EQ(c.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(c.Abs(), "M(0,0>0,0) A(0,0;1,1,0,0,1>5,5) A(5,5;1,1,0,1,0>6,6)");
  Parsed bad("M0 0 A1 1 0 2 0 5 5");
  EXPECT_EQ(bad.status, GVEC_SVG_PARSE_PARTIAL);
  EXPECT_EQ(bad.path.segment_count, 1u);
  EXPECT_EQ(bad.err.offset, (size_t) 12);
  Parsed bad2("M0 0 A1 1 0 1 x 5 5");
  EXPECT_EQ(bad2.status, GVEC_SVG_PARSE_PARTIAL);
}

TEST(Path, SmoothCurvesReflectThePreviousControlPointOnlyAfterTheSameFamily) {
  Parsed s("M0 0 C 1 1 2 2 3 3 S 5 5 6 6");
  ASSERT_EQ(s.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(s.Abs(), "M(0,0>0,0) C(0,0;1,1;2,2>3,3) C(3,3;4,4;5,5>6,6)");
  Parsed alone("M0 0 S 1 1 2 2");
  ASSERT_EQ(alone.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(alone.Abs(), "M(0,0>0,0) C(0,0;0,0;1,1>2,2)") << "no previous cubic: the first control point is the current point";
  Parsed afterq("M0 0 Q 1 1 2 2 S 5 5 6 6");
  ASSERT_EQ(afterq.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(afterq.Abs(), "M(0,0>0,0) Q(0,0;1,1>2,2) C(2,2;2,2;5,5>6,6)") << "a quadratic does not feed a smooth cubic";
  Parsed t("M0 0 Q 1 1 2 2 T 4 4");
  ASSERT_EQ(t.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(t.Abs(), "M(0,0>0,0) Q(0,0;1,1>2,2) Q(2,2;3,3>4,4)");
  Parsed tl("M0 0 L 1 1 T 2 2");
  ASSERT_EQ(tl.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(tl.Abs(), "M(0,0>0,0) L(0,0>1,1) Q(1,1;1,1>2,2)");
  Parsed tc("M0 0 C 1 1 2 2 3 3 T 5 5");
  ASSERT_EQ(tc.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(tc.Abs(), "M(0,0>0,0) C(0,0;1,1;2,2>3,3) Q(3,3;3,3>5,5)") << "a cubic does not feed a smooth quadratic";
  Parsed tt("M0 0 Q 1 1 2 2 T 4 4 T 6 6");
  ASSERT_EQ(tt.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(tt.Abs(), "M(0,0>0,0) Q(0,0;1,1>2,2) Q(2,2;3,3>4,4) Q(4,4;5,5>6,6)");
  Parsed rel("m 0 0 c 1 1 2 2 3 3 s 1 1 2 2");
  ASSERT_EQ(rel.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(rel.Abs(), "M(0,0>0,0) C(0,0;1,1;2,2>3,3) C(3,3;4,4;4,4>5,5)");
}

TEST(Path, ClosepathReturnsToTheSubpathStartAndTheNextCommandStartsThere) {
  Parsed p("M 10 10 L 20 20 Z m 5 5");
  ASSERT_EQ(p.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(p.Abs(), "M(0,0>10,10) L(10,10>20,20) Z(20,20>10,10) M(10,10>15,15)");
  Parsed q("M 10 10 L 20 20 Z L 30 30");
  ASSERT_EQ(q.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(q.Abs(), "M(0,0>10,10) L(10,10>20,20) Z(20,20>10,10) L(10,10>30,30)");
  Parsed h("M 1 2 H 5 V 9 h -1 v -1");
  ASSERT_EQ(h.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(h.Abs(), "M(0,0>1,2) L(1,2>5,2) L(5,2>5,9) L(5,9>4,9) L(4,9>4,8)");
  Parsed zz("M 1 1 Z Z");
  ASSERT_EQ(zz.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(zz.path.segment_count, 3u);
}

TEST(Path, AnEmptyValueIsAnEmptyPath) {
  for (const char * s : {"", "   ", "\n\t "}) {
    Parsed p(s);
    EXPECT_EQ(p.status, GVEC_SVG_PARSE_OK) << "'" << s << "'";
    EXPECT_EQ(p.path.segment_count, 0u);
  }
}

TEST(Path, AnErrorKeepsTheSegmentsBeforeItAndSaysWhere) {
  struct Row {
    const char * text;
    gvec_svg_parse_status status;
    size_t segments;
    size_t offset;
  };
  const Row rows[] = {
      {"L 1 1", GVEC_SVG_PARSE_INVALID, 0, 0},       // must begin with a moveto
      {"1 1", GVEC_SVG_PARSE_INVALID, 0, 0},
      {"M", GVEC_SVG_PARSE_INVALID, 0, 1},
      {"M 1", GVEC_SVG_PARSE_INVALID, 0, 3},
      {"M 1 2 L 3", GVEC_SVG_PARSE_PARTIAL, 1, 9},   // the second argument is missing
      {"M 1 2 X", GVEC_SVG_PARSE_PARTIAL, 1, 6},
      {"M 1 2, L 3 4", GVEC_SVG_PARSE_PARTIAL, 1, 5},  // a comma before a command
      {"M 1 2 3", GVEC_SVG_PARSE_PARTIAL, 1, 7},     // an implicit lineto that is not complete
      {"M 1 2 L 3 4,", GVEC_SVG_PARSE_PARTIAL, 2, 11},  // a trailing comma
      {"M 0 0 Z 5", GVEC_SVG_PARSE_PARTIAL, 2, 8},   // a number after closepath
      {"M 1e999 2", GVEC_SVG_PARSE_INVALID, 0, 2},
      {"M 0 0 L 1e999 2", GVEC_SVG_PARSE_PARTIAL, 1, 8},
      {"M 0 0 q 1 1 2", GVEC_SVG_PARSE_PARTIAL, 1, 13},
  };
  for (const Row & r : rows) {
    Parsed p(r.text);
    EXPECT_EQ(p.status, r.status) << r.text;
    EXPECT_EQ(p.path.segment_count, r.segments) << r.text;
    EXPECT_EQ(p.err.offset, r.offset) << r.text;
    EXPECT_NE(p.err.reason, nullptr) << r.text;
  }
}

TEST(Path, ARelativeSumThatOverflowsIsAnError) {
  Parsed p("M 1e308 0 l 1e307 0 l 1e308 0");
  EXPECT_EQ(p.status, GVEC_SVG_PARSE_PARTIAL);
  EXPECT_EQ(p.path.segment_count, 2u) << "the third lineto reaches 2.1e308 > DBL_MAX";
  Parsed q("M 1e308 0 l 1e308 0");
  EXPECT_EQ(q.status, GVEC_SVG_PARSE_PARTIAL);
  EXPECT_EQ(q.path.segment_count, 1u) << "the moveto is complete";
}

TEST(Path, TheSegmentBoundKeepsThePrefixAndSaysSo) {
  Parsed p("M 0 0 L 1 1 L 2 2 L 3 3 L 4 4 L 5 5", 4);
  EXPECT_EQ(p.status, GVEC_SVG_PARSE_LIMIT);
  EXPECT_EQ(p.path.segment_count, 4u);
  EXPECT_EQ(p.Abs(), "M(0,0>0,0) L(0,0>1,1) L(1,1>2,2) L(2,2>3,3)");
  Parsed q("M 0 0 L 1 1 L 2 2 L 3 3", 4);
  EXPECT_EQ(q.status, GVEC_SVG_PARSE_OK) << "exactly the bound is not over it";
  Parsed r("M 0 0 1 1 2 2 3 3 4 4 5 5", 4);
  EXPECT_EQ(r.status, GVEC_SVG_PARSE_LIMIT) << "implicit repeats count too";
  EXPECT_EQ(r.path.segment_count, 4u);
}

TEST(Path, EveryCommandInBothCasesHasItsArgumentCount) {
  Parsed p("M1 2 m1 2 L1 2 l1 2 H1 h1 V1 v1 C1 2 3 4 5 6 c1 2 3 4 5 6 S1 2 3 4 s1 2 3 4 Q1 2 3 4 q1 2 3 4 T1 2 t1 2 A1 2 3 0 1 4 5 a1 2 3 1 0 4 5 Z z");
  ASSERT_EQ(p.status, GVEC_SVG_PARSE_OK);
  EXPECT_EQ(p.path.command_count, 20u);
  EXPECT_EQ(p.path.segment_count, 20u);
  size_t args = 0;
  for (size_t i = 0; i < p.path.command_count; i++) {
    args += p.path.commands[i].arg_count;
  }
  EXPECT_EQ(args, (size_t) (2 + 2 + 2 + 2 + 1 + 1 + 1 + 1 + 6 + 6 + 4 + 4 + 4 + 4 + 2 + 2 + 7 + 7));
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
