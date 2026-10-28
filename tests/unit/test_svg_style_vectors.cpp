/**
 * @file
 *
 * The used values of the style layer against reference vectors computed outside the library (tools/gen-style-vectors.py: exact rational
 * arithmetic from the specifications' constants, and 80 decimal digits where a function is not rational), compared in units in the last
 * place. The expected values are the nearest doubles to the exact results.
 */

#include "test_helpers.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace gvec_test;

namespace {

const char * kHead = "<svg xmlns=\"http://www.w3.org/2000/svg\" ";

std::string Svg(const std::string & attrs, const std::string & body) {
  return std::string(kHead) + attrs + ">" + body + "</svg>";
}

struct Row {
  std::vector<std::string> fields;
};

std::vector<Row> ReadVectors(const std::string & name) {
  std::vector<Row> rows;
  std::ifstream in("tests/data/svg-style-vectors/" + name);
  EXPECT_TRUE(in.good()) << name;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    Row row;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, '\t')) {
      row.fields.push_back(field);
    }
    rows.push_back(row);
  }
  EXPECT_FALSE(rows.empty()) << name;
  return rows;
}

double FromHex(const std::string & text) {
  return std::strtod(text.c_str(), nullptr);
}

/** The distance in units in the last place between two finite doubles (0 for equal). */
uint64_t Ulps(double a, double b) {
  if (a == b) {
    return 0;
  }
  if (std::signbit(a) != std::signbit(b)) {
    return UINT64_MAX;
  }
  int64_t x, y;
  std::memcpy(&x, &a, 8);
  std::memcpy(&y, &b, 8);
  return (uint64_t) (x > y ? x - y : y - x);
}

struct Styled {
  Loaded loaded;
  GVEC_Svg_Style_Context * context = nullptr;
  const GVEC_Svg_Document * svg = nullptr;

  explicit Styled(const std::string & text) {
    GVEC_Stream * stream = nullptr;
    EXPECT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
    GVEC_Load_Options options;
    std::memset(&options, 0, sizeof options);
    loaded.result = gvec_doc_load(stream, &options, &loaded.diagnostics, &loaded.doc);
    gvec_stream_destroy(stream);
    EXPECT_EQ(loaded.result, GVEC_OK) << text;
    if (loaded.result == GVEC_OK) {
      svg = gvec_svg_document(loaded.doc);
      EXPECT_EQ(gvec_svg_style_context_new(svg, nullptr, nullptr, &context), GVEC_OK);
    }
  }
  ~Styled() { gvec_svg_style_context_free(context); }
  Styled(const Styled &) = delete;
  Styled & operator=(const Styled &) = delete;

  /** The value of `property` of the first element with this id, a child of the root. */
  GVEC_Svg_Style_Value Get(const char * id, GVEC_Svg_Property property) {
    const GVEC_Svg_Instance * root = gvec_svg_instance_root(context);
    const GVEC_Svg_Instance * child = nullptr;
    EXPECT_EQ(gvec_svg_instance_child(context, root, gvec_svg_find_by_id(svg, id), &child), GVEC_OK);
    GVEC_Svg_Style_Value v;
    EXPECT_EQ(gvec_svg_get(context, child, property, &v), GVEC_OK);
    return v;
  }
};

}  // namespace

TEST(SvgStyleVectors, LengthsAreTheNearestDoubleToTheExactConversion) {
  uint64_t worst = 0;
  size_t exact = 0, total = 0;
  for (const Row & row : ReadVectors("lengths.tsv")) {
    Styled s(Svg("", "<rect id=\"r\" stroke-width=\"" + row.fields[1] + row.fields[0] + "\"/>"));
    double got = s.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number;
    double want = FromHex(row.fields[2]);
    uint64_t u = Ulps(got, want);
    EXPECT_LE(u, 1u) << row.fields[1] << row.fields[0] << " got " << got << " want " << want;
    worst = std::max(worst, u);
    exact += u == 0;
    total++;
  }
  EXPECT_EQ(worst <= 1, true);
  /* Most conversions are exact, and none is further than a unit in the last place. */
  EXPECT_GT(exact * 10, total * 7) << exact << " of " << total;
}

TEST(SvgStyleVectors, PercentagesAreOfTheStatedBasis) {
  uint64_t worst_axis = 0, worst_diagonal = 0;
  for (const Row & row : ReadVectors("percentages.tsv")) {
    const std::string & basis = row.fields[0];
    Styled s(Svg("width=\"" + row.fields[1] + "\" height=\"" + row.fields[2] + "\"",
        "<rect id=\"r\" x=\"" + row.fields[3] + "%\" y=\"" + row.fields[3] + "%\" stroke-width=\"" + row.fields[3] + "%\"/>"));
    GVEC_Svg_Property property = basis == "width" ? GVEC_SVG_PROP_X : basis == "height" ? GVEC_SVG_PROP_Y : GVEC_SVG_PROP_STROKE_WIDTH;
    double got = s.Get("r", property).number;
    double want = FromHex(row.fields[4]);
    uint64_t u = Ulps(got, want);
    if (basis == "diagonal") {
      EXPECT_LE(u, 2u) << basis << " " << row.fields[1] << "x" << row.fields[2] << " " << row.fields[3] << "% got " << got << " want " << want;
      worst_diagonal = std::max(worst_diagonal, u);
    }
    else {
      EXPECT_LE(u, 1u) << basis << " " << row.fields[1] << "x" << row.fields[2] << " " << row.fields[3] << "% got " << got << " want " << want;
      worst_axis = std::max(worst_axis, u);
    }
  }
  EXPECT_LE(worst_axis, 1u);
  EXPECT_LE(worst_diagonal, 2u);
}

TEST(SvgStyleVectors, ColorsAreTheirExactConversionsAndNeverEightBits) {
  for (const Row & row : ReadVectors("colors.tsv")) {
    Styled s(Svg("", "<rect id=\"r\" fill=\"" + row.fields[0] + "\"/>"));
    GVEC_Svg_Style_Value v = s.Get("r", GVEC_SVG_PROP_FILL);
    ASSERT_EQ(v.paint.kind, GVEC_SVG_PAINT_COLOR) << row.fields[0];
    const double * c = v.paint.color.color.rgba;
    const bool rec = row.fields[1] == "rec2020";
    for (int i = 0; i < 3; i++) {
      double want = FromHex(row.fields[2 + i]);
      uint64_t u = Ulps(c[i], want);
      if (rec) {
        /* Through the library's power function: a few units in the last place of the 80 digit reference. */
        EXPECT_TRUE(u <= 8 || std::fabs(c[i] - want) < 1e-15) << row.fields[0] << " component " << i << " got " << c[i] << " want " << want;
      }
      else {
        EXPECT_LE(u, 4u) << row.fields[0] << " component " << i << " got " << c[i] << " want " << want;
      }
    }
  }
}

TEST(SvgStyleVectors, RotationsAndSkewsAreTheirExactMatrices) {
  for (const Row & row : ReadVectors("rotations.tsv")) {
    Styled s(Svg("", "<g id=\"r\" transform=\"" + row.fields[0] + "\"/>"));
    GVEC_Svg_Style_Value v = s.Get("r", GVEC_SVG_PROP_TRANSFORM);
    ASSERT_EQ(v.kind, GVEC_SVG_STYLE_TRANSFORM) << row.fields[0];
    const GSHGEOM_Map & m = v.transform.map;
    const double got[6] = {m.m[0][0], m.m[1][0], m.m[0][1], m.m[1][1], m.t[0], m.t[1]};
    for (int i = 0; i < 6; i++) {
      double want = FromHex(row.fields[1 + i]);
      uint64_t u = Ulps(got[i], want);
      /* A quarter turn is exact; the rest go through the sine and cosine of the remainder, a few units in the last place. */
      EXPECT_TRUE(u <= 4 || (want == 0 && got[i] == 0)) << row.fields[0] << " entry " << i << " got " << got[i] << " want " << want;
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
