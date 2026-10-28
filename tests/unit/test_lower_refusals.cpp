/**
 * @file
 *
 * The refusals (P6, P9, P10): every reason, each located; an element is refused whole; the report is what a caller prints.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** What a caller prints for a refusal. */
std::string Printed(const GVEC_Diagnostic & d) {
  return "line " + std::to_string(d.line) + ", " + (d.element ? d.element : "?") + ": refused, " +
         gvec_lower_reason_string((GVEC_Lower_Reason) d.detail);
}

struct Case {
  const char * name;
  GVEC_Lower_Reason reason;
  const char * body;      ///< Document content; the refused element is on the line named below, after a first line.
  const char * element;
  size_t line;
  size_t drawn;           ///< The fills the document still draws.
};

}  // namespace

TEST(LowerRefusals, EveryReasonIsLocatedNamedAndRefusesTheElementWhole) {
  static const char * const square = "<path d=\"M0 0H4V4H0Z\"/>";
  const Case cases[] = {
      {"arc", GVEC_LOWER_ARC, "\n<path d=\"M0 0A5 5 0 0 1 9 9\"/>", "path", 2, 0},
      {"rect", GVEC_LOWER_RECT, "\n<rect width=\"4\" height=\"4\"/>", "rect", 2, 0},
      {"circle", GVEC_LOWER_RECT, "\n<circle r=\"4\"/>", "circle", 2, 0},
      {"ellipse", GVEC_LOWER_RECT, "\n<ellipse rx=\"4\" ry=\"2\"/>", "ellipse", 2, 0},
      {"vector-effect", GVEC_LOWER_VECTOR_EFFECT, "\n<path d=\"M0 0H4V4H0Z\" stroke=\"red\" style=\"vector-effect: non-scaling-size\"/>", "path", 2, 0},
      {"marker", GVEC_LOWER_MARKER, "\n<defs><marker id=\"m\"/></defs><path d=\"M0 0H4V4H0Z\" marker-start=\"url(#m)\"/>", "path", 2, 0},
      {"gradient", GVEC_LOWER_GRADIENT, "<defs><linearGradient id=\"g\"/></defs>\n<path d=\"M0 0H4V4H0Z\" fill=\"url(#g)\"/>", "path", 2, 0},
      {"pattern", GVEC_LOWER_PATTERN, "<defs><pattern id=\"p\"/></defs>\n<path d=\"M0 0H4V4H0Z\" fill=\"url(#p)\"/>", "path", 2, 0},
      {"clip-path", GVEC_LOWER_CLIP_PATH, "<defs><clipPath id=\"c\"/></defs>\n<path d=\"M0 0H4V4H0Z\" clip-path=\"url(#c)\"/>", "path", 2, 0},
      {"clip-path shape", GVEC_LOWER_CLIP_PATH, "\n<path d=\"M0 0H4V4H0Z\" style=\"clip-path: inset(1px)\"/>", "path", 2, 0},
      {"clip-path group", GVEC_LOWER_CLIP_PATH, "<defs><clipPath id=\"c\"/></defs>\n<g clip-path=\"url(#c)\"><path d=\"M0 0H4V4H0Z\"/></g>", "g", 2, 0},
      {"mask", GVEC_LOWER_MASK, "<defs><mask id=\"k\"/></defs>\n<path d=\"M0 0H4V4H0Z\" mask=\"url(#k)\"/>", "path", 2, 0},
      {"image", GVEC_LOWER_IMAGE, "\n<image width=\"4\" height=\"4\"/>", "image", 2, 0},
      {"filter", GVEC_LOWER_FILTER, "<defs><filter id=\"f\"/></defs>\n<path d=\"M0 0H4V4H0Z\" filter=\"url(#f)\"/>", "path", 2, 0},
      {"filter to nothing", GVEC_LOWER_FILTER, "\n<path d=\"M0 0H4V4H0Z\" filter=\"url(#nothing)\"/>", "path", 2, 0},
      {"text", GVEC_LOWER_TEXT, "\n<text>hello</text>", "text", 2, 0},
      {"blend", GVEC_LOWER_BLEND, "\n<path d=\"M0 0H4V4H0Z\" style=\"mix-blend-mode: multiply\"/>", "path", 2, 0},
      {"context paint", GVEC_LOWER_CONTEXT_PAINT, "\n<path d=\"M0 0H4V4H0Z\" fill=\"context-stroke\"/>", "path", 2, 0},
      {"color space", GVEC_LOWER_UNSUPPORTED_COLOR_SPACE, "\n<path d=\"M0 0H4V4H0Z\" fill=\"lab(50% 40 59)\"/>", "path", 2, 0},
      {"foreignObject", GVEC_LOWER_FOREIGN_OBJECT, "\n<foreignObject width=\"4\" height=\"4\"/>", "foreignObject", 2, 0},
      {"external reference", GVEC_LOWER_EXTERNAL_REFERENCE, "\n<use href=\"other.svg#x\"/>", "use", 2, 0},
      {"data reference", GVEC_LOWER_EXTERNAL_REFERENCE, "\n<use href=\"data:image/svg+xml;utf8,&lt;svg/&gt;\"/>", "use", 2, 0},
      {"cycle", GVEC_LOWER_REFERENCE_CYCLE, "\n<use id=\"u\" href=\"#u\"/>", "use", 2, 0},
      {"transform box", GVEC_LOWER_TRANSFORM_BOX, "\n<path d=\"M0 0H4V4H0Z\" style=\"transform: translate(5%, 0); transform-box: fill-box\"/>", "path", 2, 0},
      {"video", GVEC_LOWER_EMBEDDED_CONTENT, "\n<video width=\"4\" height=\"4\"/>", "video", 2, 0},
  };
  for (const Case & c : cases) {
    Lowered l;
    LowerSvg(Svg("width=\"20\" height=\"20\"", std::string(c.body) + square), l);
    ASSERT_EQ(l.Refusals(), (size_t) 1) << c.name;
    const GVEC_Diagnostic * d = l.Refusal(c.reason);
    ASSERT_NE(d, nullptr) << c.name;
    EXPECT_EQ(d->severity, GVEC_DIAG_ERROR) << c.name;
    EXPECT_STREQ(d->element, c.element) << c.name;
    EXPECT_EQ(d->line, c.line) << c.name;
    EXPECT_GT(d->column, (size_t) 0) << c.name;
    EXPECT_EQ(Printed(*d), "line " + std::to_string(c.line) + ", " + c.element + ": refused, " + gvec_lower_reason_string(c.reason)) << c.name;
    // The element is refused whole, and the rest of the document is drawn: the one trailing square.
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1) << c.name;
  }
}

TEST(LowerRefusals, TheExpansionLimitIsReportedAtTheUse) {
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_reference_expansion = 5;
  Lowered l;
  LoadSvg(Svg("width=\"20\" height=\"20\"",
              "<defs><g id=\"g\"><path d=\"M0 0H1V1Z\"/><path d=\"M0 0H1V1Z\"/><path d=\"M0 0H1V1Z\"/><path d=\"M0 0H1V1Z\"/>"
              "<path d=\"M0 0H1V1Z\"/><path d=\"M0 0H1V1Z\"/><path d=\"M0 0H1V1Z\"/></g></defs>\n<use href=\"#g\"/>"),
      l.loaded, 0, 0, &limits);
  ASSERT_EQ(l.loaded.result, GVEC_OK);
  LowerBoth(l.loaded, l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  const GVEC_Diagnostic * d = l.Refusal(GVEC_LOWER_EXPANSION_LIMIT);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->line, (size_t) 2);
}

TEST(LowerRefusals, ASinkSeenElementIsRefusedOnceHoweverOftenItIsDrawn) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<defs><path id=\"a\" d=\"M0 0A1 1 0 0 1 2 2\"/></defs><use href=\"#a\"/><use href=\"#a\" x=\"5\"/><g><use href=\"#a\" x=\"9\"/></g>"),
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 1);
}

TEST(LowerRefusals, ARefusedElementInsideANonRenderedContainerIsNotReported) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<defs><path d=\"M0 0A1 1 0 0 1 2 2\"/><rect width=\"1\" height=\"1\"/></defs>"
               "<g display=\"none\"><text>x</text><image/></g><path d=\"M0 0H3V3Z\" stroke=\"red\" visibility=\"hidden\"/>"),
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Items(), (size_t) 0);
}

TEST(LowerRefusals, ARectThatPaintsNothingIsNotARefusal) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"", "<rect width=\"4\" height=\"4\" fill=\"none\"/><circle r=\"3\" fill-opacity=\"0\"/>"), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerRefusals, ADanglingClipPathIsIgnoredAndADanglingMaskOrFilterIsNot) {
  Lowered l;
  LowerSvg(Svg("width=\"20\" height=\"20\"",
               "<path d=\"M0 0H4V4H0Z\" clip-path=\"url(#nothing)\"/><path d=\"M0 0H4V4H0Z\" mask=\"url(#nothing)\"/>"),
      l);
  EXPECT_EQ(l.Items(), (size_t) 1);
  EXPECT_NE(l.Refusal(GVEC_LOWER_MASK), nullptr);
  EXPECT_EQ(l.Refusals(), (size_t) 1);
}

TEST(LowerRefusals, EachReasonHasANameAndTheNumbersAreStable) {
  EXPECT_EQ(GVEC_LOWER_ARC, 1);
  EXPECT_EQ(GVEC_LOWER_RECT, 2);
  EXPECT_EQ(GVEC_LOWER_MARKER, 3);
  EXPECT_STREQ(gvec_lower_reason_string(GVEC_LOWER_ARC), "arc");
  EXPECT_STREQ(gvec_lower_reason_string(GVEC_LOWER_CLIP_PATH), "clip-path");
  EXPECT_STREQ(gvec_lower_reason_string((GVEC_Lower_Reason) 0), "unknown");
  EXPECT_STREQ(gvec_lower_reason_string(GVEC_LOWER_REASON_COUNT), "unknown");
  EXPECT_STREQ(gvec_lower_reason_string((GVEC_Lower_Reason) 1000), "unknown");
  std::set<std::string> names;
  for (int i = 1; i < GVEC_LOWER_REASON_COUNT; i++) {
    const char * name = gvec_lower_reason_string((GVEC_Lower_Reason) i);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown") << i;
    names.insert(name);
  }
  EXPECT_EQ(names.size(), (size_t) GVEC_LOWER_REASON_COUNT - 1);
  EXPECT_LT(GVEC_LOWER_REASON_COUNT, 32);  // a bit of a word per element and reason marks it reported
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
