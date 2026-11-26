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
 * A lowering at a time (design.md A1 to A12). Each expected value is what both Chromium 156 and Firefox 158 compute for the same
 * document when it is paused and set to the same time (`pauseAnimations()`, `setCurrentTime()`, every CSS animation paused at the
 * time), unless the test says the two disagree or do not apply the text.
 */

#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/paint.h>

#include <array>
#include <cmath>

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

const char * const kSize = "width=\"300\" height=\"150\"";

/** Lower `body` at `t` milliseconds (static when `t` is negative). */
void At(const std::string & body, double t, Lowered & l, const std::string & attributes = kSize) {
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  if (t >= 0) {
    options.flags = GVEC_LOWER_ANIMATED;
    options.animation_time_ms = t;
  }
  LowerSvg(Svg(attributes, body), l, 0, 0, &options);
}

/** The items of a list, the pictures of its instances in their place (a `use` is an instance of a picture). */
void Flatten(const GSHSCENE_List * list, std::vector<GSHSCENE_Item> & out) {
  for (size_t i = 0; list && i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * item = nullptr;
    if (gshscene_list_item(list, i, &item) != GSHSCENE_OK) {
      continue;
    }
    if (item->kind == GSHSCENE_ITEM_INSTANCE) {
      Flatten(gshscene_resource_picture(item->resource), out);
      continue;
    }
    out.push_back(*item);
  }
}

std::vector<GSHSCENE_Item> Flat(const Lowered & l) {
  std::vector<GSHSCENE_Item> out;
  Flatten(l.list, out);
  return out;
}

/** The first fill (or the one after `skip` others), the pictures of instances included. */
const GSHSCENE_Item * FirstFill(const Lowered & l, size_t skip = 0) {
  static thread_local std::vector<GSHSCENE_Item> items;
  items = Flat(l);
  for (const GSHSCENE_Item & item : items) {
    if (item.kind == GSHSCENE_ITEM_FILL && skip-- == 0) {
      return &item;
    }
  }
  return nullptr;
}

/** The fill's color as 8-bit channels (straight). */
std::array<int, 3> Rgb(const GSHSCENE_Item * fill) {
  std::array<int, 3> c{-1, -1, -1};
  if (fill && fill->paint.kind == GSHSCENE_PAINT_SOLID) {
    for (int k = 0; k < 3; k++) {
      c[k] = (int) std::lround(fill->paint.rgba[k] * 255);
    }
  }
  return c;
}

/** The opacity the first fill is drawn with: its alpha times the opacities of the groups it is in; -1 when nothing is filled. */
double Opacity(const Lowered & l) {
  double o = 1;
  for (const GSHSCENE_Item & item : Flat(l)) {
    if (item.kind == GSHSCENE_ITEM_GROUP_PUSH) {
      o *= item.group.opacity;
    }
    if (item.kind == GSHSCENE_ITEM_FILL) {
      return o * item.paint.rgba[3];
    }
  }
  return -1;
}

/** The bounds of a fill, in the user space it is drawn in. */
GSHGEOM_Bounds Bounds(const GSHSCENE_Item * fill) {
  GSHGEOM_Bounds box;
  box.empty = 1;
  const GSHGEOM_Path * path = fill ? gshscene_resource_path(fill->resource) : nullptr;
  if (!path || gshgeom_path_bounds(path, &box) != GSHGEOM_OK) {
    box.empty = 1;
  }
  return box;
}

size_t Invalid(const Lowered & l) {
  return gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_ANIMATION_INVALID);
}

const std::array<int, 3> kGreen{0, 128, 0}, kRed{255, 0, 0}, kBlue{0, 0, 255};

}  // namespace

// ---- A1: modes ---------------------------------------------------------------------------------------------------------------------

TEST(LowerAnimate, AStaticLoweringAppliesNothingNotEvenABeginOfZero) {
  // SVG 2, conform.html: animations "must not be applied or run"; "This is not the same as pausing ... at 0s".
  Lowered still, moving;
  const std::string body = "<rect width=\"10\" height=\"10\" fill=\"red\"><set attributeName=\"fill\" to=\"green\"/></rect>";
  At(body, -1, still);
  At(body, 0, moving);
  EXPECT_EQ(Rgb(FirstFill(still)), kRed);
  EXPECT_EQ(Rgb(FirstFill(moving)), kGreen);
}

TEST(LowerAnimate, ATimeThatIsNegativeOrNotFiniteIsInvalid) {
  for (double t : {-1.0, (double) NAN, (double) INFINITY}) {
    Loaded loaded;
    LoadSvg(Svg(kSize, "<rect width=\"1\" height=\"1\"/>"), loaded);
    GSHSCENE_Scene * scene = nullptr;
    ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
    GSHSCENE_List * list = nullptr;
    ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
    GVEC_Lower_Options options;
    std::memset(&options, 0, sizeof options);
    options.store = gshscene_scene_store(scene);
    options.flags = GVEC_LOWER_ANIMATED;
    options.animation_time_ms = t;
    EXPECT_EQ(gvec_doc_lower_list(loaded.doc, &options, list, nullptr), GVEC_ERR_INVALID) << t;
    gshscene_list_destroy(list);
    gshscene_scene_destroy(scene);
  }
}

// ---- A4: where an animated property goes in the cascade -----------------------------------------------------------------------------

TEST(LowerAnimate, ASmilValueIsAboveTheStyleAttributeAndBelowImportantAndCssAnimations) {
  Lowered over_style, under_important, under_css;
  At("<rect width=\"10\" height=\"10\" style=\"fill:red\"><set attributeName=\"fill\" to=\"green\"/></rect>", 500, over_style);
  At("<style>rect{fill:red !important}</style><rect width=\"10\" height=\"10\"><set attributeName=\"fill\" to=\"green\"/></rect>", 500,
      under_important);
  At("<style>@keyframes k{from{fill:blue}to{fill:blue}} rect{animation:k 10s}</style><rect width=\"10\" height=\"10\"><set attributeName=\"fill\" "
     "to=\"green\"/></rect>",
      500, under_css);
  EXPECT_EQ(Rgb(FirstFill(over_style)), kGreen);
  EXPECT_EQ(Rgb(FirstFill(under_important)), kRed);
  EXPECT_EQ(Rgb(FirstFill(under_css)), kBlue);
}

TEST(LowerAnimate, AttributeTypeDoesNotChangeThatAPresentationAttributeIsTheProperty) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\" style=\"fill:red\"><set attributeName=\"fill\" attributeType=\"XML\" to=\"green\"/></rect>", 500, l);
  EXPECT_EQ(Rgb(FirstFill(l)), kGreen);
}

TEST(LowerAnimate, ACssPropertyWithNoPresentationAttributeIsNotAnimated) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><set attributeName=\"mix-blend-mode\" to=\"multiply\"/></rect>", 500, l);
  EXPECT_EQ(Invalid(l), (size_t) 1);
  for (size_t i = 0; i < l.Items(); i++) {
    if (l.Item(i).kind == GSHSCENE_ITEM_GROUP_PUSH) {
      EXPECT_EQ(l.Item(i).group.composite, (uint32_t) GSHSCENE_COMPOSITE_SRC_OVER) << "no blend mode";
    }
  }
}

TEST(LowerAnimate, AnAnimatedPropertyIsInherited) {
  Lowered l;
  At("<g><animate attributeName=\"fill\" from=\"rgb(0,0,0)\" to=\"rgb(200,0,0)\" dur=\"1s\"/><rect width=\"10\" height=\"10\"/></g>", 500, l);
  EXPECT_EQ(Rgb(FirstFill(l)), (std::array<int, 3>{100, 0, 0}));
}

TEST(LowerAnimate, AClassAnimatedIsMatchedBySelectors) {
  Lowered l;
  At("<style>.g{fill:green}</style><rect width=\"10\" height=\"10\" class=\"x\"><set attributeName=\"class\" to=\"g\"/></rect>", 500, l);
  EXPECT_EQ(Rgb(FirstFill(l)), kGreen);
}

// ---- A3: timing -----------------------------------------------------------------------------------------------------------------------

TEST(LowerAnimate, NoDurSamplesTheFirstValue) {
  Lowered from_to, values;
  At("<rect width=\"10\" height=\"10\" opacity=\"1\"><animate attributeName=\"opacity\" from=\"0.2\" to=\"0.8\"/></rect>", 500, from_to);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" values=\"0.2;0.5;0.8\"/></rect>", 500, values);
  EXPECT_NEAR(Opacity(from_to), 0.2, 1e-6);
  EXPECT_NEAR(Opacity(values), 0.2, 1e-6);
}

TEST(LowerAnimate, ClockValuesInEveryForm) {
  struct Row {
    const char * dur;
    double expect;
  } rows[] = {{"00:00:01.000", 0.5}, {"00:02", 0.25}, {"2000ms", 0.25}, {"0.02min", 0.5 / 1.2}, {"0.0005h", 0.5 / 1.8}};
  for (const Row & row : rows) {
    Lowered l;
    At(std::string("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"") + row.dur + "\"/></rect>", 500, l);
    EXPECT_NEAR(Opacity(l), row.expect, 1e-6) << row.dur;
  }
}

TEST(LowerAnimate, ARepeatCountFreezesAtTheEndOfItsActiveDuration) {
  Lowered partial, whole;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" repeatCount=\"2.5\" fill=\"freeze\"/></rect>", 5000,
      partial);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" repeatCount=\"2\" fill=\"freeze\"/></rect>", 5000,
      whole);
  EXPECT_NEAR(Opacity(partial), 0.5, 1e-6);
  EXPECT_NEAR(Opacity(whole), 1.0, 1e-6) << "a whole number of repeats freezes at the end of the last, not the start of the next";
}

TEST(LowerAnimate, RepeatDurMaxAndMin) {
  Lowered repeat_dur, max, min;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" repeatDur=\"1.25s\" fill=\"freeze\"/></rect>", 2000,
      repeat_dur);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" max=\"0.4s\" fill=\"freeze\"/></rect>", 500, max);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" min=\"2s\" fill=\"freeze\"/></rect>", 1500, min);
  EXPECT_NEAR(Opacity(repeat_dur), 0.25, 1e-6);
  EXPECT_NEAR(Opacity(max), 0.4, 1e-6);
  EXPECT_NEAR(Opacity(min), 1.0, 1e-6);
}

TEST(LowerAnimate, EndValuesAndAListOfIntervals) {
  Lowered before, list;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" end=\"0.25s\" fill=\"freeze\"/></rect>", 500, before);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"0;0.6s\" end=\"0.3s;0.9s\" fill=\"freeze\"/></rect>",
      500, list);
  EXPECT_NEAR(Opacity(before), 0.25, 1e-6);
  EXPECT_NEAR(Opacity(list), 0.3, 1e-6);
}

TEST(LowerAnimate, Restart) {
  const std::string head = "<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"0;0.5s\" ";
  Lowered never, when_not_active, always, ended;
  At(head + "restart=\"never\"/></rect>", 750, never);
  At(head + "restart=\"whenNotActive\"/></rect>", 750, when_not_active);
  At(head + "/></rect>", 750, always);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"0;0.25s\" end=\"0.75s\"/></rect>", 600, ended);
  EXPECT_NEAR(Opacity(never), 0.75, 1e-6);
  EXPECT_NEAR(Opacity(when_not_active), 0.75, 1e-6);
  EXPECT_NEAR(Opacity(always), 0.25, 1e-6);
  EXPECT_NEAR(Opacity(ended), 0.35, 1e-6);
}

TEST(LowerAnimate, ANegativeBeginIsUnderWayAtZero) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"-0.25s\"/></rect>", 0, l);
  EXPECT_NEAR(Opacity(l), 0.25, 1e-6);
}

TEST(LowerAnimate, SyncbaseValuesAndTheLoopTheyMake) {
  Lowered offset, loop, dotted;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"b.begin - 0.25s\"/></rect>"
     "<rect width=\"9\" height=\"9\"><animate id=\"b\" attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"0.5s\"/></rect>",
      700, offset);
  At("<rect width=\"10\" height=\"10\"><animate id=\"a\" attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"0;b.end\"/>"
     "<animate id=\"b\" attributeName=\"opacity\" from=\"1\" to=\"0\" dur=\"1s\" begin=\"a.end\"/></rect>",
      4250, loop);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"a\\.b.begin+0.25s\"/></rect>"
     "<rect width=\"9\" height=\"9\"><animate id=\"a.b\" attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\"/></rect>",
      500, dotted);
  EXPECT_NEAR(Opacity(offset), 0.45, 1e-6);
  EXPECT_NEAR(Opacity(loop), 0.25, 1e-6) << "a's third interval, 4 s to 5 s";
  EXPECT_NEAR(Opacity(dotted), 0.25, 1e-6);
}

TEST(LowerAnimate, ValuesThatNeedEventsOrAClockNeverBegin) {
  for (const char * begin : {"click", "r.click+1s", "indefinite", "wallclock(2000-01-01T00:00:00Z)", "accessKey(a)", "b.repeat(2)"}) {
    Lowered l;
    At(std::string("<rect id=\"r\" width=\"10\" height=\"10\"><set attributeName=\"opacity\" to=\"0.5\" begin=\"") + begin +
           "\"/></rect><rect width=\"9\" height=\"9\"><animate id=\"b\" attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"0.1s\" repeatCount=\"5\"/></rect>",
        450, l);
    EXPECT_NEAR(Opacity(l), 1.0, 1e-6) << begin;
  }
}

TEST(LowerAnimate, AValueOfABeginListThatIsInvalidIsDroppedAlone) {
  const std::string head = "<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"";
  struct Row {
    const char * begin;
    double expect;
  } rows[] = {{"0.25s;nonsense here", 0.25}, {"x y", 1}, {"0.25q", 1}, {"nope.begin", 1}, {"nope.begin;0.25s", 0.25}};
  for (const Row & row : rows) {
    Lowered l;
    At(head + row.begin + "\"/></rect>", 500, l);
    EXPECT_NEAR(Opacity(l), row.expect, 1e-6) << row.begin;
  }
}

TEST(LowerAnimate, ARingOfShortIntervalsPassesTheLimitAndIsDrawnStatic) {
  // Two sets of a microsecond that start each other: two million boundaries in half a second, past the default of a million.
  Lowered l;
  At("<rect width=\"10\" height=\"10\" fill=\"red\"><set id=\"a\" attributeName=\"fill\" to=\"green\" dur=\"0.001ms\" begin=\"0;b.end\"/>"
     "<set id=\"b\" attributeName=\"fill\" to=\"green\" dur=\"0.001ms\" begin=\"a.end\"/></rect>",
      500, l);
  EXPECT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_LIMIT), (size_t) 1);
  EXPECT_EQ(Rgb(FirstFill(l)), kRed);
}

// ---- A5: values -----------------------------------------------------------------------------------------------------------------------

TEST(LowerAnimate, CalcModes) {
  Lowered spline, discrete, paced;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" values=\"0;1\" keyTimes=\"0;1\" keySplines=\"0.5 0 0.5 1\" calcMode=\"spline\" "
     "dur=\"1s\"/></rect>",
      250, spline);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" values=\"0;0.5;1\" calcMode=\"discrete\" dur=\"1s\"/></rect>", 700, discrete);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"fill\" values=\"rgb(0,0,0);rgb(30,0,0);rgb(30,90,0)\" calcMode=\"paced\" dur=\"1s\"/></rect>",
      250, paced);
  EXPECT_NEAR(Opacity(spline), 0.105893, 2e-6);
  EXPECT_NEAR(Opacity(discrete), 1.0, 1e-6);
  EXPECT_EQ(Rgb(FirstFill(paced)), (std::array<int, 3>{30, 0, 0}));
}

TEST(LowerAnimate, ToByAndAdditive) {
  Lowered to, by, from_by;
  At("<rect width=\"10\" height=\"10\" opacity=\"0.2\"><animate attributeName=\"opacity\" to=\"1\" dur=\"1s\" additive=\"sum\"/></rect>", 500, to);
  At("<rect width=\"10\" height=\"10\" x=\"10\"><animate attributeName=\"x\" by=\"100\" dur=\"1s\"/></rect>", 500, by);
  At("<rect width=\"10\" height=\"10\" x=\"10\"><animate attributeName=\"x\" from=\"20\" by=\"100\" dur=\"1s\" additive=\"sum\"/></rect>", 500, from_by);
  EXPECT_NEAR(Opacity(to), 0.6, 1e-6) << "a to-animation is never additive";
  EXPECT_NEAR(Bounds(FirstFill(by)).min.x, 60, 1e-9);
  EXPECT_NEAR(Bounds(FirstFill(from_by)).min.x, 80, 1e-9);
}

TEST(LowerAnimate, LengthsInDifferentUnitsMeetInUserUnits) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"x\" from=\"0\" to=\"100%\" dur=\"1s\"/></rect>", 500, l);
  EXPECT_NEAR(Bounds(FirstFill(l)).min.x, 150, 1e-9);
}

TEST(LowerAnimate, AValueThatIsNotOneOfThePropertyMakesTheAnimationApplyNothing) {
  Lowered middle, to;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" values=\"0;x;1\" dur=\"1s\"/></rect>", 500, middle);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"red\" dur=\"1s\"/></rect>", 500, to);
  EXPECT_NEAR(Opacity(middle), 1.0, 1e-6);
  EXPECT_NEAR(Opacity(to), 1.0, 1e-6);
  EXPECT_EQ(Invalid(middle), (size_t) 1);
}

TEST(LowerAnimate, KeyTimesAndKeySplinesInErrorMakeTheAnimationApplyNothing) {
  const std::string head = "<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" ";
  for (const char * extra : {"values=\"0;1\" keyTimes=\"0;0.5;1\"", "values=\"0;1\" keyTimes=\"0.2;1\"", "values=\"0;1\" keyTimes=\"0;0.8\"",
           "calcMode=\"spline\" keySplines=\"0 0 1\""}) {
    Lowered l;
    At(head + extra + "/></rect>", 500, l);
    EXPECT_NEAR(Opacity(l), 1.0, 1e-6) << extra;
    EXPECT_EQ(Invalid(l), (size_t) 1) << extra;
  }
}

TEST(LowerAnimate, ARepeatCountThatIsNotPositiveIsIgnoredAlone) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" repeatCount=\"-1\"/></rect>", 500, l);
  EXPECT_NEAR(Opacity(l), 0.5, 1e-6);
}

TEST(LowerAnimate, PathDataInterpolatesWhenItHasTheSameCommandsAndIsDiscreteOtherwise) {
  Lowered morph, mismatch;
  At("<path d=\"M0 0 L10 0 L10 10 Z\"><animate attributeName=\"d\" from=\"M0 0 L10 0 L10 10 Z\" to=\"M0 0 L20 0 L20 30 Z\" dur=\"1s\"/></path>", 500, morph);
  At("<path d=\"M0 0 L10 0 L10 10 Z\"><animate attributeName=\"d\" from=\"M0 0 L10 0 L10 10 Z\" to=\"M0 0 H20 L20 30 Z\" dur=\"1s\"/></path>", 400, mismatch);
  GSHGEOM_Bounds m = Bounds(FirstFill(morph)), d = Bounds(FirstFill(mismatch));
  EXPECT_NEAR(m.max.x, 15, 1e-9);
  EXPECT_NEAR(m.max.y, 20, 1e-9);
  EXPECT_NEAR(d.max.x, 10, 1e-9) << "L and H differ: discrete, the first value until half way";
}

TEST(LowerAnimate, AnAttributeThatIsNotAPropertyIsAnimatedInTheDocument) {
  Lowered points, offset, href, view_box;
  At("<polygon points=\"0,0 10,0 10,10\"><animate attributeName=\"points\" to=\"0,0 20,0 20,20\" dur=\"1s\"/></polygon>", 500, points);
  At("<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"red\"><animate attributeName=\"offset\" to=\"1\" dur=\"1s\"/></stop>"
     "<stop offset=\"1\" stop-color=\"blue\"/></linearGradient><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>",
      500, offset);
  At("<defs><rect id=\"t\" width=\"5\" height=\"7\"/></defs><use href=\"#none\"><set attributeName=\"href\" to=\"#t\"/></use>", 500, href);
  At("<svg viewBox=\"0 0 10 10\" width=\"100\" height=\"100\"><animate attributeName=\"viewBox\" to=\"0 0 20 20\" dur=\"1s\"/><rect width=\"10\" height=\"10\"/></svg>",
      500, view_box);
  EXPECT_NEAR(Bounds(FirstFill(points)).max.x, 15, 1e-9);
  const GSHSCENE_Item * gradient_fill = FirstFill(offset);
  ASSERT_NE(gradient_fill, nullptr);
  GSHSCENE_Gradient g;
  ASSERT_EQ(gshscene_resource_gradient(gradient_fill->paint.resource, &g), GSHSCENE_OK);
  ASSERT_GE(g.stop_count, (size_t) 2);
  EXPECT_NEAR(g.stops[0].offset, 0.5, 1e-9);
  const GSHSCENE_Item * used = FirstFill(href);
  ASSERT_NE(used, nullptr) << "the use draws the element its animated href names";
  EXPECT_NEAR(Bounds(used).max.y, 7, 1e-9);
  const GSHSCENE_Item * inner = FirstFill(view_box);
  ASSERT_NE(inner, nullptr);
  EXPECT_NEAR(inner->map.m[0][0], 100.0 / 15.0, 1e-9) << "the viewBox is 15 wide half way";
}

// ---- A6, A7: transforms and motion -----------------------------------------------------------------------------------------------------

TEST(LowerAnimate, AnimateTransform) {
  Lowered translate, additive, accumulate;
  At("<rect width=\"10\" height=\"10\"><animateTransform attributeName=\"transform\" type=\"translate\" from=\"0 0\" to=\"100 0\" dur=\"1s\"/></rect>", 500,
      translate);
  At("<rect width=\"10\" height=\"10\" transform=\"scale(2)\"><animateTransform attributeName=\"transform\" type=\"translate\" from=\"0 0\" to=\"100 0\" "
     "dur=\"1s\" additive=\"sum\"/></rect>",
      500, additive);
  At("<rect width=\"10\" height=\"10\"><animateTransform attributeName=\"transform\" type=\"rotate\" from=\"0\" to=\"90\" dur=\"1s\" repeatCount=\"3\" "
     "accumulate=\"sum\"/></rect>",
      1500, accumulate);
  const GSHSCENE_Item * t = FirstFill(translate);
  ASSERT_NE(t, nullptr);
  EXPECT_NEAR(t->map.t[0], 50, 1e-9);
  const GSHSCENE_Item * a = FirstFill(additive);
  ASSERT_NE(a, nullptr);
  EXPECT_NEAR(a->map.m[0][0], 2, 1e-9);
  EXPECT_NEAR(a->map.t[0], 100, 1e-9) << "the animation follows the attribute's scale(2)";
  const GSHSCENE_Item * r = FirstFill(accumulate);
  ASSERT_NE(r, nullptr);
  EXPECT_NEAR(r->map.m[0][0], -std::sqrt(0.5), 1e-9) << "135 degrees: one iteration's 90 accumulated and half of the next";
  EXPECT_NEAR(r->map.m[1][0], std::sqrt(0.5), 1e-9);
}

TEST(LowerAnimate, AnimateTransformIsAboveAStyleRulesTransform) {
  // Styling 6.6: animating a presentation attribute animates the property. Firefox agrees; Chromium keeps the rule's transform.
  Lowered l;
  At("<style>rect{transform:translate(1px,1px)}</style><rect width=\"10\" height=\"10\"><animateTransform attributeName=\"transform\" type=\"translate\" "
     "from=\"0 0\" to=\"100 0\" dur=\"1s\"/></rect>",
      500, l);
  const GSHSCENE_Item * f = FirstFill(l);
  ASSERT_NE(f, nullptr);
  EXPECT_NEAR(f->map.t[0], 50, 1e-9);
  EXPECT_NEAR(f->map.t[1], 0, 1e-9);
}

TEST(LowerAnimate, AnimateMotionIsOutsideTheTransformAndTurnsWithAuto) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\" transform=\"scale(2)\"><animateMotion path=\"M0 0 L100 0 L100 100\" dur=\"2s\" rotate=\"auto\"/></rect>", 1500, l);
  const GSHSCENE_Item * f = FirstFill(l);
  ASSERT_NE(f, nullptr);
  EXPECT_NEAR(f->map.m[0][0], 0, 1e-9);
  EXPECT_NEAR(f->map.m[1][0], 2, 1e-9);
  EXPECT_NEAR(f->map.m[0][1], -2, 1e-9);
  EXPECT_NEAR(f->map.t[0], 100, 1e-6);
  EXPECT_NEAR(f->map.t[1], 50, 1e-6);
}

// ---- A8, A9, A11 -----------------------------------------------------------------------------------------------------------------------

TEST(LowerAnimate, DiscardRemovesItsTargetAtItsBegin) {
  Lowered before, after;
  const std::string body = "<rect width=\"10\" height=\"10\"/><rect width=\"20\" height=\"20\"><discard begin=\"0.2s\"/></rect>";
  At(body, 100, before);
  At(body, 500, after);
  EXPECT_EQ(before.Count(GSHSCENE_ITEM_FILL), (size_t) 2);
  EXPECT_EQ(after.Count(GSHSCENE_ITEM_FILL), (size_t) 1);
}

TEST(LowerAnimate, CssAnimationsAtTheDocumentTimeWithDelaysAndPlayState) {
  Lowered delayed, paused, negative;
  At("<style>@keyframes k{from{opacity:0}to{opacity:1}} rect{animation:k 1s 250ms linear}</style><rect width=\"10\" height=\"10\"/>", 500, delayed);
  At("<style>@keyframes k{from{opacity:0}to{opacity:1}} rect{animation:k 1s linear paused}</style><rect width=\"10\" height=\"10\"/>", 500, paused);
  At("<style>@keyframes k{to{opacity:0}} rect{animation:k 2s -1s linear}</style><rect width=\"10\" height=\"10\"/>", 0, negative);
  EXPECT_NEAR(Opacity(delayed), 0.25, 1e-6);
  EXPECT_EQ(Opacity(paused), -1.0) << "paused from the start: held at opacity 0, so nothing is drawn";
  EXPECT_NEAR(Opacity(negative), 0.5, 1e-6);
}

TEST(LowerAnimate, AUseDrawsTheAnimationsOfWhatItRefersTo) {
  Lowered l;
  At("<defs><rect id=\"s\" width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\"/></rect></defs><use href=\"#s\"/>", 500, l);
  EXPECT_NEAR(Opacity(l), 0.5, 1e-6);
}


TEST(LowerAnimate, AnInvalidDurIsIgnoredAloneAndTheFirstValueShows) {
  for (const char * dur : {"0s", "-1s", "abc", "media"}) {
    Lowered l;
    At(std::string("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0.3\" to=\"0.9\" dur=\"") + dur + "\"/></rect>", 500, l);
    EXPECT_NEAR(Opacity(l), 0.3, 1e-6) << dur;
    EXPECT_EQ(Invalid(l), (size_t) 0) << dur;
  }
}

TEST(LowerAnimate, MinWithoutFreezeAppliesNothingPastTheRepeats) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"0.5\" dur=\"1s\" min=\"2s\"/></rect>", 1500, l);
  EXPECT_NEAR(Opacity(l), 1.0, 1e-6);
}

TEST(LowerAnimate, TheSandwichPutsTheLaterBeginAboveThenTheLaterInDocumentOrder) {
  Lowered begin, order, sum, removed, set_ended;
  At("<rect width=\"10\" height=\"10\"><set attributeName=\"opacity\" to=\"0.3\" begin=\"0.2s\"/><set attributeName=\"opacity\" to=\"0.6\" begin=\"0.1s\"/></rect>",
      500, begin);
  At("<rect width=\"10\" height=\"10\"><set attributeName=\"opacity\" to=\"0.3\"/><set attributeName=\"opacity\" to=\"0.6\"/></rect>", 500, order);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"x\" from=\"0\" to=\"10\" dur=\"1s\" additive=\"sum\"/><animate attributeName=\"x\" from=\"0\" "
     "to=\"100\" dur=\"1s\" additive=\"sum\"/></rect>",
      500, sum);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"0.5\" dur=\"0.2s\"/></rect>", 500, removed);
  At("<rect width=\"10\" height=\"10\"><set attributeName=\"opacity\" to=\"0.5\" dur=\"0.2s\"/></rect>", 500, set_ended);
  EXPECT_NEAR(Opacity(begin), 0.3, 1e-6);
  EXPECT_NEAR(Opacity(order), 0.6, 1e-6);
  EXPECT_NEAR(Bounds(FirstFill(sum)).min.x, 55, 1e-9);
  EXPECT_NEAR(Opacity(removed), 1.0, 1e-6) << "fill=\"remove\": nothing once it ends";
  EXPECT_NEAR(Opacity(set_ended), 1.0, 1e-6);
}

TEST(LowerAnimate, DiscreteToAnimationsAndKeyTimesThatEndEarly) {
  Lowered to, keys;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" to=\"0.5\" calcMode=\"discrete\" dur=\"1s\"/></rect>", 400, to);
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" values=\"0;0.5;1\" keyTimes=\"0;0.2;0.6\" calcMode=\"discrete\" dur=\"1s\"/></rect>", 700,
      keys);
  EXPECT_NEAR(Opacity(to), 1.0, 1e-6) << "the value beneath until half way";
  EXPECT_NEAR(Opacity(keys), 1.0, 1e-6);
}

TEST(LowerAnimate, TheMotionIsOutsideTheTransformOrigin) {
  // Chromium: 0 2 -2 0 105 45, the motion outside the transform with its origin. Firefox (115, 45) turns the origin about the motion too.
  Lowered l;
  At("<rect width=\"10\" height=\"10\" style=\"transform-origin:5px 5px\" transform=\"scale(2)\"><animateMotion path=\"M0 0 L100 0 L100 100\" dur=\"2s\" "
     "rotate=\"auto\"/></rect>",
      1500, l);
  const GSHSCENE_Item * f = FirstFill(l);
  ASSERT_NE(f, nullptr);
  EXPECT_NEAR(f->map.t[0], 105, 1e-6);
  EXPECT_NEAR(f->map.t[1], 45, 1e-6);
}

TEST(LowerAnimate, MotionAlongMpathKeyPointsValuesAndBy) {
  struct Row {
    const char * body;
    double t, x, y, a, b;
  } rows[] = {
      {"<defs><path id=\"p\" d=\"M0 0 H100\"/></defs><rect width=\"10\" height=\"10\"><animateMotion dur=\"1s\"><mpath href=\"#p\"/></animateMotion></rect>", 500, 50, 0, 1, 0},
      {"<rect width=\"10\" height=\"10\"><animateMotion path=\"M0 0 H100\" dur=\"1s\" keyPoints=\"0;0.8;1\" keyTimes=\"0;0.5;1\" calcMode=\"linear\"/></rect>", 250, 40,
          0, 1, 0},
      {"<rect width=\"10\" height=\"10\"><animateMotion path=\"M0 0 H100\" dur=\"1s\" rotate=\"auto-reverse\"/></rect>", 500, 50, 0, -1, 0},
      // Firefox; Chromium ignores an angle.
      {"<rect width=\"10\" height=\"10\"><animateMotion path=\"M0 0 H100\" dur=\"1s\" rotate=\"30\"/></rect>", 500, 50, 0, std::sqrt(0.75), 0.5},
      // SMIL 3.0: the path's points share the time (Firefox); Chromium moves by distance.
      {"<rect width=\"10\" height=\"10\"><animateMotion path=\"M0 0 H10 V100\" dur=\"1s\" calcMode=\"linear\"/></rect>", 250, 5, 0, 1, 0},
      {"<rect width=\"10\" height=\"10\"><animateMotion values=\"0,0;10,0;10,90\" dur=\"1s\"/></rect>", 250, 10, 15, 1, 0},
      {"<rect width=\"10\" height=\"10\"><animateMotion by=\"100,0\" dur=\"1s\"/></rect>", 500, 50, 0, 1, 0},
      {"<rect width=\"10\" height=\"10\"><animateMotion path=\"M0 0 H100\" dur=\"1s\" repeatCount=\"3\" accumulate=\"sum\"/></rect>", 1500, 150, 0, 1, 0},
  };
  for (const Row & row : rows) {
    Lowered l;
    At(row.body, row.t, l);
    const GSHSCENE_Item * f = FirstFill(l);
    ASSERT_NE(f, nullptr) << row.body;
    EXPECT_NEAR(f->map.t[0], row.x, 1e-6) << row.body;
    EXPECT_NEAR(f->map.t[1], row.y, 1e-6) << row.body;
    EXPECT_NEAR(f->map.m[0][0], row.a, 1e-9) << row.body;
    EXPECT_NEAR(f->map.m[1][0], row.b, 1e-9) << row.body;
  }
}

TEST(LowerAnimate, AttributesOfFiltersAndLists) {
  Lowered blur, accumulate;
  At("<filter id=\"f\"><feGaussianBlur stdDeviation=\"0\"><animate attributeName=\"stdDeviation\" to=\"10\" dur=\"1s\"/></feGaussianBlur></filter>"
     "<rect width=\"10\" height=\"10\" filter=\"url(#f)\"/>",
      500, blur);
  At("<polygon points=\"0,0 1,1 0,1\"><animate attributeName=\"points\" from=\"0,0 1,1 0,1\" to=\"10,0 1,1 0,1\" dur=\"1s\" repeatCount=\"3\" accumulate=\"sum\"/></polygon>",
      1500, accumulate);
  bool blurred = false;
  for (const GSHSCENE_Item & item : Flat(blur)) {
    if (item.kind == GSHSCENE_ITEM_GROUP_PUSH && item.group.filter) {
      blurred = true;
    }
  }
  EXPECT_TRUE(blurred) << "stdDeviation 5 half way is a blur (a filter attribute is a list of numbers)";
  EXPECT_NEAR(Bounds(FirstFill(accumulate)).max.x, 15, 1e-9) << "one iteration's 10 accumulated and half of the next";
}

TEST(LowerAnimate, APercentageMeetsANumberAsAFractionInBoundingBoxUnits) {
  // Chromium: 25% half way from 0 to 50%. Firefox resolves the percentage against the viewport (75), which is not the box's.
  Lowered l;
  At("<linearGradient id=\"g\" x1=\"0\" x2=\"1\"><animate attributeName=\"x1\" to=\"50%\" dur=\"1s\"/><stop offset=\"0\" stop-color=\"red\"/>"
     "<stop offset=\"1\" stop-color=\"blue\"/></linearGradient><rect width=\"100\" height=\"10\" fill=\"url(#g)\"/>",
      500, l);
  const GSHSCENE_Item * f = FirstFill(l);
  ASSERT_NE(f, nullptr);
  GSHSCENE_Gradient g;
  ASSERT_EQ(gshscene_resource_gradient(f->paint.resource, &g), GSHSCENE_OK);
  EXPECT_NEAR(g.p0[0] * g.map.m[0][0] + g.map.t[0], 25, 1e-9) << "a quarter of the 100-wide box";
}

TEST(LowerAnimate, AKeywordAttributeIsDiscrete) {
  Lowered before, after;
  const std::string body = "<linearGradient id=\"g\" spreadMethod=\"pad\"><animate attributeName=\"spreadMethod\" from=\"pad\" to=\"repeat\" dur=\"1s\"/>"
                           "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient><rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>";
  At(body, 400, before);
  At(body, 600, after);
  GSHSCENE_Gradient g;
  ASSERT_EQ(gshscene_resource_gradient(FirstFill(before)->paint.resource, &g), GSHSCENE_OK);
  EXPECT_EQ(g.extend, (uint32_t) GSHSCENE_EXTEND_PAD);
  ASSERT_EQ(gshscene_resource_gradient(FirstFill(after)->paint.resource, &g), GSHSCENE_OK);
  EXPECT_EQ(g.extend, (uint32_t) GSHSCENE_EXTEND_REPEAT);
}

TEST(LowerAnimate, AStaticLoweringSamplesNoCssAnimation) {
  Lowered still;
  At("<style>@keyframes k{from{opacity:0.5}to{opacity:0.5}} rect{animation:k 10s}</style><rect width=\"10\" height=\"10\"/>", -1, still);
  EXPECT_NEAR(Opacity(still), 1.0, 1e-6);
}

TEST(LowerAnimate, AnSvgImageIsAtTheSameTime) {
  const std::string inner =
      "&lt;svg xmlns=&quot;http://www.w3.org/2000/svg&quot; width=&quot;4&quot; height=&quot;4&quot;&gt;&lt;rect width=&quot;4&quot; height=&quot;4&quot; "
      "fill=&quot;red&quot;&gt;&lt;set attributeName=&quot;fill&quot; to=&quot;green&quot; begin=&quot;0.2s&quot;/&gt;&lt;/rect&gt;&lt;/svg&gt;";
  Lowered before, after;
  At("<image width=\"4\" height=\"4\" href=\"data:image/svg+xml;utf8," + inner + "\"/>", 100, before);
  At("<image width=\"4\" height=\"4\" href=\"data:image/svg+xml;utf8," + inner + "\"/>", 500, after);
  EXPECT_EQ(Rgb(FirstFill(before)), kRed);
  EXPECT_EQ(Rgb(FirstFill(after)), kGreen);
}

TEST(LowerAnimate, WhatIsInErrorIsReportedOnTheAnimationElement) {
  for (const char * body : {"<rect width=\"10\" height=\"10\"><set to=\"0.5\"/></rect>", "<set href=\"#nothing\" attributeName=\"opacity\" to=\"0.5\"/>",
           "<rect id=\"r\" width=\"10\" height=\"10\"><set attributeName=\"id\" to=\"q\"/></rect>",
           "<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" dur=\"1s\"/></rect>",
           "<rect width=\"10\" height=\"10\"><animateTransform attributeName=\"x\" type=\"scale\" from=\"1\" to=\"2\" dur=\"1s\"/></rect>",
           "<rect width=\"10\" height=\"10\"><animateTransform attributeName=\"transform\" type=\"twist\" from=\"1\" to=\"2\" dur=\"1s\"/></rect>",
           "<rect width=\"10\" height=\"10\"><animateMotion path=\"Q\" dur=\"1s\"/></rect>"}) {
    Lowered l;
    At(body, 500, l);
    ASSERT_EQ(Invalid(l), (size_t) 1) << body;
    const GVEC_Diagnostic * d = nullptr;
    for (size_t i = 0; i < l.by_list.count; i++) {
      if (l.by_list.items[i].code == GVEC_DIAG_ANIMATION_INVALID) {
        d = &l.by_list.items[i];
      }
    }
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->severity, GVEC_DIAG_WARNING);
    const std::string name = d->element ? d->element : "";
    EXPECT_TRUE(name == "set" || name == "animate" || name == "animateTransform" || name == "animateMotion") << "named by the animation element: " << name;
  }
}

TEST(LowerAnimate, TheIntervalLimitIsTheDocumentsOwn) {
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_animation_intervals = 3;
  Lowered l;
  LoadSvg(Svg(kSize, "<rect width=\"10\" height=\"10\" fill=\"red\"><set attributeName=\"fill\" to=\"green\" dur=\"0.1s\" repeatCount=\"indefinite\" "
                     "begin=\"0;0.2s;0.4s\"/></rect>"),
      l.loaded, 0, 0, &limits);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.flags = GVEC_LOWER_ANIMATED;
  options.animation_time_ms = 450;
  LowerBoth(l.loaded, l, &options);
  EXPECT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_LIMIT), (size_t) 1);
  EXPECT_EQ(Rgb(FirstFill(l)), kRed) << "past the limit the document is drawn without its animation";
  Lowered under;
  limits.max_animation_intervals = 100;
  LoadSvg(Svg(kSize, "<rect width=\"10\" height=\"10\" fill=\"red\"><set attributeName=\"fill\" to=\"green\" dur=\"0.1s\" begin=\"0;0.2s;0.4s\"/></rect>"),
      under.loaded, 0, 0, &limits);
  LowerBoth(under.loaded, under, &options);
  EXPECT_EQ(Rgb(FirstFill(under)), kGreen);
}

TEST(LowerAnimate, AGradientTransformIsAnimatedAsTheGradientsTransform) {
  Lowered l;
  At("<linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x2=\"10\"><animateTransform attributeName=\"gradientTransform\" type=\"scale\" from=\"1\" "
     "to=\"3\" dur=\"1s\"/><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>"
     "<rect width=\"50\" height=\"10\" fill=\"url(#g)\"/>",
      500, l);
  const GSHSCENE_Item * f = FirstFill(l);
  ASSERT_NE(f, nullptr);
  GSHSCENE_Gradient g;
  ASSERT_EQ(gshscene_resource_gradient(f->paint.resource, &g), GSHSCENE_OK);
  EXPECT_NEAR(g.map.m[0][0], 2, 1e-9);
}

TEST(LowerAnimate, APatternTransformIsAnimatedAsThePatternsTransform) {
  Lowered l;
  At("<pattern id=\"p\" width=\"5\" height=\"5\" patternUnits=\"userSpaceOnUse\"><set attributeName=\"patternTransform\" to=\"scale(2)\"/>"
     "<rect width=\"2\" height=\"2\"/></pattern><rect width=\"50\" height=\"10\" fill=\"url(#p)\"/>",
      500, l);
  const GSHSCENE_Item * f = FirstFill(l);
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(f->paint.kind, GSHSCENE_PAINT_PATTERN);
  GSHSCENE_Pattern p;
  ASSERT_EQ(gshscene_resource_pattern(f->paint.resource, &p), GSHSCENE_OK);
  EXPECT_NEAR(p.map.m[0][0], 2, 1e-9);
}

TEST(LowerAnimate, AnIntervalIsActiveFromItsBeginUpToButNotAtItsEnd) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><set attributeName=\"opacity\" to=\"0.5\" dur=\"0.5s\"/></rect>", 500, l);
  EXPECT_NEAR(Opacity(l), 1.0, 1e-6);
}

TEST(LowerAnimate, AMinGreaterThanTheMaxIgnoresBoth) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"3s\" min=\"2s\" max=\"1s\" fill=\"freeze\"/></rect>", 1500, l);
  EXPECT_NEAR(Opacity(l), 0.5, 1e-6);
}

TEST(LowerAnimate, AnIntervalThatEndsBeforeTheDocumentBeginsIsNotOne) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"0.5\" dur=\"1s\" begin=\"-5s\" fill=\"freeze\"/></rect>", 500, l);
  EXPECT_NEAR(Opacity(l), 1.0, 1e-6);
}

TEST(LowerAnimate, AnArcFlagInterpolatedIsTrueOnceItIsNotZero) {
  // SVG 2, paths.html; Firefox. Chromium keeps the first flags until half way.
  Lowered l;
  At("<path d=\"M0 0 A10 10 0 0 0 20 0\"><animate attributeName=\"d\" from=\"M0 0 A10 10 0 0 0 20 0\" to=\"M0 0 A10 10 0 1 1 20 0\" dur=\"1s\"/></path>", 300, l);
  GSHGEOM_Bounds b = Bounds(FirstFill(l));
  EXPECT_LT(b.min.y, -5) << "the sweep flag is 1 at 0.3: the arc is above the chord";
}

TEST(LowerAnimate, AnEndValueAZeroLengthIntervalUsedDoesNotEndTheNextOne) {
  // SMIL 3.0, getNextInterval: an end equal to the end of the interval before is passed over. Chromium; Firefox makes a zero-length interval.
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" from=\"0\" to=\"1\" dur=\"1s\" begin=\"0.2s;0.5s\" end=\"0.5s\" fill=\"freeze\"/></rect>", 700,
      l);
  EXPECT_NEAR(Opacity(l), 0.3, 1e-6);
}

TEST(LowerAnimate, PacedIgnoresKeyTimes) {
  Lowered l;
  At("<rect width=\"10\" height=\"10\"><animate attributeName=\"opacity\" values=\"0;0.2;1\" keyTimes=\"0;1\" calcMode=\"paced\" dur=\"1s\"/></rect>", 500, l);
  EXPECT_NEAR(Opacity(l), 0.5, 1e-6);
  EXPECT_EQ(Invalid(l), (size_t) 0);
}

namespace {

/** Answers `other.svg` with one document, whatever kind of resource is asked for. */
struct OneFile {
  std::string text;
  static GVEC_Resource_Status Call(void * user, const GVEC_Resource_Request * request, GVEC_Resource_Reply * reply) {
    OneFile * self = static_cast<OneFile *>(user);
    if (std::string(request->url) != "other.svg") {
      return GVEC_RESOURCE_NOT_FOUND;
    }
    reply->bytes = self->text.data();
    reply->size = self->text.size();
    reply->media_type = "image/svg+xml";
    return GVEC_RESOURCE_OK;
  }
};

}  // namespace

TEST(LowerAnimate, AUseOfAnotherDocumentIsStaticAndAnImageOfItIsAtTheTime) {
  // SVG 2, conform.html: a document a `use` references is processed in secure static mode; one an `image` draws, in secure animated mode.
  OneFile file;
  file.text = Svg("width=\"4\" height=\"4\"", "<rect id=\"r\" width=\"4\" height=\"4\" fill=\"red\"><set attributeName=\"fill\" to=\"green\"/></rect>");
  for (bool image : {false, true}) {
    Lowered l;
    GVEC_Lower_Options options;
    std::memset(&options, 0, sizeof options);
    options.flags = GVEC_LOWER_ANIMATED;
    options.animation_time_ms = 500;
    options.resolver = &OneFile::Call;
    options.resolver_user = &file;
    options.base_uri = "doc.svg";
    LowerSvg(Svg(kSize, image ? "<image width=\"4\" height=\"4\" href=\"other.svg\"/>" : "<use href=\"other.svg#r\"/>"), l, 0, 0, &options);
    EXPECT_EQ(Rgb(FirstFill(l)), image ? kGreen : kRed) << (image ? "image" : "use");
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
