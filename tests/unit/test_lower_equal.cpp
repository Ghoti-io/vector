/**
 * @file
 *
 * The equality gate over the lowering's fixtures (P2, AD-17): every document of tests/data/svg-lower lowered both ways gives
 * equal lists, lowering it again from scratch gives the same list and hash, and the first fixture's hash is pinned so that a
 * target whose arithmetic differs fails here.
 */

#include "../lower_helpers.h"

#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace gvec_test;

namespace {

std::vector<std::string> FixtureNames() {
  std::vector<std::string> names;
  DIR * dir = opendir("tests/data/svg-lower");
  if (!dir) {
    ADD_FAILURE() << "cannot open tests/data/svg-lower";
    return names;
  }
  while (const dirent * e = readdir(dir)) {
    std::string name = e->d_name;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".svg") == 0) {
      names.push_back(name);
    }
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

std::string Read(const std::string & name) {
  std::ifstream in("tests/data/svg-lower/" + name, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST(LowerEqual, EveryFixtureLowersToTheSameListBothWays) {
  std::vector<std::string> names = FixtureNames();
  ASSERT_GE(names.size(), (size_t) 30);
  for (const std::string & name : names) {
    Lowered l;
    LoadSvg(Read(name), l.loaded);
    ASSERT_EQ(l.loaded.result, GVEC_OK) << name;
    LowerBoth(l.loaded, l);
    EXPECT_TRUE(gshscene_list_equal(l.list, l.emitted)) << name;
    EXPECT_GT(l.Items() + l.Refusals(), (size_t) 0) << name << ": a fixture that draws nothing and refuses nothing tests nothing";
  }
}

TEST(LowerEqual, LoweringAgainFromScratchGivesTheSameListAndHash) {
  for (const std::string & name : FixtureNames()) {
    Lowered a, b;
    LoadSvg(Read(name), a.loaded);
    LoadSvg(Read(name), b.loaded);
    LowerBoth(a.loaded, a);
    LowerBoth(b.loaded, b);
    EXPECT_EQ(gshscene_list_hash(a.list), gshscene_list_hash(b.list)) << name;
    EXPECT_EQ(gshscene_version_hash(a.version), gshscene_version_hash(b.version)) << name << ": the scene's hash, which covers the keys";
  }
}

TEST(LowerEqual, AFixturesListHashIsPinned) {
  // A list's hash is a function of the document, the library's arithmetic and the waist's spelling, and nothing else: a
  // target whose result differs (a fused multiply-add, extra precision, a byte order) fails here. The value is the one the
  // golden file of the fixture records (tests/golden/lower/par-xmidymid-meet.txt).
  Lowered l;
  LoadSvg(Read("par-xmidymid-meet.svg"), l.loaded);
  LowerBoth(l.loaded, l);
  EXPECT_EQ(gshscene_list_hash(l.list), UINT64_C(0xcbf07fda285ff3e7)) << std::hex << gshscene_list_hash(l.list);
}

TEST(LowerEqual, TheOptionsViewportIsPartOfTheDrawingAndNothingElseIsRead) {
  // A document with no size is laid out in the load's viewport: the same bytes loaded in two viewports are two drawings,
  // and the same bytes loaded in the same viewport are one.
  const std::string doc = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"><path d=\"M0 0H4V4Z\"/></svg>";
  Lowered a, b, c;
  LoadSvg(doc, a.loaded, 100, 100);
  LoadSvg(doc, b.loaded, 200, 100);
  LoadSvg(doc, c.loaded, 100, 100);
  LowerBoth(a.loaded, a);
  LowerBoth(b.loaded, b);
  LowerBoth(c.loaded, c);
  EXPECT_NE(gshscene_list_hash(a.list), gshscene_list_hash(b.list));
  EXPECT_EQ(gshscene_list_hash(a.list), gshscene_list_hash(c.list));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
