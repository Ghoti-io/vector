/**
 * @file
 *
 * The nesting limits of the lowering: a chain of masks, patterns, clip paths, filters or a filter list past the depth the lowering carries is refused
 * (not drawn without it), and a chain at the depth is lowered.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

std::string Root(const std::string & body) {
  return Svg("width=\"100\" height=\"100\"", body);
}

/** A chain of `n` elements `tag`, each (but the last) naming the next by `attribute`, as defs, and a rect that names the first by `use`. */
std::string MaskChain(int n) {
  std::string defs;
  for (int i = 0; i < n; i++) {
    defs += "<mask id=\"m" + std::to_string(i) + "\"><rect width=\"50\" height=\"50\" fill=\"white\"" +
        (i + 1 < n ? " mask=\"url(#m" + std::to_string(i + 1) + ")\"" : "") + "/></mask>";
  }
  return Root("<defs>" + defs + "</defs><rect width=\"100\" height=\"100\" mask=\"url(#m0)\"/>");
}

std::string PatternChain(int n) {
  std::string defs;
  for (int i = 0; i < n; i++) {
    defs += "<pattern id=\"p" + std::to_string(i) + "\" patternUnits=\"userSpaceOnUse\" width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\" fill=\"" +
        (i + 1 < n ? "url(#p" + std::to_string(i + 1) + ")" : std::string("green")) + "\"/></pattern>";
  }
  return Root("<defs>" + defs + "</defs><rect width=\"100\" height=\"100\" fill=\"url(#p0)\"/>");
}

std::string ClipChain(int n) {
  std::string defs;
  for (int i = 0; i < n; i++) {
    defs += "<clipPath id=\"c" + std::to_string(i) + "\"" + (i + 1 < n ? " clip-path=\"url(#c" + std::to_string(i + 1) + ")\"" : "") +
        "><rect width=\"80\" height=\"80\"/></clipPath>";
  }
  return Root("<defs>" + defs + "</defs><rect width=\"100\" height=\"100\" clip-path=\"url(#c0)\"/>");
}

std::string FilterList(int n) {
  std::string list;
  for (int i = 0; i < n; i++) {
    list += "url(#f) blur(1px) "; /* a run of functions is one step: a url between them makes each a step of its own */
  }
  return Root("<filter id=\"f\"><feOffset dx=\"1\"/></filter><rect width=\"50\" height=\"50\" style=\"filter: " + list + "\"/>");
}

/** A filter whose feImage draws a rect that is itself filtered by the next filter, `n` deep. */
std::string FilterChain(int n) {
  std::string defs;
  for (int i = 0; i < n; i++) {
    defs += "<filter id=\"f" + std::to_string(i) + "\"><feImage href=\"#r" + std::to_string(i) + "\"/></filter><rect id=\"r" + std::to_string(i) +
        "\" width=\"20\" height=\"20\"" + (i + 1 < n ? " filter=\"url(#f" + std::to_string(i + 1) + ")\"" : "") + "/>";
  }
  return Root("<defs>" + defs + "</defs><rect width=\"100\" height=\"100\" filter=\"url(#f0)\"/>");
}

/** The count of refusals of `l`, and a description of their reasons for a failure message. */
std::string Reasons(const Lowered & l) {
  std::string out;
  for (int r = 0; r < GVEC_LOWER_REASON_COUNT; r++) {
    if (l.Refusal((GVEC_Lower_Reason) r)) {
      out += std::string(out.empty() ? "" : ", ") + gvec_lower_reason_string((GVEC_Lower_Reason) r);
    }
  }
  return out.empty() ? "none" : out;
}

}  // namespace

TEST(LowerNesting, AChainOfMasksIsLoweredToTheDepthAndRefusedPastIt) {
  Lowered shallow, deep;
  LowerSvg(MaskChain(4), shallow);
  EXPECT_EQ(shallow.Refusals(), (size_t) 0) << Reasons(shallow);
  EXPECT_GT(shallow.Items(), (size_t) 0);
  LowerSvg(MaskChain(14), deep);
  EXPECT_GE(deep.Refusals(), (size_t) 1) << "a mask chain of 14 is past the lowering's depth";
}

TEST(LowerNesting, AChainOfPatternsIsLoweredToTheDepthAndRefusedPastIt) {
  Lowered shallow, deep;
  LowerSvg(PatternChain(4), shallow);
  EXPECT_EQ(shallow.Refusals(), (size_t) 0) << Reasons(shallow);
  LowerSvg(PatternChain(14), deep);
  EXPECT_GE(deep.Refusals(), (size_t) 1) << "a pattern chain of 14 is past the lowering's depth";
}

TEST(LowerNesting, AChainOfClipPathsIsLoweredToTheDepthAndRefusedPastIt) {
  Lowered shallow, deep;
  LowerSvg(ClipChain(4), shallow);
  EXPECT_EQ(shallow.Refusals(), (size_t) 0) << Reasons(shallow);
  LowerSvg(ClipChain(14), deep);
  EXPECT_GE(deep.Refusals(), (size_t) 1) << "a clip chain of 14 is past the lowering's depth";
}

TEST(LowerNesting, AFilterListLongerThanTheLoweringNestsIsRefused) {
  Lowered shallow, deep;
  LowerSvg(FilterList(2), shallow);
  EXPECT_EQ(shallow.Refusals(), (size_t) 0) << Reasons(shallow);
  LowerSvg(FilterList(6), deep);
  EXPECT_GE(deep.Refusals(), (size_t) 1) << "a list of twelve steps";
}

TEST(LowerNesting, FiltersThatDrawElementsThroughFiltersAreLoweredToTheDepthAndRefusedPastIt) {
  Lowered shallow, deep;
  LowerSvg(FilterChain(2), shallow);
  EXPECT_EQ(shallow.Refusals(), (size_t) 0) << Reasons(shallow);
  LowerSvg(FilterChain(9), deep);
  EXPECT_GE(deep.Refusals(), (size_t) 1) << "feImages through nine filters";
}

TEST(LowerNesting, AClipOfUnionsWhoseTermsPassTheLimitIsRefused) {
  // each clipPath is a union of six shapes, each cut by the next clipPath: 6^5 = 7776 terms of the region, past the 4096 the lowering keeps
  auto chain = [](int levels) {
    std::string defs;
    for (int level = 0; level < levels; level++) {
      defs += "<clipPath id=\"c" + std::to_string(level) + "\">";
      for (int i = 0; i < 6; i++) {
        defs += "<rect x=\"" + std::to_string(i) + "\" width=\"90\" height=\"90\"" +
            (level + 1 < levels ? " clip-path=\"url(#c" + std::to_string(level + 1) + ")\"" : "") + "/>";
      }
      defs += "</clipPath>";
    }
    return Root("<defs>" + defs + "</defs><rect width=\"100\" height=\"100\" clip-path=\"url(#c0)\"/>");
  };
  auto message = [&](int levels, size_t * items) {
    Lowered l;
    LowerSvg(chain(levels), l);
    *items = l.Items();
    const GVEC_Diagnostic * d = l.Refusal(GVEC_LOWER_LIMIT);
    return d ? std::string(d->message) : (l.Refusals() ? "(another reason: " + Reasons(l) + ")" : std::string());
  };
  size_t items = 0;
  EXPECT_EQ(message(2, &items), "") << "6 x 6 terms are lowered";
  EXPECT_GT(items, (size_t) 0);
  // past the union the waist holds, the clip is refused at the shapes of the union before its terms are many
  EXPECT_EQ(message(3, &items), "the clip has more shapes than the waist's union holds");
  EXPECT_EQ(items, (size_t) 0);
  // and past the terms the lowering keeps of one region (6^5), at the term
  EXPECT_EQ(message(6, &items), "a clip that clips its own shapes is past what the waist's union holds");
  EXPECT_EQ(items, (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
