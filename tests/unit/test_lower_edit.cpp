/**
 * @file
 *
 * The edit emitter (AD-7, AD-9): keys, lowering again, and where the drawing goes. A document lowered twice into one scene
 * touches nothing the second time; a changed attribute touches only its element's node; elements with an `id` keep their
 * nodes through an insertion, a removal and a reordering; nothing outside the drawing's group is touched.
 */

#include "../lower_helpers.h"

#include <set>

using namespace gvec_test;

namespace {

/** A scene with the document lowered into it, and what is needed to lower another. */
struct Scened {
  Loaded loaded;
  GSHSCENE_Scene * scene = nullptr;
  const GSHSCENE_Version * version = nullptr;

  Scened() = default;
  Scened(const Scened &) = delete;
  Scened & operator=(const Scened &) = delete;
  ~Scened() {
    gshscene_version_release(version);
    gshscene_scene_destroy(scene);
  }

  void Make() {
    ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
  }

  /** Lower `svg` into the scene (as an edit on its current version) and publish; `version` is the result. */
  void Lower(const std::string & svg, const GVEC_Lower_Options * options = nullptr) {
    Loaded next;
    LoadSvg(svg, next);
    ASSERT_EQ(next.result, GVEC_OK);
    GSHSCENE_Edit * edit = nullptr;
    ASSERT_EQ(gshscene_scene_open_edit(scene, nullptr, &edit), GSHSCENE_OK);
    ASSERT_EQ(gvec_doc_lower_edit(next.doc, options, edit, nullptr), GVEC_OK);
    const GSHSCENE_Version * published = nullptr;
    ASSERT_EQ(gshscene_scene_publish(scene, edit, nullptr, &published), GSHSCENE_OK);
    gshscene_edit_destroy(edit);
    gshscene_version_release(version);
    version = published;
  }

  size_t Changed(const GSHSCENE_Version * before) const {
    GSHSCENE_Diff * diff = nullptr;
    EXPECT_EQ(gshscene_diff(before, version, nullptr, nullptr, &diff), GSHSCENE_OK);
    size_t n = gshscene_diff_count(diff);
    gshscene_diff_destroy(diff);
    return n;
  }
};

std::string Shapes(const std::string & second_fill = "blue", bool with_middle = true, bool swap = false) {
  std::string a = "<path id=\"a\" d=\"M0 0H4V4H0Z\" fill=\"red\"/>";
  std::string m = with_middle ? "<path id=\"m\" d=\"M5 0H9V4H5Z\" fill=\"" + second_fill + "\"/>" : "";
  std::string b = "<path id=\"b\" d=\"M10 0H14V4H10Z\" fill=\"green\"/>";
  return swap ? b + m + a : a + m + b;
}

}  // namespace

TEST(LowerEdit, LoweringAnUnchangedDocumentAgainTouchesNothing) {
  Scened s;
  s.Make();
  const std::string svg = Svg("width=\"20\" height=\"20\"",
      Shapes() + "<defs><g id=\"g\"><path id=\"p\" d=\"M0 0H3V3Z\"/></g></defs><use href=\"#g\" x=\"3\"/><use href=\"#g\" x=\"9\"/><g opacity=\".5\"><path d=\"M0 5H4V9Z\"/></g>"
                 "<svg x=\"1\" y=\"1\" width=\"6\" height=\"6\"><path d=\"M0 0H9V9Z\"/></svg>");
  s.Lower(svg);
  const GSHSCENE_Version * first = gshscene_version_retain(s.version);
  uint64_t hash = gshscene_version_hash(first);
  s.Lower(svg);
  EXPECT_EQ(gshscene_version_serial(s.version), gshscene_version_serial(first)) << "the second publication is empty: no new version";
  EXPECT_EQ(gshscene_version_hash(s.version), hash);
  EXPECT_EQ(s.Changed(first), (size_t) 0);
  gshscene_version_release(first);
}

TEST(LowerEdit, OneChangedAttributeTouchesOnlyItsElementsNode) {
  Scened s;
  s.Make();
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("blue")));
  const GSHSCENE_Version * before = gshscene_version_retain(s.version);
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("yellow")));
  EXPECT_EQ(s.Changed(before), (size_t) 1);
  GSHSCENE_Diff * diff = nullptr;
  ASSERT_EQ(gshscene_diff(before, s.version, nullptr, nullptr, &diff), GSHSCENE_OK);
  uint64_t id;
  GSHSCENE_Diff_Kind kind;
  ASSERT_EQ(gshscene_diff_get(diff, 0, &id, &kind), GSHSCENE_OK);
  EXPECT_EQ(kind, GSHSCENE_DIFF_CHANGED);
  GSHSCENE_Node_Info info;
  ASSERT_EQ(gshscene_version_node(s.version, id, &info), GSHSCENE_OK);
  EXPECT_EQ(info.node.kind, GSHSCENE_NODE_SHAPE);
  EXPECT_EQ(info.node.paint.rgba[0], 1.0);  // yellow
  EXPECT_EQ(info.node.paint.rgba[1], 1.0);
  gshscene_diff_destroy(diff);
  gshscene_version_release(before);
}

TEST(LowerEdit, ElementsWithAnIdKeepTheirNodesWhenOneIsInsertedOrRemoved) {
  Scened s;
  s.Make();
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("blue", false)));
  uint64_t a = 0;
  GSHSCENE_Node_Info drawing;
  auto node_ids = [&](uint64_t * out_a, uint64_t * out_b) {
    ASSERT_EQ(gshscene_version_child(s.version, GSHSCENE_ROOT_ID, 0, &a), GSHSCENE_OK);
    ASSERT_EQ(gshscene_version_node(s.version, a, &drawing), GSHSCENE_OK);
    ASSERT_GE(drawing.child_count, (size_t) 2);
    ASSERT_EQ(gshscene_version_child(s.version, a, 0, out_a), GSHSCENE_OK);
    ASSERT_EQ(gshscene_version_child(s.version, a, drawing.child_count - 1, out_b), GSHSCENE_OK);
  };
  uint64_t first_a, first_b, again_a, again_b;
  node_ids(&first_a, &first_b);
  const GSHSCENE_Version * before = gshscene_version_retain(s.version);
  // Inserted between them: the two keep their nodes and a new one appears, and the drawing's group changes.
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("blue", true)));
  node_ids(&again_a, &again_b);
  EXPECT_EQ(again_a, first_a);
  EXPECT_EQ(again_b, first_b);
  EXPECT_EQ(drawing.child_count, (size_t) 3);
  EXPECT_EQ(s.Changed(before), (size_t) 2);  // the added shape and the drawing's group
  gshscene_version_release(before);
  // Removed again: the middle node goes, the others stay.
  before = gshscene_version_retain(s.version);
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("blue", false)));
  node_ids(&again_a, &again_b);
  EXPECT_EQ(again_a, first_a);
  EXPECT_EQ(again_b, first_b);
  EXPECT_EQ(drawing.child_count, (size_t) 2);
  EXPECT_EQ(s.Changed(before), (size_t) 2);  // the removed shape and the drawing's group
  gshscene_version_release(before);
}

TEST(LowerEdit, ReorderingKeepsTheNodesAndChangesTheOrder) {
  Scened s;
  s.Make();
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("blue", true, false)));
  uint64_t drawing, a, m, b;
  ASSERT_EQ(gshscene_version_child(s.version, GSHSCENE_ROOT_ID, 0, &drawing), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, drawing, 0, &a), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, drawing, 1, &m), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, drawing, 2, &b), GSHSCENE_OK);
  const GSHSCENE_Version * before = gshscene_version_retain(s.version);
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("blue", true, true)));
  uint64_t first, second, third;
  ASSERT_EQ(gshscene_version_child(s.version, drawing, 0, &first), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, drawing, 1, &second), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, drawing, 2, &third), GSHSCENE_OK);
  EXPECT_EQ(first, b);
  EXPECT_EQ(second, m);
  EXPECT_EQ(third, a);
  EXPECT_EQ(s.Changed(before), (size_t) 1);  // only the group whose children are ordered differently
  gshscene_version_release(before);
}

TEST(LowerEdit, TwoUsesOfOneReferentAreTwoNodesWithTheirOwnKeys) {
  Scened s;
  s.Make();
  s.Lower(Svg("width=\"20\" height=\"20\"", "<defs><path id=\"p\" d=\"M0 0H3V3Z\"/></defs><use id=\"u1\" href=\"#p\"/><use id=\"u2\" href=\"#p\" x=\"5\"/>"));
  uint64_t drawing;
  ASSERT_EQ(gshscene_version_child(s.version, GSHSCENE_ROOT_ID, 0, &drawing), GSHSCENE_OK);
  GSHSCENE_Node_Info info;
  ASSERT_EQ(gshscene_version_node(s.version, drawing, &info), GSHSCENE_OK);
  EXPECT_EQ(info.child_count, (size_t) 4);  // a hidden container and an instance, twice
  size_t hidden = 0, instances = 0;
  std::set<uint64_t> keys;
  for (size_t i = 0; i < info.child_count; i++) {
    uint64_t id;
    ASSERT_EQ(gshscene_version_child(s.version, drawing, i, &id), GSHSCENE_OK);
    GSHSCENE_Node_Info child;
    ASSERT_EQ(gshscene_version_node(s.version, id, &child), GSHSCENE_OK);
    EXPECT_TRUE(child.has_key);
    keys.insert(child.key.value);
    hidden += (child.node.flags & GSHSCENE_NODE_HIDDEN) != 0;
    instances += child.node.kind == GSHSCENE_NODE_INSTANCE;
  }
  EXPECT_EQ(hidden, (size_t) 2);
  EXPECT_EQ(instances, (size_t) 2);
  EXPECT_EQ(keys.size(), (size_t) 4);
  // The shape inside each picture is its own node.
  EXPECT_EQ(gshscene_version_key_count(s.version), (size_t) 1 + 4 + 2);
}

TEST(LowerEdit, TheDrawingIsPlacedUnderTheParentAtThePositionAndOnlyItsGroupIsTouched) {
  Scened s;
  s.Make();
  // The caller's own nodes: a group with one shape, and a shape beside it.
  GSHSCENE_Edit * edit = nullptr;
  ASSERT_EQ(gshscene_scene_open_edit(s.scene, nullptr, &edit), GSHSCENE_OK);
  GSHSCENE_Node group, shape;
  gshscene_node_init(GSHSCENE_NODE_GROUP, &group);
  uint64_t group_id = 0, other = 0;
  ASSERT_EQ(gshscene_edit_add(edit, GSHSCENE_ROOT_ID, GSHSCENE_APPEND, &group, nullptr, &group_id), GSHSCENE_OK);
  GSHSCENE_Store * store = gshscene_edit_store(edit);
  GSHGEOM_Path * path = nullptr;
  ASSERT_EQ(gshgeom_path_create(nullptr, nullptr, 2, &path), GSHGEOM_OK);
  gshgeom_path_move_to(path, GSHGEOM_Point{0, 0, 0});
  gshgeom_path_line_to(path, GSHGEOM_Point{1, 0, 0});
  gshgeom_path_line_to(path, GSHGEOM_Point{0, 1, 0});
  gshgeom_path_close(path);
  const GSHSCENE_Resource * resource = nullptr;
  ASSERT_EQ(gshscene_store_intern_path(store, path, &resource), GSHSCENE_OK);
  gshgeom_path_destroy(path);
  gshscene_node_init(GSHSCENE_NODE_SHAPE, &shape);
  shape.resource = resource;
  ASSERT_EQ(gshscene_edit_add(edit, group_id, GSHSCENE_APPEND, &shape, nullptr, &other), GSHSCENE_OK);
  uint64_t mine = 0;
  ASSERT_EQ(gshscene_edit_add(edit, group_id, GSHSCENE_APPEND, &shape, nullptr, &mine), GSHSCENE_OK);
  gshscene_resource_release(resource);
  const GSHSCENE_Version * base = nullptr;
  ASSERT_EQ(gshscene_scene_publish(s.scene, edit, nullptr, &base), GSHSCENE_OK);
  gshscene_edit_destroy(edit);

  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.parent = group_id;
  options.flags = GVEC_LOWER_AT_POSITION;
  options.position = 1;
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes()), &options);
  GSHSCENE_Node_Info info;
  ASSERT_EQ(gshscene_version_node(s.version, group_id, &info), GSHSCENE_OK);
  ASSERT_EQ(info.child_count, (size_t) 3);
  uint64_t first, second, third;
  ASSERT_EQ(gshscene_version_child(s.version, group_id, 0, &first), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, group_id, 1, &second), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, group_id, 2, &third), GSHSCENE_OK);
  EXPECT_EQ(first, other);
  EXPECT_EQ(third, mine);
  GSHSCENE_Node_Info drawing;
  ASSERT_EQ(gshscene_version_node(s.version, second, &drawing), GSHSCENE_OK);
  EXPECT_EQ(drawing.node.kind, GSHSCENE_NODE_GROUP);
  EXPECT_EQ(drawing.child_count, (size_t) 3);

  // A different document lowered there again leaves the caller's nodes alone, and the drawing in its place.
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes("blue", false)), &options);
  ASSERT_EQ(gshscene_version_node(s.version, group_id, &info), GSHSCENE_OK);
  ASSERT_EQ(info.child_count, (size_t) 3);
  uint64_t again;
  ASSERT_EQ(gshscene_version_child(s.version, group_id, 1, &again), GSHSCENE_OK);
  EXPECT_EQ(again, second);
  ASSERT_EQ(gshscene_version_node(s.version, again, &drawing), GSHSCENE_OK);
  EXPECT_EQ(drawing.child_count, (size_t) 2);
  GSHSCENE_Node_Info kept;
  EXPECT_EQ(gshscene_version_node(s.version, other, &kept), GSHSCENE_OK);
  EXPECT_EQ(gshscene_version_node(s.version, mine, &kept), GSHSCENE_OK);
  gshscene_version_release(base);
}

TEST(LowerEdit, AParentThatIsNotAGroupOrAPositionPastTheEndIsInvalid) {
  Scened s;
  s.Make();
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes()));
  Loaded doc;
  LoadSvg(Svg("width=\"20\" height=\"20\"", Shapes()), doc);
  GSHSCENE_Edit * edit = nullptr;
  ASSERT_EQ(gshscene_scene_open_edit(s.scene, nullptr, &edit), GSHSCENE_OK);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  uint64_t drawing, shape;
  ASSERT_EQ(gshscene_version_child(s.version, GSHSCENE_ROOT_ID, 0, &drawing), GSHSCENE_OK);
  ASSERT_EQ(gshscene_version_child(s.version, drawing, 0, &shape), GSHSCENE_OK);
  options.parent = shape;
  EXPECT_EQ(gvec_doc_lower_edit(doc.doc, &options, edit, nullptr), GVEC_ERR_INVALID);
  options.parent = 999999;
  EXPECT_EQ(gvec_doc_lower_edit(doc.doc, &options, edit, nullptr), GVEC_ERR_INVALID);
  options.parent = 0;
  options.flags = GVEC_LOWER_AT_POSITION;
  options.position = 50;
  EXPECT_EQ(gvec_doc_lower_edit(doc.doc, &options, edit, nullptr), GVEC_ERR_INVALID);
  gshscene_edit_destroy(edit);
}

TEST(LowerEdit, TheForegroundIsSetOnTheEditAndAChangeIsTheOnlyDifference) {
  Scened s;
  s.Make();
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.flags = GVEC_LOWER_SET_FOREGROUND;
  gshscene_color_init(&options.foreground);
  options.foreground.rgba[1] = 0.5;
  s.Lower(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4Z\" fill=\"currentColor\"/>"), &options);
  GSHSCENE_Color c;
  ASSERT_EQ(gshscene_version_foreground(s.version, &c), GSHSCENE_OK);
  EXPECT_EQ(c.rgba[1], 0.5);
  const GSHSCENE_Version * before = gshscene_version_retain(s.version);
  options.foreground.rgba[1] = 0.75;
  s.Lower(Svg("width=\"20\" height=\"20\"", "<path d=\"M0 0H4V4Z\" fill=\"currentColor\"/>"), &options);
  GSHSCENE_Diff * diff = nullptr;
  ASSERT_EQ(gshscene_diff(before, s.version, nullptr, nullptr, &diff), GSHSCENE_OK);
  EXPECT_EQ(gshscene_diff_count(diff), (size_t) 0);
  EXPECT_TRUE(gshscene_diff_foreground_changed(diff));
  gshscene_diff_destroy(diff);
  gshscene_version_release(before);
}

TEST(LowerEdit, ADocumentThatDrawsNothingLeavesAnEmptyDrawingGroup) {
  Scened s;
  s.Make();
  s.Lower(Svg("width=\"20\" height=\"20\"", Shapes()));
  s.Lower(Svg("width=\"20\" height=\"20\"", ""));
  uint64_t drawing;
  ASSERT_EQ(gshscene_version_child(s.version, GSHSCENE_ROOT_ID, 0, &drawing), GSHSCENE_OK);
  GSHSCENE_Node_Info info;
  ASSERT_EQ(gshscene_version_node(s.version, drawing, &info), GSHSCENE_OK);
  EXPECT_EQ(info.child_count, (size_t) 0);
  EXPECT_EQ(gshscene_version_key_count(s.version), (size_t) 1);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
