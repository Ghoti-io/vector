/**
 * @file
 *
 * The root map and the viewports (P3, P4): the map from a viewBox to its viewport for all nineteen forms of
 * preserveAspectRatio against an independent exact reference, the size of the viewport, the nested `svg` and its clip.
 */

#include "../lower_helpers.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "../../src/codec/svg/lower/lower_internal.h"

using namespace gvec_test;

namespace {

struct Vector {
  std::string vx, vy, vw, vh, w, h, align;
  int slice;
  double a, d, tx, ty;
};

std::vector<Vector> ReadVectors() {
  std::vector<Vector> rows;
  std::ifstream in("tests/data/svg-lower-vectors/viewbox.tsv");
  EXPECT_TRUE(in.good());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) {
      f.push_back(cell);
    }
    EXPECT_EQ(f.size(), (size_t) 12);
    if (f.size() != 12) {
      continue;
    }
    Vector v{f[0], f[1], f[2], f[3], f[4], f[5], f[6], std::atoi(f[7].c_str()), std::strtod(f[8].c_str(), nullptr),
        std::strtod(f[9].c_str(), nullptr), std::strtod(f[10].c_str(), nullptr), std::strtod(f[11].c_str(), nullptr)};
    rows.push_back(v);
  }
  return rows;
}

GVEC_Svg_Align AlignOf(const std::string & name) {
  static const char * const names[] = {"none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"};
  for (int i = 0; i < 10; i++) {
    if (name == names[i]) {
      return (GVEC_Svg_Align) i;
    }
  }
  ADD_FAILURE() << name;
  return GVEC_SVG_ALIGN_NONE;
}

uint64_t Bits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, sizeof b);
  return b;
}

std::string Where(const Vector & v) {
  return v.vx + " " + v.vy + " " + v.vw + " " + v.vh + " in " + v.w + "x" + v.h + " " + v.align + (v.slice ? " slice" : " meet");
}

}  // namespace

TEST(LowerViewport, TheMapIsTheExactValueRoundedOnceForEveryVector) {
  std::vector<Vector> rows = ReadVectors();
  ASSERT_GT(rows.size(), (size_t) 200);
  for (const Vector & v : rows) {
    GVEC_Svg_View_Box box{std::strtod(v.vx.c_str(), nullptr), std::strtod(v.vy.c_str(), nullptr), std::strtod(v.vw.c_str(), nullptr),
        std::strtod(v.vh.c_str(), nullptr)};
    GVEC_Svg_Preserve_Aspect_Ratio par{AlignOf(v.align), (uint8_t) v.slice, 0};
    GSHGEOM_Map map;
    ASSERT_TRUE(gvec_lw_viewbox_map(&box, &par, std::strtod(v.w.c_str(), nullptr), std::strtod(v.h.c_str(), nullptr), &map)) << Where(v);
    EXPECT_EQ(Bits(map.m[0][0]), Bits(v.a)) << Where(v);
    EXPECT_EQ(Bits(map.m[1][1]), Bits(v.d)) << Where(v);
    EXPECT_EQ(Bits(map.t[0]), Bits(v.tx)) << Where(v);
    EXPECT_EQ(Bits(map.t[1]), Bits(v.ty)) << Where(v);
    EXPECT_EQ(map.m[0][1], 0.0);
    EXPECT_EQ(map.m[1][0], 0.0);
  }
}

TEST(LowerViewport, EveryVectorThroughTheWholeLoweringIsTheSameMap) {
  // The numbers are written into a document, read by the grammar, and the fill's map is the vector: the load, the
  // style layer, the viewport maths and the composition agree with the reference.
  for (const Vector & v : ReadVectors()) {
    std::string par = v.align + (v.slice ? " slice" : "");
    Lowered l;
    LowerSvg(Svg("width=\"" + v.w + "\" height=\"" + v.h + "\" viewBox=\"" + v.vx + " " + v.vy + " " + v.vw + " " + v.vh +
                     "\" preserveAspectRatio=\"" + par + "\"",
                 "<path d=\"M0 0H1V1Z\"/>"),
        l);
    ASSERT_EQ(l.Items(), (size_t) 1) << Where(v);
    const GSHGEOM_Map & m = l.Item(0).map;
    EXPECT_EQ(Bits(m.m[0][0]), Bits(v.a)) << Where(v);
    EXPECT_EQ(Bits(m.m[1][1]), Bits(v.d)) << Where(v);
    EXPECT_EQ(Bits(m.t[0]), Bits(v.tx)) << Where(v);
    EXPECT_EQ(Bits(m.t[1]), Bits(v.ty)) << Where(v);
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_GROUP_PUSH) + l.Count(GSHSCENE_ITEM_CLIP_PUSH), (size_t) 0) << Where(v);
  }
}

TEST(LowerViewport, NineteenCombinationsAreNineAlignmentsTimesMeetAndSliceAndNone) {
  std::set<std::string> forms;
  for (const Vector & v : ReadVectors()) {
    forms.insert(v.align + (v.slice ? " slice" : " meet"));
  }
  EXPECT_EQ(forms.size(), (size_t) 19);
}

TEST(LowerViewport, ADegenerateOrHugeBoxHasNoMap) {
  GSHGEOM_Map map;
  GVEC_Svg_Preserve_Aspect_Ratio par{GVEC_SVG_ALIGN_X_MID_Y_MID, 0, 0};
  GVEC_Svg_View_Box zero{0, 0, 0, 10};
  EXPECT_FALSE(gvec_lw_viewbox_map(&zero, &par, 10, 10, &map));
  GVEC_Svg_View_Box ok{0, 0, 10, 10};
  EXPECT_FALSE(gvec_lw_viewbox_map(&ok, &par, 0, 10, &map));
  EXPECT_FALSE(gvec_lw_viewbox_map(&ok, &par, -1, 10, &map));
  EXPECT_FALSE(gvec_lw_viewbox_map(&ok, &par, 1e300, 10, &map));
  GVEC_Svg_View_Box tiny{0, 0, 1e-300, 1};
  EXPECT_FALSE(gvec_lw_viewbox_map(&tiny, &par, 10, 10, &map));
  GVEC_Svg_View_Box far{1e250, 0, 1, 1};
  EXPECT_FALSE(gvec_lw_viewbox_map(&far, &par, 10, 10, &map));
}

TEST(LowerViewport, WithoutAViewBoxTheMapIsTheIdentityAndNothingIsGrouped) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"30\"", "<path d=\"M1 1H5V5Z\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_TRUE(IsIdentity(l.Item(0).map));
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerViewport, TheDocumentSizeIsTheOutermostSvgsUsedSize) {
  struct Case {
    const char * attributes;
    double viewport;
    double width;
    double height;
  } const cases[] = {
      {"width=\"200\" height=\"100\" viewBox=\"0 0 200 200\"", 0, 200, 100},
      {"viewBox=\"0 0 10 10\"", 0, 300, 150},                  // the default viewport
      {"viewBox=\"0 0 10 10\"", 500, 500, 500},                // the load's viewport
      {"width=\"50%\" height=\"auto\"", 400, 200, 400},       // a percentage is of the viewport
      {"width=\"2in\"", 0, 192, 150},
  };
  for (const Case & c : cases) {
    Loaded loaded;
    LoadSvg(Svg(c.attributes, ""), loaded, c.viewport, c.viewport);
    ASSERT_EQ(loaded.result, GVEC_OK) << c.attributes;
    double w = -1, h = -1;
    ASSERT_EQ(gvec_doc_size(loaded.doc, &w, &h), GVEC_OK) << c.attributes;
    EXPECT_EQ(w, c.width) << c.attributes;
    EXPECT_EQ(h, c.height) << c.attributes;
  }
  double w, h;
  EXPECT_EQ(gvec_doc_size(nullptr, &w, &h), GVEC_ERR_INVALID);
}

TEST(LowerViewport, ASizeNeedsNoBoxAndAMissingSizeIsTheOptionsViewport) {
  {
    Lowered l;
    LowerSvg(Svg("viewBox=\"0 0 10 10\"", "<path d=\"M0 0H1V1Z\"/>"), l, 300, 150);
    ASSERT_EQ(l.Items(), (size_t) 1);
    EXPECT_EQ(l.Item(0).map.m[0][0], 15.0);
    EXPECT_EQ(l.Item(0).map.t[0], 75.0);
    EXPECT_EQ(l.Item(0).map.t[1], 0.0);
  }
  {
    Lowered l;  // the default viewport is 300 by 150
    LowerSvg(Svg("viewBox=\"0 0 10 10\"", "<path d=\"M0 0H1V1Z\"/>"), l);
    ASSERT_EQ(l.Items(), (size_t) 1);
    EXPECT_EQ(l.Item(0).map.m[0][0], 15.0);
  }
  {
    Lowered l;  // percentages are of the viewport
    LowerSvg(Svg("width=\"50%\" height=\"50%\" viewBox=\"0 0 10 10\"", "<path d=\"M0 0H1V1Z\"/>"), l, 200, 100);
    ASSERT_EQ(l.Items(), (size_t) 1);
    EXPECT_EQ(l.Item(0).map.m[0][0], 5.0);
    EXPECT_EQ(l.Item(0).map.t[0], 25.0);
  }
}

TEST(LowerViewport, AZeroSizedBoxOrViewportDisablesRenderingWithANoticeAndNoRefusal) {
  for (const char * attributes : {"width=\"10\" height=\"10\" viewBox=\"0 0 0 10\"", "width=\"10\" height=\"10\" viewBox=\"0 0 10 0\"",
           "width=\"0\" height=\"10\"", "width=\"10\" height=\"0\" viewBox=\"0 0 10 10\""}) {
    Lowered l;
    LowerSvg(Svg(attributes, "<path d=\"M0 0H1V1Z\"/>"), l);
    EXPECT_EQ(l.Items(), (size_t) 0) << attributes;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << attributes;
    ASSERT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_RENDERING_DISABLED), (size_t) 1) << attributes;
    EXPECT_EQ(gvec_diagnostics_count_code(&l.by_edit, GVEC_DIAG_RENDERING_DISABLED), (size_t) 1) << attributes;
  }
}

TEST(LowerViewport, ANegativeBoxIsAnInvalidAttributeAndIsIgnored) {
  Lowered l;
  LowerSvg(Svg("width=\"10\" height=\"10\" viewBox=\"0 0 -5 10\"", "<path d=\"M0 0H1V1Z\"/>"), l);
  ASSERT_EQ(l.Items(), (size_t) 1);
  EXPECT_TRUE(IsIdentity(l.Item(0).map));
  EXPECT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_RENDERING_DISABLED), (size_t) 0);
}

TEST(LowerViewport, ANestedSvgClipsToItsViewportByDefault) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<svg x=\"10\" y=\"20\" width=\"30\" height=\"40\"><path d=\"M0 0H5V5Z\"/></svg>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(l.Item(2).kind, GSHSCENE_ITEM_CLIP_POP);
  EXPECT_EQ(Describe(l.Item(0).resource), "M10,20 L40,20 L40,60 L10,60 Z");
  EXPECT_TRUE(IsIdentity(l.Item(0).map));
  EXPECT_EQ(l.Item(1).map.t[0], 10.0);
  EXPECT_EQ(l.Item(1).map.t[1], 20.0);
  EXPECT_EQ(l.Item(1).map.m[0][0], 1.0);
}

TEST(LowerViewport, OverflowDecidesTheClipForEachValue) {
  struct Case {
    const char * overflow;
    bool clipped;
  } cases[] = {{"visible", false}, {"auto", false}, {"hidden", true}, {"scroll", true}, {"clip", true}};
  for (const Case & c : cases) {
    Lowered l;
    LowerSvg(Svg("width=\"100\" height=\"100\"", std::string("<svg x=\"10\" y=\"20\" width=\"30\" height=\"40\" overflow=\"") + c.overflow +
                                                     "\"><path d=\"M0 0H5V5Z\"/></svg>"),
        l);
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_CLIP_PUSH), c.clipped ? (size_t) 1 : (size_t) 0) << c.overflow;
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_CLIP_POP), c.clipped ? (size_t) 1 : (size_t) 0) << c.overflow;
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1) << c.overflow;
  }
}

TEST(LowerViewport, ANestedViewBoxScalesIntoItsViewport) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<svg x=\"10\" y=\"20\" width=\"30\" height=\"40\" viewBox=\"0 0 3 4\"><path d=\"M0 0H1V1Z\"/></svg>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  const GSHGEOM_Map & m = l.Item(1).map;
  EXPECT_EQ(m.m[0][0], 10.0);
  EXPECT_EQ(m.m[1][1], 10.0);
  EXPECT_EQ(m.t[0], 10.0);
  EXPECT_EQ(m.t[1], 20.0);
  // The clip stays the viewport rectangle in the parent's space, not the box.
  EXPECT_EQ(Describe(l.Item(0).resource), "M10,20 L40,20 L40,60 L10,60 Z");
}

TEST(LowerViewport, ANestedSvgsPercentagesAreOfItsParentsViewport) {
  Lowered l;
  LowerSvg(Svg("width=\"200\" height=\"100\"", "<svg x=\"10%\" y=\"10%\" width=\"50%\" height=\"50%\"><path d=\"M0 0H5V5Z\"/></svg>"), l);
  ASSERT_EQ(l.Items(), (size_t) 3);
  EXPECT_EQ(Describe(l.Item(0).resource), "M20,10 L120,10 L120,60 L20,60 Z");
}

TEST(LowerViewport, ANestedSvgWithATransformAndOpacityGroupsOutsideTheClip) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\"",
               "<svg x=\"10\" y=\"20\" width=\"30\" height=\"40\" opacity=\".5\" transform=\"translate(1 2)\"><path d=\"M0 0H5V5Z\"/></svg>"),
      l);
  ASSERT_EQ(l.Items(), (size_t) 5);
  EXPECT_EQ(l.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(l.Item(0).group.opacity, 0.5);
  EXPECT_EQ(l.Item(1).kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(l.Item(1).map.t[0], 1.0);
  EXPECT_EQ(l.Item(2).map.t[0], 11.0);
  EXPECT_EQ(l.Item(2).map.t[1], 22.0);
}

TEST(LowerViewport, ATransformOnTheRootIsAppliedAndAnEmptyViewportIsNoGroup) {
  Lowered l;
  LowerSvg(Svg("width=\"100\" height=\"100\" transform=\"scale(2)\" opacity=\".5\"", ""), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
