/**
 * @file
 *
 * The typed SVG document: kinds, attribute classes and values, text, ids, references, cycles, expansion,
 * conditional processing, keys, limits, the dump and the failing-allocator sweep (svg.h).
 */

#include "test_helpers.h"

#include <cmath>
#include <cstring>
#include <set>
#include <string>

#include <ghoti.io/cutil/string.h>

using namespace gvec_test;

namespace {

const char * kHead = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" ";

std::string Svg(const std::string & attrs, const std::string & body) {
  return std::string(kHead) + attrs + ">" + body + "</svg>";
}

struct Doc {
  Loaded loaded;
  const GVEC_Svg_Document * svg = nullptr;

  explicit Doc(const std::string & text, const GVEC_Limits * limits = nullptr,
      std::vector<const char *> languages = {}, const GVEC_Allocator * allocator = nullptr) {
    GVEC_Stream * stream = nullptr;
    EXPECT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
    GVEC_Load_Options options;
    std::memset(&options, 0, sizeof options);
    options.limits = limits;
    options.allocator = allocator;
    options.languages = languages.empty() ? nullptr : languages.data();
    options.language_count = languages.size();
    loaded.result = gvec_doc_load(stream, &options, &loaded.diagnostics, &loaded.doc);
    gvec_stream_destroy(stream);
    if (loaded.result == GVEC_OK) {
      svg = gvec_svg_document(loaded.doc);
    }
  }

  const GVEC_Svg_Element * Root() const { return gvec_svg_root(svg); }

  /** The first element with this id. */
  const GVEC_Svg_Element * Id(const char * id) const { return gvec_svg_find_by_id(svg, id); }

  /** The nth child element of `e` (0-based). */
  static const GVEC_Svg_Element * Child(const GVEC_Svg_Element * e, size_t n) {
    const GVEC_Svg_Element * c = gvec_svg_element_first_child(e);
    while (c && n--) {
      c = gvec_svg_element_next_sibling(c);
    }
    return c;
  }

  std::string Dump() const {
    GVEC_Stream * out = nullptr;
    EXPECT_EQ(gvec_stream_create_memory_output(&out), GVEC_OK);
    EXPECT_EQ(gvec_doc_dump(loaded.doc, out), GVEC_OK);
    const void * data = nullptr;
    size_t size = 0;
    gvec_stream_output_buffer(out, &data, &size);
    std::string s((const char *) data, size);
    gvec_stream_destroy(out);
    return s;
  }
};

const GVEC_Svg_Attribute * Attr(const GVEC_Svg_Element * e, const char * name) {
  return gvec_svg_element_find_attribute(e, name);
}

}  // namespace

TEST(Document, EverySvgElementHasItsKindAndTheRestAreUnknownOrForeign) {
  Doc d(Svg("", "<g><rect/><circle/><ellipse/><line/><path/><polygon/><polyline/><use/><symbol/><defs/><switch/><text>t</text>"
                "<linearGradient/><radialGradient/><stop/><clipPath/><mask/><pattern/><marker/><image/><a/><title/><desc/>"
                "<metadata/><style/><script/><filter/><feGaussianBlur/><foreignObject/><view/><textPath/><tspan/>"
                "<animate/><set/><blink/><x:y xmlns:x=\"urn:x\"/></g>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  const GVEC_Svg_Element * g = Doc::Child(d.Root(), 0);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(gvec_svg_element_kind(d.Root()), GVEC_SVG_KIND_SVG);
  EXPECT_EQ(gvec_svg_element_kind(g), GVEC_SVG_KIND_G);
  const GVEC_Svg_Kind want[] = {GVEC_SVG_KIND_RECT, GVEC_SVG_KIND_CIRCLE, GVEC_SVG_KIND_ELLIPSE, GVEC_SVG_KIND_LINE,
      GVEC_SVG_KIND_PATH, GVEC_SVG_KIND_POLYGON, GVEC_SVG_KIND_POLYLINE, GVEC_SVG_KIND_USE, GVEC_SVG_KIND_SYMBOL,
      GVEC_SVG_KIND_DEFS, GVEC_SVG_KIND_SWITCH, GVEC_SVG_KIND_TEXT, GVEC_SVG_KIND_LINEAR_GRADIENT,
      GVEC_SVG_KIND_RADIAL_GRADIENT, GVEC_SVG_KIND_STOP, GVEC_SVG_KIND_CLIP_PATH, GVEC_SVG_KIND_MASK,
      GVEC_SVG_KIND_PATTERN, GVEC_SVG_KIND_MARKER, GVEC_SVG_KIND_IMAGE, GVEC_SVG_KIND_A, GVEC_SVG_KIND_TITLE,
      GVEC_SVG_KIND_DESC, GVEC_SVG_KIND_METADATA, GVEC_SVG_KIND_STYLE, GVEC_SVG_KIND_SCRIPT, GVEC_SVG_KIND_FILTER,
      GVEC_SVG_KIND_FE_GAUSSIAN_BLUR, GVEC_SVG_KIND_FOREIGN_OBJECT, GVEC_SVG_KIND_VIEW, GVEC_SVG_KIND_TEXT_PATH,
      GVEC_SVG_KIND_TSPAN, GVEC_SVG_KIND_ANIMATE, GVEC_SVG_KIND_SET, GVEC_SVG_KIND_UNKNOWN, GVEC_SVG_KIND_FOREIGN};
  size_t i = 0;
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(g); c; c = gvec_svg_element_next_sibling(c), i++) {
    ASSERT_LT(i, sizeof want / sizeof want[0]);
    EXPECT_EQ(gvec_svg_element_kind(c), want[i]) << i << " " << gvec_svg_element_name(c);
  }
  EXPECT_EQ(i, sizeof want / sizeof want[0]);
  const GVEC_Svg_Element * blink = Doc::Child(g, i - 2);
  EXPECT_STREQ(gvec_svg_element_name(blink), "blink");
  EXPECT_STREQ(gvec_svg_element_namespace(blink), "http://www.w3.org/2000/svg");
  const GVEC_Svg_Element * foreign = Doc::Child(g, i - 1);
  EXPECT_STREQ(gvec_svg_element_name(foreign), "y");
  EXPECT_STREQ(gvec_svg_element_namespace(foreign), "urn:x");
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_UNKNOWN_ELEMENT), 1u);
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_ANIMATION_NOT_APPLIED), 1u) << "once per document";
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_SCRIPT_IGNORED), 1u) << "a script element";
  EXPECT_EQ(gvec_svg_element_count(d.svg), (size_t) 2 + sizeof want / sizeof want[0]);
}

TEST(Document, EveryAttributeIsKeptInOneOfSixClasses) {
  Doc d(Svg("version=\"1.1\" data-x=\"1\" foo:bar=\"x\" xmlns:foo=\"urn:foo\" width=\"100\"",
      "<line id=\"l\" x1=\"1\" y2=\"2em\" fill=\"red\" style=\"stroke:blue\" onclick=\"alert(1)\" mystery=\"1\" "
      "aria-label=\"x\" begin=\"0s\" xml:space=\"preserve\" systemLanguage=\"en\" pathLength=\"-1\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  const GVEC_Svg_Element * root = d.Root();
  EXPECT_EQ(gvec_svg_attribute_class(Attr(root, "version")), GVEC_SVG_ATTR_RAW) << "an SVG 1.1 attribute SVG 2 removed";
  EXPECT_EQ(gvec_svg_attribute_class(Attr(root, "data-x")), GVEC_SVG_ATTR_RAW);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(root, "width")), GVEC_SVG_ATTR_PROPERTY) << "a geometry presentation attribute of svg";
  EXPECT_TRUE(gvec_svg_attribute_is_presentation(Attr(root, "width")));
  EXPECT_EQ(Attr(root, "xmlns:foo"), nullptr) << "a namespace declaration is not an attribute";
  EXPECT_EQ(gvec_svg_element_attribute_count(root), (size_t) 4) << "version, data-x, foo:bar, width";
  const GVEC_Svg_Attribute * foreign = gvec_svg_element_attribute_at(root, 2);
  EXPECT_EQ(gvec_svg_attribute_class(foreign), GVEC_SVG_ATTR_UNKNOWN);
  EXPECT_STREQ(gvec_svg_attribute_namespace(foreign), "urn:foo");
  const GVEC_Svg_Element * line = d.Id("l");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "x1")), GVEC_SVG_ATTR_TYPED);
  EXPECT_EQ(gvec_svg_attribute_value_kind(Attr(line, "y2")), GVEC_SVG_VALUE_LENGTH);
  GVEC_Svg_Length len;
  ASSERT_TRUE(gvec_svg_attribute_length(Attr(line, "y2"), &len));
  EXPECT_EQ(len.value, 2.0);
  EXPECT_EQ(len.unit, GVEC_SVG_UNIT_EM);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "fill")), GVEC_SVG_ATTR_PROPERTY);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "style")), GVEC_SVG_ATTR_PROPERTY);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "onclick")), GVEC_SVG_ATTR_SCRIPT);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "mystery")), GVEC_SVG_ATTR_UNKNOWN);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "aria-label")), GVEC_SVG_ATTR_RAW);
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "begin")), GVEC_SVG_ATTR_UNKNOWN) << "begin is a timing attribute of animation elements only";
  EXPECT_EQ(gvec_svg_attribute_class(Attr(line, "xml:space")), GVEC_SVG_ATTR_TYPED);
  size_t index;
  const char * word;
  ASSERT_TRUE(gvec_svg_attribute_keyword(Attr(line, "xml:space"), &index, &word));
  EXPECT_STREQ(word, "preserve");
  EXPECT_EQ(gvec_svg_attribute_value_kind(Attr(line, "systemLanguage")), GVEC_SVG_VALUE_LANGUAGES);
  const GVEC_Svg_Attribute * neg = Attr(line, "pathLength");
  EXPECT_EQ(gvec_svg_attribute_class(neg), GVEC_SVG_ATTR_INVALID);
  EXPECT_EQ(gvec_svg_attribute_value_kind(neg), GVEC_SVG_VALUE_NONE);
  EXPECT_NE(gvec_svg_attribute_error_reason(neg), nullptr);
  double number;
  EXPECT_FALSE(gvec_svg_attribute_number(neg, &number));
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_UNKNOWN_ATTRIBUTE), 2u) << "once per name: mystery and begin";
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_SCRIPT_IGNORED), 1u);
  const GVEC_Diagnostic * invalid = d.loaded.First(GVEC_DIAG_ATTRIBUTE_INVALID);
  ASSERT_NE(invalid, nullptr);
  EXPECT_STREQ(invalid->element, "line");
  EXPECT_EQ(invalid->severity, GVEC_DIAG_WARNING);
  EXPECT_EQ(invalid->line, (size_t) 1);
  EXPECT_GT(invalid->column, (size_t) 1);
}

TEST(Document, RepeatedUnknownNamesAreNotedOnce) {
  Doc d(Svg("", "<rect zz=\"1\"/><rect zz=\"2\"/><circle zz=\"3\" yy=\"1\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_UNKNOWN_ATTRIBUTE), 2u);
}

TEST(Document, TypedValuesAreReadThroughTheirGetters) {
  Doc d(Svg("viewBox=\"0 0 10 20\" preserveAspectRatio=\"xMinYMax slice\"",
      "<polygon id=\"p\" points=\"0,0 10,0 10,10\"/>"
      "<path id=\"d\" d=\"M1 2 L3 4 Z\"/>"
      "<marker id=\"m\" orient=\"auto-start-reverse\" refX=\"center\" markerWidth=\"2\" markerUnits=\"strokeWidth\"/>"
      "<stop id=\"s\" offset=\"50%\"/>"
      "<linearGradient id=\"g\" gradientUnits=\"objectBoundingBox\" spreadMethod=\"reflect\"/>"
      "<use id=\"u\" href=\"#p\"/><a id=\"x\" tabindex=\"3\" href=\"http://example.org/\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  GVEC_Svg_View_Box vb;
  ASSERT_TRUE(gvec_svg_attribute_view_box(Attr(d.Root(), "viewBox"), &vb));
  EXPECT_EQ(vb.height, 20.0);
  GVEC_Svg_Preserve_Aspect_Ratio par;
  ASSERT_TRUE(gvec_svg_attribute_preserve_aspect_ratio(Attr(d.Root(), "preserveAspectRatio"), &par));
  EXPECT_EQ(par.align, GVEC_SVG_ALIGN_X_MIN_Y_MAX);
  EXPECT_EQ(par.slice, 1);
  const double * pts;
  size_t n;
  ASSERT_TRUE(gvec_svg_attribute_numbers(Attr(d.Id("p"), "points"), &pts, &n));
  EXPECT_EQ(n, (size_t) 6);
  const GVEC_Svg_Path_Command * cmds;
  const GVEC_Svg_Path_Segment * segs;
  size_t nc, ns;
  ASSERT_TRUE(gvec_svg_attribute_path(Attr(d.Id("d"), "d"), &cmds, &nc, &segs, &ns));
  EXPECT_EQ(nc, (size_t) 3);
  EXPECT_EQ(segs[1].x, 3.0);
  EXPECT_TRUE(gvec_svg_attribute_is_presentation(Attr(d.Id("d"), "d"))) << "d is a presentation attribute of path, read as path data";
  GVEC_Svg_Orient o;
  ASSERT_TRUE(gvec_svg_attribute_orient(Attr(d.Id("m"), "orient"), &o));
  EXPECT_EQ(o.mode, GVEC_SVG_ORIENT_AUTO_START_REVERSE);
  GVEC_Svg_Length l;
  ASSERT_TRUE(gvec_svg_attribute_length(Attr(d.Id("m"), "refX"), &l));
  EXPECT_EQ(l.unit, GVEC_SVG_UNIT_PERCENT);
  EXPECT_EQ(l.value, 50.0);
  double off;
  ASSERT_TRUE(gvec_svg_attribute_number(Attr(d.Id("s"), "offset"), &off));
  EXPECT_EQ(off, 0.5);
  ASSERT_TRUE(gvec_svg_attribute_keyword(Attr(d.Id("g"), "spreadMethod"), &n, (const char **) &pts));
  long tab;
  ASSERT_TRUE(gvec_svg_attribute_integer(Attr(d.Id("x"), "tabindex"), &tab));
  EXPECT_EQ(tab, 3);
  GVEC_Svg_Reference ref;
  ASSERT_TRUE(gvec_svg_attribute_reference(Attr(d.Id("u"), "href"), &ref));
  EXPECT_EQ(ref.kind, GVEC_SVG_REF_INTERNAL);
  ASSERT_TRUE(gvec_svg_attribute_reference(Attr(d.Id("x"), "href"), &ref));
  EXPECT_EQ(ref.kind, GVEC_SVG_REF_EXTERNAL);
  EXPECT_FALSE(gvec_svg_attribute_number(Attr(d.Id("x"), "href"), &off)) << "a getter of another kind refuses";
}

TEST(Document, APartlyInvalidPathKeepsItsValidPrefix) {
  Doc d(Svg("", "<path id=\"p\" d=\"M 0 0 L 10 10 X 5\"/><path id=\"q\" d=\"L 1 1\"/><polyline id=\"r\" points=\"1 2 3\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  const GVEC_Svg_Attribute * p = Attr(d.Id("p"), "d");
  EXPECT_EQ(gvec_svg_attribute_class(p), GVEC_SVG_ATTR_TYPED);
  EXPECT_TRUE(gvec_svg_attribute_is_partial(p));
  EXPECT_EQ(gvec_svg_attribute_error_offset(p), (size_t) 14);
  const GVEC_Svg_Path_Command * cmds;
  const GVEC_Svg_Path_Segment * segs;
  size_t nc, ns;
  ASSERT_TRUE(gvec_svg_attribute_path(p, &cmds, &nc, &segs, &ns));
  EXPECT_EQ(ns, (size_t) 2);
  const GVEC_Svg_Attribute * q = Attr(d.Id("q"), "d");
  EXPECT_EQ(gvec_svg_attribute_class(q), GVEC_SVG_ATTR_INVALID) << "no complete segment: nothing to keep";
  const GVEC_Svg_Attribute * r = Attr(d.Id("r"), "points");
  EXPECT_TRUE(gvec_svg_attribute_is_partial(r));
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_ATTRIBUTE_INVALID), 3u);
}

TEST(Document, CharacterDataIsKeptWhereItIsReadAndInOrder) {
  Doc d(Svg("", "<g>\n  <title>The <b>title</b></title>\n  <text>one<tspan>two</tspan>three</text>  \n</g>stray"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  const GVEC_Svg_Element * g = Doc::Child(d.Root(), 0);
  // White space between the elements of a g is dropped; the text under the svg that is not white space is kept.
  size_t texts = 0;
  for (const GVEC_Svg_Node * n = gvec_svg_element_first_node(g); n; n = gvec_svg_node_next(n)) {
    texts += gvec_svg_node_element(n) ? 0 : 1;
  }
  EXPECT_EQ(texts, 0u);
  const GVEC_Svg_Node * last = gvec_svg_element_first_node(d.Root());
  while (gvec_svg_node_next(last)) {
    last = gvec_svg_node_next(last);
  }
  size_t length = 0;
  ASSERT_EQ(gvec_svg_node_element(last), nullptr);
  EXPECT_STREQ(gvec_svg_node_text(last, &length), "stray");
  EXPECT_EQ(length, (size_t) 5);
  const GVEC_Svg_Element * text = Doc::Child(g, 1);
  ASSERT_EQ(gvec_svg_element_kind(text), GVEC_SVG_KIND_TEXT);
  std::string seq;
  for (const GVEC_Svg_Node * n = gvec_svg_element_first_node(text); n; n = gvec_svg_node_next(n)) {
    if (gvec_svg_node_element(n)) {
      seq += "[" + std::string(gvec_svg_element_name(gvec_svg_node_element(n))) + "]";
    } else {
      seq += gvec_svg_node_text(n, nullptr);
    }
  }
  EXPECT_EQ(seq, "one[tspan]three") << "text nodes sit between the elements they were written between";
  const GVEC_Svg_Element * title = Doc::Child(g, 0);
  const GVEC_Svg_Node * first = gvec_svg_element_first_node(title);
  EXPECT_STREQ(gvec_svg_node_text(first, nullptr), "The ");
  const GVEC_Svg_Element * b = Doc::Child(title, 0);
  EXPECT_EQ(gvec_svg_element_kind(b), GVEC_SVG_KIND_UNKNOWN);
}

TEST(Document, DuplicateIdsKeepTheFirstAndSayWhereTheLaterOneIs) {
  Doc d(Svg("", "<rect id=\"a\"/>\n<circle id=\"a\"/><g id=\"b\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_EQ(gvec_svg_element_kind(d.Id("a")), GVEC_SVG_KIND_RECT);
  EXPECT_EQ(gvec_svg_element_kind(d.Id("b")), GVEC_SVG_KIND_G);
  EXPECT_EQ(d.Id("missing"), nullptr);
  EXPECT_EQ(d.Id(nullptr), nullptr);
  const GVEC_Diagnostic * dup = d.loaded.First(GVEC_DIAG_DUPLICATE_ID);
  ASSERT_NE(dup, nullptr);
  EXPECT_EQ(dup->line, (size_t) 2);
  EXPECT_STREQ(dup->element, "circle");
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_DUPLICATE_ID), 1u);
}

TEST(Document, AnEmptyOrSpacedIdIsInvalid) {
  Doc d(Svg("", "<rect id=\"\"/><rect id=\"a b\"/><rect id=\"ok\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_ATTRIBUTE_INVALID), 2u);
  EXPECT_NE(d.Id("ok"), nullptr);
  EXPECT_EQ(gvec_svg_element_id(Doc::Child(d.Root(), 0)), nullptr);
}

TEST(References, HrefResolvesToTheElementOrIsMissingAndSaidSo) {
  Doc d(Svg("", "<defs><rect id=\"r\"/></defs><use id=\"u1\" href=\"#r\"/><use id=\"u2\" xlink:href=\"#r\"/>"
                "<use id=\"u3\" href=\"#nowhere\"/><use id=\"u4\" href=\"other.svg#x\"/><a id=\"a\" href=\"#nowhere\"/>"
                "<use id=\"u5\" href=\"#r\" xlink:href=\"#nowhere\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  const GVEC_Svg_Element * r = d.Id("r");
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("u1")), r);
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("u2")), r) << "xlink:href when there is no href";
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("u3")), nullptr);
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("u4")), nullptr);
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("u5")), r) << "href wins over xlink:href";
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_REFERENCE_MISSING), 1u) << "a missing target of a use; a link's is not an error";
  const GVEC_Diagnostic * missing = d.loaded.First(GVEC_DIAG_REFERENCE_MISSING);
  EXPECT_STREQ(missing->element, "use");
}

TEST(References, ACycleOfUseElementsIsCutAndEveryMemberIsMarkedOnce) {
  Doc d(Svg("", "<use id=\"self\" href=\"#self\"/>"
                "<g id=\"anc\"><use id=\"inner\" href=\"#anc\"/></g>"
                "<g id=\"a\"><use id=\"ua\" href=\"#b\"/></g><g id=\"b\"><use id=\"ub\" href=\"#a\"/></g>"
                "<rect id=\"ok\"/><use id=\"fine\" href=\"#ok\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  for (const char * id : {"self", "inner", "ua", "ub"}) {
    EXPECT_TRUE(gvec_svg_element_on_cycle(d.Id(id))) << id;
    EXPECT_EQ(gvec_svg_element_href_target(d.Id(id)), nullptr) << id;
    EXPECT_EQ(gvec_svg_element_expansion(d.Id(id)), (uint64_t) 0) << id;
  }
  EXPECT_FALSE(gvec_svg_element_on_cycle(d.Id("fine")));
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("fine")), d.Id("ok"));
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_REFERENCE_CYCLE), 4u) << "once per member";
}

TEST(References, AGradientHrefChainThatLoopsMarksEveryMember) {
  Doc d(Svg("", "<linearGradient id=\"g1\" href=\"#g2\"/><radialGradient id=\"g2\" href=\"#g3\"/>"
                "<linearGradient id=\"g3\" href=\"#g1\"/><linearGradient id=\"h1\" href=\"#h2\"/><linearGradient id=\"h2\"/>"
                "<pattern id=\"p\" href=\"#p\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  for (const char * id : {"g1", "g2", "g3", "p"}) {
    EXPECT_TRUE(gvec_svg_element_on_cycle(d.Id(id))) << id;
    EXPECT_EQ(gvec_svg_element_href_target(d.Id(id)), nullptr) << id;
  }
  EXPECT_FALSE(gvec_svg_element_on_cycle(d.Id("h1")));
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("h1")), d.Id("h2"));
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_REFERENCE_CYCLE), 4u);
}

/** The pyramid: a0 holds a rect; a_k holds two uses of a_{k-1}. Returns the document text. */
static std::string Pyramid(int levels) {
  std::string body = "<defs><g id=\"a0\"><rect/></g>";
  for (int k = 1; k <= levels; k++) {
    body += "<g id=\"a" + std::to_string(k) + "\"><use id=\"u" + std::to_string(k) + "a\" href=\"#a" + std::to_string(k - 1) +
        "\"/><use id=\"u" + std::to_string(k) + "b\" href=\"#a" + std::to_string(k - 1) + "\"/></g>";
  }
  body += "</defs><use id=\"top\" href=\"#a" + std::to_string(levels) + "\"/>";
  return Svg("", body);
}

TEST(Expansion, TheCountOfInstantiatedElementsIsComputedWithoutExpanding) {
  // E(use of a_k) = 5 * 2^k - 3: a0 is a g and a rect (2 elements); a_k is a g, two uses, and two copies of a_{k-1}'s uses.
  Doc d(Pyramid(40));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_EQ(gvec_svg_element_expansion(d.Id("u1a")), (uint64_t) 2);
  EXPECT_EQ(gvec_svg_element_expansion(d.Id("u2a")), (uint64_t) 7);
  EXPECT_EQ(gvec_svg_element_expansion(d.Id("u3a")), (uint64_t) 17);
  EXPECT_EQ(gvec_svg_element_expansion(d.Id("u10b")), (uint64_t) 5 * 512 - 3);
  EXPECT_EQ(gvec_svg_element_expansion(d.Id("top")), (uint64_t) 5 * ((uint64_t) 1 << 40) - 3);
}

TEST(Expansion, AUseOverTheBoundIsMarkedAndNotInstantiatedAndTheBoundItselfIsAllowed) {
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_reference_expansion = 17;
  Doc d(Pyramid(5), &limits);
  ASSERT_EQ(d.loaded.result, GVEC_OK) << "an expansion past the bound does not refuse the document";
  EXPECT_EQ(gvec_svg_element_expansion(d.Id("u3a")), (uint64_t) 17);
  EXPECT_FALSE(gvec_svg_element_expansion_limited(d.Id("u3a"))) << "exactly the bound is not over it";
  EXPECT_NE(gvec_svg_element_href_target(d.Id("u3a")), nullptr);
  EXPECT_TRUE(gvec_svg_element_expansion_limited(d.Id("u4a"))) << "37 > 17";
  EXPECT_EQ(gvec_svg_element_href_target(d.Id("u4a")), nullptr);
  EXPECT_TRUE(gvec_svg_element_expansion_limited(d.Id("top")));
  const GVEC_Diagnostic * diag = d.loaded.First(GVEC_DIAG_EXPANSION_LIMIT);
  ASSERT_NE(diag, nullptr);
  EXPECT_EQ(diag->detail, (uint32_t) GVEC_LIMIT_REFERENCE_EXPANSION);
}

TEST(Expansion, UsesThatPointAtEnormousRangesAreCountedWithoutVisitingEveryPair) {
  // 20,000 uses inside one group, each of a leaf; and 20,000 uses outside it, each of the whole group. Visited pair
  // by pair the second set alone is 4e8 steps; the count must be exact and the load quick.
  const int n = 20000;
  std::string body = "<defs><rect id=\"leaf\"/><g id=\"big\">";
  for (int i = 0; i < n; i++) {
    body += "<use href=\"#leaf\"/>";
  }
  body += "</g></defs>";
  for (int i = 0; i < n; i++) {
    body += "<use href=\"#big\"/>";
  }
  Doc d(Svg("", body));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  const GVEC_Svg_Element * defs = Doc::Child(d.Root(), 0);
  const GVEC_Svg_Element * big = Doc::Child(defs, 1);
  EXPECT_EQ(gvec_svg_element_expansion(gvec_svg_element_first_child(big)), (uint64_t) 1);
  const GVEC_Svg_Element * outer = Doc::Child(d.Root(), 1);
  EXPECT_EQ(gvec_svg_element_expansion(outer), (uint64_t) n + 1 + n) << "the group, its uses, and what each of them instantiates";
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_REFERENCE_CYCLE), 0u);
  // The same with every use inside the group it points at: each is a cycle of one.
  std::string loop = "<g id=\"all\">";
  for (int i = 0; i < n; i++) {
    loop += "<use href=\"#all\"/>";
  }
  loop += "</g>";
  Doc e(Svg("", loop));
  ASSERT_EQ(e.loaded.result, GVEC_OK);
  size_t marked = 0;
  for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(Doc::Child(e.Root(), 0)); c; c = gvec_svg_element_next_sibling(c)) {
    marked += gvec_svg_element_on_cycle(c) ? 1 : 0;
  }
  EXPECT_EQ(marked, (size_t) n);
  EXPECT_EQ(e.loaded.Count(GVEC_DIAG_REFERENCE_CYCLE) + e.loaded.diagnostics.dropped, (size_t) n) << "past the cap they are counted, not lost";
}

TEST(References, ALongerCycleIsFoundEvenWhenASmallerOneHidesInsideIt) {
  // use1 instantiates i3, which holds use2; use2 instantiates i2, which holds use1 and use2 itself.
  Doc d(Svg("", "<text id=\"i2\"><use id=\"u1\" href=\"#i3\"/><text id=\"i3\"><use id=\"u2\" href=\"#i2\"/></text></text>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_TRUE(gvec_svg_element_on_cycle(d.Id("u1")));
  EXPECT_TRUE(gvec_svg_element_on_cycle(d.Id("u2")));
  EXPECT_EQ(d.loaded.Count(GVEC_DIAG_REFERENCE_CYCLE), 2u);
}

TEST(References, AUseThatLeadsIntoACycleWithoutBeingOnItIsNotOnIt) {
  Doc d(Svg("", "<g id=\"a\"><use id=\"ua\" href=\"#b\"/></g><g id=\"b\"><use id=\"ub\" href=\"#c\"/></g>"
                "<g id=\"c\"><use id=\"uc\" href=\"#b\"/></g><use id=\"start\" href=\"#a\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_FALSE(gvec_svg_element_on_cycle(d.Id("start")));
  EXPECT_FALSE(gvec_svg_element_on_cycle(d.Id("ua")));
  EXPECT_TRUE(gvec_svg_element_on_cycle(d.Id("ub")));
  EXPECT_TRUE(gvec_svg_element_on_cycle(d.Id("uc")));
  EXPECT_NE(gvec_svg_element_href_target(d.Id("ua")), nullptr);
  EXPECT_EQ(gvec_svg_element_expansion(d.Id("ua")), (uint64_t) 2 + 0) << "g b, its use ub (cut, so it adds nothing)";
}

TEST(Expansion, TheDocumentTotalIsBoundedToo) {
  std::string body = "<defs><g id=\"a\"><rect/><rect/><rect/></g></defs>";  // 4 elements per use, each use of a counts 4
  for (int i = 0; i < 6; i++) {
    body += "<use id=\"u" + std::to_string(i) + "\" href=\"#a\"/>";
  }
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_reference_expansion = 10;
  Doc d(Svg("", body), &limits);
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_FALSE(gvec_svg_element_expansion_limited(d.Id("u0")));
  EXPECT_FALSE(gvec_svg_element_expansion_limited(d.Id("u1"))) << "8 <= 10";
  EXPECT_TRUE(gvec_svg_element_expansion_limited(d.Id("u2"))) << "12 > 10";
  EXPECT_TRUE(gvec_svg_element_expansion_limited(d.Id("u5")));
}

TEST(Conditional, SystemLanguageMatchesByCaseInsensitiveEqualityOrPrefixBeforeAHyphen) {
  const std::string body =
      "<g id=\"a\" systemLanguage=\"en-US\"/><g id=\"b\" systemLanguage=\"fr, EN\"/><g id=\"c\" systemLanguage=\"\"/>"
      "<g id=\"d\" systemLanguage=\"english\"/><g id=\"e\"/><g id=\"f\" systemLanguage=\"de-CH-1996\"/>";
  {
    Doc d(Svg("", body));  // default languages {"en"}
    ASSERT_EQ(d.loaded.result, GVEC_OK);
    EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("a"))) << "the user tag en is a prefix of en-US followed by -";
    EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("b"))) << "EN equals en, ignoring case";
    EXPECT_TRUE(gvec_svg_element_conditional_false(d.Id("c"))) << "an empty value evaluates to false";
    EXPECT_TRUE(gvec_svg_element_conditional_false(d.Id("d"))) << "en is a prefix of english but no - follows";
    EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("e")));
    EXPECT_TRUE(gvec_svg_element_conditional_false(d.Id("f")));
  }
  {
    Doc d(Svg("", body), nullptr, {"de", "fr"});
    EXPECT_TRUE(gvec_svg_element_conditional_false(d.Id("a")));
    EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("b"))) << "fr";
    EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("f"))) << "de is a prefix of de-CH-1996";
  }
  {
    Doc d(Svg("", body), nullptr, {"en-US"});
    EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("a")));
    EXPECT_TRUE(gvec_svg_element_conditional_false(d.Id("b"))) << "en-US is not a prefix of en";
  }
}

TEST(Conditional, RequiredExtensionsIsFalseForAnyListAndTrueWhenAbsent) {
  Doc d(Svg("", "<g id=\"a\" requiredExtensions=\"http://example.org/x\"/><g id=\"b\" requiredExtensions=\"\"/><g id=\"c\"/>"
                "<g id=\"d\" requiredFeatures=\"http://www.w3.org/TR/SVG11/feature#Shape\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_TRUE(gvec_svg_element_conditional_false(d.Id("a")));
  EXPECT_TRUE(gvec_svg_element_conditional_false(d.Id("b")));
  EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("c")));
  EXPECT_FALSE(gvec_svg_element_conditional_false(d.Id("d"))) << "requiredFeatures is not in SVG 2: ignored, and true";
  EXPECT_EQ(gvec_svg_attribute_class(Attr(d.Id("d"), "requiredFeatures")), GVEC_SVG_ATTR_RAW);
}

TEST(Conditional, ASwitchSelectsItsFirstTrueRenderableChild) {
  Doc d(Svg("", "<switch id=\"s1\"><animate/><title/><g id=\"x\" systemLanguage=\"fr\"/><g id=\"y\" systemLanguage=\"en\"/><g id=\"z\"/></switch>"
                "<switch id=\"s2\"><g systemLanguage=\"fr\"/><rect systemLanguage=\"de\"/></switch>"
                "<switch id=\"s3\"><foo/><g id=\"third\"/></switch><switch id=\"s4\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  EXPECT_EQ(gvec_svg_element_switch_selected(d.Id("s1")), d.Id("y"));
  EXPECT_EQ(gvec_svg_element_switch_selected(d.Id("s2")), nullptr);
  EXPECT_EQ(gvec_svg_element_switch_selected(d.Id("s3")), d.Id("third")) << "an unknown element is not a candidate";
  EXPECT_EQ(gvec_svg_element_switch_selected(d.Id("s4")), nullptr);
  EXPECT_EQ(gvec_svg_element_switch_selected(d.Id("x")), nullptr);
}

TEST(Keys, TheSameBytesGiveTheSameKeysAndTheyAreDistinct) {
  const std::string text = Svg("", "<g><rect/><rect/><circle/></g><g id=\"named\"><rect/><rect/></g><x:foo xmlns:x=\"urn:x\"/><x:foo xmlns:x=\"urn:x\"/>");
  Doc a(text), b(text);
  ASSERT_EQ(a.loaded.result, GVEC_OK);
  std::set<uint64_t> keys;
  const GVEC_Svg_Element * ea = a.Root();
  std::vector<const GVEC_Svg_Element *> stack{ea};
  size_t count = 0;
  while (!stack.empty()) {
    const GVEC_Svg_Element * e = stack.back();
    stack.pop_back();
    keys.insert(gvec_svg_element_key(e));
    count++;
    for (const GVEC_Svg_Element * c = gvec_svg_element_first_child(e); c; c = gvec_svg_element_next_sibling(c)) {
      stack.push_back(c);
    }
  }
  EXPECT_EQ(keys.size(), count);
  EXPECT_EQ(a.Dump(), b.Dump());
  uint64_t ha, hb;
  ASSERT_EQ(gvec_doc_hash(a.loaded.doc, &ha), GVEC_OK);
  ASSERT_EQ(gvec_doc_hash(b.loaded.doc, &hb), GVEC_OK);
  EXPECT_EQ(ha, hb);
}

TEST(Keys, AnElementWithAnIdKeepsItsKeyWhenSiblingsAreInserted) {
  Doc a(Svg("", "<g><rect id=\"x\"/></g><circle id=\"c\"/><rect/>"));
  Doc b(Svg("", "<ellipse/><g/><g><line/><rect id=\"x\"/></g><circle id=\"c\"/><rect/><rect/>"));
  ASSERT_EQ(a.loaded.result, GVEC_OK);
  EXPECT_EQ(gvec_svg_element_key(a.Id("c")), gvec_svg_element_key(b.Id("c"))) << "same parent, same id";
  // x lives under a g that has no id and became the second g, so its key changes: only id-based chains are stable.
  EXPECT_NE(gvec_svg_element_key(a.Id("x")), gvec_svg_element_key(b.Id("x")));
  Doc c(Svg("", "<g id=\"grp\"><rect id=\"x\"/></g>"));
  Doc e(Svg("", "<ellipse/><g id=\"grp\"><line/><rect id=\"x\"/></g>"));
  EXPECT_EQ(gvec_svg_element_key(c.Id("x")), gvec_svg_element_key(e.Id("x"))) << "an id chain survives edits around it";
  EXPECT_EQ(gvec_svg_element_key(c.Id("grp")), gvec_svg_element_key(e.Id("grp")));
}

TEST(Keys, TwoElementsWithTheKeyInputsAreMadeDistinctDeterministically) {
  // The same id under the same parent: the same key input. The later one is re-keyed, and the dump says so.
  Doc a(Svg("", "<rect id=\"same\"/><rect id=\"same\"/>"));
  Doc b(Svg("", "<rect id=\"same\"/><rect id=\"same\"/>"));
  ASSERT_EQ(a.loaded.result, GVEC_OK);
  const GVEC_Svg_Element * first = Doc::Child(a.Root(), 0);
  const GVEC_Svg_Element * second = Doc::Child(a.Root(), 1);
  EXPECT_NE(gvec_svg_element_key(first), gvec_svg_element_key(second));
  EXPECT_EQ(gvec_svg_element_key(second), gvec_svg_element_key(Doc::Child(b.Root(), 1)));
  EXPECT_NE(a.Dump().find("key-collision"), std::string::npos);
  EXPECT_EQ(a.Dump().find("key-collision"), a.Dump().rfind("key-collision"));
  Doc plain(Svg("", "<rect/><rect/><rect/><g><rect/><rect/></g>"));
  ASSERT_EQ(plain.loaded.result, GVEC_OK);
  EXPECT_EQ(plain.Dump().find("key-collision"), std::string::npos) << "siblings of one kind are told apart by their place, not re-keyed";
}

TEST(Dump, ItIsCanonicalAndIdenticalForTheSameDocumentAndSaysWhatIsInIt) {
  Doc d(Svg("viewBox=\"0 0 1.5 2\"", "<path id=\"p\" d=\"M 1 2 L 3 4\"/><text>hi \xc3\xa9</text><use id=\"u\" href=\"#p\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  std::string dump = d.Dump();
  EXPECT_EQ(dump.rfind("vector-doc 1\nformat svg\ntransport none\nsvg elements 4 text-bytes 5\n", 0), 0u) << dump;
  EXPECT_NE(dump.find("vb:0e0,0e0,15e-1,2e0"), std::string::npos) << dump;
  EXPECT_NE(dump.find("p:M 1e0,2e0;L 3e0,4e0 abs:M 0e0,0e0,1e0,2e0;L 1e0,2e0,3e0,4e0"), std::string::npos) << dump;
  EXPECT_NE(dump.find("t 2 "), std::string::npos);
  EXPECT_NE(dump.find("\\xc3\\xa9"), std::string::npos) << "non-ASCII bytes are escaped";
  EXPECT_NE(dump.find("href="), std::string::npos);
  EXPECT_NE(dump.find("expansion=1"), std::string::npos) << "the use of the path instantiates the path element";
}

TEST(Dump, QuotesBackslashesAndSignsAreSpelledOutSoTheLineCanBeReadBack) {
  Doc d(Svg("", "<line x1=\"-3\" x2=\"2.5px\" class=\"a&quot;b\\c\"/>"));
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  std::string dump = d.Dump();
  EXPECT_NE(dump.find("raw=\"a\\x22b\\x5cc\" value=s:\"a\\x22b\\x5cc\""), std::string::npos) << dump;
  EXPECT_NE(dump.find("value=l:-3e0"), std::string::npos) << dump;
  EXPECT_NE(dump.find("value=l:25e-1px"), std::string::npos) << dump;
}

TEST(Limits, TheTextBoundFailsTheLoadAndNamesTheField) {
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_text_bytes = 10;
  Doc ok(Svg("", "<text>0123456789</text>"), &limits);
  EXPECT_EQ(ok.loaded.result, GVEC_OK) << "exactly the bound";
  Doc over(Svg("", "<text>0123456789A</text>"), &limits);
  EXPECT_EQ(over.loaded.result, GVEC_ERR_LIMIT);
  const GVEC_Diagnostic * d = over.loaded.First(GVEC_DIAG_LIMIT);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->detail, (uint32_t) GVEC_LIMIT_TEXT_BYTES);
}

TEST(Limits, APathOrListOverItsBoundIsALocatedErrorOnTheAttributeNotOnTheDocument) {
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_path_segments = 3;
  limits.max_list_items = 4;
  Doc d(Svg("", "<path id=\"p\" d=\"M0 0 L1 1 L2 2 L3 3 L4 4\"/><path id=\"q\" d=\"M0 0 L1 1 L2 2\"/>"
                "<polyline id=\"l\" points=\"1 2 3 4 5 6\"/><polyline id=\"m\" points=\"1 2 3 4\"/><rect id=\"r\"/>"), &limits);
  ASSERT_EQ(d.loaded.result, GVEC_OK);
  const GVEC_Svg_Path_Command * c;
  const GVEC_Svg_Path_Segment * s;
  size_t nc, ns;
  ASSERT_TRUE(gvec_svg_attribute_path(Attr(d.Id("p"), "d"), &c, &nc, &s, &ns));
  EXPECT_EQ(ns, (size_t) 3);
  EXPECT_TRUE(gvec_svg_attribute_is_partial(Attr(d.Id("p"), "d")));
  ASSERT_TRUE(gvec_svg_attribute_path(Attr(d.Id("q"), "d"), &c, &nc, &s, &ns));
  EXPECT_EQ(ns, (size_t) 3);
  EXPECT_FALSE(gvec_svg_attribute_is_partial(Attr(d.Id("q"), "d")));
  const double * pts;
  size_t n;
  ASSERT_TRUE(gvec_svg_attribute_numbers(Attr(d.Id("l"), "points"), &pts, &n));
  EXPECT_EQ(n, (size_t) 4) << "the first four items";
  EXPECT_TRUE(gvec_svg_attribute_is_partial(Attr(d.Id("l"), "points")));
  EXPECT_FALSE(gvec_svg_attribute_is_partial(Attr(d.Id("m"), "points")));
  EXPECT_NE(d.Id("r"), nullptr) << "the rest of the document is intact";
}

TEST(Document, ASvgzAndAPlainFileGiveTheSameDocument) {
  Loaded plain, gz;
  LoadFixture("minimal.svg", nullptr, plain);
  LoadFixture("minimal.svgz", nullptr, gz);
  ASSERT_EQ(plain.result, GVEC_OK);
  ASSERT_EQ(gz.result, GVEC_OK);
  const GVEC_Svg_Document * a = gvec_svg_document(plain.doc);
  const GVEC_Svg_Document * b = gvec_svg_document(gz.doc);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(gvec_svg_element_key(gvec_svg_root(a)), gvec_svg_element_key(gvec_svg_root(b)));
  EXPECT_EQ(gvec_svg_document(nullptr), nullptr);
}

TEST(Allocation, EveryFailureOfALoadIsOomOrTheLoadThatWouldHaveSucceededAndNothingLeaks) {
  const std::string text = Svg("viewBox=\"0 0 10 10\" width=\"5\"",
      "<defs><g id=\"a\"><rect id=\"r\" x=\"1\" y=\"2\"/></g></defs><use href=\"#a\"/><use href=\"#a\"/>"
      "<path d=\"M0 0 L10 10 C 1 2 3 4 5 6 A 1 1 0 0 1 5 5 Z\"/><polygon points=\"0,0 1,1 2,0\"/>"
      "<linearGradient id=\"g\" href=\"#h\"/><linearGradient id=\"h\" href=\"#g\"/><text>text <tspan>x</tspan></text>"
      "<switch><g systemLanguage=\"en\"/></switch><animate/><rect onclick=\"x\" foo=\"1\" id=\"dup\"/><rect id=\"dup\"/>");
  Doc reference(text);
  ASSERT_EQ(reference.loaded.result, GVEC_OK);
  const std::string want = reference.Dump();
  bool complete = false;
  for (long fail = 1; fail < 100000 && !complete; fail++) {
    Failing allocator;
    allocator.fail_at = fail;
    GVEC_Allocator a = allocator.a;
    {
      Doc d(text, nullptr, {}, &a);
      if (d.loaded.result == GVEC_OK) {
        EXPECT_EQ(d.Dump(), want) << "failure at " << fail << " went unnoticed";
      } else {
        EXPECT_EQ(d.loaded.result, GVEC_ERR_OOM) << fail;
      }
    }
    EXPECT_EQ(allocator.outstanding, 0) << "a leak after a failure at " << fail;
    complete = allocator.attempts < fail;  // fewer allocations than the failure point: the sweep reached them all
  }
  EXPECT_TRUE(complete);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
