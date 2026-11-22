/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Vector.
 *
 * Ghoti.io Vector is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Vector is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The resource resolver (lower.h, lower_external.c, svg_resource.c, svg_sheet.c, lower_image.c): the one door by which an `image`, a `style`'s
 * `@import` and a `use` of an element of another document reach bytes the document does not hold. What the library asks, how it resolves a
 * reference against its base (RFC 3986, section 5.4's examples), what it does with each answer and with a hostile one, the cache and the three
 * limits, and that without a resolver nothing is ever asked and the four refusals stand.
 */

#include "../lower_helpers.h"

#include <cmath>
#include "../../src/codec/svg/svg_resource.h"

#include <ghoti.io/shape-scene/image.h>
#include <ghoti.io/shape-scene/image_paint.h>

#include <map>
#include <string>
#include <vector>

using namespace gvec_test;

namespace {

std::vector<unsigned char> FileBytes(const std::string & path) {
  std::vector<unsigned char> bytes;
  FILE * f = std::fopen(path.c_str(), "rb");
  if (!f) {
    ADD_FAILURE() << "cannot open " << path;
    return bytes;
  }
  unsigned char buffer[65536];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
    bytes.insert(bytes.end(), buffer, buffer + n);
  }
  std::fclose(f);
  return bytes;
}

std::string Str(const std::vector<unsigned char> & v) {
  return std::string(v.begin(), v.end());
}

struct Seen {
  GVEC_Resource_Kind kind;
  std::string reference, url, base;
  size_t max_bytes, depth;
};

/** One scripted answer for a url. */
struct Answer {
  GVEC_Resource_Status status = GVEC_RESOURCE_NOT_FOUND;
  std::string bytes;
  const char * media_type = nullptr;
  bool raw_size = false;        ///< Report `size` instead of the bytes' length, with `bytes` unreadable (a lie about the size must not be read).
  size_t size = 0;
  bool null_bytes = false;      ///< Report a size and no bytes.
};

/** A resolver that answers from a table and remembers every request. */
struct Fake {
  std::map<std::string, Answer> table;
  std::vector<Seen> seen;
  std::string scratch;  ///< What the last reply pointed at: reused by the next call, to show that the library copied it.

  void Put(const std::string & url, const std::string & bytes, const char * media = nullptr) {
    Answer a;
    a.status = GVEC_RESOURCE_OK;
    a.bytes = bytes;
    a.media_type = media;
    table[url] = a;
  }
  void Status(const std::string & url, GVEC_Resource_Status status) {
    Answer a;
    a.status = status;
    table[url] = a;
  }
  size_t Calls(const std::string & url = "") const {
    size_t n = 0;
    for (const Seen & s : seen) {
      n += url.empty() || s.url == url;
    }
    return n;
  }

  static GVEC_Resource_Status Call(void * user, const GVEC_Resource_Request * request, GVEC_Resource_Reply * reply) {
    Fake * self = static_cast<Fake *>(user);
    self->seen.push_back({request->kind, request->reference, request->url, request->base ? request->base : "(none)", request->max_bytes,
        request->depth});
    auto it = self->table.find(request->url);
    if (it == self->table.end()) {
      return GVEC_RESOURCE_NOT_FOUND;
    }
    const Answer & a = it->second;
    if (a.status != GVEC_RESOURCE_OK) {
      return a.status;
    }
    if (a.null_bytes) {
      reply->bytes = nullptr;
      reply->size = a.size;
      return GVEC_RESOURCE_OK;
    }
    if (a.raw_size) {
      reply->bytes = reinterpret_cast<const void *>(1);  // never to be read: the size is over the limit
      reply->size = a.size;
      return GVEC_RESOURCE_OK;
    }
    self->scratch = a.bytes;  // the library copies before the next call, which overwrites this
    reply->bytes = self->scratch.data();
    reply->size = self->scratch.size();
    reply->media_type = a.media_type;
    return GVEC_RESOURCE_OK;
  }

  void Fill(GVEC_Lower_Options & o) {
    o.resolver = &Fake::Call;
    o.resolver_user = this;
  }
};

std::string Style(const std::string & rules) {
  return "<style>" + rules + "</style>";
}

std::string Shape(const std::string & attributes = "") {
  return "<path id=\"a\" d=\"M0 0H4V4H0Z\" " + attributes + "/>";
}

std::string Doc(const std::string & body) {
  return Svg("width=\"20\" height=\"20\"", body);
}

std::string UseOf(const std::string & href, const std::string & attributes = "") {
  return "<use href=\"" + href + "\" " + attributes + "/>";
}

std::string ImageOf(const std::string & href) {
  return "<image width=\"8\" height=\"8\" href=\"" + href + "\"/>";
}

/**
 * Lower `svg` both ways (LowerBoth lowers once into a list and once into an edit). Each lowering has a cache of its own and so asks the same
 * questions; the two lists of questions must be the same, and one of them is what the test reads.
 */
void Lower(const std::string & svg, Lowered & l, Fake & fake, GVEC_Lower_Options o = GVEC_Lower_Options()) {
  fake.Fill(o);
  size_t before = fake.seen.size();
  LowerSvg(svg, l, 0, 0, &o);
  size_t asked = fake.seen.size() - before;
  ASSERT_EQ(asked % 2, (size_t) 0) << "the list lowering and the edit lowering ask the resolver the same number of questions";
  for (size_t i = 0; i < asked / 2; i++) {
    const Seen & a = fake.seen[before + i];
    const Seen & b = fake.seen[before + asked / 2 + i];
    EXPECT_EQ(a.url, b.url);
    EXPECT_EQ(a.kind, b.kind);
    EXPECT_EQ(a.depth, b.depth);
  }
  fake.seen.resize(before + asked / 2);
}

bool IsGreen(const GSHSCENE_Paint & p) {
  return p.kind == GSHSCENE_PAINT_SOLID && p.rgba[0] == 0.0 && p.rgba[1] > 0.4 && p.rgba[2] == 0.0;
}
bool IsRed(const GSHSCENE_Paint & p) {
  return p.kind == GSHSCENE_PAINT_SOLID && p.rgba[0] == 1.0 && p.rgba[1] == 0.0 && p.rgba[2] == 0.0;
}
bool IsBlue(const GSHSCENE_Paint & p) {
  return p.kind == GSHSCENE_PAINT_SOLID && p.rgba[0] == 0.0 && p.rgba[1] == 0.0 && p.rgba[2] > 0.9;
}

/** Every item of the list, the pictures of its instances expanded in place (a `use` of another document, and an SVG image, are instances). */
void Collect(const GSHSCENE_List * list, std::vector<GSHSCENE_Item> & out) {
  for (size_t i = 0; i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * item = nullptr;
    ASSERT_EQ(gshscene_list_item(list, i, &item), GSHSCENE_OK);
    if (item->kind == GSHSCENE_ITEM_INSTANCE && gshscene_resource_picture(item->resource)) {
      Collect(gshscene_resource_picture(item->resource), out);
    } else {
      out.push_back(*item);
    }
  }
}

std::vector<GSHSCENE_Item> FillItems(const Lowered & l) {
  std::vector<GSHSCENE_Item> all, fills;
  Collect(l.list, all);
  for (const GSHSCENE_Item & item : all) {
    if (item.kind == GSHSCENE_ITEM_FILL) {
      fills.push_back(item);
    }
  }
  return fills;
}

size_t Fills(const Lowered & l) {
  return FillItems(l).size();
}

/** The paint of the `n`th fill (in drawing order), or a zero paint. */
GSHSCENE_Paint FillOf(const Lowered & l, size_t n) {
  std::vector<GSHSCENE_Item> fills = FillItems(l);
  return n < fills.size() ? fills[n].paint : GSHSCENE_Paint();
}

bool AnyGreen(const Lowered & l) {
  for (const GSHSCENE_Item & item : FillItems(l)) {
    if (IsGreen(item.paint)) {
      return true;
    }
  }
  return false;
}

size_t ImageFills(const Lowered & l) {
  size_t n = 0;
  for (const GSHSCENE_Item & item : FillItems(l)) {
    n += item.paint.kind == GSHSCENE_PAINT_IMAGE;
  }
  return n;
}

const GVEC_Diagnostic * Diag(const Lowered & l, GVEC_Diag_Code code, uint32_t detail = (uint32_t) -1) {
  for (size_t i = 0; i < l.by_list.count; i++) {
    if (l.by_list.items[i].code == code && (detail == (uint32_t) -1 || l.by_list.items[i].detail == detail)) {
      return &l.by_list.items[i];
    }
  }
  return nullptr;
}

std::string Resolve(const char * base, const char * reference) {
  char * out = nullptr;
  EXPECT_EQ(gvec_res_resolve(nullptr, base, reference, &out), GVEC_OK);
  std::string s = out ? out : "(null)";
  gvec_free(nullptr, out);
  return s;
}

}  // namespace

//-----------------------------------------------------------------------------
// The default: no resolver, and the four refusals stand
//-----------------------------------------------------------------------------

TEST(LowerResolverDefault, WithoutAResolverAllFourKindsOfReferenceAreRefusedAndNothingIsAsked) {
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.base_uri = "tests/structure/x.svg";  // a base without a resolver opens nothing
  Lowered image, style, use, use_doc;
  LowerSvg(Doc(ImageOf("https://example.test/a.png")), image, 0, 0, &o);
  ASSERT_NE(image.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  EXPECT_EQ(ImageFills(image), (size_t) 0);
  LowerSvg(Doc(Style("@import \"../../resources/green.css\"") + Shape("fill=\"red\"")), style, 0, 0, &o);
  const GVEC_Diagnostic * sheet = style.Refusal(GVEC_LOWER_STYLESHEET);
  ASSERT_NE(sheet, nullptr);
  EXPECT_NE(std::string(sheet->message).find("not fetched"), std::string::npos) << sheet->message;
  LowerSvg(Doc(UseOf("other.svg#p")), use, 0, 0, &o);
  EXPECT_NE(use.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  LowerSvg(Doc(UseOf("data:image/svg+xml,%3Csvg%2F%3E#p")), use_doc, 0, 0, &o);
  EXPECT_NE(use_doc.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
}

TEST(LowerResolverDefault, AskingWithoutAResolverIsAnAnswerNotACall) {
  gvec_res_state state;
  gvec_res_init(&state, nullptr, nullptr);
  gvec_res_outcome outcome = GVEC_RES_OK;
  gvec_res_entry * entry = reinterpret_cast<gvec_res_entry *>(1);
  ASSERT_EQ(gvec_res_fetch(&state, GVEC_RESOURCE_IMAGE, nullptr, "a.png", 1000, 1, &outcome, &entry), GVEC_OK);
  EXPECT_EQ(outcome, GVEC_RES_NO_RESOLVER);
  EXPECT_EQ(entry, nullptr);
  EXPECT_EQ(state.calls, (size_t) 0);
  gvec_res_free(&state);
}

TEST(LowerResolverDefault, TheOptionsBeingZeroLeavesTheDefaultsForTheLimits) {
  gvec_res_state state;
  gvec_res_init(&state, nullptr, nullptr);
  EXPECT_EQ(state.max_bytes, GVEC_RES_DEFAULT_BYTES);
  EXPECT_EQ(state.max_count, (size_t) GVEC_RES_DEFAULT_COUNT);
  EXPECT_EQ(state.max_depth, (size_t) GVEC_RES_DEFAULT_DEPTH);
  EXPECT_EQ(state.max_instances, (size_t) GVEC_RES_DEFAULT_INSTANCES);
  EXPECT_FALSE(gvec_res_enabled(&state));
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resource_bytes = (size_t) -1;
  o.max_resources = (size_t) -1;
  o.max_resource_depth = (size_t) -1;
  o.max_resource_instances = (size_t) -1;
  gvec_res_init(&state, nullptr, &o);
  EXPECT_EQ(state.max_bytes, GVEC_RES_CEILING_BYTES);
  EXPECT_EQ(state.max_count, (size_t) GVEC_RES_CEILING_COUNT);
  EXPECT_EQ(state.max_depth, (size_t) GVEC_RES_CEILING_DEPTH);
  EXPECT_EQ(state.max_instances, GVEC_RES_CEILING_INSTANCES);
}

//-----------------------------------------------------------------------------
// The resolution of a reference against a base: RFC 3986, section 5.4
//-----------------------------------------------------------------------------

TEST(LowerResolverUri, TheNormalExamplesOfRfc3986) {
  const char * const base = "http://a/b/c/d;p?q";
  static const char * const rows[][2] = {
      {"g:h", "g:h"}, {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"}, {"/g", "http://a/g"},
      {"//g", "http://g"}, {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"}, {"#s", "http://a/b/c/d;p?q"},
      {"g#s", "http://a/b/c/g"}, {"g?y#s", "http://a/b/c/g?y"}, {";x", "http://a/b/c/;x"}, {"g;x", "http://a/b/c/g;x"},
      {"g;x?y#s", "http://a/b/c/g;x?y"}, {"", "http://a/b/c/d;p?q"}, {".", "http://a/b/c/"}, {"./", "http://a/b/c/"},
      {"..", "http://a/b/"}, {"../", "http://a/b/"}, {"../g", "http://a/b/g"}, {"../..", "http://a/"}, {"../../", "http://a/"},
      {"../../g", "http://a/g"}};
  for (const auto & row : rows) {
    EXPECT_EQ(Resolve(base, row[0]), row[1]) << "reference \"" << row[0] << "\"";
  }
}

TEST(LowerResolverUri, TheAbnormalExamplesOfRfc3986) {
  const char * const base = "http://a/b/c/d;p?q";
  static const char * const rows[][2] = {
      {"../../../g", "http://a/g"}, {"../../../../g", "http://a/g"}, {"/./g", "http://a/g"}, {"/../g", "http://a/g"},
      {"g.", "http://a/b/c/g."}, {".g", "http://a/b/c/.g"}, {"g..", "http://a/b/c/g.."}, {"..g", "http://a/b/c/..g"},
      {"./../g", "http://a/b/g"}, {"./g/.", "http://a/b/c/g/"}, {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
      {"g;x=1/./y", "http://a/b/c/g;x=1/y"}, {"g;x=1/../y", "http://a/b/c/y"}, {"g?y/./x", "http://a/b/c/g?y/./x"},
      {"g?y/../x", "http://a/b/c/g?y/../x"}, {"g#s/./x", "http://a/b/c/g"}, {"g#s/../x", "http://a/b/c/g"}, {"http:g", "http:g"}};
  for (const auto & row : rows) {
    EXPECT_EQ(Resolve(base, row[0]), row[1]) << "reference \"" << row[0] << "\"";
  }
}

TEST(LowerResolverUri, AnEmptyResultAndAVeryLongOneAreBothBuilt) {
  EXPECT_EQ(Resolve(nullptr, ""), "");
  EXPECT_EQ(Resolve("", "#only-a-fragment"), "");
  std::string long_segment(300, 'a');
  std::string base = "https://host.example.test/" + long_segment + "/dir/file.svg";
  std::string reference = "../" + std::string(150, 'c') + ".css?q=" + std::string(100, 'd');
  EXPECT_EQ(Resolve(base.c_str(), reference.c_str()),
      "https://host.example.test/" + long_segment + "/" + std::string(150, 'c') + ".css?q=" + std::string(100, 'd'));
}

TEST(LowerResolverUri, ARelativeBaseKeepsAParentThatHasNothingAboveIt) {
  // The corpus's own reference: a case three directories down, naming `resources/` three up.
  EXPECT_EQ(Resolve("tests/structure/style/external-CSS.svg", "../../../resources/green.css"), "resources/green.css");
  EXPECT_EQ(Resolve("tests/a.svg", "../../x.png"), "../x.png");           // one more than the base has: it stays, it is not guessed away
  EXPECT_EQ(Resolve("a.svg", "../x.png"), "../x.png");
  EXPECT_EQ(Resolve(nullptr, "x/./y/../z.css"), "x/z.css");
  EXPECT_EQ(Resolve(nullptr, "https://h/a/../b.png#f"), "https://h/b.png");
  EXPECT_EQ(Resolve("https://h", "x.png"), "https://h/x.png");            // an authority with no path
  EXPECT_EQ(Resolve("https://h/d/e.svg", "//other/y.png"), "https://other/y.png");
  EXPECT_EQ(Resolve("/abs/dir/e.svg", "../y.png"), "/abs/y.png");
  EXPECT_EQ(Resolve("/abs/e.svg", "../../../y.png"), "/y.png");           // rooted: nothing above the root
  EXPECT_EQ(Resolve("tests/e.svg#frag", "y.png"), "tests/y.png");        // the base's fragment is no part of it
}

//-----------------------------------------------------------------------------
// What the resolver is asked
//-----------------------------------------------------------------------------

TEST(LowerResolverRequest, AnImageReferenceIsAskedAsResolvedAgainstTheBase) {
  Fake fake;
  fake.Put("tests/data/rgb.png", Str(FileBytes("tests/data/images/rgb-3x2.png")));
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.base_uri = "tests/structure/image/case.svg";
  Lowered l;
  Lower(Doc(ImageOf("../../data/rgb.png#ignored")), l, fake, o);
  ASSERT_EQ(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].kind, GVEC_RESOURCE_IMAGE);
  EXPECT_EQ(fake.seen[0].reference, "../../data/rgb.png");  // as written, without the fragment
  EXPECT_EQ(fake.seen[0].url, "tests/data/rgb.png");
  EXPECT_EQ(fake.seen[0].base, "tests/structure/image/case.svg");
  EXPECT_EQ(fake.seen[0].depth, (size_t) 1);
  EXPECT_EQ(fake.seen[0].max_bytes, (size_t) 32 << 20);  // the image limit, which is the smaller of it and the default of max_resource_bytes
  EXPECT_EQ(ImageFills(l), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerResolverRequest, AnAbsoluteUrlIsAskedAsItIsAndAreferenceWithNoBaseHasNone) {
  Fake fake;
  Lowered l;
  Lower(Doc(ImageOf("https://upload.example.test/a/b.png")), l, fake);
  ASSERT_EQ(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].url, "https://upload.example.test/a/b.png");
  EXPECT_EQ(fake.seen[0].base, "(none)");
}

TEST(LowerResolverRequest, ADataUriAndAReferenceToTheDocumentItselfAreNeverAsked) {
  Fake fake;
  Lowered l;
  Lower(Doc("<defs><g id=\"g\"><path d=\"M0 0H3V3Z\"/></g></defs>" + UseOf("#g") + ImageOf("#g") + ImageOf("data:image/png;base64,AAAA")), l, fake);
  EXPECT_EQ(fake.seen.size(), (size_t) 0);
}

TEST(LowerResolverRequest, TheLimitOfAKindAndTheOptionsLimitBothCapTheBytes) {
  Fake fake;
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resource_bytes = 1000;
  Lowered l;
  Lower(Doc(ImageOf("a.png")), l, fake, o);
  ASSERT_EQ(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].max_bytes, (size_t) 1000);
  Fake second;
  GVEC_Limits limits;
  std::memset(&limits, 0, sizeof limits);
  limits.max_style_bytes = 500;
  Lowered s;
  LoadSvg(Doc(Style("@import 'a.css'") + Shape()), s.loaded, 0, 0, &limits);
  std::memset(&o, 0, sizeof o);
  o.max_resource_bytes = 4000;
  second.Fill(o);
  LowerBoth(s.loaded, s, &o);
  ASSERT_EQ(second.seen.size(), (size_t) 2);  // once for the list and once for the edit
  EXPECT_EQ(second.seen[0].kind, GVEC_RESOURCE_STYLE);
  EXPECT_EQ(second.seen[0].max_bytes, (size_t) 500);
}

//-----------------------------------------------------------------------------
// Images
//-----------------------------------------------------------------------------

TEST(LowerResolverImage, ARasterFromTheResolverIsDrawnAndTheBytesAreCopied) {
  Fake fake;
  std::vector<unsigned char> png = FileBytes("tests/data/images/rgb-3x2.png");
  fake.Put("a.png", std::string(png.begin(), png.end()), "image/png");
  fake.Put("b.png", "not an image", "image/png");  // the next call overwrites what a.png pointed at
  Lowered l;
  Lower(Doc(ImageOf("a.png") + ImageOf("b.png")), l, fake);
  EXPECT_EQ(ImageFills(l), (size_t) 1);
  EXPECT_NE(Diag(l, GVEC_DIAG_IMAGE_UNUSABLE, GVEC_IMAGE_UNDECODABLE), nullptr);  // b.png
}

TEST(LowerResolverImage, AnSvgDocumentFromTheResolverIsDrawnWithItsOwnReferencesResolvedAgainstItsOwnUrl) {
  Fake fake;
  fake.Put("https://h/dir/pic.svg",
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><style>@import \"s/green.css\";</style><path d=\"M0 0H10V10H0Z\" fill=\"red\"/></svg>",
      "image/svg+xml");
  fake.Put("https://h/dir/s/green.css", "path { fill: green }");
  Lowered l;
  Lower(Doc(ImageOf("https://h/dir/pic.svg")), l, fake);
  ASSERT_EQ(fake.seen.size(), (size_t) 2);
  EXPECT_EQ(fake.seen[1].kind, GVEC_RESOURCE_STYLE);
  EXPECT_EQ(fake.seen[1].url, "https://h/dir/s/green.css");
  EXPECT_EQ(fake.seen[1].base, "https://h/dir/pic.svg");
  EXPECT_EQ(fake.seen[1].depth, (size_t) 2);  // the document that named it was itself a resource
  ASSERT_GE(Fills(l), (size_t) 1);
  EXPECT_TRUE(AnyGreen(l)) << "the sheet the image document imported colours its path";
}

TEST(LowerResolverImage, EveryStatusOfTheResolverHasItsOwnEffect) {
  struct Row { GVEC_Resource_Status status; bool refused; GVEC_Image_Problem problem; };
  for (const Row & row : {Row{GVEC_RESOURCE_REFUSED, true, GVEC_IMAGE_NO_DATA}, Row{GVEC_RESOURCE_NOT_FOUND, false, GVEC_IMAGE_UNREADABLE},
           Row{GVEC_RESOURCE_FAILED, false, GVEC_IMAGE_UNREADABLE}}) {
    Fake fake;
    fake.Status("a.png", row.status);
    Lowered l;
    Lower(Doc(ImageOf("a.png")), l, fake);
    EXPECT_EQ(l.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE) != nullptr, row.refused) << row.status;
    if (!row.refused) {
      EXPECT_NE(Diag(l, GVEC_DIAG_IMAGE_UNUSABLE, row.problem), nullptr) << row.status;
    }
    EXPECT_EQ(ImageFills(l), (size_t) 0);
  }
}

TEST(LowerResolverImage, AResolverThatRunsOutOfMemoryFailsTheLowering) {
  Fake fake;
  fake.Status("a.png", GVEC_RESOURCE_OOM);
  Lowered l;
  LoadSvg(Doc(ImageOf("a.png")), l.loaded);
  ASSERT_EQ(l.loaded.result, GVEC_OK);
  GSHSCENE_Scene * scene = nullptr;
  ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
  GSHSCENE_List * list = nullptr;
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  fake.Fill(o);
  o.store = gshscene_scene_store(scene);
  EXPECT_EQ(gvec_doc_lower_list(l.loaded.doc, &o, list, nullptr), GVEC_ERR_OOM);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
}

TEST(LowerResolverImage, AnAnswerLongerThanTheLimitIsRefusedWithoutBeingRead) {
  Fake fake;
  Answer a;
  a.status = GVEC_RESOURCE_OK;
  a.raw_size = true;
  a.size = (size_t) -1 / 2;  // the bytes pointer is 1: any read of it would fault
  fake.table["huge.png"] = a;
  Lowered l;
  Lower(Doc(ImageOf("huge.png")), l, fake);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
  EXPECT_EQ(ImageFills(l), (size_t) 0);
}

TEST(LowerResolverImage, SizeWithNoBytesAndAnEmptyAnswerAreFailuresNotReads) {
  Fake fake;
  Answer a;
  a.status = GVEC_RESOURCE_OK;
  a.null_bytes = true;
  a.size = 16;
  fake.table["null.png"] = a;
  fake.Put("empty.png", "");
  Lowered l;
  Lower(Doc(ImageOf("null.png") + ImageOf("empty.png")), l, fake);
  EXPECT_EQ(ImageFills(l), (size_t) 0);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_NE(Diag(l, GVEC_DIAG_IMAGE_UNUSABLE, GVEC_IMAGE_UNREADABLE), nullptr);
  EXPECT_NE(Diag(l, GVEC_DIAG_IMAGE_UNUSABLE, GVEC_IMAGE_NO_DATA), nullptr);
}

TEST(LowerResolverImage, AMediaTypeThatNeverEndsIsIgnoredNotRead) {
  Fake fake;
  static const std::string endless(4096, 'x');
  std::vector<unsigned char> png = FileBytes("tests/data/images/rgb-3x2.png");
  fake.Put("a.png", std::string(png.begin(), png.end()), endless.c_str());
  Lowered l;
  Lower(Doc(ImageOf("a.png")), l, fake);
  EXPECT_EQ(ImageFills(l), (size_t) 1);
}

TEST(LowerResolverImage, TheBytesDecideNotTheMediaType) {
  Fake fake;
  fake.Put("p.png", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"><path d=\"M0 0H4V4H0Z\"/></svg>", "image/png");
  Lowered l;
  Lower(Doc(ImageOf("p.png")), l, fake);
  EXPECT_GE(Fills(l), (size_t) 1);
  EXPECT_EQ(ImageFills(l), (size_t) 0);  // drawn as a document, not decoded as a raster
}

TEST(LowerResolverImage, AGzipCompressedSvgFromTheResolverIsAnSvgDocumentNotARaster) {
  Fake fake;
  fake.Put("pic.svgz", Str(FileBytes("tests/data/svg/minimal.svgz")));  // a transport the loader inflates; it is not a raster
  Lowered l;
  Lower(Doc(ImageOf("pic.svgz")), l, fake);
  EXPECT_EQ(fake.Calls(), (size_t) 1);
  EXPECT_EQ(Diag(l, GVEC_DIAG_IMAGE_UNUSABLE), nullptr) << "a document that draws nothing is not a broken image";
  EXPECT_EQ(l.Refusals(), (size_t) 0);
}

TEST(LowerResolverImage, TheResolverTakesOverFromImageRootButTheRootStillServesWithoutOne) {
  Fake fake;
  std::vector<unsigned char> png = FileBytes("tests/data/images/rgb-3x2.png");
  fake.Put("rgb-3x2.png", std::string(png.begin(), png.end()));
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.image_root = "tests/data/images";
  Lowered with_resolver, without;
  Lower(Doc(ImageOf("rgb-3x2.png")), with_resolver, fake, o);
  EXPECT_EQ(fake.Calls(), (size_t) 1);
  EXPECT_EQ(ImageFills(with_resolver), (size_t) 1);
  GVEC_Lower_Options only_root;
  std::memset(&only_root, 0, sizeof only_root);
  only_root.image_root = "tests/data/images";
  LowerSvg(Doc(ImageOf("rgb-3x2.png")), without, 0, 0, &only_root);
  EXPECT_EQ(ImageFills(without), (size_t) 1);
}

//-----------------------------------------------------------------------------
// The cache and the limits
//-----------------------------------------------------------------------------

TEST(LowerResolverCache, OneReferenceIsAskedOncePerLoweringWhateverAnswerItGot) {
  Fake fake;
  std::vector<unsigned char> png = FileBytes("tests/data/images/rgb-3x2.png");
  fake.Put("a.png", std::string(png.begin(), png.end()));
  fake.Status("gone.png", GVEC_RESOURCE_REFUSED);
  fake.Put("d.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><path id=\"p\" d=\"M0 0H2V2Z\"/></svg>");
  Lowered l;
  Lower(Doc(ImageOf("a.png") + ImageOf("x/../a.png") + ImageOf("gone.png") + ImageOf("gone.png") + UseOf("d.svg#p") + UseOf("d.svg#p", "x=\"5\"")), l, fake);
  EXPECT_EQ(fake.Calls("a.png"), (size_t) 1);
  EXPECT_EQ(fake.Calls("gone.png"), (size_t) 1);
  EXPECT_EQ(fake.Calls("d.svg"), (size_t) 1);
  EXPECT_EQ(ImageFills(l), (size_t) 2);
}

TEST(LowerResolverCache, TheSameUrlAsTwoKindsIsAskedOncePerKind) {
  Fake fake;
  fake.Put("same", "path { fill: green }");
  Lowered l;
  Lower(Doc(Style("@import \"same\"") + ImageOf("same")), l, fake);
  EXPECT_EQ(fake.Calls("same"), (size_t) 2);
}

TEST(LowerResolverLimits, MoreResourcesThanMaxResourcesAreRefusedWithoutBeingAsked) {
  Fake fake;
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resources = 2;
  Lowered l;
  Lower(Doc(ImageOf("1.png") + ImageOf("2.png") + ImageOf("3.png") + ImageOf("4.png")), l, fake, o);
  EXPECT_EQ(fake.Calls(), (size_t) 2);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerResolverLimits, ReferencesNestedDeeperThanMaxResourceDepthAreRefused) {
  Fake fake;
  // image -> svg -> image -> svg -> image: the third is at depth 3.
  fake.Put("1.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"9\" height=\"9\"><image width=\"9\" height=\"9\" href=\"2.svg\"/></svg>");
  fake.Put("2.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"9\" height=\"9\"><image width=\"9\" height=\"9\" href=\"3.svg\"/></svg>");
  fake.Put("3.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"9\" height=\"9\"><path d=\"M0 0H9V9Z\"/></svg>");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resource_depth = 2;
  Lowered l;
  Lower(Doc(ImageOf("1.svg")), l, fake, o);
  EXPECT_EQ(fake.Calls(), (size_t) 2);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
  Fake all;
  all.table = fake.table;
  Lowered deep;
  Lower(Doc(ImageOf("1.svg")), deep, all);
  EXPECT_EQ(all.Calls(), (size_t) 3);
  EXPECT_EQ(deep.Refusals(), (size_t) 0);
  EXPECT_GE(Fills(deep), (size_t) 1);
}

TEST(LowerResolverLimits, ADocumentThatIsItsOwnImageIsARecursionThatDrawsNothing) {
  Fake fake;
  fake.Put("self.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"9\" height=\"9\"><image width=\"9\" height=\"9\" href=\"self.svg\"/><path d=\"M0 0H3V3Z\"/></svg>");
  Lowered l;
  Lower(Doc(ImageOf("self.svg")), l, fake);
  EXPECT_EQ(fake.Calls(), (size_t) 1);
  EXPECT_GE(Fills(l), (size_t) 1);
}

//-----------------------------------------------------------------------------
// @import
//-----------------------------------------------------------------------------

TEST(LowerResolverStyle, AnImportedSheetEntersTheCascadeWhereItStandsAndLaterRulesBeatIt) {
  Fake fake;
  fake.Put("tests/resources/green.css", "#a { fill: green }");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.base_uri = "tests/structure/style/case.svg";
  Lowered l;
  Lower(Doc(Style("@import \"../../resources/green.css\"") + Shape("fill=\"red\"")), l, fake, o);
  ASSERT_EQ(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].kind, GVEC_RESOURCE_STYLE);
  EXPECT_EQ(fake.seen[0].url, "tests/resources/green.css");
  EXPECT_EQ(fake.seen[0].depth, (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_EQ(Fills(l), (size_t) 1);
  EXPECT_TRUE(IsGreen(FillOf(l, 0))) << "a rule beats the presentation attribute";
  Fake second;
  second.Put("a.css", "#a { fill: green }");
  Lowered later;
  Lower(Doc(Style("@import url(a.css); #a { fill: blue }") + Shape()), later, second);
  EXPECT_TRUE(IsBlue(FillOf(later, 0))) << "the rule after the import is later in the order of appearance";
  Fake third;
  third.Put("a.css", "#a { fill: green }");
  Lowered inline_first;
  Lower(Doc("<style>#a { fill: blue }</style><style>@import 'a.css';</style>" + Shape()), inline_first, third);
  EXPECT_TRUE(IsGreen(FillOf(inline_first, 0))) << "an import in a later style element is later than an earlier element's rules";
}

TEST(LowerResolverStyle, ImportsOfImportsAreResolvedAgainstTheSheetThatNamesThem) {
  Fake fake;
  fake.Put("https://h/css/a.css", "@import \"sub/b.css\"; #a { stroke: none }");
  fake.Put("https://h/css/sub/b.css", "@import \"../c.css\"; #a { fill: green }");
  fake.Put("https://h/css/c.css", "#a { fill: blue }");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.base_uri = "https://h/doc/x.svg";
  Lowered l;
  Lower(Doc(Style("@import \"../css/a.css\"") + Shape()), l, fake, o);
  ASSERT_EQ(fake.seen.size(), (size_t) 3);
  EXPECT_EQ(fake.seen[0].url, "https://h/css/a.css");
  EXPECT_EQ(fake.seen[0].depth, (size_t) 1);
  EXPECT_EQ(fake.seen[1].url, "https://h/css/sub/b.css");
  EXPECT_EQ(fake.seen[1].base, "https://h/css/a.css");
  EXPECT_EQ(fake.seen[1].depth, (size_t) 2);
  EXPECT_EQ(fake.seen[2].url, "https://h/css/c.css");
  EXPECT_EQ(fake.seen[2].depth, (size_t) 3);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_EQ(Fills(l), (size_t) 1);
  EXPECT_TRUE(IsGreen(FillOf(l, 0))) << "b's own rule is after c's, which it imported";
}

TEST(LowerResolverStyle, AnImportInALayerIsInThatLayerAndLosesToUnlayeredRules) {
  Fake fake;
  fake.Put("a.css", "#a { fill: red }");
  Lowered l;
  Lower(Doc(Style("@import \"a.css\" layer(base); #a { fill: green }") + Shape()), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_EQ(Fills(l), (size_t) 1);
  EXPECT_TRUE(IsGreen(FillOf(l, 0)));
  Fake second;
  second.Put("a.css", "#a { fill: red }");
  Lowered alone;
  Lower(Doc(Style("@import \"a.css\" layer(base);") + Shape("fill=\"blue\"")), alone, second);
  EXPECT_TRUE(IsRed(FillOf(alone, 0))) << "a rule of a layer still beats a presentation attribute";
}

TEST(LowerResolverStyle, AnImportWithAMediaOrSupportsConditionIsAppliedWhenItHoldsAndAScopeConditionIsRefusedAndNotAsked) {
  struct Row { const char * condition; bool applies; };
  for (const Row & row : {Row{"screen", true}, Row{"print", false}, Row{"(min-width: 10px)", true}, Row{"(max-width: 10px)", false},
           Row{"supports(display: block)", true}, Row{"supports(not (fill: red))", false}}) {
    Fake fake;
    fake.Put("a.css", "#a { fill: green }");
    Lowered l;
    Lower(Doc(Style(std::string("@import \"a.css\" ") + row.condition + ";") + Shape("fill=\"red\"")), l, fake);
    EXPECT_EQ(l.Refusal(GVEC_LOWER_STYLESHEET), nullptr) << row.condition;
    ASSERT_EQ(Fills(l), (size_t) 1) << row.condition;
    EXPECT_EQ(IsGreen(FillOf(l, 0)), row.applies) << row.condition;
  }
  for (const char * condition : {"scope(.x)", "scope"}) {
    Fake fake;
    fake.Put("a.css", "#a { fill: green }");
    Lowered l;
    Lower(Doc(Style(std::string("@import \"a.css\" ") + condition + ";") + Shape("fill=\"red\"")), l, fake);
    EXPECT_NE(l.Refusal(GVEC_LOWER_STYLESHEET), nullptr) << condition;
    EXPECT_EQ(fake.Calls(), (size_t) 0) << condition;
    ASSERT_EQ(Fills(l), (size_t) 1) << condition;
    EXPECT_TRUE(IsRed(FillOf(l, 0))) << condition << ": drawn as the document has it, the refusal says the sheet was not applied";
  }
}

TEST(LowerResolverStyle, ASheetThatCannotBeHadRefusesTheStyleElementWithTheReasonOfTheStatus) {
  struct Row { GVEC_Resource_Status status; const char * why; };
  for (const Row & row : {Row{GVEC_RESOURCE_REFUSED, "refuses"}, Row{GVEC_RESOURCE_NOT_FOUND, "does not find"}, Row{GVEC_RESOURCE_FAILED, "failed"}}) {
    Fake fake;
    fake.Status("a.css", row.status);
    Lowered l;
    Lower(Doc(Style("@import \"a.css\"") + Shape("fill=\"red\"")), l, fake);
    const GVEC_Diagnostic * d = l.Refusal(GVEC_LOWER_STYLESHEET);
    ASSERT_NE(d, nullptr) << row.status;
    ASSERT_NE(d->message, nullptr);
    EXPECT_NE(std::string(d->message).find(row.why), std::string::npos) << d->message;
  }
}

TEST(LowerResolverStyle, ACycleOfSheetsIsRefusedAndEndsInTwoRequests) {
  Fake fake;
  fake.Put("a.css", "@import \"b.css\"; #a { fill: green }");
  fake.Put("b.css", "@import \"a.css\"; #a { fill: blue }");
  Lowered l;
  Lower(Doc(Style("@import \"a.css\"") + Shape()), l, fake);
  const GVEC_Diagnostic * d = l.Refusal(GVEC_LOWER_STYLESHEET);
  ASSERT_NE(d, nullptr);
  EXPECT_NE(std::string(d->message).find("cycle"), std::string::npos) << d->message;
  EXPECT_EQ(fake.Calls(), (size_t) 2);
  Fake self;
  self.Put("s.css", "@import \"s.css\";");
  Lowered itself;
  Lower(Doc(Style("@import \"s.css\"") + Shape()), itself, self);
  EXPECT_NE(itself.Refusal(GVEC_LOWER_STYLESHEET), nullptr);
  EXPECT_EQ(self.Calls(), (size_t) 1);
}

TEST(LowerResolverStyle, AChainOfImportsPastTheDepthLimitIsRefused) {
  Fake fake;
  for (int i = 1; i <= 6; i++) {
    fake.Put(std::to_string(i) + ".css", "@import \"" + std::to_string(i + 1) + ".css\"; #a { fill: green }");
  }
  fake.Put("7.css", "#a { fill: green }");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resource_depth = 3;
  Lowered l;
  Lower(Doc(Style("@import \"1.css\"") + Shape()), l, fake, o);
  EXPECT_NE(l.Refusal(GVEC_LOWER_STYLESHEET), nullptr);
  EXPECT_EQ(fake.Calls(), (size_t) 3);
  Fake all;
  all.table = fake.table;
  Lowered deep;
  Lower(Doc(Style("@import \"1.css\"") + Shape()), deep, all);
  EXPECT_EQ(all.Calls(), (size_t) 7);
  EXPECT_EQ(deep.Refusals(), (size_t) 0);
}

TEST(LowerResolverStyle, AnImportedSheetWithConditionalRulesIsAppliedAndOneOfTooManyBytesIsRefused) {
  Fake fake;
  fake.Put("m.css", "@media screen { #a { fill: green } }");
  Lowered l;
  Lower(Doc(Style("@import \"m.css\"") + Shape()), l, fake);
  EXPECT_EQ(l.Refusal(GVEC_LOWER_STYLESHEET), nullptr);
  Fake big;
  Answer a;
  a.status = GVEC_RESOURCE_OK;
  a.raw_size = true;
  a.size = (size_t) -1 / 2;
  big.table["big.css"] = a;
  Lowered b;
  Lower(Doc(Style("@import \"big.css\"") + Shape()), b, big);
  EXPECT_NE(b.Refusal(GVEC_LOWER_STYLESHEET), nullptr);
}

TEST(LowerResolverStyle, HostileSheetsNeverCrashAndAreRefusedOrAppliedAsTheirRulesSay) {
  const std::string garbage[] = {std::string("\x00\x01\x02\xff\xfe{}@import", 12), std::string(100000, '{'), std::string(50000, '}'),
      "@import url(", "@import \"x\" layer(", "\xef\xbb\xbf#a{fill:green}", "#a{fill:green} @import \"late.css\";", "/* unterminated"};
  for (const std::string & g : garbage) {
    Fake fake;
    fake.Put("h.css", g);
    Lowered l;
    Lower(Doc(Style("@import \"h.css\"") + Shape()), l, fake);
    EXPECT_LE(fake.Calls(), (size_t) 2);
    EXPECT_EQ(l.edit_result, GVEC_OK);
  }
}

//-----------------------------------------------------------------------------
// `use` of an element of another document
//-----------------------------------------------------------------------------

namespace {

const char * const kOther =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" viewBox=\"0 0 50 50\">"
    "<style>#styled { fill: blue } .k { stroke: none }</style>"
    "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>"
    "<g id=\"inner\"><path d=\"M0 0H2V2Z\"/></g></defs>"
    "<path id=\"p\" d=\"M0 0H4V4H0Z\"/>"
    "<path id=\"styled\" class=\"k\" d=\"M0 0H4V4H0Z\" fill=\"red\"/>"
    "<path id=\"grad\" d=\"M0 0H4V4H0Z\" fill=\"url(#g)\"/>"
    "<g id=\"group\"><use href=\"#inner\" x=\"3\"/><path d=\"M0 0H1V1Z\"/></g>"
    "<symbol id=\"sym\" viewBox=\"0 0 10 10\"><path d=\"M0 0H10V10H0Z\"/></symbol>"
    "<path id=\"own-fill\" d=\"M0 0H4V4H0Z\" fill=\"green\"/>"
    "<path id=\"ctx\" d=\"M0 0H4V4H0Z\" fill=\"context-fill\"/>"
    "</svg>";

}  // namespace

TEST(LowerResolverUse, AnElementOfAnotherDocumentIsDrawnInheritingFromTheUse) {
  Fake fake;
  fake.Put("tests/o.svg", kOther, "image/svg+xml");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.base_uri = "tests/structure/use/case.svg";
  Lowered l;
  Lower(Doc(UseOf("../../o.svg#p", "fill=\"green\"")), l, fake, o);
  ASSERT_EQ(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].kind, GVEC_RESOURCE_DOCUMENT);
  EXPECT_EQ(fake.seen[0].reference, "../../o.svg");
  EXPECT_EQ(fake.seen[0].url, "tests/o.svg");
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_EQ(Fills(l), (size_t) 1);
  EXPECT_TRUE(IsGreen(FillOf(l, 0))) << "p has no fill of its own: it takes the use's";
}

TEST(LowerResolverUse, TheElementIsStyledByItsOwnDocumentsSheetsAndPaintedFromItsOwnIds) {
  Fake fake;
  fake.Put("o.svg", kOther);
  Lowered styled;
  Lower(Doc(UseOf("o.svg#styled", "fill=\"green\"")), styled, fake);
  ASSERT_EQ(Fills(styled), (size_t) 1);
  EXPECT_TRUE(IsBlue(FillOf(styled, 0))) << "the other document's rule beats its presentation attribute and the use's inheritance";
  Fake second;
  second.Put("o.svg", kOther);
  Lowered gradient;
  Lower(Doc(UseOf("o.svg#grad")), gradient, second);
  ASSERT_EQ(Fills(gradient), (size_t) 1);
  EXPECT_EQ(FillOf(gradient, 0).kind, GSHSCENE_PAINT_GRADIENT) << "url(#g) is the other document's g";
  EXPECT_EQ(gradient.Refusals(), (size_t) 0);
}

TEST(LowerResolverUse, AGroupOfTheOtherDocumentBringsItsOwnInternalReferences) {
  Fake fake;
  fake.Put("o.svg", kOther);
  Lowered l;
  Lower(Doc(UseOf("o.svg#group", "x=\"2\" fill=\"green\"")), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(Fills(l), (size_t) 2) << "the use inside the group draws the group's own #inner (a path); the path beside it is drawn";
}

TEST(LowerResolverUse, ASymbolOfTheOtherDocumentIsPlacedByTheUsesSizeAndAnExistingDocumentIsAskedOnceForManyUses) {
  Fake fake;
  fake.Put("o.svg", kOther);
  Lowered l;
  Lower(Doc(UseOf("o.svg#sym", "width=\"10\" height=\"10\"") + UseOf("o.svg#p") + UseOf("o.svg#own-fill") + UseOf("o.svg#sym", "x=\"1\"")), l, fake);
  EXPECT_EQ(fake.Calls(), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_GE(Fills(l), (size_t) 4);
}

TEST(LowerResolverUse, TheContextFillOfTheOtherDocumentsElementIsTheUsesColor) {
  Fake fake;
  fake.Put("o.svg", kOther);
  Lowered l;
  Lower(Doc(UseOf("o.svg#ctx", "fill=\"green\"")), l, fake);
  ASSERT_EQ(Fills(l), (size_t) 1);
  EXPECT_TRUE(IsGreen(FillOf(l, 0)));
  Fake second;
  second.Put("o.svg", kOther);
  Lowered server;
  Lower(Doc("<defs><linearGradient id=\"lg\"><stop offset=\"0\" stop-color=\"red\"/></linearGradient></defs>" + UseOf("o.svg#ctx", "fill=\"url(#lg)\"")), server, second);
  EXPECT_NE(server.Refusal(GVEC_LOWER_CONTEXT_PAINT), nullptr) << "a paint server is an element of the using document: it cannot fill the other's";
  Fake third;
  third.Put("o.svg", kOther);
  Lowered inherited;
  Lower(Doc("<defs><linearGradient id=\"lg\"><stop offset=\"0\" stop-color=\"red\"/></linearGradient></defs>" + UseOf("o.svg#p", "fill=\"url(#lg)\"")), inherited, third);
  EXPECT_NE(inherited.Refusal(GVEC_LOWER_CONTEXT_PAINT), nullptr) << "the element would inherit a server of the using document, which it cannot name";
  EXPECT_EQ(Fills(inherited), (size_t) 0);
}

TEST(LowerResolverUse, TheMarkersOfExternalElementsCountAgainstTheLoweringsLimit) {
  Fake fake;
  fake.Put("m.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><defs><marker id=\"m\" markerWidth=\"2\" markerHeight=\"2\"><path d=\"M0 0H1V1Z\"/></marker></defs>"
                    "<path id=\"mk\" d=\"M0 0L5 5\" marker-start=\"url(#m)\"/></svg>");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_marker_instances = 2;
  Lowered l;
  Lower(Doc(UseOf("m.svg#mk") + UseOf("m.svg#mk", "x=\"1\"") + UseOf("m.svg#mk", "x=\"2\"") + UseOf("m.svg#mk", "x=\"3\"") + UseOf("m.svg#mk", "x=\"4\"")), l, fake, o);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr) << "five uses, each with a marker, against a limit of two marker instances for the whole lowering";
  Fake roomy;
  roomy.Put("m.svg", fake.table["m.svg"].bytes);
  Lowered all;
  Lower(Doc(UseOf("m.svg#mk") + UseOf("m.svg#mk", "x=\"1\"") + UseOf("m.svg#mk", "x=\"2\"") + UseOf("m.svg#mk", "x=\"3\"") + UseOf("m.svg#mk", "x=\"4\"")), all, roomy);
  EXPECT_EQ(all.Refusals(), (size_t) 0);
}

TEST(LowerResolverUse, ATextInAnExternalDocumentIsDrawnWithTheFontsOfTheLowering) {
  // The external document is lowered in a context of its own, whose style is read through the one of the parent: they share the engine that
  // answers the font units, or a property of the text is read from a style the other engine made and the text is refused.
  GFNT_FontSet * set = nullptr;
  const char * dirs[] = {"tests/data/fonts"};
  static const GFNT_FontAlias aliases[] = {{"sans-serif", "Vector Test"}};
  ASSERT_EQ(gfnt_fontset_scan(dirs, 1, aliases, 1, nullptr, &set, nullptr), GFNT_OK);
  GVEC_Fonts * fonts = nullptr;
  ASSERT_EQ(gvec_fonts_new(set, nullptr, &fonts), GVEC_OK);
  Fake fake;
  fake.Put("t.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><text id=\"t\" font-family=\"sans-serif\" font-size=\"10\">A</text></svg>");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.fonts = fonts;
  Lowered l;
  Lower(Doc(UseOf("t.svg#t")), l, fake, o);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_GE(Fills(l), (size_t) 1);
  gvec_fonts_free(fonts);
  gfnt_fontset_free(set);
}

TEST(LowerResolverUse, ANestedExternalReferenceIsResolvedAgainstTheDocumentThatHoldsIt) {
  Fake fake;
  fake.Put("https://h/a/b.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"x\"><use href=\"../c/d.svg#y\"/></g></svg>");
  fake.Put("https://h/c/d.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><path id=\"y\" d=\"M0 0H4V4H0Z\" fill=\"green\"/></svg>");
  Lowered l;
  Lower(Doc(UseOf("https://h/a/b.svg#x")), l, fake);
  ASSERT_EQ(fake.seen.size(), (size_t) 2);
  EXPECT_EQ(fake.seen[1].url, "https://h/c/d.svg");
  EXPECT_EQ(fake.seen[1].base, "https://h/a/b.svg");
  EXPECT_EQ(fake.seen[1].depth, (size_t) 2);
  ASSERT_EQ(Fills(l), (size_t) 1);
  EXPECT_TRUE(IsGreen(FillOf(l, 0)));
}

TEST(LowerResolverUse, WhatIsNotThereDrawsNothingAndSaysSoWithoutRefusing) {
  struct Row { const char * href; const char * what; };
  Fake fake;
  fake.Put("o.svg", kOther);
  fake.Put("text.svg", "this is not xml");
  fake.Status("gone.svg", GVEC_RESOURCE_NOT_FOUND);
  fake.Status("fail.svg", GVEC_RESOURCE_FAILED);
  for (const Row & row : {Row{"o.svg#nothing", "an id the document lacks"}, Row{"o.svg", "no id"}, Row{"o.svg#", "an empty id"},
           Row{"text.svg#p", "a document that is not SVG"}, Row{"gone.svg#p", "not found"}, Row{"fail.svg#p", "a failed resolver"},
           Row{"o.svg#%zz", "a bad escape"}, Row{"#p", "a fragment alone is the document's own (nothing named p)"}}) {
    Lowered l;
    Lower(Doc(UseOf(row.href)), l, fake);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << row.what;
    EXPECT_EQ(Fills(l), (size_t) 0) << row.what;
  }
  Lowered l;
  Lower(Doc(UseOf("o.svg#nothing")), l, fake);
  EXPECT_NE(Diag(l, GVEC_DIAG_REFERENCE_MISSING), nullptr);
}

TEST(LowerResolverUse, APercentEncodedFragmentNamesTheElementOfThatId) {
  Fake fake;
  fake.Put("o.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><path id=\"a/b\" d=\"M0 0H4V4H0Z\"/></svg>");
  Lowered l;
  Lower(Doc(UseOf("o.svg#a%2Fb")), l, fake);
  EXPECT_EQ(Fills(l), (size_t) 1);
}

TEST(LowerResolverUse, TheResolversRefusalLimitsAndAnOverlongDocumentAreRefusedAsTheirKindOfReason) {
  Fake fake;
  fake.Status("no.svg", GVEC_RESOURCE_REFUSED);
  Answer big;
  big.status = GVEC_RESOURCE_OK;
  big.raw_size = true;
  big.size = (size_t) -1 / 2;
  fake.table["big.svg"] = big;
  Lowered refused, large;
  Lower(Doc(UseOf("no.svg#p")), refused, fake);
  EXPECT_NE(refused.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  Lower(Doc(UseOf("big.svg#p")), large, fake);
  EXPECT_NE(large.Refusal(GVEC_LOWER_LIMIT), nullptr);
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resources = 1;
  Fake two;
  two.Put("o.svg", kOther);
  two.Put("q.svg", kOther);
  Lowered l;
  Lower(Doc(UseOf("o.svg#p") + UseOf("q.svg#p")), l, two, o);
  EXPECT_EQ(two.Calls(), (size_t) 1);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerResolverUse, DocumentsThatNameOneAnotherInACycleAreRefusedAndTheLoweringEnds) {
  Fake fake;
  fake.Put("a.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"y\"><use href=\"b.svg#x\"/></g></svg>");
  fake.Put("b.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"x\"><use href=\"a.svg#y\"/><path d=\"M0 0H3V3Z\"/></g></svg>");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.base_uri = "a.svg";
  Lowered l;
  Lower(Doc(UseOf("b.svg#x")), l, fake, o);  // the document being lowered is a.svg's twin: its use names b.svg, which names a.svg, which names b.svg
  EXPECT_NE(l.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  EXPECT_LE(fake.Calls(), (size_t) 2);
  Fake self;
  self.Put("s.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"x\"><use href=\"s.svg#x\"/><path d=\"M0 0H3V3Z\"/></g></svg>");
  Lowered itself;
  Lower(Doc(UseOf("s.svg#x")), itself, self);
  EXPECT_NE(itself.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  EXPECT_EQ(self.Calls(), (size_t) 1);
  EXPECT_GE(Fills(itself), (size_t) 1) << "what is outside the cycle is still drawn";
}

TEST(LowerResolverUse, ADeepChainOfDocumentsIsStoppedByTheDepthLimit) {
  Fake fake;
  for (int i = 1; i <= 10; i++) {
    fake.Put(std::to_string(i) + ".svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"x\"><use href=\"" + std::to_string(i + 1) + ".svg#x\"/></g></svg>");
  }
  fake.Put("11.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"x\"><path d=\"M0 0H3V3Z\"/></g></svg>");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resource_depth = 4;
  Lowered l;
  Lower(Doc(UseOf("1.svg#x")), l, fake, o);
  EXPECT_EQ(fake.Calls(), (size_t) 4);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerResolverUse, AFanOutOfDocumentsThatNameEachOtherIsBoundedNotExponential) {
  // b.svg has 12 uses of c.svg#x and c.svg has 12 uses of b.svg#x: 12^n instantiations without the bounds.
  std::string uses_c, uses_b;
  for (int i = 0; i < 12; i++) {
    uses_c += "<use href=\"c.svg#x\" x=\"" + std::to_string(i) + "\"/>";
    uses_b += "<use href=\"b.svg#x\" x=\"" + std::to_string(i) + "\"/>";
  }
  Fake fake;
  fake.Put("b.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"x\">" + uses_c + "<path d=\"M0 0H3V3Z\"/></g></svg>");
  fake.Put("c.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"x\">" + uses_b + "<path d=\"M0 0H3V3Z\"/></g></svg>");
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resource_instances = 200;
  Lowered l;
  Lower(Doc(UseOf("b.svg#x")), l, fake, o);
  EXPECT_LE(fake.Calls(), (size_t) 2);
  EXPECT_EQ(l.edit_result, GVEC_OK);
  bool said = false;
  for (size_t i = 0; i < l.by_list.count; i++) {
    const GVEC_Diagnostic & d = l.by_list.items[i];
    said = said || (d.code == GVEC_DIAG_LOWER_REFUSED && d.detail == (uint32_t) GVEC_LOWER_LIMIT && d.message &&
        std::string(d.message).find("max_resource_instances") != std::string::npos);
  }
  EXPECT_TRUE(said) << "the instantiations past max_resource_instances are refused, and the reason names the limit";
}

TEST(LowerResolverUse, TheInstanceLimitIsExactlyThatManyUsesAndTheRestAreRefused) {
  Fake fake;
  fake.Put("o.svg", kOther);
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_resource_instances = 3;
  Lowered l;
  Lower(Doc(UseOf("o.svg#p") + UseOf("o.svg#p", "x=\"1\"") + UseOf("o.svg#p", "x=\"2\"") + UseOf("o.svg#p", "x=\"3\"") + UseOf("o.svg#p", "x=\"4\"")), l, fake, o);
  EXPECT_EQ(Fills(l), (size_t) 3);
  EXPECT_EQ(fake.Calls(), (size_t) 1);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerResolverUse, ManyUsesOfOneExternalDocumentAreWithinTheDefaultInstanceLimit) {
  Fake fake;
  fake.Put("o.svg", kOther);
  std::string uses;
  for (int i = 0; i < 300; i++) {
    uses += UseOf("o.svg#p", "x=\"" + std::to_string(i) + "\"");
  }
  Lowered l;
  Lower(Doc(uses), l, fake);
  EXPECT_EQ(fake.Calls(), (size_t) 1);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(Fills(l), (size_t) 300);
}

TEST(LowerResolverUse, AnExternalDocumentWithItsOwnSheetThatCannotBeAppliedIsReportedAtThatSheet) {
  Fake fake;
  fake.Put("o.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><style>@import \"nope.css\"</style><path id=\"p\" d=\"M0 0H4V4H0Z\"/></svg>");
  Lowered l;
  Lower(Doc(UseOf("o.svg#p", "fill=\"green\"")), l, fake);
  EXPECT_NE(l.Refusal(GVEC_LOWER_STYLESHEET), nullptr);
  EXPECT_EQ(fake.Calls("nope.css"), (size_t) 1);
  EXPECT_EQ(fake.seen.back().base, "o.svg");
  EXPECT_EQ(fake.seen.back().depth, (size_t) 2);
}

TEST(LowerResolverUse, TheDocumentBeingLoweredIsIndependentOfWhatTheOtherDocumentDoes) {
  // The same lowering twice gives the same list: the resolver is asked afresh each time, its cache is the lowering's.
  Fake fake;
  fake.Put("o.svg", kOther);
  Lowered a, b;
  Lower(Doc(UseOf("o.svg#p", "fill=\"green\"")), a, fake);
  Lower(Doc(UseOf("o.svg#p", "fill=\"green\"")), b, fake);
  EXPECT_EQ(fake.Calls(), (size_t) 2);
  EXPECT_EQ(a.Items(), b.Items());
}

//-----------------------------------------------------------------------------
// Allocation failure
//-----------------------------------------------------------------------------

TEST(LowerResolverAllocation, EveryFailureOnTheResolverPathIsOutOfMemoryAndNothingLeaks) {
  Fake fake;
  std::vector<unsigned char> png = FileBytes("tests/data/images/rgb-3x2.png");
  fake.Put("a.png", std::string(png.begin(), png.end()));
  fake.Put("pic.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"9\" height=\"9\"><style>@import \"in.css\"</style><path d=\"M0 0H9V9Z\"/></svg>");
  fake.Put("in.css", "path { fill: green }");
  fake.Put("o.svg", kOther);
  fake.Put("s.css", "#a { fill: green } @import \"t.css\";");
  fake.Put("t.css", "#a { stroke: red }");
  const std::string text = Doc(Style("@import \"s.css\"") + Shape() + ImageOf("a.png") + ImageOf("pic.svg") + UseOf("o.svg#group") + UseOf("o.svg#styled"));
  long total = 0;
  for (long fail_at = 0; fail_at < 4000; fail_at++) {
    Failing alloc;
    alloc.fail_at = fail_at == 0 ? -1 : fail_at;
    GVEC_Stream * stream = nullptr;
    ASSERT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
    GVEC_Load_Options load;
    std::memset(&load, 0, sizeof load);
    load.allocator = &alloc.a;
    GVEC_Doc * doc = nullptr;
    GVEC_Diagnostics diagnostics;
    gvec_diagnostics_init(&diagnostics, &alloc.a);
    GVEC_Result r = gvec_doc_load(stream, &load, &diagnostics, &doc);
    gvec_stream_destroy(stream);
    long loaded = alloc.attempts;
    GSHSCENE_Scene * scene = nullptr;
    GSHSCENE_List * list = nullptr;
    if (r == GVEC_OK) {
      ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
      ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
      GVEC_Lower_Options o;
      std::memset(&o, 0, sizeof o);
      fake.Fill(o);
      o.store = gshscene_scene_store(scene);
      r = gvec_doc_lower_list(doc, &o, list, &diagnostics);
      EXPECT_TRUE(r == GVEC_OK || r == GVEC_ERR_OOM) << fail_at << " " << gvec_result_string(r);
    }
    long used = alloc.attempts - loaded;
    gshscene_list_destroy(list);
    gshscene_scene_destroy(scene);
    gvec_doc_free(doc);
    gvec_diagnostics_destroy(&diagnostics);
    EXPECT_EQ(alloc.outstanding, 0) << "leak after failing allocation " << fail_at;
    if (fail_at == 0) {
      total = alloc.attempts;
      ASSERT_EQ(r, GVEC_OK);
      ASSERT_GT(used, 50);
    } else if (fail_at > total) {
      break;
    }
  }
  EXPECT_GT(total, 100);
}

//-----------------------------------------------------------------------------
// @color-profile
//-----------------------------------------------------------------------------

namespace {

std::string Profile(const char * name) {
  std::vector<unsigned char> bytes = FileBytes(std::string("tests/data/icc/") + name);
  return std::string(bytes.begin(), bytes.end());
}

// The first fill of a lowering that is a plain color.
bool FirstFill(const Lowered & l, GSHSCENE_Paint * out) {
  for (const GSHSCENE_Item & item : FillItems(l)) {
    if (item.paint.kind == GSHSCENE_PAINT_SOLID) {
      *out = item.paint;
      return true;
    }
  }
  return false;
}

std::string ProfileDoc(const std::string & fill, const char * src = "p.icc") {
  return Doc(Style(std::string("@color-profile --p { src: url(") + src + "); }") + Shape("fill=\"" + fill + "\""));
}

}  // namespace

//-----------------------------------------------------------------------------
// @font-face
//-----------------------------------------------------------------------------

namespace {

struct TestFonts {
  GFNT_FontSet * set = nullptr;
  GVEC_Fonts * fonts = nullptr;
  TestFonts() {
    const char * dirs[] = {"tests/data/fonts"};
    static const GFNT_FontAlias aliases[] = {{"sans-serif", "Vector Test"}, {"serif", "Vector Test"}};
    EXPECT_EQ(gfnt_fontset_scan(dirs, 1, aliases, 2, nullptr, &set, nullptr), GFNT_OK);
    EXPECT_EQ(gvec_fonts_new(set, nullptr, &fonts), GVEC_OK);
  }
  ~TestFonts() {
    gvec_fonts_free(fonts);
    gfnt_fontset_free(set);
  }
};

/** The advance between the two glyphs of `AA` set in the family `E` at 100: 60 for the regular face of the test font, 70 for the bold. */
double Advance(Fake & fake, const std::string & rule, TestFonts & f, Lowered & l) {
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.fonts = f.fonts;
  Lower(Svg("width=\"400\" height=\"200\" font-size=\"100\"", Style(rule) + "<text font-family=\"E\" y=\"100\">AA</text>"), l, fake, o);
  GSHSCENE_Glyph_Run run;
  if (l.Items() < 1 || gshscene_resource_glyph_run(l.Item(0).resource, &run) != GSHSCENE_OK || run.glyph_count != 2) {
    return -1;
  }
  return run.glyphs[1].x - run.glyphs[0].x;
}

}  // namespace

TEST(LowerResolverFont, AFontFaceThatNamesAFileIsAskedOfTheResolverOnceAndItsFontIsUsed) {
  TestFonts f;
  Fake fake;
  std::vector<unsigned char> bold = FileBytes("tests/data/fonts/vector-test-bold.ttf");
  fake.Put("e.ttf", std::string(bold.begin(), bold.end()));
  Lowered l;
  EXPECT_EQ(Advance(fake, "@font-face { font-family: E; src: url(e.ttf); }", f, l), 70.0);
  EXPECT_EQ(fake.Calls("e.ttf"), (size_t) 1);
  ASSERT_GE(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].kind, GVEC_RESOURCE_FONT);
  EXPECT_EQ(fake.seen[0].reference, "e.ttf");
  EXPECT_EQ(fake.seen[0].max_bytes, (size_t) 32 << 20);
}

TEST(LowerResolverFont, AFontTheResolverDoesNotGiveIsPassedOverForTheNextSourceOrTheFonts) {
  TestFonts f;
  Fake fake;
  std::vector<unsigned char> bold = FileBytes("tests/data/fonts/vector-test-bold.ttf");
  fake.Put("b.ttf", std::string(bold.begin(), bold.end()));
  fake.Put("junk.ttf", "not a font at all");
  fake.Status("refused.ttf", GVEC_RESOURCE_REFUSED);
  Lowered next, junk, refused, missing;
  EXPECT_EQ(Advance(fake, "@font-face { font-family: E; src: url(gone.ttf), url(b.ttf); }", f, next), 70.0);
  EXPECT_EQ(Advance(fake, "@font-face { font-family: E; src: url(junk.ttf), url(b.ttf); }", f, junk), 70.0);
  EXPECT_EQ(Advance(fake, "@font-face { font-family: E; src: url(refused.ttf); }", f, refused), 60.0) << "the fonts' own serif";
  EXPECT_EQ(Advance(fake, "@font-face { font-family: E; src: url(gone.ttf); }", f, missing), 60.0);
  EXPECT_EQ(refused.Refusals(), (size_t) 0);
}

TEST(LowerResolverFont, WithNoResolverOnlyADataUriIsAFont) {
  TestFonts f;
  Lowered l;
  GVEC_Lower_Options o;
  std::memset(&o, 0, sizeof o);
  o.fonts = f.fonts;
  LowerSvg(Svg("width=\"400\" height=\"200\" font-size=\"100\"", "<style>@font-face { font-family: E; src: url(e.ttf); }</style><text font-family=\"E\" y=\"100\">AA</text>"), l, 0, 0, &o);
  GSHSCENE_Glyph_Run run;
  ASSERT_EQ(gshscene_resource_glyph_run(l.Item(0).resource, &run), GSHSCENE_OK);
  EXPECT_EQ(run.glyphs[1].x - run.glyphs[0].x, 60.0);
}

TEST(LowerResolverProfile, AColorInTheSpaceOfAProfileIsConvertedThroughItToRec2020) {
  Fake fake;
  fake.Put("p.icc", Profile("srgb_g22.icc"));
  Lowered l;
  Lower(ProfileDoc("color(--p 1 0 0)"), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Paint p;
  ASSERT_TRUE(FirstFill(l, &p));
  // sRGB red in linear Rec. 2020 (ITU-R BT.2087)
  EXPECT_EQ(p.space.primaries, GCOL_PRIMARIES_BT2020);
  EXPECT_EQ(p.space.transfer, GCOL_TRANSFER_LINEAR);
  EXPECT_NEAR(p.rgba[0], 0.6274, 1e-3);
  EXPECT_NEAR(p.rgba[1], 0.0691, 1e-3);
  EXPECT_NEAR(p.rgba[2], 0.0164, 1e-3);
  ASSERT_EQ(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].kind, GVEC_RESOURCE_COLOR_PROFILE);
  EXPECT_EQ(fake.seen[0].reference, "p.icc");
  EXPECT_EQ(fake.seen[0].max_bytes, (size_t) 32 << 20);
}

TEST(LowerResolverProfile, TheProfileIsAskedOnceHoweverManyColorsNameIt) {
  Fake fake;
  fake.Put("p.icc", Profile("srgb_g22.icc"));
  Lowered l;
  Lower(Doc(Style("@color-profile --p { src: url(p.icc); }") + Shape("fill=\"color(--p 1 0 0)\" stroke=\"color(--p 0 0 1)\"") +
            "<path d=\"M5 5h4v4h-4z\" fill=\"color(--p 0 1 0 / 0.5)\"/>"),
      l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(fake.Calls(), (size_t) 1);
}

TEST(LowerResolverProfile, AProfileThatSwapsRedAndGreenSwapsThem) {
  Fake fake;
  fake.Put("p.icc", Profile("swap_rg.icc"));
  Lowered l;
  Lower(ProfileDoc("color(--p 1 0 0)"), l, fake);
  GSHSCENE_Paint p;
  ASSERT_TRUE(FirstFill(l, &p));
  // the device's red is the primary the profile gives it: sRGB's green, in Rec. 2020 (0.3293, 0.9195, 0.0880)
  EXPECT_NEAR(p.rgba[0], 0.3293, 2e-3);
  EXPECT_NEAR(p.rgba[1], 0.9195, 2e-3);
  EXPECT_NEAR(p.rgba[2], 0.0880, 2e-3);
}

TEST(LowerResolverProfile, MoreProfilesThanTheFirstTableHoldsAreAllKept) {
  // The table of profiles begins with room for four; each of five names has its own entry, and a failure of the allocator at the growth is out of memory.
  Fake fake;
  std::string rules, shapes;
  for (int i = 0; i < 5; i++) {
    const std::string name = "--p" + std::to_string(i);
    fake.Put("p" + std::to_string(i) + ".icc", Profile("srgb_g22.icc"));
    rules += "@color-profile " + name + " { src: url(p" + std::to_string(i) + ".icc); } ";
    shapes += "<path d=\"M" + std::to_string(i) + " 5h2v2h-2z\" fill=\"color(" + name + " 1 0 0)\"/>";
  }
  const std::string text = Doc(Style(rules) + shapes);
  Lowered l;
  Lower(text, l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(fake.Calls(), (size_t) 5) << "each name is a profile of its own";
  long total = 0;
  for (long fail_at = 0; fail_at < 4000; fail_at++) {
    Failing alloc;
    alloc.fail_at = fail_at == 0 ? -1 : fail_at;
    GVEC_Stream * stream = nullptr;
    ASSERT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
    GVEC_Load_Options load;
    std::memset(&load, 0, sizeof load);
    load.allocator = &alloc.a;
    GVEC_Doc * doc = nullptr;
    GVEC_Diagnostics diagnostics;
    gvec_diagnostics_init(&diagnostics, &alloc.a);
    GVEC_Result r = gvec_doc_load(stream, &load, &diagnostics, &doc);
    gvec_stream_destroy(stream);
    GSHSCENE_Scene * scene = nullptr;
    GSHSCENE_List * list = nullptr;
    if (r == GVEC_OK) {
      ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
      ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
      GVEC_Lower_Options o;
      std::memset(&o, 0, sizeof o);
      fake.Fill(o);
      o.store = gshscene_scene_store(scene);
      r = gvec_doc_lower_list(doc, &o, list, &diagnostics);
      EXPECT_TRUE(r == GVEC_OK || r == GVEC_ERR_OOM) << fail_at << " " << gvec_result_string(r);
    }
    gshscene_list_destroy(list);
    gshscene_scene_destroy(scene);
    gvec_doc_free(doc);
    gvec_diagnostics_destroy(&diagnostics);
    EXPECT_EQ(alloc.outstanding, 0) << "leak after failing allocation " << fail_at;
    if (fail_at == 0) {
      total = alloc.attempts;
      ASSERT_EQ(r, GVEC_OK);
    }
    else if (fail_at > total) {
      break;
    }
  }
}

TEST(LowerResolverProfile, AGrayProfileTakesOneChannel) {
  Fake fake;
  fake.Put("p.icc", Profile("gray_g22.icc"));
  Lowered l;
  Lower(ProfileDoc("color(--p 0.5)"), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Paint p;
  ASSERT_TRUE(FirstFill(l, &p));
  EXPECT_NEAR(p.rgba[0], std::pow(0.5, 2.2), 2e-3);  // a neutral: the same in each channel
  EXPECT_NEAR(p.rgba[1], p.rgba[0], 1e-3);
  EXPECT_NEAR(p.rgba[2], p.rgba[0], 1e-3);
  Fake other;
  other.Put("p.icc", Profile("gray_g22.icc"));
  Lowered wrong;
  Lower(ProfileDoc("color(--p 0.5 0.5 0.5)"), wrong, other);
  EXPECT_NE(wrong.Refusal(GVEC_LOWER_UNSUPPORTED_COLOR_SPACE), nullptr) << "three channels for a gray profile";
}

TEST(LowerResolverProfile, ATableProfileIsAppliedAndAlphaIsKept) {
  Fake fake;
  fake.Put("p.icc", Profile("rgb_mab.icc"));
  Lowered l;
  Lower(ProfileDoc("color(--p 0.25 0.5 0.75 / 0.5)"), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Paint p;
  ASSERT_TRUE(FirstFill(l, &p));
  EXPECT_NEAR(p.rgba[3], 0.5, 1e-9);
}

TEST(LowerResolverProfile, DeviceCmykUsesAProfileThatTheDocumentNamesForIt) {
  Fake fake;
  fake.Put("c.icc", Profile("cmyk_mft2.icc"));
  Lowered with;
  Lower(Doc(Style("@color-profile device-cmyk { src: url(c.icc); }") + Shape("fill=\"device-cmyk(0 0 0 0)\"")), with, fake);
  EXPECT_EQ(with.Refusals(), (size_t) 0);
  GSHSCENE_Paint p;
  ASSERT_TRUE(FirstFill(with, &p));
  EXPECT_EQ(p.space.primaries, GCOL_PRIMARIES_BT2020) << "converted through the profile, not the naive sRGB conversion";
  EXPECT_GT(p.rgba[1], 0.8) << "no ink on the page is paper white (a CMYK profile read as light would give black)";
  EXPECT_EQ(fake.seen.size(), (size_t) 1);
  Lowered naive;
  Fake none;
  Lower(Doc(Shape("fill=\"device-cmyk(0 0 0 0)\"")), naive, none);
  ASSERT_TRUE(FirstFill(naive, &p));
  EXPECT_EQ(p.space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(none.seen.size(), (size_t) 0) << "with no profile named, nothing is asked";
}

TEST(LowerResolverProfile, AProfileThatCannotBeHadRefusesTheElementAndSaysWhy) {
  for (GVEC_Resource_Status status : {GVEC_RESOURCE_REFUSED, GVEC_RESOURCE_NOT_FOUND, GVEC_RESOURCE_FAILED}) {
    Fake fake;
    fake.Status("p.icc", status);
    Lowered l;
    Lower(ProfileDoc("color(--p 1 0 0)"), l, fake);
    EXPECT_NE(l.Refusal(GVEC_LOWER_UNSUPPORTED_COLOR_SPACE), nullptr) << "status " << (int) status;
  }
  Fake junk;
  junk.Put("p.icc", "this is not an ICC profile");
  Lowered bad;
  Lower(ProfileDoc("color(--p 1 0 0)"), bad, junk);
  EXPECT_NE(bad.Refusal(GVEC_LOWER_UNSUPPORTED_COLOR_SPACE), nullptr);
  Lowered nothing;  // no resolver at all
  LowerSvg(ProfileDoc("color(--p 1 0 0)"), nothing);
  EXPECT_NE(nothing.Refusal(GVEC_LOWER_UNSUPPORTED_COLOR_SPACE), nullptr);
}

//-----------------------------------------------------------------------------
// Clips, masks, filters and paint servers of another document
//-----------------------------------------------------------------------------

namespace {

std::string Other(const std::string & body) {
  return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\">" + body + "</svg>";
}

size_t Count(const Lowered & l, GSHSCENE_Item_Kind kind) {
  std::vector<GSHSCENE_Item> all;
  Collect(l.list, all);
  size_t n = 0;
  for (const GSHSCENE_Item & item : all) {
    n += item.kind == kind;
  }
  return n;
}

}  // namespace

TEST(LowerResolverProperty, AClipPathOfAnotherDocumentClipsTheElement) {
  Fake fake;
  fake.Put("c.svg", Other("<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/></clipPath>"));
  Lowered l;
  Lower(Doc("<rect width=\"20\" height=\"20\" clip-path=\"url(c.svg#c)\"/>"), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(Count(l, GSHSCENE_ITEM_CLIP_PUSH), (size_t) 1);
  ASSERT_GE(fake.seen.size(), (size_t) 1);
  EXPECT_EQ(fake.seen[0].kind, GVEC_RESOURCE_DOCUMENT);
  EXPECT_EQ(fake.seen[0].url, "c.svg");
}

TEST(LowerResolverProperty, TheSameMarkupInTwoDocumentsIsTwoClips) {
  // a clipPath of the same id and shape in the document and in the other one, differing only in size: neither may be taken for the other
  Fake fake;
  fake.Put("c.svg", Other("<clipPath id=\"c\"><rect width=\"5\" height=\"5\"/></clipPath>"));
  Lowered l;
  Lower(Doc("<clipPath id=\"c\"><rect width=\"9\" height=\"9\"/></clipPath><rect width=\"20\" height=\"20\" clip-path=\"url(#c)\"/>"
            "<rect width=\"20\" height=\"20\" clip-path=\"url(c.svg#c)\"/>"),
      l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  std::vector<GSHSCENE_Item> all;
  Collect(l.list, all);
  std::vector<double> widths;
  for (const GSHSCENE_Item & item : all) {
    GSHGEOM_Bounds b;
    if (item.kind == GSHSCENE_ITEM_CLIP_PUSH && gshscene_resource_path(item.resource) &&
        gshgeom_path_bounds(gshscene_resource_path(item.resource), &b) == GSHGEOM_OK) {
      widths.push_back(b.max.x - b.min.x);
    }
  }
  ASSERT_EQ(widths.size(), (size_t) 2);
  EXPECT_EQ(widths[0], 9.0);
  EXPECT_EQ(widths[1], 5.0);
}

TEST(LowerResolverProperty, TheSameMaskMarkupInTwoDocumentsIsTwoMasksInTheEdit) {
  // the nodes of a mask are kept in the edit under keys made from the element's key: the same markup in two documents must not be the same node
  Fake fake;
  fake.Put("m.svg", Other("<mask id=\"m\"><rect width=\"20\" height=\"20\" fill=\"white\"/></mask>"));
  Lowered l;
  Lower(Doc("<mask id=\"m\"><rect width=\"20\" height=\"20\" fill=\"white\"/></mask><rect width=\"10\" height=\"10\" mask=\"url(#m)\"/>"
            "<rect x=\"10\" width=\"10\" height=\"10\" mask=\"url(m.svg#m)\"/>"),
      l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  size_t masked = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    masked += l.Item(i).kind == GSHSCENE_ITEM_GROUP_PUSH && l.Item(i).group.soft_mask;
  }
  EXPECT_EQ(masked, (size_t) 2);
  size_t from_edit = 0;
  for (size_t i = 0; l.emitted && i < gshscene_list_count(l.emitted); i++) {
    const GSHSCENE_Item * item = nullptr;
    ASSERT_EQ(gshscene_list_item(l.emitted, i, &item), GSHSCENE_OK);
    from_edit += item->kind == GSHSCENE_ITEM_GROUP_PUSH && item->group.soft_mask;
  }
  EXPECT_EQ(from_edit, (size_t) 2) << "the published edit holds both";
}

TEST(LowerResolverProperty, AGradientAndAPatternOfAnotherDocumentPaint) {
  Fake fake;
  fake.Put("g.svg", Other("<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>"
                           "<pattern id=\"p\" width=\"4\" height=\"4\" patternUnits=\"userSpaceOnUse\"><rect width=\"2\" height=\"2\" fill=\"green\"/></pattern>"));
  Lowered l;
  Lower(Doc("<rect width=\"10\" height=\"10\" fill=\"url(g.svg#g)\"/><rect width=\"10\" height=\"10\" fill=\"url(g.svg#p)\"/>"), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_EQ(Fills(l), (size_t) 2);
  EXPECT_NE(FillOf(l, 0).kind, GSHSCENE_PAINT_SOLID) << "a gradient";
  EXPECT_NE(FillOf(l, 1).kind, GSHSCENE_PAINT_SOLID) << "a pattern";
  EXPECT_EQ(fake.Calls("g.svg"), (size_t) 1) << "the document is asked once";
}

TEST(LowerResolverProperty, AMaskAndAFilterOfAnotherDocumentApply) {
  Fake fake;
  fake.Put("m.svg", Other("<mask id=\"m\"><rect width=\"20\" height=\"20\" fill=\"white\"/></mask><filter id=\"f\"><feOffset dx=\"2\"/></filter>"));
  Lowered masked, filtered;
  Lower(Doc("<rect width=\"10\" height=\"10\" mask=\"url(m.svg#m)\"/>"), masked, fake);
  EXPECT_EQ(masked.Refusals(), (size_t) 0);
  bool soft = false;
  std::vector<GSHSCENE_Item> all;
  Collect(masked.list, all);
  for (const GSHSCENE_Item & item : all) {
    soft = soft || (item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.soft_mask);
  }
  EXPECT_TRUE(soft);
  Lower(Doc("<rect width=\"10\" height=\"10\" filter=\"url(m.svg#f)\"/>"), filtered, fake);
  EXPECT_EQ(filtered.Refusals(), (size_t) 0);
  bool filter = false;
  Collect(filtered.list, all = {});
  for (const GSHSCENE_Item & item : all) {
    filter = filter || (item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.filter);
  }
  EXPECT_TRUE(filter);
}

TEST(LowerResolverProperty, AnElementOfAnotherDocumentReadsItsOwnReferencesAndItsOwnStyle) {
  // the other document's clipPath is clipped by another of its own, and is styled by its own sheet: neither is the document's
  Fake fake;
  fake.Put("c.svg", Other("<style>#inner rect { }</style><clipPath id=\"inner\"><rect width=\"3\" height=\"3\"/></clipPath>"
                          "<clipPath id=\"c\" clip-path=\"url(#inner)\"><rect width=\"5\" height=\"5\"/></clipPath>"));
  Lowered l;
  Lower(Doc("<rect width=\"20\" height=\"20\" clip-path=\"url(c.svg#c)\"/>"), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(Count(l, GSHSCENE_ITEM_CLIP_PUSH), (size_t) 2) << "the clipPath and the clip of the clipPath";
}

TEST(LowerResolverProperty, AMarkerOfAnotherDocumentIsDrawnAtTheVertices) {
  Fake fake;
  fake.Put("m.svg", Other("<marker id=\"m\" markerWidth=\"4\" markerHeight=\"4\" markerUnits=\"userSpaceOnUse\" orient=\"0\"><path d=\"M0 0H4V4H0Z\"/></marker>"));
  Lowered l;
  Lower(Doc("<path d=\"M2 2L18 2\" fill=\"none\" stroke=\"black\" marker-start=\"url(m.svg#m)\" marker-end=\"url(m.svg#m)\"/>"), l, fake);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  size_t instances = 0, strokes = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    instances += l.Item(i).kind == GSHSCENE_ITEM_INSTANCE;
  }
  EXPECT_EQ(instances, (size_t) 2) << "a marker at the start and one at the end";
  strokes = Count(l, GSHSCENE_ITEM_STROKE);
  EXPECT_EQ(strokes, (size_t) 1) << "the path's own stroke; the marker's path has a fill";
  Lowered local;  // the same marker in the document draws the same
  Lower(Doc("<marker id=\"m\" markerWidth=\"4\" markerHeight=\"4\" markerUnits=\"userSpaceOnUse\" orient=\"0\"><path d=\"M0 0H4V4H0Z\"/></marker>"
            "<path d=\"M2 2L18 2\" fill=\"none\" stroke=\"black\" marker-start=\"url(#m)\" marker-end=\"url(#m)\"/>"),
      local, fake);
  size_t local_instances = 0;
  for (size_t i = 0; i < local.Items(); i++) {
    local_instances += local.Item(i).kind == GSHSCENE_ITEM_INSTANCE;
  }
  EXPECT_EQ(instances, local_instances);
}

TEST(LowerResolverProperty, AReferenceTheResolverDoesNotGiveIsRefusedAsBefore) {
  for (GVEC_Resource_Status status : {GVEC_RESOURCE_REFUSED, GVEC_RESOURCE_NOT_FOUND, GVEC_RESOURCE_FAILED}) {
    Fake fake;
    fake.Status("c.svg", status);
    Lowered l;
    Lower(Doc("<rect width=\"20\" height=\"20\" clip-path=\"url(c.svg#c)\"/>"), l, fake);
    EXPECT_NE(l.Refusal(GVEC_LOWER_CLIP_PATH), nullptr) << "status " << (int) status;
  }
  Fake none;  // the document has no such element
  none.Put("c.svg", Other("<clipPath id=\"other\"/>"));
  Lowered l;
  Lower(Doc("<rect width=\"20\" height=\"20\" clip-path=\"url(c.svg#c)\"/>"), l, none);
  EXPECT_NE(l.Refusal(GVEC_LOWER_CLIP_PATH), nullptr);
}

TEST(LowerResolverProperty, DocumentsThatNameEachOtherThroughClipsEndAndAreRefusedOrDrawnOnce) {
  Fake fake;
  fake.Put("a.svg", Other("<clipPath id=\"a\" clip-path=\"url(b.svg#b)\"><rect width=\"5\" height=\"5\"/></clipPath>"));
  fake.Put("b.svg", Other("<clipPath id=\"b\" clip-path=\"url(a.svg#a)\"><rect width=\"6\" height=\"6\"/></clipPath>"));
  Lowered l;
  Lower(Doc("<rect width=\"20\" height=\"20\" clip-path=\"url(a.svg#a)\"/>"), l, fake);
  EXPECT_LE(fake.seen.size(), (size_t) 8) << "the documents are asked once each however they name one another";
  SUCCEED();
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
