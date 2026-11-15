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
 * The `filter` property lowered (lower_filter.c): a filter element becomes a filter resource of the waist on a group around the element's
 * content, its region and lengths put into the list's space through the element's map, an invalid or dangling filter draws nothing, and
 * what no sink draws yet is refused whole.
 */

#include "../lower_helpers.h"

#include <ghoti.io/shape-scene/filter.h>

#include <cmath>
#include <string>

using namespace gvec_test;

namespace {

const char * const kRect = "<rect x=\"2\" y=\"3\" width=\"10\" height=\"10\" fill=\"red\" filter=\"url(#f)\"";

/** The filter of the first group of the list, read back. */
bool FilterOf(const Lowered & l, GSHSCENE_Filter * out) {
  for (size_t i = 0; i < l.Items(); i++) {
    const GSHSCENE_Item & it = l.Item(i);
    if (it.kind == GSHSCENE_ITEM_GROUP_PUSH && it.group.filter) {
      return gshscene_resource_filter(it.group.filter, out) == GSHSCENE_OK;
    }
  }
  return false;
}

std::string Doc(const std::string & filter, const std::string & element = kRect) {
  return Svg("width=\"40\" height=\"40\"", "<defs>" + filter + "</defs>" + element + "/>");
}

}  // namespace

TEST(LowerFilter, ABlurOnARectIsAGroupWithTheFilterAndTheDefaultRegionOfTheBox) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feGaussianBlur stdDeviation=\"2 3\"/></filter>"), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 1u);
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_GAUSSIAN_BLUR);
  EXPECT_EQ(f.primitives[0].sx, 2.0);
  EXPECT_EQ(f.primitives[0].sy, 3.0);
  ASSERT_EQ(f.primitives[0].input_count, 1u);
  EXPECT_EQ(f.primitives[0].inputs[0].kind, (uint32_t) GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC);
  // -10% and 120% of the box (2, 3, 10, 10)
  EXPECT_EQ(f.region_mode, (uint32_t) GSHSCENE_FILTER_REGION_EXPLICIT);
  EXPECT_DOUBLE_EQ(f.region[0], 1.0);
  EXPECT_DOUBLE_EQ(f.region[1], 2.0);
  EXPECT_DOUBLE_EQ(f.region[2], 12.0);
  EXPECT_DOUBLE_EQ(f.region[3], 12.0);
}

TEST(LowerFilter, TheRegionAndTheLengthsGoThroughTheElementsMap) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"1\" y=\"2\" width=\"3\" height=\"4\">"
               "<feOffset dx=\"1\" dy=\"-1\"/><feGaussianBlur stdDeviation=\"1\"/></filter>",
               "<rect width=\"10\" height=\"10\" transform=\"translate(5 6) scale(2 3)\" filter=\"url(#f)\""), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  EXPECT_DOUBLE_EQ(f.region[0], 7.0);   // 2 * 1 + 5
  EXPECT_DOUBLE_EQ(f.region[1], 12.0);  // 3 * 2 + 6
  EXPECT_DOUBLE_EQ(f.region[2], 6.0);
  EXPECT_DOUBLE_EQ(f.region[3], 12.0);
  ASSERT_EQ(f.primitive_count, 2u);
  EXPECT_DOUBLE_EQ(f.primitives[0].dx, 2.0);
  EXPECT_DOUBLE_EQ(f.primitives[0].dy, -3.0);
  EXPECT_DOUBLE_EQ(f.primitives[1].sx, 2.0);
  EXPECT_DOUBLE_EQ(f.primitives[1].sy, 3.0);
  EXPECT_EQ(f.primitives[1].inputs[0].kind, (uint32_t) GSHSCENE_FILTER_INPUT_RESULT) << "the default input is the previous result";
  EXPECT_EQ(f.primitives[1].inputs[0].index, 0u);
}

TEST(LowerFilter, NamedResultsBecomeIndicesAndTheRemovedInputsAnEmptyFlood) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feGaussianBlur in=\"SourceAlpha\" stdDeviation=\"1\" result=\"b\"/>"
               "<feOffset in=\"b\" dx=\"2\" dy=\"2\" result=\"o\"/>"
               "<feFlood flood-color=\"blue\" flood-opacity=\"0.5\" result=\"c\"/>"
               "<feComposite in=\"c\" in2=\"o\" operator=\"in\" result=\"s\"/>"
               "<feMerge><feMergeNode in=\"s\"/><feMergeNode in=\"SourceGraphic\"/><feMergeNode in=\"FillPaint\"/></feMerge></filter>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 6u) << "five primitives and the flood that stands for FillPaint";
  EXPECT_EQ(f.primitives[0].inputs[0].kind, (uint32_t) GSHSCENE_FILTER_INPUT_SOURCE_ALPHA);
  EXPECT_EQ(f.primitives[1].inputs[0].index, 0u);
  EXPECT_EQ(f.primitives[2].kind, (uint32_t) GSHSCENE_FILTER_FLOOD);
  EXPECT_DOUBLE_EQ(f.primitives[2].opacity, 0.5);
  EXPECT_EQ(f.primitives[2].color.rgba[2], 1.0);
  EXPECT_EQ(f.primitives[3].kind, (uint32_t) GSHSCENE_FILTER_COMPOSITE);
  EXPECT_EQ(f.primitives[3].mode, (uint32_t) GSHSCENE_FILTER_COMPOSITE_IN);
  EXPECT_EQ(f.primitives[3].inputs[0].index, 2u);
  EXPECT_EQ(f.primitives[3].inputs[1].index, 1u);
  EXPECT_EQ(f.primitives[4].kind, (uint32_t) GSHSCENE_FILTER_FLOOD) << "FillPaint is transparent black";
  EXPECT_EQ(f.primitives[5].kind, (uint32_t) GSHSCENE_FILTER_MERGE);
  ASSERT_EQ(f.primitives[5].input_count, 3u);
  EXPECT_EQ(f.primitives[5].inputs[0].index, 3u);
  EXPECT_EQ(f.primitives[5].inputs[1].kind, (uint32_t) GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC);
  EXPECT_EQ(f.primitives[5].inputs[2].index, 4u);
}

TEST(LowerFilter, ColorMatrixTransferShadowMorphologyAndConvolutionCarryTheirAttributes) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\" color-interpolation-filters=\"sRGB\">"
               "<feColorMatrix type=\"saturate\" values=\"0.5\"/>"
               "<feComponentTransfer><feFuncR type=\"table\" tableValues=\"0 0.5 1\"/><feFuncA type=\"linear\" slope=\"0.5\" intercept=\"0.25\"/></feComponentTransfer>"
               "<feDropShadow dx=\"3\" dy=\"4\" stdDeviation=\"1.5\" flood-color=\"#00ff00\"/>"
               "<feMorphology operator=\"dilate\" radius=\"2\"/>"
               "<feConvolveMatrix order=\"3\" kernelMatrix=\"0 1 0 1 -4 1 0 1 0\" edgeMode=\"wrap\" preserveAlpha=\"true\"/>"
               "<feDisplacementMap in2=\"SourceGraphic\" scale=\"5\" xChannelSelector=\"R\" yChannelSelector=\"G\"/></filter>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 6u);
  EXPECT_EQ(f.primitives[0].mode, (uint32_t) GSHSCENE_FILTER_MATRIX_SATURATE);
  EXPECT_EQ(f.primitives[0].values[0], 0.5);
  EXPECT_EQ(f.primitives[0].color_space, (uint32_t) GSHSCENE_FILTER_SPACE_SRGB);
  EXPECT_EQ(f.primitives[1].transfer[0].type, (uint32_t) GSHSCENE_FILTER_TRANSFER_TABLE);
  EXPECT_EQ(f.primitives[1].transfer[0].count, 3u);
  EXPECT_EQ(f.primitives[1].transfer[3].type, (uint32_t) GSHSCENE_FILTER_TRANSFER_LINEAR);
  EXPECT_EQ(f.primitives[1].transfer[3].slope, 0.5);
  EXPECT_EQ(f.primitives[2].kind, (uint32_t) GSHSCENE_FILTER_DROP_SHADOW);
  EXPECT_EQ(f.primitives[2].dx, 3.0);
  EXPECT_EQ(f.primitives[2].sx, 1.5);
  EXPECT_EQ(f.primitives[2].color.rgba[1], 1.0);
  EXPECT_EQ(f.primitives[3].mode, (uint32_t) GSHSCENE_FILTER_MORPHOLOGY_DILATE);
  EXPECT_EQ(f.primitives[3].sx, 2.0);
  EXPECT_EQ(f.primitives[4].order_x, 3u);
  EXPECT_EQ(f.primitives[4].target_x, 1u);
  EXPECT_EQ(f.primitives[4].divisor, 1.0) << "the kernel sums to zero: the divisor is 1";
  EXPECT_EQ(f.primitives[4].edge_mode, (uint32_t) GSHSCENE_FILTER_EDGE_WRAP);
  EXPECT_EQ(f.primitives[4].preserve_alpha, 1u);
  EXPECT_EQ(f.primitives[5].kind, (uint32_t) GSHSCENE_FILTER_DISPLACEMENT_MAP);
  EXPECT_EQ(f.primitives[5].scale, 5.0);
  EXPECT_EQ(f.primitives[5].x_channel, (uint32_t) GSHSCENE_FILTER_CHANNEL_R);
  EXPECT_EQ(f.primitives[5].y_channel, (uint32_t) GSHSCENE_FILTER_CHANNEL_G);
}

TEST(LowerFilter, AnInvalidOrDanglingFilterDrawsTheElementAsNothingAndIsNoRefusal) {
  const char * bad[] = {
      "<filter id=\"f\"/>",                                                          // no primitives
      "<filter id=\"f\" width=\"0\"><feOffset dx=\"1\"/></filter>",                  // no region
      "<filter id=\"f\"><feConvolveMatrix order=\"3\" kernelMatrix=\"1 2\"/></filter>",  // a kernel of the wrong size
  };
  for (const char * filter : bad) {
    Lowered l;
    LowerSvg(Doc(filter), l);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << filter;
    EXPECT_EQ(l.Items(), (size_t) 0) << filter;
  }
  Lowered dangling;
  LowerSvg(Doc("", "<rect width=\"4\" height=\"4\" filter=\"url(#nothing)\""), dangling);
  EXPECT_EQ(dangling.Refusals(), (size_t) 0);
  EXPECT_EQ(dangling.Items(), (size_t) 0);
}

TEST(LowerFilter, AnInvalidValueDisablesTheEffectAndTheElementIsDrawnThroughIt) {
  const char * disabled[] = {
      "<filter id=\"f\"><feGaussianBlur stdDeviation=\"-1\"/></filter>",
      "<filter id=\"f\"><feGaussianBlur stdDeviation=\"\"/></filter>",
      "<filter id=\"f\"><feMorphology radius=\"-2\"/></filter>",
      "<filter id=\"f\"><feOffset dx=\"20%\" dy=\"x\"/></filter>",
  };
  for (const char * filter : disabled) {
    Lowered l;
    LowerSvg(Doc(filter), l);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << filter;
    GSHSCENE_Filter f;
    ASSERT_TRUE(FilterOf(l, &f)) << filter;
    EXPECT_EQ(f.primitives[0].sx + f.primitives[0].sy + f.primitives[0].dx + f.primitives[0].dy, 0.0) << filter;
  }
}

TEST(LowerFilter, AFilterTakesItsPrimitivesAndAttributesFromTheOneItsHrefNames) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"g\" x=\"0.1\" y=\"0.2\" width=\"0.8\" height=\"1\"><feGaussianBlur stdDeviation=\"4\"/></filter>"
               "<filter id=\"f\" href=\"#g\" y=\"0\"/>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 1u);
  EXPECT_EQ(f.primitives[0].sx, 4.0);
  EXPECT_DOUBLE_EQ(f.region[0], 3.0);   // 2 + 0.1 * 10
  EXPECT_DOUBLE_EQ(f.region[1], 3.0);   // the referring filter's own y
  EXPECT_DOUBLE_EQ(f.region[2], 8.0);
  EXPECT_DOUBLE_EQ(f.region[3], 10.0);
}

TEST(LowerFilter, WhatNoSinkDrawsYetIsRefusedWhole) {
  const char * refused[] = {
      "<filter id=\"f\"><feTurbulence baseFrequency=\"0.1\"/></filter>",
      "<filter id=\"f\"><feImage href=\"#x\"/></filter>",
      "<filter id=\"f\"><feDiffuseLighting><feDistantLight/></feDiffuseLighting></filter>",
  };
  for (const char * filter : refused) {
    Lowered l;
    LowerSvg(Doc(filter), l);
    EXPECT_NE(l.Refusal(GVEC_LOWER_FILTER), nullptr) << filter;
    EXPECT_EQ(l.Items(), (size_t) 0) << filter;
  }
  Lowered fn;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<rect width=\"4\" height=\"4\" style=\"filter: blur(2px)\"/>"), fn);
  EXPECT_NE(fn.Refusal(GVEC_LOWER_FILTER), nullptr) << "a filter function";
  Lowered turned;
  LowerSvg(Doc("<filter id=\"f\"><feOffset dx=\"1\"/></filter>", "<rect width=\"10\" height=\"10\" transform=\"rotate(30)\" filter=\"url(#f)\""), turned);
  EXPECT_EQ(turned.Refusals(), (size_t) 0) << "a rotated element is filtered in a picture of its own";
  bool instance = false;
  for (size_t i = 0; i < turned.Items(); i++) {
    instance = instance || turned.Item(i).kind == GSHSCENE_ITEM_INSTANCE;
  }
  EXPECT_TRUE(instance);
  Lowered quarter;
  LowerSvg(Doc("<filter id=\"f\"><feOffset dx=\"1\"/></filter>", "<rect width=\"10\" height=\"10\" transform=\"rotate(90)\" filter=\"url(#f)\""), quarter);
  EXPECT_EQ(quarter.Refusals(), (size_t) 0) << "a quarter turn is axis aligned";
}

TEST(LowerFilter, TheFilterIsInsideTheClipTheMaskAndTheOpacity) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feOffset dx=\"1\"/></filter><clipPath id=\"c\"><rect width=\"20\" height=\"20\"/></clipPath>",
               "<rect width=\"10\" height=\"10\" opacity=\"0.5\" clip-path=\"url(#c)\" filter=\"url(#f)\""),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  // the order of the items: the clip, the opacity group, the filter group, the shape
  size_t clip = SIZE_MAX, opacity = SIZE_MAX, filter = SIZE_MAX;
  for (size_t i = 0; i < l.Items(); i++) {
    const GSHSCENE_Item & it = l.Item(i);
    if (it.kind == GSHSCENE_ITEM_CLIP_PUSH && clip == SIZE_MAX) {
      clip = i;
    }
    if (it.kind == GSHSCENE_ITEM_GROUP_PUSH && it.group.filter && filter == SIZE_MAX) {
      filter = i;
      if (it.group.opacity == 0.5) {
        opacity = i; // the filter's own group carries the opacity
      }
    }
    if (it.kind == GSHSCENE_ITEM_GROUP_PUSH && !it.group.filter && it.group.opacity == 0.5) {
      opacity = SIZE_MAX - 1; // an opacity group inside the filter would fade what the filter reads
    }
  }
  ASSERT_NE(filter, SIZE_MAX);
  ASSERT_NE(clip, SIZE_MAX);
  EXPECT_LT(clip, filter);
  EXPECT_EQ(opacity, filter) << "the opacity is the filter group's: it applies to the filtered result";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
