/**
 * @file
 *
 * The structural limits of the scene (P5, AD-19): both forms are given a tree that fits, so a hostile document is refused
 * where it does not fit and never makes the caller's publish fail. The walk's model of each limit is checked against the
 * scene's own: the largest drawing it accepts is the largest the scene publishes.
 */

#include "../lower_helpers.h"

#include <algorithm>

using namespace gvec_test;

namespace {

/** `depth` nested groups with an opacity, around one square. */
std::string Nested(int depth) {
  std::string s;
  for (int i = 0; i < depth; i++) {
    s += "<g opacity=\".5\">";
  }
  s += "<path d=\"M0 0H4V4H0Z\"/>";
  for (int i = 0; i < depth; i++) {
    s += "</g>";
  }
  return s;
}

/** A chain of `uses` uses, each of the one before, and the square at the bottom. */
std::string Chain(int uses) {
  std::string body = "<defs><path id=\"u0\" d=\"M0 0H5V5Z\"/>";
  for (int i = 1; i < uses; i++) {
    body += "<use id=\"u" + std::to_string(i) + "\" href=\"#u" + std::to_string(i - 1) + "\"/>";
  }
  body += "</defs><use href=\"#u" + std::to_string(uses - 1) + "\"/>";
  return body;
}

GSHSCENE_Limits Defaults() {
  GSHSCENE_Limits l;
  gshscene_limits_default(&l);
  return l;
}

}  // namespace

TEST(LowerLimits, TheNodeDepthOfTheWalkIsTheScenesOwn) {
  for (size_t cap = 4; cap <= 12; cap++) {
    GSHSCENE_Limits scene = Defaults();
    scene.max_node_depth = cap;
    // The walk is told the same cap: a group is drawn exactly when it fits, and the edit always publishes.
    for (int depth = 0; depth <= 12; depth++) {
      Lowered l;
      l.scene_limits = &scene;
      LoadSvg(Svg("width=\"20\" height=\"20\"", Nested(depth)), l.loaded);
      LowerBoth(l.loaded, l);
      bool drawn = l.Count(GSHSCENE_ITEM_FILL) == 1;
      EXPECT_EQ(drawn, (size_t) depth + 3 <= cap) << "cap " << cap << " depth " << depth;
      EXPECT_EQ(l.publish_result, GSHSCENE_OK) << "cap " << cap << " depth " << depth;
      if (!drawn) {
        EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
        EXPECT_EQ(l.Items(), (size_t) 0) << "an element is refused whole";
      }
    }
  }
}

TEST(LowerLimits, TheWalksModelIsNotTighterThanTheScenes) {
  // Tell the walk a generous cap and the scene a tight one: the deepest drawing the scene still publishes is the model's.
  GSHSCENE_Limits generous = Defaults();
  for (size_t cap = 4; cap <= 10; cap++) {
    GSHSCENE_Limits tight = Defaults();
    tight.max_node_depth = cap;
    int deepest_published = -1;
    for (int depth = 0; depth <= 12; depth++) {
      Lowered l;
      l.scene_limits = &tight;
      l.walk_limits = &generous;
      l.expect_publish = false;
      LoadSvg(Svg("width=\"20\" height=\"20\"", Nested(depth)), l.loaded);
      // The recorded list has no node depth, so only the edit side is looked at.
      ASSERT_EQ(l.loaded.result, GVEC_OK);
      ASSERT_EQ(gshscene_scene_create(nullptr, &tight, nullptr, &l.scene), GSHSCENE_OK);
      GSHSCENE_Edit * edit = nullptr;
      ASSERT_EQ(gshscene_scene_open_edit(l.scene, nullptr, &edit), GSHSCENE_OK);
      GVEC_Lower_Options options;
      std::memset(&options, 0, sizeof options);
      options.scene_limits = &generous;
      ASSERT_EQ(gvec_doc_lower_edit(l.loaded.doc, &options, edit, nullptr), GVEC_OK);
      const GSHSCENE_Version * v = nullptr;
      GSHSCENE_Result r = gshscene_scene_publish(l.scene, edit, nullptr, &v);
      gshscene_edit_destroy(edit);
      gshscene_version_release(v);
      if (r == GSHSCENE_OK) {
        deepest_published = depth;
      }
    }
    EXPECT_EQ(deepest_published, (int) cap - 3) << "cap " << cap;
  }
}

TEST(LowerLimits, OpenGroupsAndClipsAreCappedPerList) {
  for (size_t cap = 1; cap <= 6; cap++) {
    GSHSCENE_Limits scene = Defaults();
    scene.max_depth = cap;
    for (int depth = 0; depth <= 8; depth++) {
      Lowered l;
      l.scene_limits = &scene;
      LoadSvg(Svg("width=\"20\" height=\"20\"", Nested(depth)), l.loaded);
      LowerBoth(l.loaded, l);
      EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL) == 1, (size_t) depth <= cap) << "cap " << cap << " depth " << depth;
    }
  }
}

TEST(LowerLimits, PicturesNestedInPicturesAreCapped) {
  for (size_t cap = 1; cap <= 6; cap++) {
    GSHSCENE_Limits scene = Defaults();
    scene.max_picture_depth = cap;
    for (int uses = 1; uses <= 8; uses++) {
      Lowered l;
      l.scene_limits = &scene;
      LoadSvg(Svg("width=\"20\" height=\"20\"", Chain(uses)), l.loaded);
      LowerBoth(l.loaded, l);
      EXPECT_EQ(l.Count(GSHSCENE_ITEM_INSTANCE) == 1, (size_t) uses <= cap) << "cap " << cap << " uses " << uses;
      EXPECT_EQ(l.publish_result, GSHSCENE_OK);
    }
  }
}

TEST(LowerLimits, TheNodeDepthCountsTheInstancesOfAChain) {
  GSHSCENE_Limits generous = Defaults();
  for (size_t cap = 6; cap <= 16; cap++) {
    GSHSCENE_Limits tight = Defaults();
    tight.max_node_depth = cap;
    int deepest = 0;
    for (int uses = 1; uses <= 10; uses++) {
      Lowered l;
      LoadSvg(Svg("width=\"20\" height=\"20\"", Chain(uses)), l.loaded);
      ASSERT_EQ(l.loaded.result, GVEC_OK);
      ASSERT_EQ(gshscene_scene_create(nullptr, &tight, nullptr, &l.scene), GSHSCENE_OK);
      GSHSCENE_Edit * edit = nullptr;
      ASSERT_EQ(gshscene_scene_open_edit(l.scene, nullptr, &edit), GSHSCENE_OK);
      GVEC_Lower_Options options;
      std::memset(&options, 0, sizeof options);
      options.scene_limits = &generous;
      ASSERT_EQ(gvec_doc_lower_edit(l.loaded.doc, &options, edit, nullptr), GVEC_OK);
      const GSHSCENE_Version * v = nullptr;
      if (gshscene_scene_publish(l.scene, edit, nullptr, &v) == GSHSCENE_OK) {
        deepest = uses;
      }
      gshscene_edit_destroy(edit);
      gshscene_version_release(v);
    }
    // And with the walk told the same cap, exactly that many are drawn and every edit publishes.
    for (int uses = 1; uses <= 10; uses++) {
      Lowered l;
      l.scene_limits = &tight;
      LoadSvg(Svg("width=\"20\" height=\"20\"", Chain(uses)), l.loaded);
      LowerBoth(l.loaded, l);
      EXPECT_EQ(l.Count(GSHSCENE_ITEM_INSTANCE) == 1, uses <= deepest) << "cap " << cap << " uses " << uses;
    }
  }
}

TEST(LowerLimits, ListItemsAreCappedWhereTheListBegins) {
  GSHSCENE_Limits scene = Defaults();
  scene.max_list_items = 5;
  std::string body;
  for (int i = 0; i < 8; i++) {
    body += "<path d=\"M0 0H4V4H0Z\"/>";
  }
  Lowered l;
  l.scene_limits = &scene;
  LoadSvg(Svg("width=\"20\" height=\"20\"", body), l.loaded);
  LowerBoth(l.loaded, l);
  EXPECT_EQ(l.Items(), (size_t) 5);
  EXPECT_EQ(l.Refusals(), (size_t) 3);
}

TEST(LowerLimits, TheExpansionOfInstancesIsCapped) {
  GSHSCENE_Limits scene = Defaults();
  scene.max_expansion = 12;
  std::string body = "<defs><g id=\"g\"><path d=\"M0 0H4V4H0Z\"/><path d=\"M0 0H3V3H0Z\"/><path d=\"M0 0H2V2H0Z\"/></g></defs>";
  for (int i = 0; i < 6; i++) {
    body += "<use href=\"#g\" x=\"" + std::to_string(i) + "\"/>";
  }
  Lowered l;
  l.scene_limits = &scene;
  LoadSvg(Svg("width=\"20\" height=\"20\"", body), l.loaded);
  LowerBoth(l.loaded, l);
  // The root and the drawing's group count 2, and a use is its instance and the three fills of its picture, so 4: two uses make
  // 10, a third would make 14.
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_INSTANCE), (size_t) 2);
  EXPECT_EQ(l.Refusals(), (size_t) 4);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerLimits, TheOccurrencesOfNodesAreCappedAsTheSceneCountsThem) {
  // A use is an instance and the container it draws, and so three occurrences with a fill inside: against the cap it is the
  // scene's count that binds, not the list's two items. The walk's model against the scene's: the most uses the scene
  // publishes under a tight cap is the most the walk draws under the same.
  GSHSCENE_Limits generous = Defaults();
  for (size_t cap = 5; cap <= 20; cap++) {
    GSHSCENE_Limits tight = Defaults();
    tight.max_expansion = cap;
    std::string body = "<defs><path id=\"p\" d=\"M0 0H4V4H0Z\"/></defs>";
    for (int i = 0; i < 12; i++) {
      body += "<use href=\"#p\" x=\"" + std::to_string(i) + "\"/>";
    }
    Lowered probe;
    LoadSvg(Svg("width=\"20\" height=\"20\"", body), probe.loaded);
    ASSERT_EQ(probe.loaded.result, GVEC_OK);
    ASSERT_EQ(gshscene_scene_create(nullptr, &tight, nullptr, &probe.scene), GSHSCENE_OK);
    GSHSCENE_Edit * edit = nullptr;
    ASSERT_EQ(gshscene_scene_open_edit(probe.scene, nullptr, &edit), GSHSCENE_OK);
    GVEC_Lower_Options options;
    std::memset(&options, 0, sizeof options);
    options.scene_limits = &generous;
    ASSERT_EQ(gvec_doc_lower_edit(probe.loaded.doc, &options, edit, nullptr), GVEC_OK);
    // The scene refuses the whole edit when it holds more than it allows, so ask how many uses fit one by one.
    gshscene_edit_destroy(edit);
    size_t most = 0;
    for (int uses = 1; uses <= 12; uses++) {
      std::string few = "<defs><path id=\"p\" d=\"M0 0H4V4H0Z\"/></defs>";
      for (int i = 0; i < uses; i++) {
        few += "<use href=\"#p\" x=\"" + std::to_string(i) + "\"/>";
      }
      Lowered l;
      LoadSvg(Svg("width=\"20\" height=\"20\"", few), l.loaded);
      ASSERT_EQ(gshscene_scene_create(nullptr, &tight, nullptr, &l.scene), GSHSCENE_OK);
      GSHSCENE_Edit * e = nullptr;
      ASSERT_EQ(gshscene_scene_open_edit(l.scene, nullptr, &e), GSHSCENE_OK);
      ASSERT_EQ(gvec_doc_lower_edit(l.loaded.doc, &options, e, nullptr), GVEC_OK);
      const GSHSCENE_Version * v = nullptr;
      if (gshscene_scene_publish(l.scene, e, nullptr, &v) == GSHSCENE_OK) {
        most = (size_t) uses;
      }
      gshscene_edit_destroy(e);
      gshscene_version_release(v);
    }
    EXPECT_EQ(most, (cap - 2) / 3) << "cap " << cap;
    // And told the same cap, the walk draws exactly that many, and the edit publishes.
    Lowered l;
    l.scene_limits = &tight;
    LoadSvg(Svg("width=\"20\" height=\"20\"", body), l.loaded);
    LowerBoth(l.loaded, l);
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_INSTANCE), std::min((size_t) 12, (cap - 2) / 3)) << "cap " << cap;
  }
}

TEST(LowerLimits, TheNodeCountIsCapped) {
  GSHSCENE_Limits scene = Defaults();
  scene.max_nodes = 6;  // the root, the drawing's group, and four fills
  std::string body;
  for (int i = 0; i < 9; i++) {
    body += "<path d=\"M0 0H4V4H0Z\"/>";
  }
  Lowered l;
  l.scene_limits = &scene;
  LoadSvg(Svg("width=\"20\" height=\"20\"", body), l.loaded);
  LowerBoth(l.loaded, l);
  EXPECT_EQ(l.Items(), (size_t) 4);
  EXPECT_EQ(l.Refusals(), (size_t) 5);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
