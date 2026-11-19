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
 * `feImage` lowered (lower_filter.c, lower_image.c): the image of its `href` fitted into the primitive subregion by
 * `preserveAspectRatio` and drawn by the waist's IMAGE primitive under a placement map into the list's space; an image that cannot be
 * used is an empty result; an element of the document as the `href` is refused by name.
 */

#include "../lower_helpers.h"

#include <ghoti.io/shape-scene/filter.h>
#include <ghoti.io/shape-scene/image.h>
#include <ghoti.io/shape-scene/image_paint.h>

#include <string>

using namespace gvec_test;

namespace {

std::vector<unsigned char> ImageBytes(const std::string & name) {
  std::string path = "tests/data/images/" + name;
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

std::string Data(const std::string & name) {
  static const char * const digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<unsigned char> in = ImageBytes(name);
  std::string out = "data:image/png;base64,";
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned v = (unsigned) in[i] << 16 | (unsigned) in[i + 1] << 8 | in[i + 2];
    out += digits[v >> 18];
    out += digits[(v >> 12) & 63];
    out += digits[(v >> 6) & 63];
    out += digits[v & 63];
  }
  if (i + 1 == in.size()) {
    unsigned v = (unsigned) in[i] << 16;
    out += digits[v >> 18];
    out += digits[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    unsigned v = (unsigned) in[i] << 16 | (unsigned) in[i + 1] << 8;
    out += digits[v >> 18];
    out += digits[(v >> 12) & 63];
    out += digits[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

/** A filter of one feImage with `attributes`, on a rect at (2, 3) of 10 by 10 (whose default region is 1, 2, 12, 12) under `transform`. */
std::string Doc(const std::string & attributes, const std::string & filter_attributes = "", const std::string & element_attributes = "") {
  return Svg("width=\"40\" height=\"40\"",
      "<defs><filter id=\"f\" " + filter_attributes + "><feImage " + attributes + "/></filter></defs>"
      "<rect x=\"2\" y=\"3\" width=\"10\" height=\"10\" fill=\"red\" filter=\"url(#f)\" " + element_attributes + "/>");
}

/** The filter of the first group that has one in `list`, or in the picture of an instance in it (a rotated element is filtered in one). */
bool FilterIn(const GSHSCENE_List * list, GSHSCENE_Filter * f) {
  for (size_t i = 0; i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * it = nullptr;
    if (gshscene_list_item(list, i, &it) != GSHSCENE_OK) {
      return false;
    }
    if (it->kind == GSHSCENE_ITEM_GROUP_PUSH && it->group.filter) {
      return gshscene_resource_filter(it->group.filter, f) == GSHSCENE_OK;
    }
    if (it->kind == GSHSCENE_ITEM_INSTANCE && gshscene_resource_picture(it->resource) && FilterIn(gshscene_resource_picture(it->resource), f)) {
      return true;
    }
  }
  return false;
}

bool PrimitiveOf(const Lowered & l, GSHSCENE_Filter * f) {
  return FilterIn(l.list, f);
}

void LowerIn(const std::string & svg, Lowered & l) {
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.image_root = "tests/data/images";
  options.image_base = "tests/data/images";
  LowerSvg(svg, l, 0, 0, &options);
}

size_t Unusable(const Lowered & l) {
  return gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_IMAGE_UNUSABLE);
}

/** The first primitive is an empty result: a flood of transparent black. */
void ExpectEmpty(const Lowered & l, const char * what) {
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f)) << what;
  ASSERT_EQ(f.primitive_count, 1u) << what;
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_FLOOD) << what;
  EXPECT_EQ(f.primitives[0].opacity, 0.0) << what;
  EXPECT_EQ(f.primitives[0].color.rgba[3], 0.0) << what;
  EXPECT_EQ(l.Refusals(), (size_t) 0) << what;
}

}  // namespace

TEST(LowerFeImage, TheImageIsFittedIntoTheFilterRegionAndDrawnByTheImagePrimitive) {
  Lowered l;
  LowerIn(Doc("href=\"" + Data("red-blue-2x1.png") + "\""), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  ASSERT_EQ(f.primitive_count, 1u);
  const GSHSCENE_Filter_Primitive & p = f.primitives[0];
  EXPECT_EQ(p.kind, (uint32_t) GSHSCENE_FILTER_IMAGE);
  EXPECT_EQ(p.input_count, 0u);
  EXPECT_EQ(p.subregion_mask, 0u) << "no subregion is given: the filter region";
  EXPECT_NE(p.image, nullptr);
  GSHSCENE_Image_Info info;
  ASSERT_EQ(gshscene_resource_image(p.image, &info), GSHSCENE_OK);
  EXPECT_EQ(info.width, 2u);
  EXPECT_EQ(info.height, 1u);
  // The region is 1, 2, 12, 12: a 2 by 1 image meets it at a scale of 6, 12 by 6, centered vertically (y = 2 + 3).
  EXPECT_EQ(p.placement.m[0][0], 6.0);
  EXPECT_EQ(p.placement.m[1][1], 6.0);
  EXPECT_EQ(p.placement.t[0], 1.0);
  EXPECT_EQ(p.placement.t[1], 5.0);
  EXPECT_EQ(p.rendering, (uint32_t) GSHSCENE_IMAGE_FILTER_CUBIC);
}

TEST(LowerFeImage, PreserveAspectRatioFitsTheImageIntoTheSubregion) {
  Lowered none;
  LowerIn(Doc("preserveAspectRatio=\"none\" href=\"" + Data("red-blue-2x1.png") + "\""), none);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(none, &f));
  EXPECT_EQ(f.primitives[0].placement.m[0][0], 6.0);
  EXPECT_EQ(f.primitives[0].placement.m[1][1], 12.0);
  EXPECT_EQ(f.primitives[0].placement.t[0], 1.0);
  EXPECT_EQ(f.primitives[0].placement.t[1], 2.0);
  Lowered slice;
  LowerIn(Doc("preserveAspectRatio=\"xMinYMin slice\" href=\"" + Data("red-blue-2x1.png") + "\""), slice);
  ASSERT_TRUE(PrimitiveOf(slice, &f));
  EXPECT_EQ(f.primitives[0].placement.m[0][0], 12.0) << "a slice fills the region, so the image is 24 by 12";
  EXPECT_EQ(f.primitives[0].placement.t[0], 1.0);
  EXPECT_EQ(f.primitives[0].placement.t[1], 2.0);
  Lowered max;
  LowerIn(Doc("preserveAspectRatio=\"xMaxYMax meet\" href=\"" + Data("red-blue-2x1.png") + "\""), max);
  ASSERT_TRUE(PrimitiveOf(max, &f));
  EXPECT_EQ(f.primitives[0].placement.t[0], 1.0);
  EXPECT_EQ(f.primitives[0].placement.t[1], 8.0);
}

TEST(LowerFeImage, TheSubregionIsTheViewportAndIsPassedToTheWaistToClipTheResult) {
  Lowered l;
  LowerIn(Doc("x=\"2\" y=\"3\" width=\"4\" height=\"2\" href=\"" + Data("red-blue-2x1.png") + "\""), l);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  const GSHSCENE_Filter_Primitive & p = f.primitives[0];
  EXPECT_EQ(p.subregion_mask, 15u);
  EXPECT_EQ(p.subregion[0], 2.0);
  EXPECT_EQ(p.subregion[1], 3.0);
  EXPECT_EQ(p.subregion[2], 4.0);
  EXPECT_EQ(p.subregion[3], 2.0);
  EXPECT_EQ(p.placement.m[0][0], 2.0);
  EXPECT_EQ(p.placement.t[0], 2.0);
  EXPECT_EQ(p.placement.t[1], 3.0);
  // A given x with no width: the width is the region's, wherever x puts the left edge (as the waist reads the subregion).
  Lowered partial;
  LowerIn(Doc("x=\"4\" href=\"" + Data("red-blue-2x1.png") + "\""), partial);
  ASSERT_TRUE(PrimitiveOf(partial, &f));
  EXPECT_EQ(f.primitives[0].subregion_mask, 1u);
  EXPECT_EQ(f.primitives[0].placement.t[0], 4.0);
  EXPECT_EQ(f.primitives[0].placement.m[0][0], 6.0) << "12 wide (the region's width, from 4) and 12 high: the scale is 6";
}

TEST(LowerFeImage, PrimitiveUnitsOfTheBoxAreFractionsOfTheBox) {
  Lowered l;
  LowerIn(Doc("x=\"0.25\" width=\"0.5\" preserveAspectRatio=\"none\" href=\"" + Data("red-blue-2x1.png") + "\"", "primitiveUnits=\"objectBoundingBox\""), l);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  const GSHSCENE_Filter_Primitive & p = f.primitives[0];
  // The box is 2, 3, 10, 10: x = 2 + 2.5, width 5; the height is the region's (2 to 14).
  EXPECT_EQ(p.subregion[0], 4.5);
  EXPECT_EQ(p.subregion[2], 5.0);
  EXPECT_EQ(p.placement.t[0], 4.5);
  EXPECT_EQ(p.placement.m[0][0], 2.5);
}

TEST(LowerFeImage, TheElementsMapPutsThePlacementIntoTheLists) {
  Lowered l;
  LowerIn(Doc("preserveAspectRatio=\"none\" href=\"" + Data("red-blue-2x1.png") + "\"", "", "transform=\"translate(10 20) scale(2)\""), l);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  // The region 1, 2, 12, 12 and a map of scale 2 and (10, 20): the list's region is 12, 24, 24, 24, and the placement is that fit of the image.
  EXPECT_EQ(f.region[0], 12.0);
  EXPECT_EQ(f.primitives[0].placement.m[0][0], 12.0);
  EXPECT_EQ(f.primitives[0].placement.m[1][1], 24.0);
  EXPECT_EQ(f.primitives[0].placement.t[0], 12.0);
  EXPECT_EQ(f.primitives[0].placement.t[1], 24.0);
}

TEST(LowerFeImage, ImageRenderingIsTheImagePrimitivesRendering) {
  Lowered l;
  LowerIn(Doc("image-rendering=\"optimizeSpeed\" href=\"" + Data("red-blue-2x1.png") + "\""), l);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  EXPECT_EQ(f.primitives[0].rendering, (uint32_t) GSHSCENE_IMAGE_FILTER_NEAREST);
}

TEST(LowerFeImage, AFileUnderTheImageRootIsReadAndAFeImageIsOtherwiseLikeAnImage) {
  Lowered l;
  LowerIn(Doc("href=\"red-blue-2x1.png\""), l);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_IMAGE);
  Lowered network;
  LowerIn(Doc("href=\"https://example.com/a.png\""), network);
  EXPECT_NE(network.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  EXPECT_EQ(network.Items(), (size_t) 0) << "the element is not drawn";
  Lowered svg;
  LowerIn(Doc("href=\"data:image/svg+xml,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20width%3D%224%22%20height%3D%224%22%3E%3Crect%20width%3D%224%22%20height%3D%224%22%2F%3E%3C%2Fsvg%3E\""), svg);
  EXPECT_EQ(svg.Refusals(), (size_t) 0) << "an SVG document is a slot of the filter's picture";
  GSHSCENE_Filter sf;
  ASSERT_TRUE(PrimitiveOf(svg, &sf));
  EXPECT_EQ(sf.primitives[0].use_picture, 1u);
  EXPECT_NE(sf.picture, nullptr);
  Lowered unrooted;
  LowerSvg(Doc("href=\"red-blue-2x1.png\""), unrooted);
  EXPECT_NE(unrooted.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_image_bytes = 10;
  Lowered limited;
  LoadSvg(Doc("href=\"" + Data("red-blue-2x1.png") + "\""), limited.loaded, 0, 0, &limits);
  LowerBoth(limited.loaded, limited);
  EXPECT_NE(limited.Refusal(GVEC_LOWER_LIMIT), nullptr);
  EXPECT_EQ(limited.Items(), (size_t) 0);
}

TEST(LowerFeImage, NothingToDrawIsAnEmptyResultAndTheElementIsStillDrawnThroughTheFilter) {
  Lowered none;
  LowerIn(Doc(""), none);
  ExpectEmpty(none, "no href");
  EXPECT_EQ(Unusable(none), (size_t) 1);
  Lowered empty;
  LowerIn(Doc("href=\"\""), empty);
  ExpectEmpty(empty, "empty href");
  Lowered missing;
  LowerIn(Doc("href=\"#nothing\""), missing);
  ExpectEmpty(missing, "an id that names nothing");
  Lowered broken;
  LowerIn(Doc("href=\"data:image/png;base64,AAAA\""), broken);
  ExpectEmpty(broken, "undecodable");
  EXPECT_EQ(Unusable(broken), (size_t) 1);
  Lowered absent;
  LowerIn(Doc("href=\"no-such-file.png\""), absent);
  ExpectEmpty(absent, "a file that is not there");
  Lowered flat;
  LowerIn(Doc("width=\"0\" href=\"" + Data("red-blue-2x1.png") + "\""), flat);
  ExpectEmpty(flat, "a subregion of no width");
}

namespace {

/** The picture of the first instance in the filter's picture (the one slot's element), and the instance's map. */
const GSHSCENE_Item * SlotInstance(const GSHSCENE_Filter & f, size_t slot) {
  const GSHSCENE_List * slots = gshscene_resource_picture(f.picture);
  const GSHSCENE_Item * it = nullptr;
  return slots && gshscene_list_item(slots, slot, &it) == GSHSCENE_OK ? it : nullptr;
}

}  // namespace

TEST(LowerFeImage, AnElementOfTheDocumentIsADrawnSlotOfTheFiltersPicture) {
  for (const char * target : {"<rect id=\"x\" width=\"4\" height=\"4\"/>", "<g id=\"x\"><rect width=\"4\" height=\"4\"/></g>",
           "<use id=\"x\" href=\"#y\"/><path id=\"y\" d=\"M0 0H1V1Z\"/>"}) {
    Lowered l;
    LowerSvg(Svg("width=\"40\" height=\"40\"", std::string("<defs><filter id=\"f\"><feImage href=\"#x\"/></filter>") + target +
                  "</defs><rect width=\"10\" height=\"10\" filter=\"url(#f)\"/><rect x=\"20\" width=\"4\" height=\"4\"/>"),
        l);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << target;
    GSHSCENE_Filter f;
    ASSERT_TRUE(PrimitiveOf(l, &f)) << target;
    ASSERT_EQ(f.primitive_count, (size_t) 1);
    EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_IMAGE);
    EXPECT_EQ(f.primitives[0].use_picture, 1u);
    EXPECT_EQ(f.primitives[0].slot, 0u);
    EXPECT_EQ(f.primitives[0].image, nullptr);
    ASSERT_NE(f.picture, nullptr) << target;
    EXPECT_EQ(gshscene_list_count(gshscene_resource_picture(f.picture)), (size_t) 1);
    const GSHSCENE_Item * inst = SlotInstance(f, 0);
    ASSERT_NE(inst, nullptr);
    EXPECT_EQ(inst->kind, GSHSCENE_ITEM_INSTANCE);
    // the element is placed at the filter region's origin (the default subregion): the region of a 10 by 10 rect at the origin is -1, -1
    EXPECT_NEAR(inst->map.t[0], -1.0, 1e-9) << target;
    EXPECT_NEAR(inst->map.t[1], -1.0, 1e-9) << target;
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 2) << "the filtered rect and the rest of the document are drawn";
  }
}

TEST(LowerFeImage, TheSubregionsXAndYPlaceTheElementAndTwoElementsAreTwoSlots) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<defs><filter id=\"f\"><feImage href=\"#a\" x=\"5\" y=\"6\" result=\"i\"/><feImage href=\"#b\" x=\"1\"/>"
               "<feMerge><feMergeNode in=\"i\"/><feMergeNode/></feMerge></filter><rect id=\"a\" width=\"4\" height=\"4\"/>"
               "<circle id=\"b\" r=\"2\"/></defs><rect width=\"10\" height=\"10\" filter=\"url(#f)\"/>"),
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  ASSERT_EQ(f.primitive_count, (size_t) 3);
  ASSERT_NE(f.picture, nullptr);
  EXPECT_EQ(gshscene_list_count(gshscene_resource_picture(f.picture)), (size_t) 2);
  EXPECT_EQ(f.primitives[0].slot, 0u);
  EXPECT_EQ(f.primitives[1].slot, 1u);
  const GSHSCENE_Item * a = SlotInstance(f, 0);
  const GSHSCENE_Item * b = SlotInstance(f, 1);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NEAR(a->map.t[0], 5.0, 1e-9);
  EXPECT_NEAR(a->map.t[1], 6.0, 1e-9);
  EXPECT_NEAR(b->map.t[0], 1.0, 1e-9);
  EXPECT_NEAR(b->map.t[1], -1.0, 1e-9) << "y is the filter region's where the element gives none";
}

TEST(LowerFeImage, AnElementThatDrawsNothingOrIsNotThereIsAnEmptyResultAndAFilterOnItselfIsACycle) {
  const char * empty[] = {
      "<defs><filter id=\"f\"><feImage href=\"#nothing\"/></filter></defs><rect width=\"10\" height=\"10\" filter=\"url(#f)\"/>",
      "<defs><filter id=\"f\"><feImage href=\"#x\"/></filter><rect id=\"x\" width=\"4\" height=\"4\" display=\"none\"/></defs>"
      "<rect width=\"10\" height=\"10\" filter=\"url(#f)\"/>",
      // the element draws through this very filter: a cycle
      "<defs><filter id=\"f\"><feImage href=\"#x\"/></filter></defs><rect id=\"x\" width=\"10\" height=\"10\" filter=\"url(#f)\"/>",
  };
  for (const char * doc : empty) {
    Lowered l;
    LowerSvg(Svg("width=\"40\" height=\"40\"", doc), l);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << doc;
    GSHSCENE_Filter f;
    ASSERT_TRUE(PrimitiveOf(l, &f)) << doc;
    EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_FLOOD) << doc << ": transparent black";
    EXPECT_EQ(f.picture, nullptr) << doc;
  }
}

TEST(LowerFeImage, AFeImageIsAPrimitiveWithNoInputAndTheNextOneTakesItsResult) {
  Lowered l;
  LowerIn(Svg("width=\"40\" height=\"40\"",
              "<defs><filter id=\"f\"><feImage result=\"i\" href=\"" + Data("red-blue-2x1.png") + "\"/><feOffset in=\"i\" dx=\"2\"/><feImage href=\"red-blue-2x1.png\"/>"
              "<feMerge><feMergeNode in=\"i\"/><feMergeNode/></feMerge></filter></defs><rect width=\"10\" height=\"10\" filter=\"url(#f)\"/>"),
      l);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  ASSERT_EQ(f.primitive_count, 4u);
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_IMAGE);
  EXPECT_EQ(f.primitives[1].kind, (uint32_t) GSHSCENE_FILTER_OFFSET);
  ASSERT_EQ(f.primitives[1].input_count, 1u);
  EXPECT_EQ(f.primitives[1].inputs[0].kind, (uint32_t) GSHSCENE_FILTER_INPUT_RESULT);
  EXPECT_EQ(f.primitives[1].inputs[0].index, 0u);
  EXPECT_EQ(f.primitives[2].kind, (uint32_t) GSHSCENE_FILTER_IMAGE);
  EXPECT_EQ(f.primitives[2].input_count, 0u);
  EXPECT_EQ(f.primitives[3].kind, (uint32_t) GSHSCENE_FILTER_MERGE);
}

TEST(LowerFeImage, ARotatedElementIsFilteredInAPictureOfItsOwnAndTheImageIsPlacedInThatSpace) {
  Lowered l;
  LowerIn(Doc("preserveAspectRatio=\"none\" href=\"" + Data("red-blue-2x1.png") + "\"", "", "transform=\"rotate(30)\""), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(PrimitiveOf(l, &f));
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_IMAGE);
  // The filter is in the element's own space (the instance carries the rotation): the region 1, 2, 12, 12 as it is.
  EXPECT_EQ(f.primitives[0].placement.m[0][0], 6.0);
  EXPECT_EQ(f.primitives[0].placement.m[1][1], 12.0);
  EXPECT_EQ(f.primitives[0].placement.t[0], 1.0);
  EXPECT_EQ(f.primitives[0].placement.t[1], 2.0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
