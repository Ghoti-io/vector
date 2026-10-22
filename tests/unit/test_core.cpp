/**
 * @file
 *
 * Results (L1), limits (L3) and diagnostics (L4).
 */

#include "test_helpers.h"

#include <set>
#include <string>

#include <ghoti.io/text/xml.h>

#include "../../src/core/core_internal.h"

using namespace gvec_test;

TEST(Results, EveryResultHasADistinctNameAndUnknownValuesAreUnknown) {
  std::set<std::string> names;
  for (int r = 0; r < GVEC_RESULT_COUNT; r++) {
    std::string name = gvec_result_string((GVEC_Result) r);
    EXPECT_NE(name, "unknown") << r;
    names.insert(name);
  }
  EXPECT_EQ(names.size(), (size_t) GVEC_RESULT_COUNT);
  EXPECT_STREQ(gvec_result_string(GVEC_RESULT_COUNT), "unknown");
  EXPECT_STREQ(gvec_result_string((GVEC_Result) 1000), "unknown");
}

TEST(Results, TheNumbersAreTheVocabularysOrder) {
  EXPECT_EQ(GVEC_OK, 0);
  EXPECT_EQ(GVEC_ERR_IO, 1);
  EXPECT_EQ(GVEC_ERR_FORMAT, 2);
  EXPECT_EQ(GVEC_ERR_UNSUPPORTED, 3);
  EXPECT_EQ(GVEC_ERR_LIMIT, 4);
  EXPECT_EQ(GVEC_ERR_CORRUPT, 5);
  EXPECT_EQ(GVEC_ERR_OOM, 6);
  EXPECT_EQ(GVEC_ERR_INVALID, 7);
  EXPECT_EQ(GVEC_ERR_INTERNAL, 8);
  EXPECT_EQ(GVEC_RESULT_COUNT, 9);
}

static size_t Field(const GVEC_Limits & l, int f) {
  const size_t v[] = {l.max_input_bytes, l.max_inflated_bytes, l.max_elements, l.max_depth, l.max_attributes,
      l.max_entity_expanded_bytes, l.max_entity_depth, l.max_entity_references, l.max_path_segments, l.max_list_items,
      l.max_reference_expansion, l.max_text_bytes, l.max_diagnostics};
  return v[f];
}

static void SetField(GVEC_Limits & l, int f, size_t value) {
  size_t * p[] = {&l.max_input_bytes, &l.max_inflated_bytes, &l.max_elements, &l.max_depth, &l.max_attributes,
      &l.max_entity_expanded_bytes, &l.max_entity_depth, &l.max_entity_references, &l.max_path_segments, &l.max_list_items,
      &l.max_reference_expansion, &l.max_text_bytes, &l.max_diagnostics};
  *p[f] = value;
}

TEST(Limits, TheDefaultsAreTheXmlParsersOwnLimits) {
  GVEC_Limits l;
  gvec_limits_default(&l);
  // L3: the default of each field fed to the XML parser is that parser's own.
  GTEXT_XML_Parse_Options x = gtext_xml_parse_options_default();
  EXPECT_EQ(l.max_input_bytes, x.max_input_bytes);
  EXPECT_EQ(l.max_inflated_bytes, x.max_input_bytes);
  EXPECT_EQ(l.max_elements, x.max_nodes);
  EXPECT_EQ(l.max_depth, x.max_depth);
  EXPECT_EQ(l.max_attributes, x.max_attributes);
  EXPECT_EQ(l.max_entity_expanded_bytes, x.max_expanded_bytes);
  EXPECT_EQ(l.max_entity_depth, x.max_entity_depth);
  EXPECT_EQ(l.max_entity_references, x.max_references);
  EXPECT_EQ(l.max_path_segments, (size_t) 4194304);
  EXPECT_EQ(l.max_list_items, (size_t) 1048576);
  EXPECT_EQ(l.max_reference_expansion, (size_t) 1000000);
  EXPECT_EQ(l.max_text_bytes, (size_t) 64 * 1024 * 1024);
  EXPECT_EQ(l.max_diagnostics, (size_t) 4096);
  for (unsigned char b : l._reserved) {
    EXPECT_EQ(b, 0);
  }
  gvec_limits_default(nullptr);
}

TEST(Limits, ZeroSelectsTheDefaultAndNeverUnlimited) {
  GVEC_Limits zero;
  std::memset(&zero, 0, sizeof(zero));
  GVEC_Limits resolved, defaults;
  gvec_limits_default(&defaults);
  ASSERT_EQ(gvec_limits_resolve(&zero, &resolved, nullptr), GVEC_OK);
  for (int f = 0; f < GVEC_LIMIT_FIELD_COUNT; f++) {
    EXPECT_EQ(Field(resolved, f), Field(defaults, f)) << f;
    EXPECT_NE(Field(resolved, f), (size_t) 0) << f;
  }
  ASSERT_EQ(gvec_limits_resolve(nullptr, &resolved, nullptr), GVEC_OK);
  EXPECT_EQ(std::memcmp(&resolved, &defaults, sizeof(resolved)), 0);
}

TEST(Limits, EachFieldIsAcceptedAtItsCeilingAndRefusedOnePast) {
  for (int f = 0; f < GVEC_LIMIT_FIELD_COUNT; f++) {
    GVEC_Limits l, resolved;
    std::memset(&l, 0, sizeof(l));
    size_t ceiling = gvec_limits_ceiling((GVEC_Limit_Field) f);
    SetField(l, f, ceiling);
    EXPECT_EQ(gvec_limits_resolve(&l, &resolved, nullptr), GVEC_OK) << f;
    EXPECT_EQ(Field(resolved, f), ceiling);
    SetField(l, f, ceiling + 1);
    GVEC_Limit_Field which = GVEC_LIMIT_FIELD_COUNT;
    EXPECT_EQ(gvec_limits_resolve(&l, &resolved, &which), GVEC_ERR_INVALID) << f;
    EXPECT_EQ(which, (GVEC_Limit_Field) f);
    SetField(l, f, 1);
    EXPECT_EQ(gvec_limits_resolve(&l, &resolved, nullptr), GVEC_OK) << f;
    EXPECT_EQ(Field(resolved, f), (size_t) 1);
  }
  EXPECT_EQ(gvec_limits_ceiling(GVEC_LIMIT_DIAGNOSTICS), (size_t) 1 << 20);
  EXPECT_EQ(gvec_limits_ceiling(GVEC_LIMIT_FIELD_COUNT), (size_t) 0);
}

TEST(Limits, AReservedByteIsRefused) {
  GVEC_Limits l, resolved;
  std::memset(&l, 0, sizeof(l));
  l._reserved[3] = 1;
  EXPECT_EQ(gvec_limits_resolve(&l, &resolved, nullptr), GVEC_ERR_INVALID);
}

TEST(Limits, ALoadWithALimitAboveItsCeilingIsInvalidAndSaysWhichField) {
  GVEC_Limits l;
  std::memset(&l, 0, sizeof(l));
  l.max_depth = 257;
  Loaded out;
  LoadFixture("minimal.svg", &l, out);
  EXPECT_EQ(out.result, GVEC_ERR_INVALID);
  ASSERT_NE(out.First(GVEC_DIAG_LIMIT), nullptr);
  EXPECT_EQ(out.First(GVEC_DIAG_LIMIT)->detail, (uint32_t) GVEC_LIMIT_DEPTH);
  EXPECT_EQ(out.doc, nullptr);
}

TEST(Diagnostics, AnEntryKeepsEveryFieldAndOwnsACopyOfTheElementName) {
  GVEC_Diagnostics d;
  gvec_diagnostics_init(&d, nullptr);
  char name[] = "rect";
  ASSERT_EQ(gvec_diag_add(&d, GVEC_DIAG_WARNING, GVEC_DIAG_ENTITY_SKIPPED, 7, 3, 4, 50, name, "m"), GVEC_OK);
  name[0] = 'X';
  ASSERT_EQ(d.count, (size_t) 1);
  EXPECT_EQ(d.items[0].severity, GVEC_DIAG_WARNING);
  EXPECT_EQ(d.items[0].code, GVEC_DIAG_ENTITY_SKIPPED);
  EXPECT_EQ(d.items[0].detail, 7u);
  EXPECT_EQ(d.items[0].line, (size_t) 3);
  EXPECT_EQ(d.items[0].column, (size_t) 4);
  EXPECT_EQ(d.items[0].offset, (size_t) 50);
  EXPECT_STREQ(d.items[0].element, "rect");
  EXPECT_STREQ(d.items[0].message, "m");
  ASSERT_EQ(gvec_diag_add(&d, GVEC_DIAG_NOTICE, GVEC_DIAG_XML, 0, 0, 0, 0, nullptr, nullptr), GVEC_OK);
  EXPECT_EQ(d.items[1].element, nullptr);
  EXPECT_STREQ(d.items[1].message, "");
  EXPECT_EQ(gvec_diagnostics_count_code(&d, GVEC_DIAG_XML), (size_t) 1);
  EXPECT_EQ(gvec_diagnostics_count_code(nullptr, GVEC_DIAG_XML), (size_t) 0);
  gvec_diagnostics_clear(&d);
  EXPECT_EQ(d.count, (size_t) 0);
  gvec_diagnostics_destroy(&d);
  gvec_diagnostics_destroy(nullptr);
  gvec_diagnostics_clear(nullptr);
  EXPECT_EQ(gvec_diag_add(nullptr, GVEC_DIAG_ERROR, GVEC_DIAG_XML, 0, 0, 0, 0, nullptr, "x"), GVEC_OK);
}

TEST(Diagnostics, PastTheCapEntriesAreCountedNotKept) {
  GVEC_Diagnostics d;
  gvec_diagnostics_init(&d, nullptr);
  gvec_diag_set_cap(&d, 10);
  for (int i = 0; i < 15; i++) {
    ASSERT_EQ(gvec_diag_add(&d, GVEC_DIAG_NOTICE, GVEC_DIAG_XML, (uint32_t) i, 0, 0, 0, "g", "m"), GVEC_OK);
  }
  EXPECT_EQ(d.count, (size_t) 10);
  EXPECT_EQ(d.dropped, (size_t) 5);
  EXPECT_EQ(d.items[9].detail, 9u); // the first ones are the ones kept
  gvec_diagnostics_clear(&d);
  EXPECT_EQ(d.dropped, (size_t) 0);
  gvec_diagnostics_destroy(&d);
}

TEST(Diagnostics, ALoadSetsTheCapFromItsLimits) {
  // external-ge.svg reports one warning and nothing else; with a cap of 1 and
  // the same file loaded twice into one list, the second entry is dropped.
  GVEC_Limits l;
  std::memset(&l, 0, sizeof(l));
  l.max_diagnostics = 1;
  Loaded out;
  LoadFixture("external-ge.svg", &l, out);
  ASSERT_EQ(out.result, GVEC_OK);
  EXPECT_EQ(out.diagnostics.cap, (size_t) 1);
  EXPECT_EQ(out.diagnostics.count, (size_t) 1);
  GVEC_Result r = gvec_doc_lower_list(out.doc, nullptr, (GSHSCENE_List *) &l, &out.diagnostics);
  EXPECT_EQ(r, GVEC_OK);
  EXPECT_EQ(out.diagnostics.count, (size_t) 1);
  EXPECT_EQ(out.diagnostics.dropped, (size_t) 1);
}

TEST(Diagnostics, TheCodesAndSeveritiesHaveTheirStableNumbers) {
  EXPECT_EQ(GVEC_DIAG_NOTICE, 0);
  EXPECT_EQ(GVEC_DIAG_WARNING, 1);
  EXPECT_EQ(GVEC_DIAG_ERROR, 2);
  EXPECT_EQ(GVEC_DIAG_XML, 1);
  EXPECT_EQ(GVEC_DIAG_NOT_SVG, 2);
  EXPECT_EQ(GVEC_DIAG_ENTITY_SKIPPED, 3);
  EXPECT_EQ(GVEC_DIAG_DTD_INCOMPLETE, 4);
  EXPECT_EQ(GVEC_DIAG_GZIP, 5);
  EXPECT_EQ(GVEC_DIAG_LIMIT, 6);
  EXPECT_EQ(GVEC_DIAG_LOWER_REFUSED, 7);
  EXPECT_EQ(GVEC_LIMIT_INPUT_BYTES, 0);
  EXPECT_EQ(GVEC_LIMIT_PATH_SEGMENTS, 8);
  EXPECT_EQ(GVEC_LIMIT_LIST_ITEMS, 9);
  EXPECT_EQ(GVEC_LIMIT_REFERENCE_EXPANSION, 10);
  EXPECT_EQ(GVEC_LIMIT_TEXT_BYTES, 11);
  EXPECT_EQ(GVEC_LIMIT_DIAGNOSTICS, 12);
  EXPECT_EQ(GVEC_DIAG_ATTRIBUTE_INVALID, 8);
  EXPECT_EQ(GVEC_DIAG_UNKNOWN_ELEMENT, 9);
  EXPECT_EQ(GVEC_DIAG_UNKNOWN_ATTRIBUTE, 10);
  EXPECT_EQ(GVEC_DIAG_SCRIPT_IGNORED, 11);
  EXPECT_EQ(GVEC_DIAG_ANIMATION_NOT_APPLIED, 12);
  EXPECT_EQ(GVEC_DIAG_DUPLICATE_ID, 13);
  EXPECT_EQ(GVEC_DIAG_REFERENCE_MISSING, 14);
  EXPECT_EQ(GVEC_DIAG_REFERENCE_CYCLE, 15);
  EXPECT_EQ(GVEC_DIAG_EXPANSION_LIMIT, 16);
  EXPECT_EQ(GVEC_LOWER_NOT_YET_LOWERED, 1);
}

TEST(Version, TheVersionFunctionsReportTheBuild) {
  EXPECT_EQ(gvec_version_number(), (unsigned) GVEC_VERSION_NUMBER);
  EXPECT_STREQ(gvec_version_string(), GVEC_VERSION_STRING);
  EXPECT_EQ(gvec_allocator_default(), gcu_allocator_default());
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
