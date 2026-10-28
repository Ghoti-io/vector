/**
 * @file
 *
 * The style of the typed document (style.h): the property view, instances and the inheritance of a `use` clone, the cascade of
 * presentation attributes and `style`, and the used values of lengths, colors, paints, dash arrays, transforms and references.
 */

#include "test_helpers.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace gvec_test;

namespace {

const char * kHead = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" ";

std::string Svg(const std::string & attrs, const std::string & body) {
  return std::string(kHead) + attrs + ">" + body + "</svg>";
}

/** A loaded document with its style context. */
struct Styled {
  Loaded loaded;
  const GVEC_Svg_Document * svg = nullptr;
  GVEC_Svg_Style_Context * context = nullptr;
  GVEC_Diagnostics diagnostics{};

  explicit Styled(const std::string & text, double viewport_width = 0, double viewport_height = 0,
      GVEC_Color_Scheme scheme = GVEC_COLOR_SCHEME_LIGHT, const GVEC_Limits * limits = nullptr) {
    gvec_diagnostics_init(&diagnostics, nullptr);
    GVEC_Stream * stream = nullptr;
    EXPECT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
    GVEC_Load_Options options;
    std::memset(&options, 0, sizeof options);
    options.limits = limits;
    options.viewport_width = viewport_width;
    options.viewport_height = viewport_height;
    options.color_scheme = scheme;
    loaded.result = gvec_doc_load(stream, &options, &loaded.diagnostics, &loaded.doc);
    gvec_stream_destroy(stream);
    EXPECT_EQ(loaded.result, GVEC_OK);
    if (loaded.result == GVEC_OK) {
      svg = gvec_svg_document(loaded.doc);
      EXPECT_EQ(gvec_svg_style_context_new(svg, &diagnostics, nullptr, &context), GVEC_OK);
    }
  }
  Styled(const Styled &) = delete;
  Styled & operator=(const Styled &) = delete;
  ~Styled() {
    gvec_svg_style_context_free(context);
    gvec_diagnostics_destroy(&diagnostics);
  }

  const GVEC_Svg_Element * Id(const char * id) const { return gvec_svg_find_by_id(svg, id); }

  /** The instance of an element drawn in the document's own context. */
  const GVEC_Svg_Instance * Of(const GVEC_Svg_Element * e) {
    std::vector<const GVEC_Svg_Element *> path;
    for (const GVEC_Svg_Element * p = e; p; p = gvec_svg_element_parent(p)) {
      path.push_back(p);
    }
    const GVEC_Svg_Instance * instance = gvec_svg_instance_root(context);
    for (size_t i = path.size() - 1; i-- > 0;) {
      const GVEC_Svg_Instance * child = nullptr;
      EXPECT_EQ(gvec_svg_instance_child(context, instance, path[i], &child), GVEC_OK);
      instance = child;
    }
    return instance;
  }
  const GVEC_Svg_Instance * Of(const char * id) { return Of(Id(id)); }

  GVEC_Svg_Style_Value Get(const GVEC_Svg_Instance * instance, GVEC_Svg_Property property, GVEC_Result expect = GVEC_OK) {
    GVEC_Svg_Style_Value v;
    EXPECT_EQ(gvec_svg_get(context, instance, property, &v), expect);
    return v;
  }
  GVEC_Svg_Style_Value Get(const char * id, GVEC_Svg_Property property, GVEC_Result expect = GVEC_OK) {
    return Get(Of(id), property, expect);
  }

  size_t Count(GVEC_Diag_Code code) const { return gvec_diagnostics_count_code(&diagnostics, code); }
};

}  // namespace

TEST(SvgStyleProperties, EveryNameRoundTrips) {
  for (int p = 0; p < GVEC_SVG_PROP_COUNT; p++) {
    const char * name = gvec_svg_property_name((GVEC_Svg_Property) p);
    ASSERT_NE(name, nullptr) << p;
    GVEC_Svg_Property found = GVEC_SVG_PROP_COUNT;
    ASSERT_TRUE(gvec_svg_property_find(name, std::strlen(name), &found)) << name;
    EXPECT_EQ(found, p) << name;
  }
  GVEC_Svg_Property none;
  EXPECT_FALSE(gvec_svg_property_find("fillx", 5, &none));
  EXPECT_FALSE(gvec_svg_property_find("fil", 3, &none));
  EXPECT_FALSE(gvec_svg_property_find("Fill", 4, &none));
  EXPECT_EQ(gvec_svg_property_name(GVEC_SVG_PROP_COUNT), nullptr);
}

TEST(SvgStyleProperties, TheRegistryHasEveryPropertyAndTheOverridesAreReported) {
  Styled s(Svg("", ""));
  ASSERT_NE(s.context, nullptr);
  /* The context could be made, so every generated row exists in the pin and every registered one does not. */
  size_t overrides = gvec_svg_style_context_override_count(s.context);
  EXPECT_EQ(overrides, 7u);
  std::vector<std::string> names;
  for (size_t i = 0; i < overrides; i++) {
    GVEC_Svg_Property p;
    const char * note = nullptr;
    ASSERT_TRUE(gvec_svg_style_context_override_at(s.context, i, &p, &note));
    names.push_back(gvec_svg_property_name(p));
    EXPECT_NE(note, nullptr);
    EXPECT_EQ(gvec_svg_property_source(p), GVEC_SVG_PROPERTY_OVERRIDDEN);
  }
  EXPECT_FALSE(gvec_svg_style_context_override_at(s.context, overrides, nullptr, nullptr));
  EXPECT_EQ(names, (std::vector<std::string>{"fill", "glyph-orientation-vertical", "stop-color", "stop-opacity", "stroke", "stroke-linejoin", "stroke-width"}));
  EXPECT_EQ(gvec_svg_property_source(GVEC_SVG_PROP_COLOR_RENDERING), GVEC_SVG_PROPERTY_REGISTERED);
  EXPECT_EQ(gvec_svg_property_source(GVEC_SVG_PROP_OPACITY), GVEC_SVG_PROPERTY_GENERATED);
}

TEST(SvgStyleCascade, InheritanceAndTheInitialValues) {
  Styled s(Svg("", "<g id=\"g\" fill=\"red\" opacity=\"0.5\" stroke-width=\"3\"><rect id=\"r\"/></g>"));
  GVEC_Svg_Style_Value fill = s.Get("r", GVEC_SVG_PROP_FILL);
  ASSERT_EQ(fill.kind, GVEC_SVG_STYLE_PAINT);
  EXPECT_EQ(fill.paint.kind, GVEC_SVG_PAINT_COLOR);
  EXPECT_EQ(fill.paint.color.color.rgba[0], 1.0);
  EXPECT_EQ(fill.paint.color.color.rgba[1], 0.0);
  EXPECT_EQ(s.Get("g", GVEC_SVG_PROP_OPACITY).number, 0.5);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_OPACITY).number, 1.0);  /* opacity is not inherited */
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 3.0);
  GVEC_Svg_Style_Value stroke = s.Get("r", GVEC_SVG_PROP_STROKE);
  EXPECT_EQ(stroke.paint.kind, GVEC_SVG_PAINT_NONE);
  GVEC_Svg_Style_Value rule = s.Get("r", GVEC_SVG_PROP_FILL_RULE);
  ASSERT_EQ(rule.kind, GVEC_SVG_STYLE_KEYWORD);
  EXPECT_STREQ(rule.keyword, "nonzero");
}

TEST(SvgStyleCascade, StyleBeatsPresentationAndImportantBeatsBoth) {
  Styled s(Svg("", "<rect id=\"a\" fill=\"red\" style=\"fill:blue\"/><rect id=\"b\" fill=\"red\"/>"
                    "<rect id=\"c\" fill=\"red\" style=\"fill:blue !important\"/>"));
  EXPECT_EQ(s.Get("a", GVEC_SVG_PROP_FILL).paint.color.color.rgba[2], 1.0);
  EXPECT_EQ(s.Get("b", GVEC_SVG_PROP_FILL).paint.color.color.rgba[0], 1.0);
  GVEC_Svg_Style_Specified spec;
  ASSERT_EQ(gvec_svg_get_specified(s.context, s.Of("a"), GVEC_SVG_PROP_FILL, &spec), GVEC_OK);
  EXPECT_EQ(spec.source, GVEC_SVG_STYLE_FROM_STYLE_ATTRIBUTE);
  ASSERT_EQ(gvec_svg_get_specified(s.context, s.Of("b"), GVEC_SVG_PROP_FILL, &spec), GVEC_OK);
  EXPECT_EQ(spec.source, GVEC_SVG_STYLE_FROM_PRESENTATION);
  ASSERT_EQ(gvec_svg_get_specified(s.context, s.Of("c"), GVEC_SVG_PROP_FILL, &spec), GVEC_OK);
  EXPECT_TRUE(spec.important);
  ASSERT_EQ(gvec_svg_get_specified(s.context, s.Of("b"), GVEC_SVG_PROP_STROKE, &spec), GVEC_OK);
  EXPECT_EQ(spec.source, GVEC_SVG_STYLE_FROM_INHERITANCE);
}

TEST(SvgStyleCascade, TheAttributeOnlySpellings) {
  Styled s(Svg("width=\"100\" height=\"50\"",
      "<rect id=\"r\" x=\"10\" y=\"5\" width=\"20\" height=\"6\" rx=\"2\" stroke-width=\"2\"/>"));
  GVEC_Svg_Style_Value x = s.Get("r", GVEC_SVG_PROP_X);
  ASSERT_EQ(x.kind, GVEC_SVG_STYLE_LENGTH);
  EXPECT_EQ(x.number, 10.0);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_WIDTH).number, 20.0);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_RX).number, 2.0);
  EXPECT_TRUE(s.Get("r", GVEC_SVG_PROP_RY).length_auto);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 2.0);
}

TEST(SvgStyleCascade, InvalidValuesAreDroppedAndReported) {
  Styled s(Svg("", "<rect id=\"r\" fill=\"rgb(1\" stroke-width=\"abc\" style=\"a:;b:c;fill-opacity:0.25\"/>"));
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 1.0);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_FILL).paint.color.kind, GVEC_SVG_COLOR_FOREGROUND == 0 ? GVEC_SVG_COLOR_SOLID : GVEC_SVG_COLOR_SOLID);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_FILL_OPACITY).number, 0.25);
  EXPECT_GE(s.Count(GVEC_DIAG_STYLE_INVALID), 3u);
  /* Once for the element: reading again reports nothing more. */
  size_t before = s.Count(GVEC_DIAG_STYLE_INVALID);
  s.Get("r", GVEC_SVG_PROP_OPACITY);
  EXPECT_EQ(s.Count(GVEC_DIAG_STYLE_INVALID), before);
}

TEST(SvgStyleUsed, AbsoluteLengths) {
  Styled s(Svg("", "<rect id=\"in\" stroke-width=\"1in\"/><rect id=\"cm\" stroke-width=\"1cm\"/><rect id=\"mm\" stroke-width=\"10mm\"/>"
                   "<rect id=\"q\" stroke-width=\"40Q\"/><rect id=\"pt\" stroke-width=\"72pt\"/><rect id=\"pc\" stroke-width=\"6pc\"/>"
                   "<rect id=\"px\" stroke-width=\"7px\"/><rect id=\"upper\" stroke-width=\"1IN\"/>"));
  EXPECT_EQ(s.Get("in", GVEC_SVG_PROP_STROKE_WIDTH).number, 96.0);
  EXPECT_DOUBLE_EQ(s.Get("cm", GVEC_SVG_PROP_STROKE_WIDTH).number, 96.0 / 2.54);
  EXPECT_DOUBLE_EQ(s.Get("mm", GVEC_SVG_PROP_STROKE_WIDTH).number, 96.0 / 2.54);
  EXPECT_DOUBLE_EQ(s.Get("q", GVEC_SVG_PROP_STROKE_WIDTH).number, 96.0 / 2.54);
  EXPECT_EQ(s.Get("pt", GVEC_SVG_PROP_STROKE_WIDTH).number, 96.0);
  EXPECT_EQ(s.Get("pc", GVEC_SVG_PROP_STROKE_WIDTH).number, 96.0);
  EXPECT_EQ(s.Get("px", GVEC_SVG_PROP_STROKE_WIDTH).number, 7.0);
  EXPECT_EQ(s.Get("upper", GVEC_SVG_PROP_STROKE_WIDTH).number, 96.0);
}

TEST(SvgStyleUsed, FontRelativeLengths) {
  Styled s(Svg("", "<g font-size=\"20\"><rect id=\"em\" stroke-width=\"2em\"/><rect id=\"ex\" stroke-width=\"3ex\"/>"
                   "<rect id=\"ch\" stroke-width=\"1ch\"/><rect id=\"rem\" stroke-width=\"1rem\"/></g>"));
  EXPECT_EQ(s.Get("em", GVEC_SVG_PROP_STROKE_WIDTH).number, 40.0);
  EXPECT_EQ(s.Get("ex", GVEC_SVG_PROP_STROKE_WIDTH).number, 30.0);
  EXPECT_EQ(s.Get("ch", GVEC_SVG_PROP_STROKE_WIDTH).number, 10.0);
  EXPECT_EQ(s.Get("rem", GVEC_SVG_PROP_STROKE_WIDTH).number, 16.0);  /* the root's font-size is `medium` */
  EXPECT_EQ(s.Get("em", GVEC_SVG_PROP_FONT_SIZE).number, 20.0);
}

TEST(SvgStyleUsed, ViewportUnitsFollowTheLoadOptions) {
  Styled s(Svg("", "<rect id=\"w\" stroke-width=\"10vw\"/><rect id=\"h\" stroke-width=\"10vh\"/><rect id=\"mn\" stroke-width=\"10vmin\"/>"
                   "<rect id=\"mx\" stroke-width=\"10vmax\"/>"), 200, 100);
  EXPECT_EQ(s.Get("w", GVEC_SVG_PROP_STROKE_WIDTH).number, 20.0);
  EXPECT_EQ(s.Get("h", GVEC_SVG_PROP_STROKE_WIDTH).number, 10.0);
  EXPECT_EQ(s.Get("mn", GVEC_SVG_PROP_STROKE_WIDTH).number, 10.0);
  EXPECT_EQ(s.Get("mx", GVEC_SVG_PROP_STROKE_WIDTH).number, 20.0);
  Styled d(Svg("", "<rect id=\"w\" stroke-width=\"10vw\"/>"));  /* the default viewport is 300 by 150 */
  EXPECT_EQ(d.Get("w", GVEC_SVG_PROP_STROKE_WIDTH).number, 30.0);
}

TEST(SvgStyleUsed, PercentagesAreOfTheNearestViewport) {
  Styled s(Svg("width=\"200\" height=\"100\"",
      "<rect id=\"x\" x=\"50%\" y=\"50%\" width=\"10%\" height=\"20%\"/><circle id=\"r\" r=\"50%\" stroke-width=\"10%\"/>"));
  EXPECT_EQ(s.Get("x", GVEC_SVG_PROP_X).number, 100.0);
  EXPECT_EQ(s.Get("x", GVEC_SVG_PROP_Y).number, 50.0);
  EXPECT_EQ(s.Get("x", GVEC_SVG_PROP_WIDTH).number, 20.0);
  EXPECT_EQ(s.Get("x", GVEC_SVG_PROP_HEIGHT).number, 20.0);
  double diagonal = std::sqrt((200.0 * 200.0 + 100.0 * 100.0) / 2.0);
  EXPECT_DOUBLE_EQ(s.Get("r", GVEC_SVG_PROP_R).number, diagonal * 0.5);
  EXPECT_DOUBLE_EQ(s.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, diagonal * 0.1);
}

TEST(SvgStyleUsed, AViewBoxIsTheBasisWhenThereIsOne) {
  Styled s(Svg("width=\"200\" height=\"100\" viewBox=\"0 0 20 10\"",
      "<circle id=\"c\" r=\"50%\" cx=\"50%\"/>"));
  double diagonal = std::sqrt((20.0 * 20.0 + 10.0 * 10.0) / 2.0);
  EXPECT_DOUBLE_EQ(s.Get("c", GVEC_SVG_PROP_R).number, diagonal * 0.5);
  EXPECT_EQ(s.Get("c", GVEC_SVG_PROP_CX).number, 10.0);
  double w, h;
  gvec_svg_instance_viewport(s.Of("c"), &w, &h);
  EXPECT_EQ(w, 20.0);
  EXPECT_EQ(h, 10.0);
}

TEST(SvgStyleUsed, ANestedSvgEstablishesItsOwnViewport) {
  Styled s(Svg("width=\"200\" height=\"100\"",
      "<svg id=\"n\" width=\"50%\" height=\"50%\"><rect id=\"r\" width=\"50%\" height=\"50%\"/></svg>"));
  EXPECT_EQ(s.Get("n", GVEC_SVG_PROP_WIDTH).number, 100.0);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_WIDTH).number, 50.0);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_HEIGHT).number, 25.0);
}

TEST(SvgStyleUse, ACloneInheritsFromTheUseNotFromWhereItIsWritten) {
  Styled s(Svg("", "<defs><g fill=\"blue\"><rect id=\"a\" width=\"1\" height=\"1\"/></g></defs>"
                   "<g fill=\"red\"><use id=\"u\" href=\"#a\"/></g>"));
  const GVEC_Svg_Instance * use = s.Of("u");
  const GVEC_Svg_Instance * clone = nullptr;
  ASSERT_EQ(gvec_svg_instance_use(s.context, use, &clone), GVEC_OK);
  EXPECT_EQ(gvec_svg_instance_parent(clone), use);
  EXPECT_EQ(gvec_svg_instance_use_context(clone), use);
  EXPECT_EQ(gvec_svg_instance_element(clone), s.Id("a"));
  GVEC_Svg_Style_Value fill = s.Get(clone, GVEC_SVG_PROP_FILL);
  EXPECT_EQ(fill.paint.color.color.rgba[0], 1.0);
  EXPECT_EQ(fill.paint.color.color.rgba[2], 0.0);
  /* The same element in the document's own context inherits from where it is written. */
  EXPECT_EQ(s.Get("a", GVEC_SVG_PROP_FILL).paint.color.color.rgba[2], 1.0);
}

TEST(SvgStyleUse, AChainOfUsesAndASymbolsViewport) {
  Styled s(Svg("width=\"200\" height=\"100\"",
      "<defs><symbol id=\"s\" viewBox=\"0 0 10 10\"><rect id=\"in\" width=\"50%\"/></symbol>"
      "<g id=\"g\" fill=\"green\"><use id=\"u2\" href=\"#s\" width=\"40\" height=\"40\"/></g></defs>"
      "<use id=\"u1\" href=\"#g\" fill=\"red\"/>"));
  const GVEC_Svg_Instance *u1 = s.Of("u1"), *g = nullptr;
  ASSERT_EQ(gvec_svg_instance_use(s.context, u1, &g), GVEC_OK);
  const GVEC_Svg_Instance* u2 = nullptr;
  ASSERT_EQ(gvec_svg_instance_child(s.context, g, s.Id("u2"), &u2), GVEC_OK);
  const GVEC_Svg_Instance* symbol = nullptr;
  ASSERT_EQ(gvec_svg_instance_use(s.context, u2, &symbol), GVEC_OK);
  const GVEC_Svg_Instance* rect = nullptr;
  ASSERT_EQ(gvec_svg_instance_child(s.context, symbol, s.Id("in"), &rect), GVEC_OK);
  /* g's own fill beats the use's inheritance; u2 inherits green from g; the symbol from u2; the rect from the symbol. */
  EXPECT_EQ(s.Get(rect, GVEC_SVG_PROP_FILL).paint.color.color.rgba[1], 128.0 / 255.0);
  /* The symbol's viewBox is the basis inside it, and 50% of 10 is 5. */
  EXPECT_EQ(s.Get(rect, GVEC_SVG_PROP_WIDTH).number, 5.0);
  double w, h;
  gvec_svg_instance_viewport(rect, &w, &h);
  EXPECT_EQ(w, 10.0);
  EXPECT_EQ(h, 10.0);
  EXPECT_EQ(gvec_svg_instance_use_context(rect), u2);
  /* A use whose target is missing, and an instance that is not a use. */
  const GVEC_Svg_Instance* none = nullptr;
  EXPECT_EQ(gvec_svg_instance_use(s.context, s.Of("g"), &none), GVEC_ERR_INVALID);
}

TEST(SvgStyleUsed, CurrentColorIsTheElementsColor) {
  Styled s(Svg("", "<g id=\"a\" fill=\"currentColor\"><rect id=\"b\" color=\"red\"/></g>"
                   "<g color=\"blue\"><rect id=\"c\" fill=\"currentColor\" stroke=\"currentcolor\"/></g>"
                   "<rect id=\"d\" fill=\"currentColor\" color=\"rgb(0 128 0 / 50%)\"/>"));
  /* `color` is never specified: the waist's foreground. */
  GVEC_Svg_Style_Value a = s.Get("a", GVEC_SVG_PROP_FILL);
  EXPECT_EQ(a.paint.kind, GVEC_SVG_PAINT_COLOR);
  EXPECT_EQ(a.paint.color.kind, GVEC_SVG_COLOR_FOREGROUND);
  EXPECT_EQ(a.paint.color.color.rgba[3], 1.0);
  /* fill inherited as the keyword and resolved against the element that uses it. */
  EXPECT_EQ(s.Get("b", GVEC_SVG_PROP_FILL).paint.color.kind, GVEC_SVG_COLOR_SOLID);
  EXPECT_EQ(s.Get("b", GVEC_SVG_PROP_FILL).paint.color.color.rgba[0], 1.0);
  GVEC_Svg_Style_Value c = s.Get("c", GVEC_SVG_PROP_FILL);
  EXPECT_EQ(c.paint.color.color.rgba[2], 1.0);
  EXPECT_EQ(s.Get("c", GVEC_SVG_PROP_STROKE).paint.color.color.rgba[2], 1.0);
  GVEC_Svg_Style_Value d = s.Get("d", GVEC_SVG_PROP_FILL);
  EXPECT_EQ(d.paint.color.kind, GVEC_SVG_COLOR_SOLID);
  EXPECT_EQ(d.paint.color.color.rgba[3], 0.5);
  /* A specified color is concrete even when it is `black`. */
  GVEC_Svg_Style_Value color = s.Get("b", GVEC_SVG_PROP_COLOR);
  EXPECT_EQ(color.color.kind, GVEC_SVG_COLOR_SOLID);
  EXPECT_EQ(s.Get("a", GVEC_SVG_PROP_COLOR).color.kind, GVEC_SVG_COLOR_FOREGROUND);
}

namespace {

/** The color of `fill` of an element whose fill is `value`. */
GVEC_Svg_Color FillOf(Styled & s, const char * id) {
  GVEC_Svg_Style_Value v = s.Get(id, GVEC_SVG_PROP_FILL);
  EXPECT_EQ(v.paint.kind, GVEC_SVG_PAINT_COLOR) << id;
  return v.paint.color;
}

}  // namespace

TEST(SvgStyleColor, TheSpellingsOfSrgb) {
  Styled s(Svg("", "<rect id=\"named\" fill=\"RebeccaPurple\"/><rect id=\"h3\" fill=\"#f80\"/><rect id=\"h4\" fill=\"#f808\"/>"
                   "<rect id=\"h6\" fill=\"#ff8800\"/><rect id=\"h8\" fill=\"#ff880080\"/>"
                   "<rect id=\"legacy\" fill=\"rgba(255, 136, 0, .5)\"/><rect id=\"modern\" fill=\"rgb(100% 53.3333% 0% / 50%)\"/>"
                   "<rect id=\"clamp\" fill=\"rgb(300 -5 0)\"/><rect id=\"none\" fill=\"rgb(none 128 0)\"/>"
                   "<rect id=\"t\" fill=\"transparent\"/>"));
  GVEC_Svg_Color c = FillOf(s, "named");
  EXPECT_EQ(c.color.rgba[0], 102.0 / 255.0);
  EXPECT_EQ(c.color.rgba[1], 51.0 / 255.0);
  EXPECT_EQ(c.color.rgba[2], 153.0 / 255.0);
  EXPECT_EQ(c.color.rgba[3], 1.0);
  EXPECT_EQ(c.color.space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(c.color.space.transfer, GCOL_TRANSFER_SRGB);
  EXPECT_EQ(FillOf(s, "h3").color.rgba[0], 1.0);
  EXPECT_EQ(FillOf(s, "h3").color.rgba[1], 136.0 / 255.0);
  EXPECT_EQ(FillOf(s, "h6").color.rgba[1], 136.0 / 255.0);
  EXPECT_EQ(FillOf(s, "h4").color.rgba[3], 136.0 / 255.0);
  EXPECT_EQ(FillOf(s, "h8").color.rgba[3], 128.0 / 255.0);
  EXPECT_EQ(FillOf(s, "legacy").color.rgba[3], 0.5);
  EXPECT_EQ(FillOf(s, "legacy").color.rgba[1], 136.0 / 255.0);
  EXPECT_EQ(FillOf(s, "modern").color.rgba[3], 0.5);
  EXPECT_EQ(FillOf(s, "clamp").color.rgba[0], 1.0);
  EXPECT_EQ(FillOf(s, "clamp").color.rgba[1], 0.0);
  EXPECT_EQ(FillOf(s, "none").color.rgba[0], 0.0);
  EXPECT_EQ(FillOf(s, "t").color.rgba[3], 0.0);
}

TEST(SvgStyleColor, HslAndHwbAreNotRoundedTo8Bits) {
  Styled s(Svg("", "<rect id=\"hsl\" fill=\"hsl(120 100% 25%)\"/><rect id=\"hwb\" fill=\"hwb(0 0% 0%)\"/>"
                   "<rect id=\"odd\" fill=\"hsl(210deg 50% 33.3%)\"/><rect id=\"gray\" fill=\"hwb(200 60% 60%)\"/>"
                   "<rect id=\"neg\" fill=\"hsl(-120 100% 50%)\"/>"));
  GVEC_Svg_Color c = FillOf(s, "hsl");
  EXPECT_EQ(c.color.rgba[0], 0.0);
  EXPECT_EQ(c.color.rgba[1], 0.5);
  EXPECT_EQ(c.color.rgba[2], 0.0);
  EXPECT_EQ(FillOf(s, "hwb").color.rgba[0], 1.0);
  GVEC_Svg_Color odd = FillOf(s, "odd");
  /* 33.3% lightness, 50% saturation: not a multiple of 1/255. */
  EXPECT_NE(odd.color.rgba[0] * 255.0, std::round(odd.color.rgba[0] * 255.0));
  EXPECT_DOUBLE_EQ(FillOf(s, "gray").color.rgba[0], 0.5);
  EXPECT_EQ(FillOf(s, "neg").color.rgba[2], 1.0);
}

TEST(SvgStyleColor, WideGamutsAreCarriedAsTheyAre) {
  Styled s(Svg("", "<rect id=\"p3\" fill=\"color(display-p3 1 0.5 0)\"/><rect id=\"p3l\" fill=\"color(display-p3-linear 0.2 0.3 0.4)\"/>"
                   "<rect id=\"srgb\" fill=\"color(srgb 1.5 -0.25 0.5 / 0.5)\"/><rect id=\"lin\" fill=\"color(srgb-linear 0.5 0.5 0.5)\"/>"
                   "<rect id=\"a98\" fill=\"color(a98-rgb 0.1 0.2 0.3)\"/><rect id=\"rec\" fill=\"color(rec2020 0.5 0.5 0.5)\"/>"
                   "<rect id=\"pro\" fill=\"color(prophoto-rgb 0.5 0.5 0.5)\"/>"));
  GVEC_Svg_Color c = FillOf(s, "p3");
  EXPECT_EQ(c.color.space.primaries, GCOL_PRIMARIES_DISPLAY_P3);
  EXPECT_EQ(c.color.space.transfer, GCOL_TRANSFER_SRGB);
  EXPECT_EQ(c.color.rgba[1], 0.5);
  EXPECT_EQ(FillOf(s, "p3l").color.space.transfer, GCOL_TRANSFER_LINEAR);
  GVEC_Svg_Color srgb = FillOf(s, "srgb");
  EXPECT_EQ(srgb.color.rgba[0], 1.5);   /* out of gamut, not clamped */
  EXPECT_EQ(srgb.color.rgba[1], -0.25);
  EXPECT_EQ(srgb.color.rgba[3], 0.5);
  EXPECT_EQ(FillOf(s, "lin").color.space.transfer, GCOL_TRANSFER_LINEAR);
  GVEC_Svg_Color a98 = FillOf(s, "a98");
  EXPECT_EQ(a98.color.space.primaries, GCOL_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(a98.color.space.transfer, GCOL_TRANSFER_GAMMA);
  EXPECT_EQ(a98.color.space.gamma, 563.0 / 256.0);
  GVEC_Svg_Color rec = FillOf(s, "rec");
  EXPECT_EQ(rec.color.space.primaries, GCOL_PRIMARIES_BT2020);
  EXPECT_EQ(rec.color.space.transfer, GCOL_TRANSFER_LINEAR);
  EXPECT_NEAR(rec.color.rgba[0], std::pow((0.5 + 0.09929682680944) / 1.09929682680944, 1 / 0.45), 1e-12);
  GVEC_Svg_Color pro = FillOf(s, "pro");
  EXPECT_EQ(pro.color.space.primaries, GCOL_PRIMARIES_BT2020);
  EXPECT_NEAR(pro.color.rgba[0], pro.color.rgba[1], 1e-6);  /* a gray stays gray */
  EXPECT_NEAR(pro.color.rgba[1], pro.color.rgba[2], 1e-6);
  EXPECT_GT(pro.color.rgba[0], 0.0);
  EXPECT_LT(pro.color.rgba[0], 0.5);
}

TEST(SvgStyleColor, SpacesTheWaistCannotCarryAreRefusedAndReported) {
  Styled s(Svg("", "<rect id=\"lab\" fill=\"lab(50% 40 59)\"/><rect id=\"oklch\" fill=\"oklch(60% 0.15 50)\"/>"
                   "<rect id=\"mix\" fill=\"color-mix(in srgb, red, blue)\"/><rect id=\"xyz\" fill=\"color(xyz 0.2 0.3 0.4)\"/>"
                   "<rect id=\"ok\" fill=\"red\"/>"));
  EXPECT_EQ(s.Get("lab", GVEC_SVG_PROP_FILL, GVEC_ERR_UNSUPPORTED).kind, GVEC_SVG_STYLE_PAINT);
  s.Get("oklch", GVEC_SVG_PROP_FILL, GVEC_ERR_UNSUPPORTED);
  s.Get("mix", GVEC_SVG_PROP_FILL, GVEC_ERR_UNSUPPORTED);
  s.Get("xyz", GVEC_SVG_PROP_FILL, GVEC_ERR_UNSUPPORTED);
  EXPECT_EQ(s.Count(GVEC_DIAG_UNSUPPORTED_COLOR_SPACE), 4u);
  const GVEC_Diagnostic * d = nullptr;
  for (size_t i = 0; i < s.diagnostics.count; i++) {
    if (s.diagnostics.items[i].code == GVEC_DIAG_UNSUPPORTED_COLOR_SPACE) {
      d = &s.diagnostics.items[i];
      break;
    }
  }
  ASSERT_NE(d, nullptr);
  EXPECT_STREQ(d->element, "rect");
  EXPECT_GT(d->line, 0u);
  FillOf(s, "ok");
}

TEST(SvgStyleColor, SystemColors) {
  Styled light(Svg("", "<rect id=\"t\" fill=\"CanvasText\"/><rect id=\"c\" fill=\"Canvas\"/><rect id=\"x\" fill=\"ButtonFace\"/>"));
  EXPECT_EQ(FillOf(light, "t").kind, GVEC_SVG_COLOR_FOREGROUND);
  GVEC_Svg_Color canvas = FillOf(light, "c");
  EXPECT_EQ(canvas.color.rgba[0], 1.0);
  light.Get("x", GVEC_SVG_PROP_FILL, GVEC_ERR_UNSUPPORTED);
  /* The preference only matters to a document that says it supports the dark scheme (CSS Color Adjust 1). */
  Styled normal(Svg("", "<rect id=\"c\" fill=\"Canvas\"/><rect id=\"l\" fill=\"light-dark(red, blue)\"/>"), 0, 0, GVEC_COLOR_SCHEME_DARK);
  EXPECT_EQ(FillOf(normal, "c").color.rgba[0], 1.0);
  EXPECT_EQ(FillOf(normal, "l").color.rgba[0], 1.0);
  Styled dark(Svg("style=\"color-scheme: light dark\"", "<rect id=\"c\" fill=\"Canvas\"/><rect id=\"l\" fill=\"light-dark(red, blue)\"/>"), 0, 0,
      GVEC_COLOR_SCHEME_DARK);
  EXPECT_EQ(FillOf(dark, "c").color.rgba[0], 0.0);
  EXPECT_EQ(FillOf(dark, "l").color.rgba[2], 1.0);
  Styled preferred_light(Svg("style=\"color-scheme: light dark\"", "<rect id=\"l\" fill=\"light-dark(red, blue)\"/>"));
  EXPECT_EQ(FillOf(preferred_light, "l").color.rgba[0], 1.0);
  Styled only_dark(Svg("style=\"color-scheme: dark\"", "<rect id=\"c\" fill=\"Canvas\"/>"));
  EXPECT_EQ(FillOf(only_dark, "c").color.rgba[0], 0.0);
}

TEST(SvgStylePaint, ServersFallbacksAndKeywords) {
  Styled s(Svg("", "<defs><linearGradient id=\"g\"/><pattern id=\"p\"/><rect id=\"notaserver\"/></defs>"
                   "<rect id=\"a\" fill=\"url(#g)\"/><rect id=\"b\" fill=\"url(#p) red\"/><rect id=\"c\" fill=\"url(#missing) blue\"/>"
                   "<rect id=\"d\" fill=\"url(#missing)\"/><rect id=\"e\" fill=\"url(#notaserver) none\"/><rect id=\"f\" fill=\"url(other.svg#x)\"/>"
                   "<rect id=\"g2\" fill=\"context-fill\" stroke=\"context-stroke\"/><rect id=\"h\" fill=\"none\"/>"
                   "<rect id=\"i\" fill=\"url(#g) none\"/><rect id=\"j\" fill=\"url(%23g)\"/><rect id=\"k\" fill=\"url(#%67)\"/>"));
  GVEC_Svg_Paint a = s.Get("a", GVEC_SVG_PROP_FILL).paint;
  EXPECT_EQ(a.kind, GVEC_SVG_PAINT_SERVER);
  EXPECT_EQ(a.server, s.Id("g"));
  EXPECT_STREQ(a.server_id, "g");
  EXPECT_FALSE(a.has_fallback);
  GVEC_Svg_Paint b = s.Get("b", GVEC_SVG_PROP_FILL).paint;
  EXPECT_EQ(b.kind, GVEC_SVG_PAINT_SERVER);
  EXPECT_TRUE(b.has_fallback);
  EXPECT_EQ(b.fallback_kind, GVEC_SVG_PAINT_COLOR);
  EXPECT_EQ(b.fallback_color.color.rgba[0], 1.0);
  /* A missing target: the fallback, else none. */
  GVEC_Svg_Paint c = s.Get("c", GVEC_SVG_PROP_FILL).paint;
  EXPECT_EQ(c.kind, GVEC_SVG_PAINT_COLOR);
  EXPECT_EQ(c.color.color.rgba[2], 1.0);
  EXPECT_EQ(s.Get("d", GVEC_SVG_PROP_FILL).paint.kind, GVEC_SVG_PAINT_NONE);
  EXPECT_EQ(s.Get("e", GVEC_SVG_PROP_FILL).paint.kind, GVEC_SVG_PAINT_NONE);
  EXPECT_EQ(s.Get("f", GVEC_SVG_PROP_FILL).paint.kind, GVEC_SVG_PAINT_NONE);
  EXPECT_EQ(s.Get("g2", GVEC_SVG_PROP_FILL).paint.kind, GVEC_SVG_PAINT_CONTEXT_FILL);
  EXPECT_EQ(s.Get("g2", GVEC_SVG_PROP_STROKE).paint.kind, GVEC_SVG_PAINT_CONTEXT_STROKE);
  EXPECT_EQ(s.Get("h", GVEC_SVG_PROP_FILL).paint.kind, GVEC_SVG_PAINT_NONE);
  GVEC_Svg_Paint i = s.Get("i", GVEC_SVG_PROP_FILL).paint;
  EXPECT_EQ(i.kind, GVEC_SVG_PAINT_SERVER);
  EXPECT_TRUE(i.has_fallback);
  EXPECT_EQ(i.fallback_kind, GVEC_SVG_PAINT_NONE);
  /* `%23g` is a relative URL with a path, not a fragment; `#%67` is the fragment `g`. */
  EXPECT_EQ(s.Get("j", GVEC_SVG_PROP_FILL).paint.kind, GVEC_SVG_PAINT_NONE);
  EXPECT_EQ(s.Get("k", GVEC_SVG_PROP_FILL).paint.server, s.Id("g"));
  EXPECT_GE(s.Count(GVEC_DIAG_REFERENCE_MISSING), 4u);
}

TEST(SvgStyleUsed, DashArrays) {
  Styled s(Svg("width=\"200\" height=\"100\"",
      "<path id=\"a\" stroke-dasharray=\"5 10\"/><path id=\"b\" stroke-dasharray=\"5,10,15\"/><path id=\"c\" stroke-dasharray=\"0 0\"/>"
      "<path id=\"d\" stroke-dasharray=\"none\"/><path id=\"e\" stroke-dasharray=\"10% 1\"/><path id=\"f\" stroke-dasharray=\"-1 2\"/>"
      "<path id=\"g\" stroke-dasharray=\"1em 2\" font-size=\"10\"/><path id=\"h\"/>"));
  GVEC_Svg_Style_Value a = s.Get("a", GVEC_SVG_PROP_STROKE_DASHARRAY);
  ASSERT_EQ(a.kind, GVEC_SVG_STYLE_NUMBER_LIST);
  ASSERT_EQ(a.number_count, 2u);
  EXPECT_EQ(a.numbers[0], 5.0);
  EXPECT_EQ(a.numbers[1], 10.0);
  GVEC_Svg_Style_Value b = s.Get("b", GVEC_SVG_PROP_STROKE_DASHARRAY);
  ASSERT_EQ(b.number_count, 6u);  /* an odd list is repeated */
  EXPECT_EQ(b.numbers[3], 5.0);
  EXPECT_EQ(b.numbers[5], 15.0);
  EXPECT_EQ(s.Get("c", GVEC_SVG_PROP_STROKE_DASHARRAY).number_count, 0u);  /* all zero: solid */
  EXPECT_EQ(s.Get("d", GVEC_SVG_PROP_STROKE_DASHARRAY).number_count, 0u);
  GVEC_Svg_Style_Value e = s.Get("e", GVEC_SVG_PROP_STROKE_DASHARRAY);
  ASSERT_EQ(e.number_count, 2u);
  EXPECT_DOUBLE_EQ(e.numbers[0], std::sqrt((200.0 * 200.0 + 100.0 * 100.0) / 2.0) * 0.1);
  EXPECT_EQ(s.Get("f", GVEC_SVG_PROP_STROKE_DASHARRAY).number_count, 0u);  /* invalid: dropped, so none */
  GVEC_Svg_Style_Value g = s.Get("g", GVEC_SVG_PROP_STROKE_DASHARRAY);
  ASSERT_EQ(g.number_count, 2u);
  EXPECT_EQ(g.numbers[0], 10.0);
  EXPECT_EQ(s.Get("h", GVEC_SVG_PROP_STROKE_DASHARRAY).number_count, 0u);
}

TEST(SvgStyleUsed, Transforms) {
  Styled s(Svg("width=\"200\" height=\"100\"",
      "<g id=\"rot\" transform=\"rotate(90)\"/><g id=\"rot3\" transform=\"rotate(90 10 20)\"/>"
      "<g id=\"list\" transform=\"translate(10, 20) scale(2)\"/><g id=\"mat\" transform=\"matrix(1 2 3 4 5 6)\"/>"
      "<g id=\"skew\" transform=\"skewX(45)\"/><g id=\"none\"/><g id=\"bad\" transform=\"rotate(\"/>"
      "<g id=\"pct\" style=\"transform: translate(50%, 10%)\"/><g id=\"box\" style=\"transform: translate(50%, 10%); transform-box: fill-box\"/>"
      "<g id=\"origin\" style=\"transform: rotate(180deg); transform-origin: 10px 20px\"/>"
      "<g id=\"originpct\" style=\"transform: rotate(180deg); transform-origin: 50% 50%\"/>"));
  auto map = [&](const char * id) {
    GVEC_Svg_Style_Value v = s.Get(id, GVEC_SVG_PROP_TRANSFORM);
    EXPECT_EQ(v.kind, GVEC_SVG_STYLE_TRANSFORM) << id;
    return v.transform;
  };
  GVEC_Svg_Transform rot = map("rot");
  EXPECT_FALSE(rot.depends_on_box);
  EXPECT_EQ(rot.map.m[0][0], 0.0);
  EXPECT_EQ(rot.map.m[0][1], -1.0);
  EXPECT_EQ(rot.map.m[1][0], 1.0);
  EXPECT_EQ(rot.map.m[1][1], 0.0);
  GVEC_Svg_Transform rot3 = map("rot3");
  /* rotate(90 10 20) = translate(10, 20) rotate(90) translate(-10, -20): (0,0) goes to (30, 10). */
  double x = 0, y = 0;
  EXPECT_EQ(rot3.map.m[0][0] * x + rot3.map.m[0][1] * y + rot3.map.t[0], 30.0);
  EXPECT_EQ(rot3.map.m[1][0] * x + rot3.map.m[1][1] * y + rot3.map.t[1], 10.0);
  GVEC_Svg_Transform list = map("list");
  EXPECT_EQ(list.map.m[0][0], 2.0);
  EXPECT_EQ(list.map.m[1][1], 2.0);
  EXPECT_EQ(list.map.t[0], 10.0);
  EXPECT_EQ(list.map.t[1], 20.0);
  GVEC_Svg_Transform mat = map("mat");
  EXPECT_EQ(mat.map.m[0][0], 1.0);
  EXPECT_EQ(mat.map.m[1][0], 2.0);
  EXPECT_EQ(mat.map.m[0][1], 3.0);
  EXPECT_EQ(mat.map.m[1][1], 4.0);
  EXPECT_EQ(mat.map.t[0], 5.0);
  EXPECT_EQ(mat.map.t[1], 6.0);
  GVEC_Svg_Transform skew = map("skew");
  EXPECT_NEAR(skew.map.m[0][1], 1.0, 1e-15);
  GVEC_Svg_Transform none = map("none");
  EXPECT_EQ(none.op_count, 0u);
  EXPECT_EQ(none.map.m[0][0], 1.0);
  EXPECT_EQ(none.map.t[0], 0.0);
  EXPECT_EQ(map("bad").op_count, 0u);  /* invalid: dropped */
  /* A percentage of the view box is resolved already: the viewport is 200 by 100. */
  GVEC_Svg_Transform pct = map("pct");
  EXPECT_FALSE(pct.depends_on_box);
  EXPECT_EQ(pct.map.t[0], 100.0);
  EXPECT_EQ(pct.map.t[1], 10.0);
  /* Of the fill box, it depends on the bounding box. */
  GVEC_Svg_Transform box = map("box");
  EXPECT_TRUE(box.depends_on_box);
  EXPECT_EQ(box.box, GVEC_SVG_BOX_FILL);
  GSHGEOM_Map resolved;
  ASSERT_EQ(gvec_svg_transform_resolve(&box, 10, 20, 40, 80, &resolved), GVEC_OK);
  EXPECT_EQ(resolved.t[0], 20.0);
  EXPECT_EQ(resolved.t[1], 8.0);
  GVEC_Svg_Transform origin = map("origin");
  EXPECT_FALSE(origin.depends_on_box);
  /* A half turn about (10, 20): (0,0) goes to (20, 40). */
  EXPECT_EQ(origin.map.m[0][0] * 0 + origin.map.m[0][1] * 0 + origin.map.t[0], 20.0);
  EXPECT_EQ(origin.map.t[1], 40.0);
  GVEC_Svg_Transform originpct = map("originpct");
  EXPECT_FALSE(originpct.depends_on_box);
  EXPECT_EQ(originpct.map.t[0], 200.0);
  EXPECT_EQ(originpct.map.t[1], 100.0);
}

TEST(SvgStyleUsed, ReferencesOfClipMaskMarkerAndFilter) {
  Styled s(Svg("", "<defs><clipPath id=\"c\"/><mask id=\"m\"/><marker id=\"k\"/><filter id=\"f\"/></defs>"
                   "<path id=\"a\" clip-path=\"url(#c)\" mask=\"url(#m)\" marker-start=\"url(#k)\" marker-end=\"url(#nothere)\" filter=\"url(#f) blur(2px)\"/>"
                   "<path id=\"b\"/><path id=\"c2\" style=\"marker: url(#k)\"/>"));
  GVEC_Svg_Style_Value clip = s.Get("a", GVEC_SVG_PROP_CLIP_PATH);
  ASSERT_EQ(clip.kind, GVEC_SVG_STYLE_REFERENCES);
  ASSERT_EQ(clip.reference_count, 1u);
  EXPECT_TRUE(clip.references[0].is_url);
  EXPECT_TRUE(clip.references[0].local);
  EXPECT_STREQ(clip.references[0].id, "c");
  EXPECT_EQ(clip.references[0].target, s.Id("c"));
  GVEC_Svg_Style_Value mask = s.Get("a", GVEC_SVG_PROP_MASK);
  ASSERT_EQ(mask.reference_count, 1u);
  EXPECT_EQ(mask.references[0].target, s.Id("m"));
  GVEC_Svg_Style_Value start = s.Get("a", GVEC_SVG_PROP_MARKER_START);
  ASSERT_EQ(start.reference_count, 1u);
  EXPECT_EQ(start.references[0].target, s.Id("k"));
  GVEC_Svg_Style_Value end = s.Get("a", GVEC_SVG_PROP_MARKER_END);
  ASSERT_EQ(end.reference_count, 1u);
  EXPECT_TRUE(end.references[0].local);
  EXPECT_EQ(end.references[0].target, nullptr);
  GVEC_Svg_Style_Value filter = s.Get("a", GVEC_SVG_PROP_FILTER);
  ASSERT_EQ(filter.reference_count, 2u);
  EXPECT_TRUE(filter.references[0].is_url);
  EXPECT_FALSE(filter.references[1].is_url);
  EXPECT_EQ(s.Get("b", GVEC_SVG_PROP_CLIP_PATH).reference_count, 0u);
  /* The marker shorthand sets all three. */
  GVEC_Svg_Style_Value mid = s.Get("c2", GVEC_SVG_PROP_MARKER_MID);
  ASSERT_EQ(mid.reference_count, 1u);
  EXPECT_EQ(mid.references[0].target, s.Id("k"));
}

TEST(SvgStyleCascade, CustomPropertiesAndVar) {
  Styled s(Svg("", "<g style=\"--c: red; --w: 3\"><rect id=\"r\" style=\"fill: var(--c); stroke-width: var(--w)\"/>"
                   "<rect id=\"u\" style=\"fill: var(--missing, blue)\"/><rect id=\"x\" style=\"fill: var(--missing)\"/></g>"
                   "<rect id=\"cyc\" style=\"--a: var(--b); --b: var(--a); fill: var(--a, green)\"/>"));
  EXPECT_EQ(FillOf(s, "r").color.rgba[0], 1.0);
  EXPECT_EQ(s.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 3.0);
  EXPECT_EQ(FillOf(s, "u").color.rgba[2], 1.0);
  /* No value, no fallback: invalid at computed-value time, so `unset`: fill is inherited, black by default. */
  GVEC_Svg_Style_Value x = s.Get("x", GVEC_SVG_PROP_FILL);
  EXPECT_EQ(x.paint.color.kind, GVEC_SVG_COLOR_SOLID);
  EXPECT_EQ(x.paint.color.color.rgba[0], 0.0);
  EXPECT_EQ(FillOf(s, "cyc").color.rgba[1], 128.0 / 255.0);  /* a cycle is invalid, so the fallback */
}

TEST(SvgStyleDump, ACanonicalTextAndAHash) {
  Styled s(Svg("", "<g fill=\"red\"><rect id=\"r\" stroke-width=\"2\"/></g>"));
  GVEC_Stream * out = nullptr;
  ASSERT_EQ(gvec_stream_create_memory_output(&out), GVEC_OK);
  ASSERT_EQ(gvec_svg_style_dump(s.context, s.Of("r"), out), GVEC_OK);
  const void * data = nullptr;
  size_t size = 0;
  gvec_stream_output_buffer(out, &data, &size);
  std::string text((const char *) data, size);
  gvec_stream_destroy(out);
  EXPECT_EQ(text.rfind("css-computed 1\n", 0), 0u);
  EXPECT_NE(text.find("\nfill: red\n"), std::string::npos);
  EXPECT_NE(text.find("\nstroke-width: 2px\n"), std::string::npos);
  uint64_t a = 0, b = 0;
  ASSERT_EQ(gvec_svg_style_hash(s.context, &a), GVEC_OK);
  ASSERT_EQ(gvec_svg_style_hash(s.context, &b), GVEC_OK);
  EXPECT_EQ(a, b);
  Styled other(Svg("", "<g fill=\"blue\"><rect id=\"r\" stroke-width=\"2\"/></g>"));
  uint64_t c = 0;
  ASSERT_EQ(gvec_svg_style_hash(other.context, &c), GVEC_OK);
  EXPECT_NE(a, c);
}

TEST(SvgStyleReferences, AClipPathThatClipsItselfIsACycle) {
  Styled s(Svg("", "<defs><clipPath id=\"c\"><rect id=\"in\" clip-path=\"url(#c)\"/></clipPath>"
                   "<clipPath id=\"ok\"><rect id=\"fine\"/></clipPath></defs><rect id=\"r\" clip-path=\"url(#c)\"/>"
                   "<rect id=\"r2\" clip-path=\"url(#ok)\"/>"));
  ASSERT_EQ(gvec_svg_style_resolve_references(s.context), GVEC_OK);
  EXPECT_TRUE(gvec_svg_style_on_reference_cycle(s.context, s.Id("c")));
  EXPECT_TRUE(gvec_svg_style_on_reference_cycle(s.context, s.Id("in")));
  EXPECT_FALSE(gvec_svg_style_on_reference_cycle(s.context, s.Id("r")));  /* refers into the cycle; not on it */
  EXPECT_FALSE(gvec_svg_style_on_reference_cycle(s.context, s.Id("ok")));
  EXPECT_FALSE(gvec_svg_style_on_reference_cycle(s.context, s.Id("r2")));
  EXPECT_EQ(s.Count(GVEC_DIAG_REFERENCE_CYCLE), 2u);
  /* The reference that closes the cycle is cut; the reference into it is not. */
  GVEC_Svg_Style_Value in = s.Get("in", GVEC_SVG_PROP_CLIP_PATH);
  ASSERT_EQ(in.reference_count, 1u);
  EXPECT_TRUE(in.references[0].cut);
  EXPECT_EQ(in.references[0].target, nullptr);
  GVEC_Svg_Style_Value r = s.Get("r", GVEC_SVG_PROP_CLIP_PATH);
  EXPECT_FALSE(r.references[0].cut);
  EXPECT_EQ(r.references[0].target, s.Id("c"));
  /* Once more does nothing more. */
  ASSERT_EQ(gvec_svg_style_resolve_references(s.context), GVEC_OK);
  EXPECT_EQ(s.Count(GVEC_DIAG_REFERENCE_CYCLE), 2u);
}

TEST(SvgStyleReferences, APatternPaintedWithItselfAndMarkersThroughUse) {
  Styled s(Svg("", "<defs><pattern id=\"p\"><rect id=\"pr\" fill=\"url(#p) red\"/></pattern>"
                   "<marker id=\"m\"><use id=\"mu\" href=\"#shape\"/></marker>"
                   "<path id=\"shape\" marker-start=\"url(#m)\"/></defs>"
                   "<rect id=\"user\" fill=\"url(#p)\"/>"));
  ASSERT_EQ(gvec_svg_style_resolve_references(s.context), GVEC_OK);
  EXPECT_TRUE(gvec_svg_style_on_reference_cycle(s.context, s.Id("p")));
  EXPECT_TRUE(gvec_svg_style_on_reference_cycle(s.context, s.Id("pr")));
  EXPECT_TRUE(gvec_svg_style_on_reference_cycle(s.context, s.Id("m")));
  EXPECT_TRUE(gvec_svg_style_on_reference_cycle(s.context, s.Id("mu")));
  EXPECT_TRUE(gvec_svg_style_on_reference_cycle(s.context, s.Id("shape")));
  EXPECT_FALSE(gvec_svg_style_on_reference_cycle(s.context, s.Id("user")));
  /* The cut reference reads as its fallback; the one from outside the cycle is a paint server. */
  GVEC_Svg_Paint pr = s.Get("pr", GVEC_SVG_PROP_FILL).paint;
  EXPECT_EQ(pr.kind, GVEC_SVG_PAINT_COLOR);
  EXPECT_EQ(pr.color.color.rgba[0], 1.0);
  EXPECT_EQ(s.Get("user", GVEC_SVG_PROP_FILL).paint.kind, GVEC_SVG_PAINT_SERVER);
  EXPECT_TRUE(s.Get("shape", GVEC_SVG_PROP_MARKER_START).references[0].cut);
}

TEST(SvgStyleReferences, DocumentsWithoutCyclesAndManyReferences) {
  std::string body = "<defs><clipPath id=\"c\"><rect/></clipPath></defs>";
  for (int i = 0; i < 500; i++) {
    body += "<rect clip-path=\"url(#c)\" fill=\"url(#none" + std::to_string(i) + ")\"/>";
  }
  Styled s(Svg("", body));
  ASSERT_EQ(gvec_svg_style_resolve_references(s.context), GVEC_OK);
  EXPECT_EQ(s.Count(GVEC_DIAG_REFERENCE_CYCLE), 0u);
  EXPECT_FALSE(gvec_svg_style_on_reference_cycle(s.context, s.Id("c")));
}

namespace {

/** What the sequence of the allocation sweep does, once, with the allocator of `alloc`; the counts of calls after each part. */
struct Sweep {
  GVEC_Result result = GVEC_OK;
  long after_load = 0, after_context = 0, after_reads = 0;
};

Sweep RunSequence(const std::string & text, Failing & alloc) {
  Sweep sweep;
  GVEC_Stream * stream = nullptr;
  EXPECT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
  GVEC_Load_Options options;
  std::memset(&options, 0, sizeof options);
  options.allocator = &alloc.a;
  GVEC_Doc * doc = nullptr;
  GVEC_Result r = gvec_doc_load(stream, &options, nullptr, &doc);
  gvec_stream_destroy(stream);
  sweep.after_load = alloc.attempts;
  if (r != GVEC_OK) {
    sweep.result = r;
    return sweep;
  }
  GVEC_Diagnostics diagnostics;
  gvec_diagnostics_init(&diagnostics, &alloc.a);
  GVEC_Svg_Style_Context * context = nullptr;
  r = gvec_svg_style_context_new(gvec_svg_document(doc), &diagnostics, &alloc.a, &context);
  sweep.after_context = alloc.attempts;
  if (r == GVEC_OK) {
    const GVEC_Svg_Instance * root = gvec_svg_instance_root(context);
    if (!root) {
      r = GVEC_ERR_OOM;
    }
    else {
      const GVEC_Svg_Document * svg = gvec_svg_document(doc);
      const GVEC_Svg_Instance *gi = nullptr, *ui = nullptr, *clone = nullptr;
      r = gvec_svg_instance_child(context, root, gvec_svg_find_by_id(svg, "g1"), &gi);
      if (r == GVEC_OK) {
        r = gvec_svg_instance_child(context, gi, gvec_svg_find_by_id(svg, "u"), &ui);
      }
      if (r == GVEC_OK) {
        r = gvec_svg_instance_use(context, ui, &clone);
      }
      for (const GVEC_Svg_Instance * i : {gi, ui, clone}) {
        for (int p = 0; r == GVEC_OK && i && p < GVEC_SVG_PROP_COUNT; p++) {
          GVEC_Svg_Style_Value v;
          r = gvec_svg_get(context, i, (GVEC_Svg_Property) p, &v);
        }
      }
      uint64_t hash;
      if (r == GVEC_OK) {
        r = gvec_svg_style_resolve_references(context);
      }
      if (r == GVEC_OK) {
        r = gvec_svg_style_hash(context, &hash);
      }
    }
  }
  sweep.after_reads = alloc.attempts;
  sweep.result = r;
  gvec_svg_style_context_free(context);
  gvec_diagnostics_destroy(&diagnostics);
  gvec_doc_free(doc);
  return sweep;
}

}  // namespace

TEST(SvgStyleAllocation, EveryFailureIsOutOfMemoryAndNothingLeaks) {
  const std::string text = Svg("viewBox=\"0 0 100 50\" style=\"fill: red\"",
      "<defs><linearGradient id=\"g\"/><symbol id=\"s\"><rect id=\"in\" width=\"50%\"/></symbol></defs>"
      "<g id=\"g1\" fill=\"url(#g) blue\" stroke-dasharray=\"1 2 3\" transform=\"rotate(30) translate(5 5)\" style=\"--c: green; stroke: var(--c)\">"
      "<rect id=\"r\" clip-path=\"url(#none)\"/><use id=\"u\" href=\"#s\"/></g>");
  Failing baseline;
  Sweep base = RunSequence(text, baseline);
  ASSERT_EQ(base.result, GVEC_OK);
  EXPECT_EQ(baseline.outstanding, 0);
  /* A context made with another allocator builds its own engine through it: forty thousand calls of the CSS engine, which `text`'s own
   * sweeps fail one by one, and every one of them costs the sweep the making of an engine. So every call of this library's is failed
   * when GVEC_SWEEP_EVERY=1 (the thorough run, once, at the end of the work) and a spread of them otherwise: one in eight of the reads
   * and a hundred of the engine's. */
  long every = 8;
  if (const char * text_every = std::getenv("GVEC_SWEEP_EVERY")) {
    every = std::max(1L, std::atol(text_every));
  }
  long stride = every == 1 ? 1 : std::max(1L, (base.after_context - base.after_load) / 100);
  long failures = 0, oom = 0;
  for (long fail_at = 1; fail_at <= base.after_reads; fail_at++) {
    bool engine_part = fail_at > base.after_load && fail_at <= base.after_context;
    if (engine_part && (fail_at - base.after_load) % stride != 0) {
      continue;
    }
    if (fail_at > base.after_context && (fail_at - base.after_context) % every != 0) {
      continue;
    }
    Failing alloc;
    alloc.fail_at = fail_at;
    Sweep sweep = RunSequence(text, alloc);
    /* Out of memory, or the sequence that would have succeeded (an allocation that only reports something is allowed to fail). */
    EXPECT_TRUE(sweep.result == GVEC_ERR_OOM || sweep.result == GVEC_OK) << fail_at << " " << gvec_result_string(sweep.result);
    oom += sweep.result == GVEC_ERR_OOM;
    EXPECT_EQ(alloc.outstanding, 0) << "leak after failing allocation " << fail_at;
    failures++;
  }
  EXPECT_GT(failures, 100);
  EXPECT_GT(oom, failures / 2);  /* most failures are felt */
}

TEST(SvgStyleEngine, ContextsShareAnEngineAndTheCacheCanBeCleared) {
  gvec_svg_style_cache_clear();
  {
    Styled a(Svg("", "<rect id=\"r\" fill=\"red\"/>"));
    Styled b(Svg("", "<rect id=\"r\" fill=\"blue\"/>"));
    EXPECT_EQ(a.Get("r", GVEC_SVG_PROP_FILL).paint.color.color.rgba[0], 1.0);
    EXPECT_EQ(b.Get("r", GVEC_SVG_PROP_FILL).paint.color.color.rgba[2], 1.0);
    gvec_svg_style_cache_clear();  /* a context in use keeps its engine */
    EXPECT_EQ(a.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 1.0);
  }
  gvec_svg_style_cache_clear();
  Styled c(Svg("", "<rect id=\"r\" stroke-width=\"4\"/>"));
  EXPECT_EQ(c.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 4.0);
}

TEST(SvgStyleEngine, DifferentEnvironmentsDoNotShare) {
  Styled small(Svg("", "<rect id=\"r\" stroke-width=\"10vw\"/>"), 100, 50);
  Styled big(Svg("", "<rect id=\"r\" stroke-width=\"10vw\"/>"), 1000, 50);
  Styled def(Svg("", "<rect id=\"r\" stroke-width=\"10vw\"/>"));
  EXPECT_EQ(small.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 10.0);
  EXPECT_EQ(big.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 100.0);
  EXPECT_EQ(def.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number, 30.0);
}

TEST(SvgStyleLimits, TheStyleBytesAtTheBoundLoadAndOnePastIsDroppedAndReported) {
  const std::string style = "fill: red; stroke: blue";
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_style_bytes = style.size();
  Styled at(Svg("", "<rect id=\"r\" style=\"" + style + "\"/>"), 0, 0, GVEC_COLOR_SCHEME_LIGHT, &limits);
  EXPECT_EQ(at.Get("r", GVEC_SVG_PROP_FILL).paint.color.color.rgba[0], 1.0);
  EXPECT_EQ(at.Get("r", GVEC_SVG_PROP_STROKE).paint.color.color.rgba[2], 1.0);
  EXPECT_EQ(at.Count(GVEC_DIAG_LIMIT), 0u);
  limits.max_style_bytes = style.size() - 1;
  Styled past(Svg("", "<rect id=\"r\" style=\"" + style + "\" fill=\"green\"/>"), 0, 0, GVEC_COLOR_SCHEME_LIGHT, &limits);
  /* The style is dropped; the presentation attribute stands. */
  EXPECT_EQ(past.Get("r", GVEC_SVG_PROP_FILL).paint.color.color.rgba[1], 128.0 / 255.0);
  EXPECT_EQ(past.Get("r", GVEC_SVG_PROP_STROKE).paint.kind, GVEC_SVG_PAINT_NONE);
  ASSERT_EQ(past.Count(GVEC_DIAG_LIMIT), 1u);
  const GVEC_Diagnostic * d = nullptr;
  for (size_t i = 0; i < past.diagnostics.count; i++) {
    if (past.diagnostics.items[i].code == GVEC_DIAG_LIMIT) {
      d = &past.diagnostics.items[i];
    }
  }
  EXPECT_EQ(d->detail, (uint32_t) GVEC_LIMIT_STYLE_BYTES);
  EXPECT_STREQ(d->element, "rect");
}

TEST(SvgStyleLimits, TheDeclarationsAtTheBoundLoadAndOnePastIsDroppedAndReported) {
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_declarations = 3;
  Styled at(Svg("", "<rect id=\"r\" style=\"fill: red; stroke: blue; opacity: 0.5\"/>"), 0, 0, GVEC_COLOR_SCHEME_LIGHT, &limits);
  EXPECT_EQ(at.Get("r", GVEC_SVG_PROP_OPACITY).number, 0.5);
  EXPECT_EQ(at.Count(GVEC_DIAG_LIMIT), 0u);
  Styled past(Svg("", "<rect id=\"r\" style=\"fill: red; stroke: blue; opacity: 0.5; fill-opacity: 0.25\"/>"), 0, 0, GVEC_COLOR_SCHEME_LIGHT, &limits);
  EXPECT_EQ(past.Get("r", GVEC_SVG_PROP_OPACITY).number, 1.0);
  EXPECT_EQ(past.Count(GVEC_DIAG_LIMIT), 1u);
}

TEST(SvgStyleLimits, TheExpansionOfVarAtTheBoundHoldsAndOnePastIsInvalidAtComputedValueTime) {
  /* `--a` is 8 tokens, `--b` is three of them. The engine counts the tokens of what a value expands to; the bound is found, and the
   * property is asserted to hold exactly from there up and to fail exactly below it. */
  const std::string body = "<rect id=\"r\" style=\"--a: 1 2 3 4 5 6 7 8; --b: var(--a) var(--a) var(--a); stroke-dasharray: var(--b)\"/>";
  Styled unlimited(Svg("", body));
  EXPECT_EQ(unlimited.Get("r", GVEC_SVG_PROP_STROKE_DASHARRAY).number_count, 24u);
  size_t bound = 0;
  for (size_t limit = 1; limit <= 200; limit++) {
    GVEC_Limits limits;
    std::memset(&limits, 0, sizeof limits);
    limits.max_custom_expansion = limit;
    Styled s(Svg("", body), 0, 0, GVEC_COLOR_SCHEME_LIGHT, &limits);
    size_t count = s.Get("r", GVEC_SVG_PROP_STROKE_DASHARRAY).number_count;
    if (!bound && count == 24u) {
      bound = limit;
    }
    if (bound) {
      EXPECT_EQ(count, 24u) << limit;
      EXPECT_EQ(s.Count(GVEC_DIAG_LIMIT), 0u) << limit;
    }
    else {
      EXPECT_EQ(count, 0u) << limit;
      EXPECT_EQ(s.Count(GVEC_DIAG_LIMIT), 1u) << limit;
    }
  }
  EXPECT_GE(bound, 24u);
  EXPECT_LE(bound, 48u);
}

TEST(SvgStyleLimits, TheInstancesAreBounded) {
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_elements = 3;
  limits.max_reference_expansion = 1;
  Styled s(Svg("", "<g id=\"g\"><rect id=\"a\"/></g>"), 0, 0, GVEC_COLOR_SCHEME_LIGHT, &limits);  /* svg, g, rect: three elements */
  const GVEC_Svg_Instance * root = gvec_svg_instance_root(s.context);
  const GVEC_Svg_Instance * g = nullptr;
  ASSERT_EQ(gvec_svg_instance_child(s.context, root, s.Id("g"), &g), GVEC_OK);
  const GVEC_Svg_Instance * a = nullptr;
  ASSERT_EQ(gvec_svg_instance_child(s.context, g, s.Id("a"), &a), GVEC_OK);
  /* The bound is the elements and the instances the references may add: four. */
  const GVEC_Svg_Instance * more = nullptr;
  ASSERT_EQ(gvec_svg_instance_child(s.context, root, s.Id("g"), &more), GVEC_OK);
  EXPECT_EQ(gvec_svg_instance_child(s.context, root, s.Id("g"), &more), GVEC_ERR_LIMIT);
  EXPECT_EQ(s.Count(GVEC_DIAG_LIMIT), 1u);
}

TEST(SvgStyleInstances, ABadInstanceIsRefused) {
  Styled s(Svg("", "<g id=\"g\"><rect id=\"a\"/></g><rect id=\"b\"/>"));
  const GVEC_Svg_Instance * root = gvec_svg_instance_root(s.context);
  const GVEC_Svg_Instance * out = nullptr;
  EXPECT_EQ(gvec_svg_instance_child(s.context, root, s.Id("a"), &out), GVEC_ERR_INVALID);  /* not a child of the root */
  EXPECT_EQ(gvec_svg_instance_child(nullptr, root, s.Id("b"), &out), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_instance_child(s.context, nullptr, s.Id("b"), &out), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_instance_child(s.context, root, nullptr, &out), GVEC_ERR_INVALID);
  GVEC_Svg_Style_Value v;
  EXPECT_EQ(gvec_svg_get(s.context, root, GVEC_SVG_PROP_COUNT, &v), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_get(s.context, root, (GVEC_Svg_Property) -1, &v), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_get(s.context, nullptr, GVEC_SVG_PROP_FILL, &v), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_get(s.context, root, GVEC_SVG_PROP_FILL, nullptr), GVEC_ERR_INVALID);
  GVEC_Svg_Style_Specified spec;
  EXPECT_EQ(gvec_svg_get_specified(s.context, root, GVEC_SVG_PROP_COUNT, &spec), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_style_context_new(nullptr, nullptr, nullptr, &s.context), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_style_dump(s.context, root, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_style_hash(s.context, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_svg_instance_element(nullptr), nullptr);
  EXPECT_EQ(gvec_svg_instance_parent(nullptr), nullptr);
  EXPECT_EQ(gvec_svg_instance_parent(root), nullptr);
  EXPECT_EQ(gvec_svg_instance_use_context(root), nullptr);
  gvec_svg_instance_viewport(nullptr, nullptr, nullptr);
  /* A shorthand has no value of its own. */
  GVEC_Svg_Style_Value marker = s.Get(root, GVEC_SVG_PROP_MARKER);
  EXPECT_EQ(marker.kind, GVEC_SVG_STYLE_NONE);
  EXPECT_EQ(gvec_svg_property_source((GVEC_Svg_Property) 9999), GVEC_SVG_PROPERTY_GENERATED);
}

TEST(SvgStyleThreads, ContextsOnManyThreadsShareEnginesSafely) {
  gvec_svg_style_cache_clear();
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&failures, t] {
      for (int i = 0; i < 20; i++) {
        double width = 100 + 50 * ((t + i) % 3);  /* three environments, contended */
        Styled s(Svg("", "<g fill=\"red\"><rect id=\"r\" stroke-width=\"10vw\"/></g>"), width, 50);
        if (!s.context || s.Get("r", GVEC_SVG_PROP_STROKE_WIDTH).number != width / 10) {
          failures++;
        }
        if (i % 7 == 0) {
          gvec_svg_style_cache_clear();
        }
      }
    });
  }
  for (std::thread & thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
}

TEST(SvgStyleUsed, AUsesWidthAndHeightSizeTheViewportOfASymbolWithoutAViewBox) {
  Styled s(Svg("width=\"200\" height=\"100\"",
      "<defs><symbol id=\"s\"><rect id=\"in\" width=\"50%\" height=\"50%\"/></symbol>"
      "<symbol id=\"own\" width=\"60\" height=\"20\"><rect id=\"own_in\" width=\"50%\" height=\"50%\"/></symbol></defs>"
      "<use id=\"u\" href=\"#s\" width=\"40\" height=\"10\"/><use id=\"auto\" href=\"#s\"/><use id=\"o\" href=\"#own\"/>"
      "<use id=\"o2\" href=\"#own\" width=\"80\"/>"));
  auto inside = [&](const char * use, const char * rect, GVEC_Svg_Property p) {
    const GVEC_Svg_Instance * u = s.Of(use);
    const GVEC_Svg_Instance * clone = nullptr;
    EXPECT_EQ(gvec_svg_instance_use(s.context, u, &clone), GVEC_OK);
    const GVEC_Svg_Instance * r = nullptr;
    EXPECT_EQ(gvec_svg_instance_child(s.context, clone, s.Id(rect), &r), GVEC_OK);
    return s.Get(r, p).number;
  };
  EXPECT_EQ(inside("u", "in", GVEC_SVG_PROP_WIDTH), 20.0);   /* the use's width and height */
  EXPECT_EQ(inside("u", "in", GVEC_SVG_PROP_HEIGHT), 5.0);
  EXPECT_EQ(inside("auto", "in", GVEC_SVG_PROP_WIDTH), 100.0);  /* neither: 100% of the use's viewport */
  EXPECT_EQ(inside("o", "own_in", GVEC_SVG_PROP_WIDTH), 30.0);  /* the symbol's own */
  EXPECT_EQ(inside("o", "own_in", GVEC_SVG_PROP_HEIGHT), 10.0);
  EXPECT_EQ(inside("o2", "own_in", GVEC_SVG_PROP_WIDTH), 40.0);  /* the use gives the width, the symbol the height */
  EXPECT_EQ(inside("o2", "own_in", GVEC_SVG_PROP_HEIGHT), 10.0);
}

TEST(SvgStyleUsed, AMarkerEstablishesItsOwnViewport) {
  Styled s(Svg("width=\"200\" height=\"100\"",
      "<defs><marker id=\"m\" markerWidth=\"20\" markerHeight=\"8\"><rect id=\"a\" width=\"50%\" height=\"50%\"/></marker>"
      "<marker id=\"d\"><rect id=\"b\" width=\"100%\"/></marker>"
      "<marker id=\"v\" markerWidth=\"20\" markerHeight=\"8\" viewBox=\"0 0 4 4\"><rect id=\"c\" width=\"50%\"/></marker></defs>"));
  EXPECT_EQ(s.Get("a", GVEC_SVG_PROP_WIDTH).number, 10.0);
  EXPECT_EQ(s.Get("a", GVEC_SVG_PROP_HEIGHT).number, 4.0);
  EXPECT_EQ(s.Get("b", GVEC_SVG_PROP_WIDTH).number, 3.0);  /* markerWidth is 3 by default */
  EXPECT_EQ(s.Get("c", GVEC_SVG_PROP_WIDTH).number, 2.0);  /* the viewBox, 4 by 4 */
}

TEST(SvgStyleUserAgent, TheSheetOfSvg2IsApplied) {
  Styled s(Svg("", "<defs id=\"d\" style=\"display: block\"><rect id=\"in\"/></defs><symbol id=\"sym\"><rect id=\"sr\"/></symbol>"
                   "<svg id=\"nested\"><rect id=\"nr\"/></svg><marker id=\"m\"/><pattern id=\"p\"/><image id=\"i\"/><rect id=\"r\"/>"
                   "<a id=\"a\" href=\"#r\"><rect id=\"ar\"/></a><a id=\"b\"/><use id=\"u\" href=\"#sym\"/>"
                   "<text id=\"pre\" xml:space=\"preserve\">x</text><text id=\"collapse\">x</text><linearGradient id=\"lg\"/><title id=\"t\"/>"));
  auto keyword = [&](GVEC_Svg_Style_Context * context, const GVEC_Svg_Instance * instance, GVEC_Svg_Property property) {
    GVEC_Svg_Style_Value v;
    EXPECT_EQ(gvec_svg_get(context, instance, property, &v), GVEC_OK);
    return std::string(v.keyword ? v.keyword : "");
  };
  /* Never rendered, whatever the author says; the children are not affected. */
  EXPECT_EQ(keyword(s.context, s.Of("d"), GVEC_SVG_PROP_DISPLAY), "none");
  EXPECT_EQ(keyword(s.context, s.Of("in"), GVEC_SVG_PROP_DISPLAY), "inline");
  EXPECT_EQ(keyword(s.context, s.Of("m"), GVEC_SVG_PROP_DISPLAY), "none");
  EXPECT_EQ(keyword(s.context, s.Of("p"), GVEC_SVG_PROP_DISPLAY), "none");
  EXPECT_EQ(keyword(s.context, s.Of("lg"), GVEC_SVG_PROP_DISPLAY), "none");
  EXPECT_EQ(keyword(s.context, s.Of("t"), GVEC_SVG_PROP_DISPLAY), "none");
  EXPECT_EQ(keyword(s.context, s.Of("r"), GVEC_SVG_PROP_DISPLAY), "inline");
  /* A symbol is drawn when a use instantiates it. */
  EXPECT_EQ(keyword(s.context, s.Of("sym"), GVEC_SVG_PROP_DISPLAY), "none");
  const GVEC_Svg_Instance * clone = nullptr;
  ASSERT_EQ(gvec_svg_instance_use(s.context, s.Of("u"), &clone), GVEC_OK);
  EXPECT_EQ(keyword(s.context, clone, GVEC_SVG_PROP_DISPLAY), "inline");
  GVEC_Svg_Style_Specified spec;
  ASSERT_EQ(gvec_svg_get_specified(s.context, s.Of("d"), GVEC_SVG_PROP_DISPLAY, &spec), GVEC_OK);
  EXPECT_EQ(spec.source, GVEC_SVG_STYLE_FROM_USER_AGENT);
  EXPECT_TRUE(spec.important);
  /* They clip, except the root. */
  EXPECT_EQ(keyword(s.context, s.Of("nested"), GVEC_SVG_PROP_OVERFLOW), "hidden");
  EXPECT_EQ(keyword(s.context, s.Of("sym"), GVEC_SVG_PROP_OVERFLOW), "hidden");
  EXPECT_EQ(keyword(s.context, s.Of("m"), GVEC_SVG_PROP_OVERFLOW), "hidden");
  EXPECT_EQ(keyword(s.context, s.Of("p"), GVEC_SVG_PROP_OVERFLOW), "hidden");
  EXPECT_EQ(keyword(s.context, s.Of("i"), GVEC_SVG_PROP_OVERFLOW), "hidden");
  EXPECT_EQ(keyword(s.context, gvec_svg_instance_root(s.context), GVEC_SVG_PROP_OVERFLOW), "visible");
  EXPECT_EQ(keyword(s.context, s.Of("r"), GVEC_SVG_PROP_OVERFLOW), "visible");
  /* Elements turn about 0 0; the root svg about its center. */
  EXPECT_EQ(keyword(s.context, s.Of("r"), GVEC_SVG_PROP_TRANSFORM_ORIGIN), "0px 0px");
  EXPECT_EQ(keyword(s.context, s.Of("nested"), GVEC_SVG_PROP_TRANSFORM_ORIGIN), "0px 0px");
  EXPECT_EQ(keyword(s.context, gvec_svg_instance_root(s.context), GVEC_SVG_PROP_TRANSFORM_ORIGIN), "50% 50%");
  /* A link is a pointer; xml:space preserves. */
  EXPECT_EQ(keyword(s.context, s.Of("a"), GVEC_SVG_PROP_CURSOR), "pointer");
  EXPECT_EQ(keyword(s.context, s.Of("ar"), GVEC_SVG_PROP_CURSOR), "pointer");  /* inherited */
  EXPECT_EQ(keyword(s.context, s.Of("b"), GVEC_SVG_PROP_CURSOR), "auto");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
