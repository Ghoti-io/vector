/**
 * @file
 *
 * Allocation failure where a lowering grows a list it has already started (the points of a `polygon()`, the pieces and the terms of a clip, the lines of a
 * text decoration): a failure of the growth, which a small document never makes (the lowering's arena takes memory in blocks, and a list that is still
 * inside its block grows without asking the allocator), is out of memory and leaks nothing. Each document here is large enough that its list outgrows
 * its first capacity, and every allocation of its lowering is failed in turn.
 */

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/glyph.h>

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** The font set of tests/data/fonts, as the text tests use it. */
struct Fonts {
  GFNT_FontSet * set = nullptr;
  GVEC_Fonts * fonts = nullptr;
  Fonts() {
    const char * dirs[] = {"tests/data/fonts"};
    static const GFNT_FontAlias aliases[] = {{"sans-serif", "Vector Test"}, {"serif", "Vector Test"}};
    EXPECT_EQ(gfnt_fontset_scan(dirs, 1, aliases, 2, nullptr, &set, nullptr), GFNT_OK);
    EXPECT_EQ(gvec_fonts_new(set, nullptr, &fonts), GVEC_OK);
  }
  ~Fonts() {
    gvec_fonts_free(fonts);
    gfnt_fontset_free(set);
  }
};

/** An allocator that fails the Nth large request (an arena takes memory a block at a time, and it is the block that a list growing past its first
 * capacity asks for) of the lowering, once `armed`; every other request is served. Blocks outstanding are counted. */
struct BlockFail {
  GVEC_Allocator a{};
  static constexpr size_t kLarge = 8192;
  bool armed = false;
  long large_seen = 0;
  long fail_large_at = -1;
  long outstanding = 0;

  BlockFail() {
    a.ctx = this;
    a.malloc_fn = [](void * ctx, size_t size) -> void * {
      BlockFail * f = (BlockFail *) ctx;
      if (f->Fail(size)) {
        return nullptr;
      }
      void * p = std::malloc(size ? size : 1u);
      f->outstanding += p ? 1 : 0;
      return p;
    };
    a.calloc_fn = [](void * ctx, size_t n, size_t size) -> void * {
      BlockFail * f = (BlockFail *) ctx;
      if (f->Fail(n * size)) {
        return nullptr;
      }
      void * p = std::calloc(n ? n : 1u, size ? size : 1u);
      f->outstanding += p ? 1 : 0;
      return p;
    };
    a.realloc_fn = [](void * ctx, void * ptr, size_t size) -> void * {
      BlockFail * f = (BlockFail *) ctx;
      if (f->Fail(size)) {
        return nullptr;
      }
      void * p = std::realloc(ptr, size ? size : 1u);
      f->outstanding += p && !ptr ? 1 : 0;
      return p;
    };
    a.free_fn = [](void * ctx, void * ptr) {
      BlockFail * f = (BlockFail *) ctx;
      if (ptr) {
        f->outstanding--;
      }
      std::free(ptr);
    };
  }

  bool Fail(size_t size) {
    if (!armed || size < kLarge) {
      return false;
    }
    large_seen++;
    return large_seen == fail_large_at;
  }
};

/** Load `text` (every request served), then lower it with `alloc` armed; the result of the lowering and the number of items, or -1. */
long LowerWith(BlockFail & alloc, const std::string & text, const GVEC_Fonts * fonts, GVEC_Result * result) {
  GVEC_Stream * stream = nullptr;
  EXPECT_EQ(gvec_stream_create_memory(text.data(), text.size(), &stream), GVEC_OK);
  GVEC_Load_Options load;
  std::memset(&load, 0, sizeof load);
  load.allocator = &alloc.a;
  GVEC_Diagnostics diagnostics;
  gvec_diagnostics_init(&diagnostics, &alloc.a);
  GVEC_Doc * doc = nullptr;
  GVEC_Result r = gvec_doc_load(stream, &load, &diagnostics, &doc);
  gvec_stream_destroy(stream);
  long items = -1;
  GSHSCENE_Scene * scene = nullptr;
  GSHSCENE_List * list = nullptr;
  if (r == GVEC_OK) {
    EXPECT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
    EXPECT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
    GVEC_Lower_Options options;
    std::memset(&options, 0, sizeof options);
    options.store = gshscene_scene_store(scene);
    options.fonts = fonts;
    alloc.armed = true;
    r = gvec_doc_lower_list(doc, &options, list, &diagnostics);
    alloc.armed = false;
    if (r == GVEC_OK) {
      items = (long) gshscene_list_count(list);
    }
  }
  *result = r;
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&diagnostics);
  return items;
}

/** Fail each large request of the lowering of `text` in turn: each is out of memory or the lowering as it was, and nothing leaks. The number of large
 * requests is returned, to say that the document grew something. */
long SweepLowering(const std::string & text, const GVEC_Fonts * fonts = nullptr) {
  BlockFail base;
  GVEC_Result r;
  long items = LowerWith(base, text, fonts, &r);
  EXPECT_EQ(r, GVEC_OK);
  EXPECT_GT(items, 0) << "the document draws nothing: it does not grow what it is for";
  EXPECT_EQ(base.outstanding, 0);
  const long total = base.large_seen;
  for (long fail_at = 1; fail_at <= total; fail_at++) {
    BlockFail alloc;
    alloc.fail_large_at = fail_at;
    GVEC_Result result;
    LowerWith(alloc, text, fonts, &result);
    EXPECT_TRUE(result == GVEC_OK || result == GVEC_ERR_OOM || result == GVEC_ERR_LIMIT) << "failing block " << fail_at << ": " << gvec_result_string(result);
    EXPECT_EQ(alloc.outstanding, 0) << "leak after failing block " << fail_at;
  }
  return total;
}

std::string Root(const std::string & body) {
  return Svg("width=\"400\" height=\"200\" font-size=\"20\" font-family=\"sans-serif\"", body);
}

}  // namespace

TEST(LowerGrowth, APolygonOfManyPointsOutgrowsItsFirstCapacity) {
  std::string points;
  for (int i = 0; i < 700; i++) {
    points += std::string(i ? ", " : "") + std::to_string(i % 97) + "px " + std::to_string((i * 7) % 89) + "px";
  }
  EXPECT_GT(SweepLowering(Root("<rect width=\"100\" height=\"100\" style=\"clip-path: polygon(" + points + ")\"/>")), 1);
}

TEST(LowerGrowth, AClipOfUnionsMakesMoreTermsThanItsFirstCapacity) {
  // three shapes each cut by a union of three: nine terms
  const std::string rects = "<rect width=\"50\" height=\"50\"/><rect x=\"10\" width=\"50\" height=\"50\"/><rect x=\"20\" width=\"50\" height=\"50\"/>";
  EXPECT_GT(SweepLowering(Root("<clipPath id=\"u\">" + rects + "</clipPath>"
                               "<clipPath id=\"c\"><rect width=\"90\" height=\"90\" clip-path=\"url(#u)\"/><rect x=\"5\" width=\"90\" height=\"90\" clip-path=\"url(#u)\"/>"
                               "<rect x=\"9\" width=\"90\" height=\"90\" clip-path=\"url(#u)\"/></clipPath>"
                               "<rect width=\"100\" height=\"100\" clip-path=\"url(#c)\"/>")),
      1);
}

TEST(LowerGrowth, AClipOfTextMakesMorePiecesThanItHasChildren) {
  Fonts f;
  EXPECT_GT(SweepLowering(Root("<clipPath id=\"c\"><text x=\"10\" y=\"60\" font-size=\"40\">ABCDEFGHIJ<tspan> KLM</tspan></text></clipPath>"
                               "<rect width=\"400\" height=\"100\" clip-path=\"url(#c)\"/>"),
                f.fonts),
      1);
}

TEST(LowerGrowth, ATextOnAPathWithManyDecoratedGlyphsOutgrowsItsFirstLines) {
  Fonts f;
  EXPECT_GT(SweepLowering(Root("<path id=\"p\" d=\"M10 100 C100 20 200 180 390 100\" fill=\"none\"/>"
                               "<text style=\"text-decoration: underline line-through overline\"><textPath href=\"#p\">ABCDEFGHIJKLMNOP</textPath></text>"),
                f.fonts),
      1);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
