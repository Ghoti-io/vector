/**
 * @file
 *
 * The registry (L5), the probe (L7), loading (L8), the XML options (L9), the
 * dump and hash (L10) and the lowering entry points (L11), over the fixtures
 * that tools/gen-test-data.py writes.
 */

#include "test_helpers.h"

#include <string>

#include <ghoti.io/cutil/string.h>
#include <ghoti.io/text/xml.h>

#include "../../src/codec/svg/svg_internal.h"
#include "../../src/core/core_internal.h"
#include "../../src/stream/stream_internal.h"

using namespace gvec_test;

static GVEC_Probe_Result Probe(const std::vector<unsigned char> & bytes, GVEC_Result * result = nullptr) {
  GVEC_Stream * s = nullptr;
  GVEC_Probe_Result p;
  std::memset(&p, 0xff, sizeof(p));
  EXPECT_EQ(gvec_stream_create_memory(bytes.empty() ? "" : (const void *) bytes.data(), bytes.size(), &s), GVEC_OK);
  GVEC_Result r = gvec_probe(s, nullptr, &p);
  if (result) {
    *result = r;
  }
  else {
    EXPECT_EQ(r, GVEC_OK);
  }
  // The probe consumed nothing.
  EXPECT_EQ(gvec_stream_tell(s), (size_t) 0);
  gvec_stream_destroy(s);
  return p;
}

//-----------------------------------------------------------------------------
// The registry
//-----------------------------------------------------------------------------

TEST(Registry, SvgIsRegisteredWithLoadAndLowerAndNoSave) {
  ASSERT_EQ(gvec_codec_count(), (size_t) 1);
  const GVEC_Codec * svg = gvec_codec_by_name("svg");
  ASSERT_NE(svg, nullptr);
  EXPECT_EQ(gvec_codec_by_index(0), svg);
  EXPECT_EQ(gvec_codec_by_index(1), nullptr);
  EXPECT_STREQ(gvec_codec_name(svg), "svg");
  EXPECT_EQ(gvec_codec_capabilities(svg), GVEC_CAP_LOAD | GVEC_CAP_LOWER);
  EXPECT_EQ(gvec_codec_by_name("png"), nullptr);
  EXPECT_EQ(gvec_codec_by_name(nullptr), nullptr);
  EXPECT_EQ(gvec_codec_name(nullptr), nullptr);
  EXPECT_EQ(gvec_codec_capabilities(nullptr), 0u);
}

TEST(Registry, CodecNamesAreUnique) {
  for (size_t i = 0; i < gvec_codec_count(); i++) {
    for (size_t j = i + 1; j < gvec_codec_count(); j++) {
      EXPECT_STRNE(gvec_codec_name(gvec_codec_by_index(i)), gvec_codec_name(gvec_codec_by_index(j)));
    }
    EXPECT_EQ(gvec_codec_by_name(gvec_codec_name(gvec_codec_by_index(i))), gvec_codec_by_index(i));
  }
}

//-----------------------------------------------------------------------------
// The probe (L7)
//-----------------------------------------------------------------------------

TEST(Probe, EverySpellingOfAnSvgRootIsRecognised) {
  for (const char * name : {"minimal.svg", "prefixed.svg", "bom.svg", "utf16le.svg", "utf16be.svg", "utf32le.svg",
           "utf32be.svg", "latin1.svg", "ascii.svg", "prolog.svg", "entities.svg", "external-dtd.svg",
           "external-pe.svg", "external-ge.svg", "minimal.svgz", "two-members.svgz", "header-fields.svgz"}) {
    GVEC_Probe_Result p = Probe(Fixture(name));
    ASSERT_NE(p.format_name, nullptr) << name;
    EXPECT_STREQ(p.format_name, "svg") << name;
    EXPECT_EQ(p.confidence, 100u) << name;
    bool gz = std::string(name).find(".svgz") != std::string::npos;
    EXPECT_EQ(p.transport, gz ? GVEC_TRANSPORT_GZIP : GVEC_TRANSPORT_NONE) << name;
  }
}

TEST(Probe, WhatIsNotAnSvgRootIsNotRecognised) {
  // An svg element in no namespace is not SVG (Namespaces in XML), nor is svg
  // inside an XHTML root, nor another namespace's svg.
  for (const char * name : {"no-namespace.svg", "html.xhtml", "other-namespace.svg", "plain.txt", "png.bin",
           "pdf.bin", "empty.bin", "not-svg.svgz", "external-ge-attr.svg", "gzip-method-7.bin"}) {
    GVEC_Probe_Result p = Probe(Fixture(name));
    EXPECT_EQ(p.format_name, nullptr) << name;
    EXPECT_EQ(p.confidence, 0u) << name;
  }
}

TEST(Probe, TheFirstElementDecidesAndTheRestIsNotParsed) {
  // A file that is well-formed up to its root's start tag and broken after is
  // still recognised: the probe stops at the first element.
  GVEC_Probe_Result p = Probe(Fixture("mismatched.svg"));
  EXPECT_STREQ(p.format_name, "svg");
  p = Probe(Fixture("billion-laughs.svg")); // the bomb is in content, after the root's start tag
  EXPECT_STREQ(p.format_name, "svg");
}

TEST(Probe, AProbeOfAnInputPastTheLimitIsALimit) {
  std::vector<unsigned char> bytes = Fixture("minimal.svg");
  GVEC_Limits l;
  std::memset(&l, 0, sizeof(l));
  l.max_input_bytes = bytes.size() - 1;
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_memory(bytes.data(), bytes.size(), &s), GVEC_OK);
  GVEC_Probe_Result p;
  EXPECT_EQ(gvec_probe(s, &l, &p), GVEC_ERR_LIMIT);
  l.max_input_bytes = bytes.size();
  EXPECT_EQ(gvec_probe(s, &l, &p), GVEC_OK);
  EXPECT_STREQ(p.format_name, "svg");
  EXPECT_EQ(gvec_probe(nullptr, nullptr, &p), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_probe(s, nullptr, nullptr), GVEC_ERR_INVALID);
  gvec_stream_destroy(s);
}

//-----------------------------------------------------------------------------
// Loading (L8)
//-----------------------------------------------------------------------------

TEST(Load, EverySvgFixtureLoadsAsSvg) {
  for (const char * name : {"minimal.svg", "prefixed.svg", "bom.svg", "utf16le.svg", "utf16be.svg", "utf32le.svg",
           "utf32be.svg", "latin1.svg", "ascii.svg", "prolog.svg", "entities.svg", "elements-8.svg", "depth-8.svg",
           "attributes-8.svg", "references-8.svg"}) {
    Loaded out;
    LoadFixture(name, nullptr, out);
    ASSERT_EQ(out.result, GVEC_OK) << name;
    ASSERT_NE(out.doc, nullptr);
    EXPECT_STREQ(gvec_doc_format(out.doc), "svg");
    EXPECT_EQ(gvec_doc_codec(out.doc), gvec_codec_by_name("svg"));
    EXPECT_EQ(gvec_doc_transport(out.doc), GVEC_TRANSPORT_NONE);
    // Nothing but a notice that an attribute name is not SVG 2's (the attributes-8 fixture uses invented names).
    for (size_t i = 0; i < out.diagnostics.count; i++) {
      EXPECT_EQ(out.diagnostics.items[i].code, GVEC_DIAG_UNKNOWN_ATTRIBUTE) << name;
      EXPECT_EQ(out.diagnostics.items[i].severity, GVEC_DIAG_NOTICE) << name;
    }
    EXPECT_EQ(out.diagnostics.count, std::string(name) == "attributes-8.svg" ? (size_t) 7 : (size_t) 0) << name;
  }
}

TEST(Load, WhatIsNotSvgIsUnsupportedAndSaysWhy) {
  struct Case {
    const char * name;
    GVEC_Diag_Code code;
    size_t line;
    const char * element;
  };
  for (const Case & c : {Case{"no-namespace.svg", GVEC_DIAG_NOT_SVG, 1, "svg"},
           Case{"other-namespace.svg", GVEC_DIAG_NOT_SVG, 1, "svg"}, Case{"html.xhtml", GVEC_DIAG_NOT_SVG, 1, "html"},
           Case{"plain.txt", GVEC_DIAG_XML, 1, nullptr}, Case{"png.bin", GVEC_DIAG_XML, 1, nullptr},
           Case{"pdf.bin", GVEC_DIAG_XML, 1, nullptr}, Case{"empty.bin", GVEC_DIAG_XML, 1, nullptr},
           // The gzip magic with another method is not a transport; it is read
           // as XML, and is not.
           Case{"gzip-method-7.bin", GVEC_DIAG_XML, 1, nullptr},
           // An external entity in an attribute value is not well-formed, and
           // it is in the root's start tag, so the root is never reached.
           Case{"external-ge-attr.svg", GVEC_DIAG_XML, 4, nullptr}}) {
    Loaded out;
    LoadFixture(c.name, nullptr, out);
    EXPECT_EQ(out.result, GVEC_ERR_UNSUPPORTED) << c.name;
    EXPECT_EQ(out.doc, nullptr);
    const GVEC_Diagnostic * d = out.First(c.code);
    ASSERT_NE(d, nullptr) << c.name;
    EXPECT_EQ(d->severity, GVEC_DIAG_ERROR);
    EXPECT_EQ(d->line, c.line) << c.name;
    if (c.element) {
      EXPECT_STREQ(d->element, c.element) << c.name;
    }
  }
}

TEST(Load, AMalformedDocumentIsAFormatErrorAtTheParsersPosition) {
  struct Case {
    const char * name;
    size_t line;
    size_t column;
    GTEXT_XML_Status status;
  };
  for (const Case & c : {Case{"mismatched.svg", 7, 3, GTEXT_XML_E_MISMATCHED_TAG},
           Case{"truncated.svg", 1, 0, GTEXT_XML_E_UNEXPECTED_EOF}, Case{"bad-utf8.svg", 1, 0, GTEXT_XML_E_UTF8}}) {
    Loaded out;
    LoadFixture(c.name, nullptr, out);
    EXPECT_EQ(out.result, GVEC_ERR_FORMAT) << c.name;
    EXPECT_EQ(out.doc, nullptr);
    const GVEC_Diagnostic * d = out.First(GVEC_DIAG_XML);
    ASSERT_NE(d, nullptr) << c.name;
    EXPECT_EQ(d->line, c.line) << c.name;
    if (c.column) {
      EXPECT_EQ(d->column, c.column) << c.name;
    }
    EXPECT_EQ(d->detail, (uint32_t) c.status) << c.name;
    EXPECT_NE(d->message, nullptr);
  }
}

TEST(Load, InternalEntitiesExpandInAttributesAndContent) {
  Loaded out;
  LoadFixture("entities.svg", nullptr, out);
  ASSERT_EQ(out.result, GVEC_OK);
  EXPECT_EQ(out.diagnostics.count, (size_t) 0);
}

TEST(Load, AnInternalSubsetBeforeTheRootIsReadAndTheRootFound) {
  // L7: a doctype's internal subset of any size within the limits. The text
  // parser's declaration processing is quadratic for now (reported to text),
  // so this uses 2048 declarations, not the 1 MiB fixture.
  std::string decls;
  for (int i = 0; i < 2048; i++) {
    decls += "<!ENTITY e" + std::to_string(i) + " \"value " + std::to_string(i) + "\">\n";
  }
  std::vector<unsigned char> bytes =
      Bytes("<!DOCTYPE svg [\n" + decls + "]>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n");
  EXPECT_STREQ(Probe(bytes).format_name, "svg");
  Loaded out;
  Load(bytes, nullptr, out);
  EXPECT_EQ(out.result, GVEC_OK);
}

TEST(Load, AnExternalDtdOrParameterEntityIsNotReadAndIsReportedOnce) {
  for (const char * name : {"external-dtd.svg", "external-pe.svg"}) {
    Loaded out;
    LoadFixture(name, nullptr, out);
    ASSERT_EQ(out.result, GVEC_OK) << name;
    EXPECT_EQ(out.Count(GVEC_DIAG_DTD_INCOMPLETE), (size_t) 1) << name;
    EXPECT_EQ(out.First(GVEC_DIAG_DTD_INCOMPLETE)->severity, GVEC_DIAG_NOTICE);
    EXPECT_EQ(out.Count(GVEC_DIAG_ENTITY_SKIPPED), (size_t) 0) << name;
  }
}

TEST(Load, AReferencedExternalEntityIsKeptAndReportedWhereItIsUsed) {
  Loaded out;
  LoadFixture("external-ge.svg", nullptr, out);
  ASSERT_EQ(out.result, GVEC_OK);
  ASSERT_EQ(out.Count(GVEC_DIAG_ENTITY_SKIPPED), (size_t) 1);
  const GVEC_Diagnostic * d = out.First(GVEC_DIAG_ENTITY_SKIPPED);
  EXPECT_EQ(d->severity, GVEC_DIAG_WARNING);
  EXPECT_EQ(d->line, (size_t) 5);
  EXPECT_STREQ(d->element, "g");
  // A general entity is not DTD knowledge; only the reference is reported.
  EXPECT_EQ(out.Count(GVEC_DIAG_DTD_INCOMPLETE), (size_t) 0);
}

TEST(Load, TheXmlOptionsProcessTheDtdAndCanFetchNothing) {
  // L9, field by field: no resolver exists to call, access is denied, and a
  // declined external entity is kept rather than failing the load.
  GVEC_Limits l;
  gvec_limits_default(&l);
  l.max_depth = 17;
  l.max_attributes = 18;
  l.max_entity_expanded_bytes = 19;
  l.max_entity_depth = 20;
  l.max_entity_references = 21;
  GTEXT_XML_Parse_Options o;
  std::memset(&o, 0xa5, sizeof(o));
  gvec_svg_xml_options(&l, 1234, nullptr, &o);
  EXPECT_EQ(o.dtd_mode, GTEXT_XML_DTD_PROCESS);
  EXPECT_EQ(o.external_access, GTEXT_XML_EXTERNAL_ACCESS_DENY);
  EXPECT_EQ(o.dtd_external_policy, GTEXT_XML_DTD_EXTERNAL_SKIP);
  EXPECT_EQ(o.resolver, nullptr);
  EXPECT_EQ(o.resolver_user_data, nullptr);
  EXPECT_EQ(o.base_uri, nullptr);
  EXPECT_EQ(o.allocator, nullptr);
  EXPECT_EQ(o.max_input_bytes, (size_t) 1234);
  EXPECT_EQ(o.max_depth, (size_t) 17);
  EXPECT_EQ(o.max_attributes, (size_t) 18);
  EXPECT_EQ(o.max_expanded_bytes, (size_t) 19);
  EXPECT_EQ(o.max_entity_depth, (size_t) 20);
  EXPECT_EQ(o.max_references, (size_t) 21);
  // Every other field is the parser's default.
  GTEXT_XML_Parse_Options d = gtext_xml_parse_options_default();
  EXPECT_EQ(o.max_nodes, d.max_nodes);
  EXPECT_EQ(o.max_name_bytes, d.max_name_bytes);
  EXPECT_EQ(o.max_attribute_bytes, d.max_attribute_bytes);
  EXPECT_EQ(o.max_total_work, d.max_total_work);
  EXPECT_EQ(o.max_declarations, d.max_declarations);
  EXPECT_EQ(o.max_external_bytes, d.max_external_bytes);
}

TEST(Load, NullArgumentsAndReservedBytesAreInvalid) {
  std::vector<unsigned char> bytes = Fixture("minimal.svg");
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_memory(bytes.data(), bytes.size(), &s), GVEC_OK);
  GVEC_Doc * doc = nullptr;
  EXPECT_EQ(gvec_doc_load(nullptr, nullptr, nullptr, &doc), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_doc_load(s, nullptr, nullptr, nullptr), GVEC_ERR_INVALID);
  GVEC_Load_Options o;
  std::memset(&o, 0, sizeof(o));
  o._reserved[0] = 1;
  EXPECT_EQ(gvec_doc_load(s, &o, nullptr, &doc), GVEC_ERR_INVALID);
  EXPECT_EQ(doc, nullptr);
  // With no diagnostics list a load still loads.
  EXPECT_EQ(gvec_doc_load(s, nullptr, nullptr, &doc), GVEC_OK);
  gvec_doc_free(doc);
  gvec_doc_free(nullptr);
  EXPECT_EQ(gvec_doc_format(nullptr), nullptr);
  EXPECT_EQ(gvec_doc_codec(nullptr), nullptr);
  EXPECT_EQ(gvec_doc_transport(nullptr), GVEC_TRANSPORT_NONE);
  gvec_stream_destroy(s);
}

TEST(Load, EveryStreamKindGivesTheSameDocument) {
  std::vector<unsigned char> bytes = Fixture("prolog.svg");
  uint64_t expected = 0;
  {
    Loaded out;
    Load(bytes, nullptr, out);
    ASSERT_EQ(out.result, GVEC_OK);
    ASSERT_EQ(gvec_doc_hash(out.doc, &expected), GVEC_OK);
  }
  for (int kind = 0; kind < 3; kind++) {
    GVEC_Stream * s = nullptr;
    if (kind == 0) {
      ASSERT_EQ(gvec_stream_create_memory_no_seek_with_allocator(nullptr, bytes.data(), bytes.size(), &s), GVEC_OK);
    }
    else if (kind == 1) {
      ASSERT_EQ(gvec_stream_create_memory_chunked_with_allocator(nullptr, bytes.data(), bytes.size(), 1, &s), GVEC_OK);
    }
    else {
      ASSERT_EQ(gvec_stream_create_file("tests/data/svg/prolog.svg", &s), GVEC_OK);
    }
    GVEC_Doc * doc = nullptr;
    ASSERT_EQ(gvec_doc_load(s, nullptr, nullptr, &doc), GVEC_OK) << kind;
    uint64_t h = 0;
    ASSERT_EQ(gvec_doc_hash(doc, &h), GVEC_OK);
    EXPECT_EQ(h, expected) << kind;
    EXPECT_TRUE(gvec_stream_eof(s)) << kind; // the load read the stream to its end
    gvec_doc_free(doc);
    gvec_stream_destroy(s);
  }
}

//-----------------------------------------------------------------------------
// gzip (L6)
//-----------------------------------------------------------------------------

TEST(Svgz, AGzipTransportIsInflatedAndRecorded) {
  for (const char * name : {"minimal.svgz", "two-members.svgz", "header-fields.svgz"}) {
    Loaded out;
    LoadFixture(name, nullptr, out);
    ASSERT_EQ(out.result, GVEC_OK) << name;
    EXPECT_EQ(gvec_doc_transport(out.doc), GVEC_TRANSPORT_GZIP);
    EXPECT_EQ(out.diagnostics.count, (size_t) 0) << name;
  }
}

TEST(Svgz, BytesAfterTheLastMemberAreIgnoredWithAWarning) {
  Loaded out;
  LoadFixture("trailing.svgz", nullptr, out);
  ASSERT_EQ(out.result, GVEC_OK);
  const GVEC_Diagnostic * d = out.First(GVEC_DIAG_GZIP);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->severity, GVEC_DIAG_WARNING);
  EXPECT_EQ(d->offset, Fixture("minimal.svgz").size());
}

TEST(Svgz, ADamagedTransportIsCorrupt) {
  for (const char * name : {"truncated.svgz", "bad-crc.svgz"}) {
    Loaded out;
    LoadFixture(name, nullptr, out);
    EXPECT_EQ(out.result, GVEC_ERR_CORRUPT) << name;
    EXPECT_EQ(out.Count(GVEC_DIAG_GZIP), (size_t) 1) << name;
    EXPECT_EQ(out.doc, nullptr);
  }
}

TEST(Svgz, GzipInsideGzipIsNotRead) {
  Loaded out;
  LoadFixture("double.svgz", nullptr, out);
  EXPECT_EQ(out.result, GVEC_ERR_UNSUPPORTED);
  EXPECT_EQ(out.Count(GVEC_DIAG_GZIP), (size_t) 1);
}

TEST(Svgz, NotSvgInsideGzipIsUnsupportedAndSaysWhy) {
  Loaded out;
  LoadFixture("not-svg.svgz", nullptr, out);
  EXPECT_EQ(out.result, GVEC_ERR_UNSUPPORTED);
  ASSERT_NE(out.First(GVEC_DIAG_NOT_SVG), nullptr);
  EXPECT_STREQ(out.First(GVEC_DIAG_NOT_SVG)->element, "html");
}

TEST(Svgz, InflationStopsAtTheCapNotAfter) {
  // bomb.svgz is about 2 KiB inflating to about 2 MiB.
  std::vector<unsigned char> bomb = Fixture("bomb.svgz");
  ASSERT_LT(bomb.size(), (size_t) 8192);
  GVEC_Limits l;
  std::memset(&l, 0, sizeof(l));
  l.max_inflated_bytes = 1 << 20;
  Failing counting; // never fails; counts what the load holds
  Loaded out;
  Load(bomb, &l, out, &counting.a);
  EXPECT_EQ(out.result, GVEC_ERR_LIMIT);
  ASSERT_NE(out.First(GVEC_DIAG_LIMIT), nullptr);
  EXPECT_EQ(out.First(GVEC_DIAG_LIMIT)->detail, (uint32_t) GVEC_LIMIT_INFLATED_BYTES);
  EXPECT_EQ(counting.outstanding, 0);
  // The bound itself: the inflated buffer never grows past the cap plus one.
  GVEC_Diagnostics d;
  gvec_diagnostics_init(&d, nullptr);
  const unsigned char * data = nullptr;
  size_t size = 0;
  GVEC_Transport t;
  GVEC_Limits resolved;
  ASSERT_EQ(gvec_limits_resolve(&l, &resolved, nullptr), GVEC_OK);
  EXPECT_EQ(gvec_transport_decode(bomb.data(), bomb.size(), &resolved, &d, nullptr, &data, &size, &t), GVEC_ERR_LIMIT);
  resolved.max_inflated_bytes = 4 << 20;
  ASSERT_EQ(gvec_transport_decode(bomb.data(), bomb.size(), &resolved, &d, nullptr, &data, &size, &t), GVEC_OK);
  EXPECT_GT(size, (size_t) (1 << 20));
  // At exactly the inflated size the transport is accepted; one byte less is refused.
  size_t exact = size;
  gvec_transport_free(data, t, nullptr);
  resolved.max_inflated_bytes = exact;
  ASSERT_EQ(gvec_transport_decode(bomb.data(), bomb.size(), &resolved, &d, nullptr, &data, &size, &t), GVEC_OK);
  gvec_transport_free(data, t, nullptr);
  resolved.max_inflated_bytes = exact - 1;
  EXPECT_EQ(gvec_transport_decode(bomb.data(), bomb.size(), &resolved, &d, nullptr, &data, &size, &t), GVEC_ERR_LIMIT);
  gvec_diagnostics_destroy(&d);
}

//-----------------------------------------------------------------------------
// Limits, each at its bound and one past (L3)
//-----------------------------------------------------------------------------

struct LimitCase {
  const char * fixture;
  GVEC_Limit_Field field;
  size_t at_bound; // the smallest value the fixture loads with
};

static void SetLimit(GVEC_Limits & l, GVEC_Limit_Field f, size_t v) {
  size_t * p[] = {&l.max_input_bytes, &l.max_inflated_bytes, &l.max_elements, &l.max_depth, &l.max_attributes,
      &l.max_entity_expanded_bytes, &l.max_entity_depth, &l.max_entity_references, &l.max_diagnostics};
  *p[f] = v;
}

TEST(Limits, EachLimitLoadsAtItsBoundAndRefusesOnePastNamingTheField) {
  const LimitCase cases[] = {
      {"minimal.svg", GVEC_LIMIT_INPUT_BYTES, Fixture("minimal.svg").size()},
      {"minimal.svgz", GVEC_LIMIT_INFLATED_BYTES, Fixture("minimal.svg").size()},
      {"elements-8.svg", GVEC_LIMIT_ELEMENTS, 8},
      {"depth-8.svg", GVEC_LIMIT_DEPTH, 8},
      {"attributes-8.svg", GVEC_LIMIT_ATTRIBUTES, 8}, // seven attributes and the namespace declaration
      {"deep-entities.svg", GVEC_LIMIT_ENTITY_DEPTH, 0},
      {"references-8.svg", GVEC_LIMIT_ENTITY_REFERENCES, 8},
  };
  for (const LimitCase & c : cases) {
    GVEC_Limits l;
    std::memset(&l, 0, sizeof(l));
    if (c.at_bound) {
      SetLimit(l, c.field, c.at_bound);
      Loaded ok;
      LoadFixture(c.fixture, &l, ok);
      EXPECT_EQ(ok.result, GVEC_OK) << c.fixture << " at its bound";
      SetLimit(l, c.field, c.at_bound - 1);
    }
    Loaded out;
    LoadFixture(c.fixture, &l, out);
    EXPECT_EQ(out.result, GVEC_ERR_LIMIT) << c.fixture;
    EXPECT_EQ(out.doc, nullptr);
    const GVEC_Diagnostic * d = out.First(GVEC_DIAG_LIMIT);
    ASSERT_NE(d, nullptr) << c.fixture;
    EXPECT_EQ(d->detail, (uint32_t) c.field) << c.fixture;
  }
}

TEST(Limits, TheEntityBombsStopAtTheExpansionBound) {
  // The text parser's expansion is slow on these for now (reported to text),
  // so the bound is small: what is tested is that the bound stops them and is
  // named.
  for (const char * name : {"billion-laughs.svg", "quadratic.svg"}) {
    GVEC_Limits l;
    std::memset(&l, 0, sizeof(l));
    l.max_entity_expanded_bytes = 4096;
    Loaded out;
    LoadFixture(name, &l, out);
    EXPECT_EQ(out.result, GVEC_ERR_LIMIT) << name;
    ASSERT_NE(out.First(GVEC_DIAG_LIMIT), nullptr) << name;
    EXPECT_EQ(out.First(GVEC_DIAG_LIMIT)->detail, (uint32_t) GVEC_LIMIT_ENTITY_EXPANDED_BYTES) << name;
  }
  GVEC_Limits l;
  std::memset(&l, 0, sizeof(l));
  l.max_entity_depth = 32;
  Loaded deep;
  LoadFixture("deep-entities.svg", &l, deep);
  EXPECT_EQ(deep.result, GVEC_ERR_LIMIT);
  EXPECT_EQ(deep.First(GVEC_DIAG_LIMIT)->detail, (uint32_t) GVEC_LIMIT_ENTITY_DEPTH);
}

//-----------------------------------------------------------------------------
// The dump and hash (L10), and lowering (L11)
//-----------------------------------------------------------------------------

static std::string Dump(const GVEC_Doc * doc) {
  GVEC_Stream * out = nullptr;
  EXPECT_EQ(gvec_stream_create_memory_output(&out), GVEC_OK);
  EXPECT_EQ(gvec_doc_dump(doc, out), GVEC_OK);
  const void * data;
  size_t size;
  gvec_stream_output_buffer(out, &data, &size);
  std::string s((const char *) data, size);
  gvec_stream_destroy(out);
  return s;
}

TEST(Dump, TheDumpIsFormatOneWithFormatAndTransport) {
  Loaded plain, gz;
  LoadFixture("minimal.svg", nullptr, plain);
  LoadFixture("minimal.svgz", nullptr, gz);
  ASSERT_EQ(plain.result, GVEC_OK);
  ASSERT_EQ(gz.result, GVEC_OK);
  // The header lines, then the document: one element line (its key, kind, name, namespace, location, flags) and one
  // line per attribute (here the two geometry presentation attributes, held as written for the cascade). The key is
  // restated by the document tests with an independent hash.
  const std::string body =
      "svg elements 1 text-bytes 0\n"
      "e 0 90bdf1c31c91abc1 svg \"svg\" \"http://www.w3.org/2000/svg\" 1:1 -\n"
      "a \"width\" property none 1:41 presentation raw=\"10\" value=-\n"
      "a \"height\" property none 1:52 presentation raw=\"10\" value=-\n";
  EXPECT_EQ(Dump(plain.doc), "vector-doc 1\nformat svg\ntransport none\n" + body);
  EXPECT_EQ(Dump(gz.doc), "vector-doc 1\nformat svg\ntransport gzip\n" + body);
  GVEC_Stream * reading = nullptr;
  ASSERT_EQ(gvec_stream_create_memory("x", 1, &reading), GVEC_OK);
  EXPECT_EQ(gvec_doc_dump(plain.doc, reading), GVEC_ERR_IO);
  gvec_stream_destroy(reading);
  EXPECT_EQ(gvec_doc_dump(nullptr, nullptr), GVEC_ERR_INVALID);
}

TEST(Dump, TheHashIsMurmur3OfTheDumpAndTheSameOnEveryTarget) {
  Loaded plain, gz;
  LoadFixture("minimal.svg", nullptr, plain);
  LoadFixture("minimal.svgz", nullptr, gz);
  uint64_t a = 0, b = 0;
  ASSERT_EQ(gvec_doc_hash(plain.doc, &a), GVEC_OK);
  ASSERT_EQ(gvec_doc_hash(gz.doc, &b), GVEC_OK);
  EXPECT_NE(a, b);
  // Restated: the low 64 bits of MurmurHash3 x64-128, seed 0, little-endian.
  std::string dump = Dump(plain.doc);
  unsigned char digest[16];
  gcu_string_murmur3_x64_128(dump.data(), dump.size(), 0, digest);
  uint64_t restated = 0;
  for (int i = 7; i >= 0; i--) {
    restated = (restated << 8) | digest[i];
  }
  EXPECT_EQ(a, restated);
  // Committed values, so that a target whose hash differs fails here.
  EXPECT_EQ(a, UINT64_C(0x34f491cdb70fb8bb)) << std::hex << a;  // restated with an independent Murmur3
  EXPECT_EQ(b, UINT64_C(0x7148021ac4a32feb)) << std::hex << b;
  EXPECT_EQ(gvec_doc_hash(nullptr, &a), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_doc_hash(plain.doc, nullptr), GVEC_ERR_INVALID);
}

TEST(Lower, BothEntryPointsRefuseTheRootOnceAtItsLineAndEmitNothing) {
  Loaded out;
  LoadFixture("prolog.svg", nullptr, out);
  ASSERT_EQ(out.result, GVEC_OK);
  // Nothing is lowered yet, so neither the edit nor the list is touched; any
  // non-NULL pointer stands in for one.
  int edit_stand_in = 0;
  int list_stand_in = 0;
  for (int which = 0; which < 2; which++) {
    gvec_diagnostics_clear(&out.diagnostics);
    GVEC_Result r = which == 0
        ? gvec_doc_lower_edit(out.doc, nullptr, (GSHSCENE_Edit *) &edit_stand_in, &out.diagnostics)
        : gvec_doc_lower_list(out.doc, nullptr, (GSHSCENE_List *) &list_stand_in, &out.diagnostics);
    EXPECT_EQ(r, GVEC_OK);
    ASSERT_EQ(out.diagnostics.count, (size_t) 1);
    const GVEC_Diagnostic & d = out.diagnostics.items[0];
    EXPECT_EQ(d.code, GVEC_DIAG_LOWER_REFUSED);
    EXPECT_EQ(d.severity, GVEC_DIAG_ERROR);
    EXPECT_EQ(d.detail, (uint32_t) GVEC_LOWER_NOT_YET_LOWERED);
    EXPECT_EQ(d.line, (size_t) 7);
    EXPECT_STREQ(d.element, "svg");
  }
  EXPECT_EQ(edit_stand_in, 0);
  EXPECT_EQ(list_stand_in, 0);
}

TEST(Lower, NullArgumentsAndReservedBytesAreInvalid) {
  Loaded out;
  LoadFixture("minimal.svg", nullptr, out);
  ASSERT_EQ(out.result, GVEC_OK);
  int stand_in = 0;
  EXPECT_EQ(gvec_doc_lower_edit(nullptr, nullptr, (GSHSCENE_Edit *) &stand_in, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_doc_lower_edit(out.doc, nullptr, nullptr, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_doc_lower_list(nullptr, nullptr, (GSHSCENE_List *) &stand_in, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_doc_lower_list(out.doc, nullptr, nullptr, nullptr), GVEC_ERR_INVALID);
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof(o));
  o._reserved[15] = 1;
  EXPECT_EQ(gvec_doc_lower_list(out.doc, &o, (GSHSCENE_List *) &stand_in, nullptr), GVEC_ERR_INVALID);
  // With no diagnostics list the lowering still succeeds.
  EXPECT_EQ(gvec_doc_lower_list(out.doc, nullptr, (GSHSCENE_List *) &stand_in, nullptr), GVEC_OK);
}

//-----------------------------------------------------------------------------
// Allocation failure
//-----------------------------------------------------------------------------

TEST(Allocation, EveryAllocationFailureOfALoadIsOomAndLeaksNothing) {
  for (const char * name : {"prolog.svg", "external-ge.svg", "two-members.svgz", "no-namespace.svg"}) {
    std::vector<unsigned char> bytes = Fixture(name);
    long total = 0;
    uint64_t baseline_hash = 0;
    std::vector<int> baseline_codes;
    GVEC_Result baseline;
    {
      Failing f;
      Loaded out;
      Load(bytes, nullptr, out, &f.a);
      total = f.attempts;
      baseline = out.result;
      if (out.doc) {
        ASSERT_EQ(gvec_doc_hash(out.doc, &baseline_hash), GVEC_OK);
      }
      for (size_t i = 0; i < out.diagnostics.count; i++) {
        baseline_codes.push_back(out.diagnostics.items[i].code);
      }
    }
    ASSERT_GT(total, 0) << name;
    for (long fail = 1; fail <= total; fail++) {
      Failing f;
      f.fail_at = fail;
      {
        Loaded out;
        Load(bytes, nullptr, out, &f.a);
        // An allocation failure is OOM, never another error. A load that
        // survives one (the probe had already decided, say) is the unfailed
        // load exactly: the same result, document and diagnostics, or a list
        // that counts the entry it could not keep. Nothing is lost silently.
        if (out.result == baseline && out.diagnostics.dropped == 0) {
          std::vector<int> codes;
          for (size_t i = 0; i < out.diagnostics.count; i++) {
            codes.push_back(out.diagnostics.items[i].code);
          }
          EXPECT_EQ(codes, baseline_codes) << name << " at " << fail;
          if (out.doc) {
            uint64_t h = 0;
            ASSERT_EQ(gvec_doc_hash(out.doc, &h), GVEC_OK);
            EXPECT_EQ(h, baseline_hash) << name << " at " << fail;
          }
        }
        else if (out.result == baseline) {
          EXPECT_GT(out.diagnostics.dropped, (size_t) 0);
        }
        else {
          EXPECT_EQ(out.result, GVEC_ERR_OOM) << name << " at " << fail << ": " << gvec_result_string(out.result);
          EXPECT_EQ(out.doc, nullptr);
        }
      }
      EXPECT_EQ(f.outstanding, 0) << name << " at " << fail;
    }
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
