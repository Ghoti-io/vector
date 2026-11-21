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
#include <cstring>
#include <string>

#include <vector>

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

TEST(LowerFilter, AnInvalidFilterDrawsTheElementAsNothingAndADanglingOneAsItIs) {
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
  // Filter Effects 1: a reference to a non-existent object "ignores the whole filter chain", so the element is drawn as it is.
  Lowered dangling;
  LowerSvg(Doc("", "<rect width=\"4\" height=\"4\" filter=\"url(#nothing)\""), dangling);
  EXPECT_EQ(dangling.Refusals(), (size_t) 0);
  EXPECT_EQ(dangling.Items(), (size_t) 1);
  GSHSCENE_Filter none;
  EXPECT_FALSE(FilterOf(dangling, &none)) << "and it carries no filter";
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

TEST(LowerFilter, ATurbulencesFrequencyIsPerUnitOfTheListAndItsOriginIsWhereTheUserSpacesLies) {
  // The noise is a function of the point in the element's user space: under a scale of (2, 4) and a translation of (5, 6) a frequency of
  // 0.2 by 0.4 per user unit is 0.1 by 0.1 per unit of the list, and the user space's origin lies at (5, 6) of it.
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feTurbulence baseFrequency=\"0.2 0.4\" numOctaves=\"2\"/></filter>",
               "<rect width=\"10\" height=\"10\" transform=\"translate(5 6) scale(2 4)\" filter=\"url(#f)\""), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 1u);
  EXPECT_DOUBLE_EQ(f.primitives[0].sx, 0.1);
  EXPECT_DOUBLE_EQ(f.primitives[0].sy, 0.1);
  EXPECT_DOUBLE_EQ(f.primitives[0].dx, 5.0);
  EXPECT_DOUBLE_EQ(f.primitives[0].dy, 6.0);
}

TEST(LowerFilter, ATurbulenceUnderAViewBoxScaleIsTheNoiseAtTheDocumentsOwnSize) {
  // A 200 unit document drawn at 500 pixels: the base frequency of 0.01 per unit is 0.004 per pixel, not 0.01.
  Lowered l;
  LowerSvg(Svg("width=\"500\" height=\"500\" viewBox=\"0 0 200 200\"",
               "<defs><filter id=\"f\"><feTurbulence baseFrequency=\"0.01\"/></filter></defs><rect x=\"50\" y=\"50\" width=\"100\" height=\"100\" filter=\"url(#f)\"/>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  EXPECT_DOUBLE_EQ(f.primitives[0].sx, 0.004);
  EXPECT_DOUBLE_EQ(f.primitives[0].sy, 0.004);
  EXPECT_DOUBLE_EQ(f.primitives[0].dx, 0.0);
}

TEST(LowerFilter, ATurbulenceUnderAMapThatTurnsOrMirrorsItsElementIsRefused) {
  // A turn or a mirror turns the noise lattice with the element, which the waist's axis-aligned lattice cannot say.
  for (const char * transform : {"rotate(90)", "rotate(-90)", "scale(-1 1)", "scale(1 -1)"}) {
    Lowered l;
    LowerSvg(Doc("<filter id=\"f\"><feTurbulence baseFrequency=\"0.1\"/></filter>",
                 std::string("<rect width=\"10\" height=\"10\" transform=\"") + transform + "\" filter=\"url(#f)\""),
             l);
    ASSERT_GE(l.Refusals(), (size_t) 1) << transform;
    bool named = false;
    for (size_t i = 0; i < l.by_list.count; i++) {
      named = named || (l.by_list.items[i].code == GVEC_DIAG_LOWER_REFUSED && std::strstr(l.by_list.items[i].message, "turns or mirrors") != nullptr);
    }
    EXPECT_TRUE(named) << transform << ": the refusal says why, and not that a number was out of range";
  }
}

TEST(LowerFilter, TurbulenceAndTheLightingsAreLowered) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feTurbulence baseFrequency=\"0.05 0.1\" numOctaves=\"3\" seed=\"7\" type=\"fractalNoise\" stitchTiles=\"stitch\"/>"
               "<feDiffuseLighting in=\"SourceAlpha\" surfaceScale=\"2\" diffuseConstant=\"1.5\" lighting-color=\"#ff8000\"><feDistantLight azimuth=\"45\" elevation=\"60\"/></feDiffuseLighting>"
               "<feSpecularLighting specularConstant=\"2\" specularExponent=\"20\"><feSpotLight x=\"1\" y=\"2\" z=\"3\" pointsAtX=\"4\" pointsAtY=\"5\" pointsAtZ=\"6\" "
               "specularExponent=\"2\" limitingConeAngle=\"30\"/></feSpecularLighting></filter>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 3u);
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_TURBULENCE);
  EXPECT_EQ(f.primitives[0].input_count, 0u);
  EXPECT_EQ(f.primitives[0].sx, 0.05);
  EXPECT_EQ(f.primitives[0].sy, 0.1);
  EXPECT_EQ(f.primitives[0].octaves, 3u);
  EXPECT_EQ(f.primitives[0].seed, 7.0);
  EXPECT_EQ(f.primitives[0].mode, (uint32_t) GSHSCENE_FILTER_NOISE_FRACTAL);
  EXPECT_EQ(f.primitives[0].stitch_tiles, 1u);
  EXPECT_EQ(f.primitives[1].kind, (uint32_t) GSHSCENE_FILTER_DIFFUSE_LIGHTING);
  EXPECT_EQ(f.primitives[1].inputs[0].kind, (uint32_t) GSHSCENE_FILTER_INPUT_SOURCE_ALPHA);
  EXPECT_EQ(f.primitives[1].surface_scale, 2.0);
  EXPECT_EQ(f.primitives[1].constant, 1.5);
  EXPECT_EQ(f.primitives[1].light.kind, (uint32_t) GSHSCENE_FILTER_LIGHT_DISTANT);
  EXPECT_EQ(f.primitives[1].light.azimuth, 45.0);
  EXPECT_NEAR(f.primitives[1].color.rgba[1], 128.0 / 255.0, 1e-9);
  EXPECT_EQ(f.primitives[2].kind, (uint32_t) GSHSCENE_FILTER_SPECULAR_LIGHTING);
  EXPECT_EQ(f.primitives[2].exponent, 20.0);
  EXPECT_EQ(f.primitives[2].light.kind, (uint32_t) GSHSCENE_FILTER_LIGHT_SPOT);
  EXPECT_EQ(f.primitives[2].light.position[2], 3.0);
  EXPECT_EQ(f.primitives[2].light.has_cone, 1u);
  EXPECT_EQ(f.primitives[2].light.cone_angle, 30.0);
  // a specular exponent out of range, or no light, draws nothing
  Lowered bad;
  LowerSvg(Doc("<filter id=\"f\"><feSpecularLighting specularExponent=\"200\"><feDistantLight/></feSpecularLighting></filter>"), bad);
  EXPECT_EQ(bad.Items(), (size_t) 0);
  EXPECT_EQ(bad.Refusals(), (size_t) 0);
}

TEST(LowerFilter, AFilterInTheUnitsOfTheBoxOfAUseOfASymbolOrANestedSvgIsPlacedInTheirBox) {
  Lowered symbol, nested, direct;
  const std::string filter = "<filter id=\"f\"><feOffset dx=\"1\"/></filter>";
  const std::string attrs = "width=\"40\" height=\"40\"";
  LowerSvg(Svg(attrs, "<defs>" + filter + "<symbol id=\"s\" viewBox=\"0 0 5 5\"><rect width=\"5\" height=\"5\"/></symbol></defs>"
      "<g filter=\"url(#f)\"><use href=\"#s\" x=\"3\" width=\"10\" height=\"10\"/></g>"), symbol);
  LowerSvg(Svg(attrs, "<defs>" + filter + "</defs><g filter=\"url(#f)\"><svg x=\"3\" width=\"10\" height=\"10\" viewBox=\"0 0 5 5\">"
      "<rect width=\"5\" height=\"5\"/></svg></g>"), nested);
  LowerSvg(Svg(attrs, "<defs>" + filter + "</defs><g filter=\"url(#f)\"><rect x=\"3\" width=\"10\" height=\"10\"/></g>"), direct);
  EXPECT_EQ(symbol.Refusals(), (size_t) 0);
  EXPECT_EQ(nested.Refusals(), (size_t) 0);
  GSHSCENE_Filter a, b, c;
  ASSERT_TRUE(FilterOf(symbol, &a));
  ASSERT_TRUE(FilterOf(nested, &c));
  ASSERT_TRUE(FilterOf(direct, &b));
  for (int i = 0; i < 4; i++) {
    EXPECT_DOUBLE_EQ(a.region[i], b.region[i]) << "the symbol's 10 by 10 at x 3 has the region of a rect there, side " << i;
    EXPECT_DOUBLE_EQ(c.region[i], b.region[i]) << "so does the nested svg, side " << i;
  }
}

TEST(LowerFilter, WhatNoSinkDrawsYetIsRefusedWhole) {
  Lowered other;
  LowerSvg(Doc("", "<rect width=\"10\" height=\"10\" filter=\"url(other.svg#f)\"/>"), other);
  EXPECT_NE(other.Refusal(GVEC_LOWER_FILTER), nullptr) << "a filter of another document";
  EXPECT_EQ(other.Items(), (size_t) 0);
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

TEST(LowerFilter, AListOfFiltersIsNestedGroupsTheFirstInnermost) {
  const std::string two = "<filter id=\"a\"><feFlood flood-color=\"red\"/></filter><filter id=\"b\"><feGaussianBlur stdDeviation=\"2\"/></filter>";
  Lowered l;
  LowerSvg(Doc(two, "<rect width=\"10\" height=\"10\" fill=\"blue\" filter=\"url(#a) url(#b)\" opacity=\"0.5\""), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  std::vector<const GSHSCENE_Item *> groups;
  for (size_t i = 0; i < l.Items(); i++) {
    if (l.Item(i).kind == GSHSCENE_ITEM_GROUP_PUSH) {
      groups.push_back(&l.Item(i));
    }
  }
  ASSERT_EQ(groups.size(), (size_t) 2);
  GSHSCENE_Filter outer, inner;
  ASSERT_EQ(gshscene_resource_filter(groups[0]->group.filter, &outer), GSHSCENE_OK);
  ASSERT_EQ(gshscene_resource_filter(groups[1]->group.filter, &inner), GSHSCENE_OK);
  EXPECT_EQ(outer.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_GAUSSIAN_BLUR) << "the second filter is applied last, so outermost";
  EXPECT_EQ(inner.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_FLOOD) << "the first is applied to the element";
  EXPECT_DOUBLE_EQ(groups[0]->group.opacity, 0.5) << "the opacity is applied to the final result";
  EXPECT_DOUBLE_EQ(groups[1]->group.opacity, 1.0);
}

TEST(LowerFilter, AListMixingFunctionsAndUrlsKeepsRunsOfFunctionsInOneFilter) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"a\"><feGaussianBlur stdDeviation=\"1\"/></filter>",
               "<rect width=\"10\" height=\"10\" filter=\"grayscale(1) sepia(0.5) url(#a) opacity(0.5)\""),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  std::vector<GSHSCENE_Filter> filters;
  for (size_t i = 0; i < l.Items(); i++) {
    if (l.Item(i).kind == GSHSCENE_ITEM_GROUP_PUSH && l.Item(i).group.filter) {
      GSHSCENE_Filter f;
      ASSERT_EQ(gshscene_resource_filter(l.Item(i).group.filter, &f), GSHSCENE_OK);
      filters.push_back(f);
    }
  }
  ASSERT_EQ(filters.size(), (size_t) 3) << "functions, the reference, functions";
  EXPECT_EQ(filters[0].primitive_count, 1u) << "opacity()";
  EXPECT_EQ(filters[1].primitives[0].kind, (uint32_t) GSHSCENE_FILTER_GAUSSIAN_BLUR);
  EXPECT_EQ(filters[2].primitive_count, 2u) << "grayscale() and sepia() are one filter";
}

TEST(LowerFilter, AnInvalidReferenceInAListIgnoresTheWholeListAndTheElementIsDrawnAsItIs) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"a\"><feGaussianBlur stdDeviation=\"1\"/></filter>",
               "<rect width=\"10\" height=\"10\" filter=\"url(#a) url(#missing)\""),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Items(), (size_t) 1);
  GSHSCENE_Filter f;
  EXPECT_FALSE(FilterOf(l, &f)) << "the good filter of the list is ignored with the bad one";
}

TEST(LowerFilter, AnFeMergeWithNoNodesIsTransparentBlackWhichThePrimitiveAfterItTakes) {
  // Filter Effects 1: the merge of nothing is transparent black; the blur after it blurs that, so Chromium, Firefox and resvg draw nothing.
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feMerge/><feGaussianBlur stdDeviation=\"4\"/></filter>"), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 2u);
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_FLOOD);
  EXPECT_EQ(f.primitives[0].opacity, 0.0);
  EXPECT_EQ(f.primitives[0].color.rgba[3], 0.0);
  EXPECT_EQ(f.primitives[1].kind, (uint32_t) GSHSCENE_FILTER_GAUSSIAN_BLUR);
  EXPECT_EQ(f.primitives[1].inputs[0].kind, (uint32_t) GSHSCENE_FILTER_INPUT_RESULT) << "the blur's input is the empty merge, not the source graphic";
  EXPECT_EQ(f.primitives[1].inputs[0].index, 0u);
}

TEST(LowerFilter, ATableValuesThatDoesNotParseIsTheIdentityNotAnErrorOfTheFilter) {
  // "1px" is no number list: Chromium, Firefox, resvg and the corpus's reference draw the element as the identity transfer does.
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feComponentTransfer><feFuncB type=\"table\" tableValues=\"1px\"/></feComponentTransfer></filter>"), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 1u);
  EXPECT_EQ(f.primitives[0].transfer[2].type, (uint32_t) GSHSCENE_FILTER_TRANSFER_IDENTITY);
}

TEST(LowerFilter, ASpotLightsNegativeSpecularExponentIsOne) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"f\"><feDiffuseLighting><feSpotLight x=\"5\" y=\"5\" z=\"5\" specularExponent=\"-10\"/></feDiffuseLighting></filter>"), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f)) << "the light is valid, as in resvg, Chromium and Firefox";
  EXPECT_EQ(f.primitives[0].light.spot_exponent, 1.0);
}

TEST(LowerFilter, AListOnATurnedElementNestsInsideThePictureOfItsOwn) {
  Lowered l;
  LowerSvg(Doc("<filter id=\"a\"><feOffset dx=\"1\"/></filter><filter id=\"b\"><feOffset dy=\"1\"/></filter>",
               "<rect width=\"10\" height=\"10\" transform=\"rotate(30)\" filter=\"url(#a) url(#b)\""),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  bool instance = false;
  for (size_t i = 0; i < l.Items(); i++) {
    instance = instance || l.Item(i).kind == GSHSCENE_ITEM_INSTANCE;
  }
  EXPECT_TRUE(instance);
}

TEST(LowerFilter, EachCssFilterFunctionIsLowered) {
  for (const char * fn : {"blur(2px)", "brightness(150%)", "contrast(0.5)", "invert(0.25)", "opacity(50%)", "grayscale(1)", "sepia(0.5)",
           "saturate(2)", "hue-rotate(90deg)", "drop-shadow(1px 2px 4px #ff0000)", "drop-shadow(1px 2px)", "blur()", "brightness()"}) {
    Lowered l;
    LowerSvg(Svg("width=\"40\" height=\"40\"", std::string("<rect width=\"10\" height=\"10\" style=\"filter: ") + fn + "\"/>"), l);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << fn << ": " << (l.Refusal(GVEC_LOWER_FILTER) ? l.Refusal(GVEC_LOWER_FILTER)->message : "") << (l.Refusal(GVEC_LOWER_STYLE) ? l.Refusal(GVEC_LOWER_STYLE)->message : "");
    GSHSCENE_Filter f;
    EXPECT_TRUE(FilterOf(l, &f)) << fn;
  }
}

TEST(LowerFilter, ABareNumberInAPresentationAttributeIsALengthInsideAFilterFunction) {
  // SVG attribute mode reads a bare number as px (resvg's blur-function); a bare non-zero angle stays invalid in every engine we probed.
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<rect width=\"10\" height=\"10\" filter=\"blur(4)\"/>"), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  ASSERT_EQ(f.primitive_count, 1u);
  EXPECT_EQ(f.primitives[0].sx, 4.0);
}

TEST(LowerFilter, CssFilterFunctionsAreAChainOfPrimitivesInSrgbOverAnAutoRegion) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"",
               "<rect width=\"10\" height=\"10\" transform=\"scale(2)\" style=\"filter: blur(2px) brightness(150%) contrast(0.5) invert(0.25) opacity(50%) "
               "grayscale(1) sepia(0.5) saturate(2) hue-rotate(90deg) drop-shadow(1px 2px 4px #ff0000)\"/>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  EXPECT_EQ(f.region_mode, (uint32_t) GSHSCENE_FILTER_REGION_AUTO);
  EXPECT_EQ(f.color_space, (uint32_t) GSHSCENE_FILTER_SPACE_SRGB);
  ASSERT_EQ(f.primitive_count, 10u);
  EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_GAUSSIAN_BLUR);
  EXPECT_EQ(f.primitives[0].sx, 4.0) << "2px under scale(2)";
  EXPECT_EQ(f.primitives[0].color_space, (uint32_t) GSHSCENE_FILTER_SPACE_SRGB);
  EXPECT_EQ(f.primitives[0].inputs[0].kind, (uint32_t) GSHSCENE_FILTER_INPUT_SOURCE_GRAPHIC);
  EXPECT_EQ(f.primitives[1].transfer[0].slope, 1.5);
  EXPECT_EQ(f.primitives[1].inputs[0].index, 0u);
  EXPECT_EQ(f.primitives[2].transfer[1].slope, 0.5);
  EXPECT_EQ(f.primitives[2].transfer[1].intercept, 0.25);
  EXPECT_EQ(f.primitives[3].transfer[0].type, (uint32_t) GSHSCENE_FILTER_TRANSFER_TABLE);
  EXPECT_EQ(f.primitives[3].transfer[0].table[0], 0.25);
  EXPECT_EQ(f.primitives[3].transfer[0].table[1], 0.75);
  EXPECT_EQ(f.primitives[4].transfer[3].slope, 0.5);
  EXPECT_EQ(f.primitives[5].mode, (uint32_t) GSHSCENE_FILTER_MATRIX_MATRIX);
  EXPECT_DOUBLE_EQ(f.primitives[5].values[0], 0.2126);
  EXPECT_EQ(f.primitives[7].mode, (uint32_t) GSHSCENE_FILTER_MATRIX_SATURATE);
  EXPECT_EQ(f.primitives[7].values[0], 2.0);
  EXPECT_EQ(f.primitives[8].mode, (uint32_t) GSHSCENE_FILTER_MATRIX_HUE_ROTATE);
  EXPECT_DOUBLE_EQ(f.primitives[8].values[0], 90.0);
  EXPECT_EQ(f.primitives[9].kind, (uint32_t) GSHSCENE_FILTER_DROP_SHADOW);
  EXPECT_EQ(f.primitives[9].dx, 2.0);
  EXPECT_EQ(f.primitives[9].dy, 4.0);
  EXPECT_EQ(f.primitives[9].sx, 8.0) << "the third length is the deviation itself (4px), doubled by the scale";
  EXPECT_EQ(f.primitives[9].sy, 8.0);
  EXPECT_EQ(f.primitives[9].color.rgba[0], 1.0);
}

TEST(LowerFilter, ADropShadowWithNoColorOrWithCurrentColorTakesTheElementsColorProperty) {
  // Filter Effects 1: "the missing used color is taken from the color property"; currentcolor is the same.  The lengths are bare numbers,
  // which a presentation attribute reads as user units.
  struct Case {
    const char * attributes;
    double rgb[3];
  };
  const Case cases[] = {
      {"color=\"#0000ff\" filter=\"drop-shadow(4 5 6)\"", {0, 0, 1}},
      {"color=\"#0000ff\" filter=\"drop-shadow(currentColor 4 5 6)\"", {0, 0, 1}},
      {"color=\"#0000ff\" filter=\"drop-shadow(4 5 6 CURRENTCOLOR)\"", {0, 0, 1}},
      {"color=\"#0000ff\" filter=\"drop-shadow(#ff0000 4 5 6)\"", {1, 0, 0}},
      {"filter=\"drop-shadow(4 5 6)\"", {0, 0, 0}},
  };
  for (const Case & c : cases) {
    Lowered l;
    LowerSvg(Doc("", std::string("<rect width=\"10\" height=\"10\" ") + c.attributes), l);
    ASSERT_EQ(l.Refusals(), (size_t) 0) << c.attributes;
    GSHSCENE_Filter f;
    ASSERT_TRUE(FilterOf(l, &f)) << c.attributes;
    ASSERT_EQ(f.primitive_count, 1u) << c.attributes;
    EXPECT_EQ(f.primitives[0].kind, (uint32_t) GSHSCENE_FILTER_DROP_SHADOW) << c.attributes;
    EXPECT_EQ(f.primitives[0].dx, 4.0) << c.attributes;
    EXPECT_EQ(f.primitives[0].dy, 5.0) << c.attributes;
    EXPECT_EQ(f.primitives[0].sx, 6.0) << c.attributes << ": the third length is the deviation";
    for (int k = 0; k < 3; k++) {
      EXPECT_NEAR(f.primitives[0].color.rgba[k], c.rgb[k], 1e-9) << c.attributes << " channel " << k;
    }
    EXPECT_EQ(f.primitives[0].color.rgba[3], 1.0) << c.attributes;
  }
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

// A filter is applied to what its element renders, and an element that paints nothing renders an empty image: the primitives that need no
// source (a flood, a tile of one) still draw (Filter Effects 1, 3). The element's geometry is still its box, for the units and the region.

namespace {

/** A document of the filter and the element; an element that is not complete markup (it ends before its `/>`) is closed as an empty one. */
std::string DocOf(const std::string & filter, const std::string & element) {
  const bool complete = !element.empty() && element.back() == '>';
  return complete ? Svg("width=\"40\" height=\"40\"", "<defs>" + filter + "</defs>" + element) : Doc(filter, element);
}

const char * const kFlood = "<filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"30\" height=\"30\"><feFlood flood-color=\"green\"/></filter>";
const char * const kBoxFlood = "<filter id=\"f\"><feFlood flood-color=\"green\"/></filter>";

/** The list is a filter group with nothing inside it: a push and a pop, and no fill, stroke or clip. */
void ExpectOnlyTheFilterGroup(const Lowered & l, const std::string & what) {
  EXPECT_EQ(l.Refusals(), (size_t) 0) << what;
  EXPECT_EQ(l.Items(), (size_t) 2) << what;
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_GROUP_PUSH), (size_t) 1) << what;
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_GROUP_POP), (size_t) 1) << what;
  GSHSCENE_Filter f;
  EXPECT_TRUE(FilterOf(l, &f)) << what;
}

void ExpectNothing(const std::string & filter, const std::string & element, const std::string & what) {
  Lowered l;
  LowerSvg(DocOf(filter, element), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0) << what;
  EXPECT_EQ(l.Items(), (size_t) 0) << what;
}

}  // namespace

TEST(LowerFilter, AShapeThatPaintsNothingIsStillFilteredWhateverTheReasonItPaintsNothing) {
  const char * const paints[] = {
      "fill=\"none\"", "fill=\"red\" fill-opacity=\"0\"", "fill=\"rgba(255,0,0,0)\"", "fill=\"transparent\"", "fill=\"red\" stroke=\"blue\" stroke-opacity=\"0\" fill-opacity=\"0\"",
  };
  for (const char * paint : paints) {
    for (const char * shape : {"<rect x=\"2\" y=\"3\" width=\"10\" height=\"10\"", "<circle cx=\"5\" cy=\"5\" r=\"4\"", "<ellipse cx=\"5\" cy=\"5\" rx=\"4\" ry=\"2\"",
             "<path d=\"M2 3H12V13Z\"", "<polygon points=\"2,3 12,3 12,13\"", "<polyline points=\"2,3 12,3 12,13\"", "<line x1=\"2\" y1=\"3\" x2=\"12\" y2=\"13\""}) {
      Lowered l;
      LowerSvg(DocOf(kFlood, std::string(shape) + " " + paint + " filter=\"url(#f)\""), l);
      ExpectOnlyTheFilterGroup(l, std::string(shape) + " " + paint);
    }
  }
}

TEST(LowerFilter, TheBoxOfAnUnpaintedShapeIsItsGeometryForTheRegionInUnitsOfTheBox) {
  Lowered l;
  LowerSvg(DocOf(kBoxFlood, "<rect x=\"2\" y=\"3\" width=\"10\" height=\"10\" fill=\"none\" filter=\"url(#f)\""), l);
  ExpectOnlyTheFilterGroup(l, "a box region");
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  // -10% and 120% of the box (2, 3, 10, 10): the same region as the filled rect of the first test
  EXPECT_EQ(f.region_mode, (uint32_t) GSHSCENE_FILTER_REGION_EXPLICIT);
  EXPECT_DOUBLE_EQ(f.region[0], 1.0);
  EXPECT_DOUBLE_EQ(f.region[1], 2.0);
  EXPECT_DOUBLE_EQ(f.region[2], 12.0);
  EXPECT_DOUBLE_EQ(f.region[3], 12.0);
  // a group of unpainted shapes has the box of its geometry
  Lowered g;
  LowerSvg(DocOf(kBoxFlood, "<g filter=\"url(#f)\"><rect x=\"2\" y=\"3\" width=\"4\" height=\"4\" fill=\"none\"/><rect x=\"8\" y=\"9\" width=\"4\" height=\"4\" fill=\"none\"/></g>"), g);
  ExpectOnlyTheFilterGroup(g, "a group");
  ASSERT_TRUE(FilterOf(g, &f));
  EXPECT_DOUBLE_EQ(f.region[0], 1.0);
  EXPECT_DOUBLE_EQ(f.region[2], 12.0);
}

TEST(LowerFilter, AnUnfilteredShapeThatPaintsNothingIsStillDroppedAndSoIsOneWithAFilterThatIsNone) {
  Lowered l;
  LowerSvg(Svg("width=\"40\" height=\"40\"", "<rect width=\"10\" height=\"10\" fill=\"none\"/><rect width=\"10\" height=\"10\" fill-opacity=\"0\"/><g><path d=\"M0 0H5V5Z\" fill=\"none\"/></g>"), l);
  EXPECT_EQ(l.Items(), (size_t) 0);
  Lowered n;
  LowerSvg(DocOf(kFlood, "<rect width=\"10\" height=\"10\" fill=\"none\" filter=\"none\""), n);
  EXPECT_EQ(n.Items(), (size_t) 0);
}

TEST(LowerFilter, AnElementThatIsNotRenderedHasNoFilteredResult) {
  struct {
    const char * what;
    const char * element;
  } const cases[] = {
      {"display none", "<rect width=\"10\" height=\"10\" fill=\"none\" display=\"none\" filter=\"url(#f)\""},
      {"opacity zero", "<rect width=\"10\" height=\"10\" fill=\"none\" opacity=\"0\" filter=\"url(#f)\""},
      {"visibility hidden", "<rect width=\"10\" height=\"10\" fill=\"none\" visibility=\"hidden\" filter=\"url(#f)\""},
      {"visibility collapse", "<rect width=\"10\" height=\"10\" fill=\"none\" visibility=\"collapse\" filter=\"url(#f)\""},
      {"a width of zero", "<rect width=\"0\" height=\"10\" fill=\"none\" filter=\"url(#f)\""},
      {"a radius of zero", "<circle r=\"0\" fill=\"none\" filter=\"url(#f)\""},
      {"an empty path", "<path d=\"\" fill=\"none\" filter=\"url(#f)\""},
      {"a lone move", "<path d=\"M3 4\" fill=\"none\" filter=\"url(#f)\""},
      {"a false condition", "<rect width=\"10\" height=\"10\" fill=\"none\" systemLanguage=\"xx\" filter=\"url(#f)\""},
      {"a group of display none", "<g display=\"none\" filter=\"url(#f)\"><rect width=\"10\" height=\"10\" fill=\"none\"/></g>"},
      {"a use of nothing", "<use href=\"#nothing\" filter=\"url(#f)\""},
      {"a filter of what is no filter", "<rect width=\"10\" height=\"10\" fill=\"none\" filter=\"url(#nothing)\""},
  };
  for (const auto & c : cases) {
    ExpectNothing(kFlood, c.element, c.what);
  }
  // the same elements with paint and a visible state draw, so the case is the property and not the fixture
  Lowered visible;
  LowerSvg(DocOf(kFlood, "<rect width=\"10\" height=\"10\" fill=\"red\" filter=\"url(#f)\""), visible);
  EXPECT_EQ(visible.Count(GSHSCENE_ITEM_GROUP_PUSH), (size_t) 1);
  EXPECT_EQ(visible.Count(GSHSCENE_ITEM_FILL), (size_t) 1);
}

TEST(LowerFilter, AGroupWithAHiddenChildIsFilteredAndAGroupThatIsHiddenItselfIsAContainerStill) {
  Lowered l;
  LowerSvg(DocOf(kFlood, "<g visibility=\"hidden\" filter=\"url(#f)\"><rect width=\"10\" height=\"10\" fill=\"red\"/></g>"), l);
  ExpectOnlyTheFilterGroup(l, "a hidden group's children are hidden by inheritance and the group is not");
  Lowered visible_child;
  LowerSvg(DocOf(kFlood, "<g visibility=\"hidden\" filter=\"url(#f)\"><rect width=\"10\" height=\"10\" fill=\"red\" visibility=\"visible\"/></g>"), visible_child);
  EXPECT_EQ(visible_child.Count(GSHSCENE_ITEM_FILL), (size_t) 1);
  EXPECT_EQ(visible_child.Count(GSHSCENE_ITEM_GROUP_PUSH), (size_t) 1);
}

TEST(LowerFilter, TheOpacityOfAnUnpaintedFilteredShapeIsTheFilterGroupsAndAnOpacityOfZeroDrawsNothing) {
  Lowered l;
  LowerSvg(DocOf(kFlood, "<rect width=\"10\" height=\"10\" fill=\"none\" opacity=\"0.25\" filter=\"url(#f)\""), l);
  ExpectOnlyTheFilterGroup(l, "an opacity");
  ASSERT_EQ(l.Count(GSHSCENE_ITEM_GROUP_PUSH), (size_t) 1);
  for (size_t i = 0; i < l.Items(); i++) {
    if (l.Item(i).kind == GSHSCENE_ITEM_GROUP_PUSH) {
      EXPECT_EQ(l.Item(i).group.opacity, 0.25);
    }
  }
}

TEST(LowerFilter, ATurnedUnpaintedShapeIsFilteredInThePictureOfItsOwn) {
  Lowered l;
  LowerSvg(DocOf(kBoxFlood, "<rect x=\"2\" y=\"3\" width=\"10\" height=\"10\" fill=\"none\" transform=\"rotate(20)\" filter=\"url(#f)\""), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  // the filter's group is in the picture that the instance draws: the list holds the instance alone (the golden of filter-invisible.svg
  // renders it)
  EXPECT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_INSTANCE), (size_t) 1);
  Lowered nothing;
  LowerSvg(DocOf(kBoxFlood, "<rect x=\"2\" y=\"3\" width=\"10\" height=\"10\" fill=\"none\" transform=\"rotate(20)\" display=\"none\" filter=\"url(#f)\""), nothing);
  EXPECT_EQ(nothing.Items(), (size_t) 0);
}

TEST(LowerFilter, AUseOfAnUnpaintedShapeIsFilteredAndTheClipAndTheMaskAreAroundTheFilter) {
  Lowered u;
  LowerSvg(DocOf(kFlood, "<defs><path id=\"p\" d=\"M0 0H5V5Z\" fill=\"none\"/></defs><use href=\"#p\" filter=\"url(#f)\""), u);
  EXPECT_EQ(u.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  EXPECT_TRUE(FilterOf(u, &f));
  Lowered c;
  LowerSvg(DocOf(std::string(kFlood) + "<clipPath id=\"c\"><rect width=\"20\" height=\"20\"/></clipPath>",
               "<rect width=\"10\" height=\"10\" fill=\"none\" clip-path=\"url(#c)\" filter=\"url(#f)\""), c);
  EXPECT_EQ(c.Refusals(), (size_t) 0);
  ASSERT_GE(c.Items(), (size_t) 4);
  EXPECT_EQ(c.Item(0).kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(c.Item(1).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_NE(c.Item(1).group.filter, nullptr);
  EXPECT_EQ(c.Item(2).kind, GSHSCENE_ITEM_GROUP_POP);
  EXPECT_EQ(c.Item(3).kind, GSHSCENE_ITEM_CLIP_POP);
}

TEST(LowerFilter, AFilterInTheUnitsOfTheBoxOnAUseTakesTheBoxOfWhatTheUseDraws) {
  Lowered l;
  // the box of the use is the rectangle's, moved by x and y and then by the referent's own transform: (2, 3, 10, 10) to (12, 23)
  LowerSvg(Svg("width=\"60\" height=\"60\"",
               "<defs><filter id=\"f\"><feGaussianBlur stdDeviation=\"1\"/></filter>"
               "<rect id=\"r\" x=\"2\" y=\"3\" width=\"10\" height=\"10\" fill=\"red\"/></defs>"
               "<use href=\"#r\" x=\"10\" y=\"20\" filter=\"url(#f)\"/>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f)) << "the use has a filter result: its box is known";
  EXPECT_DOUBLE_EQ(f.region[0], 11.0);  // 12 - 10% of 10
  EXPECT_DOUBLE_EQ(f.region[1], 22.0);  // 23 - 10% of 10
  EXPECT_DOUBLE_EQ(f.region[2], 12.0);
  EXPECT_DOUBLE_EQ(f.region[3], 12.0);
}

TEST(LowerFilter, AFilterInTheUnitsOfTheBoxOnAUseOfAnElementThatDisplayRemovesIsNotRendered) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"",
               "<defs><filter id=\"f\"><feGaussianBlur stdDeviation=\"1\"/></filter>"
               "<rect id=\"r\" width=\"10\" height=\"10\" fill=\"red\" display=\"none\"/></defs>"
               "<use href=\"#r\" filter=\"url(#f)\"/>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  EXPECT_FALSE(FilterOf(l, &f)) << "what display removes has no box, so the filter has nothing to place itself in";
}

TEST(LowerFilter, AFilterOnAGroupHoldingAUseBoxesTheUseToo) {
  Lowered l;
  LowerSvg(Svg("width=\"60\" height=\"60\"",
               "<defs><filter id=\"f\"><feGaussianBlur stdDeviation=\"1\"/></filter>"
               "<rect id=\"r\" width=\"10\" height=\"10\" fill=\"red\"/></defs>"
               "<g filter=\"url(#f)\"><use href=\"#r\" x=\"20\" transform=\"translate(0 5)\"/></g>"),
           l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(l, &f));
  EXPECT_DOUBLE_EQ(f.region[0], 19.0);
  EXPECT_DOUBLE_EQ(f.region[1], 4.0);
  EXPECT_DOUBLE_EQ(f.region[2], 12.0);
  EXPECT_DOUBLE_EQ(f.region[3], 12.0);
}

TEST(LowerFilter, AFilteredChildDoesNotMakeItsParentsFilteredGroupRenderedAndTheOtherWayRound) {
  // The flag that says the element of a filter group is rendered is the element's own: a child's does not leak up, and a parent's does not leak down.
  Lowered up;
  LowerSvg(DocOf(kFlood, "<g filter=\"url(#f)\" display=\"none\"><rect width=\"10\" height=\"10\" fill=\"none\" filter=\"url(#f)\"/></g>"), up);
  EXPECT_EQ(up.Items(), (size_t) 0);
  Lowered down;
  LowerSvg(DocOf(kFlood, "<g filter=\"url(#f)\"><rect width=\"10\" height=\"10\" fill=\"none\" display=\"none\" filter=\"url(#f)\"/></g>"), down);
  ExpectOnlyTheFilterGroup(down, "the parent alone");
}

TEST(LowerFilter, AMagnitudePastTheWaistsBoundIsHeldToItAndSaidNotRefused) {
  Lowered big;
  LowerSvg(Doc("<filter id=\"f\"><feColorMatrix type=\"saturate\" values=\"99999\"/></filter>"), big);
  EXPECT_EQ(big.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(big, &f));
  ASSERT_EQ(f.primitive_count, (size_t) 1);
  ASSERT_NE(f.primitives[0].values, nullptr);
  EXPECT_EQ(f.primitives[0].values[0], GSHSCENE_FILTER_VALUE_MAX);
  EXPECT_GE(gvec_diagnostics_count_code(&big.by_list, GVEC_DIAG_STYLE_INVALID), (size_t) 1) << "a notice says so";
  Lowered region;
  LowerSvg(Doc("<filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"-10000\" y=\"-10000\" width=\"100000\" height=\"100000\"><feOffset dx=\"1\"/></filter>"), region);
  EXPECT_EQ(region.Refusals(), (size_t) 0);
  ASSERT_TRUE(FilterOf(region, &f));
  EXPECT_EQ(f.region[0], -10000.0);
  EXPECT_LE(f.region[2], GSHSCENE_FILTER_VALUE_MAX);
  EXPECT_GT(f.region[0] + f.region[2], 40.0) << "the region still covers the canvas";
  Lowered fine;
  LowerSvg(Doc("<filter id=\"f\"><feColorMatrix type=\"saturate\" values=\"3\"/></filter>"), fine);
  EXPECT_EQ(gvec_diagnostics_count_code(&fine.by_list, GVEC_DIAG_STYLE_INVALID), (size_t) 0) << "nothing is said of a value in range";
}

TEST(LowerFilter, CurrentColorInAFloodOrALightingIsTheForegroundAtLoweringAndSaidSo) {
  Lowered black;
  LowerSvg(Doc("<filter id=\"f\"><feFlood flood-color=\"currentColor\"/></filter>"), black);
  EXPECT_EQ(black.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_TRUE(FilterOf(black, &f));
  EXPECT_EQ(f.primitives[0].color.rgba[0], 0.0);
  EXPECT_EQ(f.primitives[0].color.rgba[3], 1.0);
  EXPECT_GE(gvec_diagnostics_count_code(&black.by_list, GVEC_DIAG_STYLE_INVALID), (size_t) 1) << "the notice";
  Lowered lit;
  LowerSvg(Doc("<filter id=\"f\"><feDiffuseLighting lighting-color=\"currentColor\"><feDistantLight azimuth=\"45\" elevation=\"45\"/></feDiffuseLighting></filter>"), lit);
  EXPECT_EQ(lit.Refusals(), (size_t) 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
