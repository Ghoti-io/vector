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
 * The `image` element lowered (lower_image.c, lower_image_file.c, lower_walk.c): the raster an `href` names (a `data:` URI or a file
 * under the options' image root), decoded under the document's limits, placed by `preserveAspectRatio`, and drawn as the waist draws an
 * image: a fill of the rectangle the image paint's window maps to. The images are those of tests/data/images (tools/gen-test-images.py).
 */

#include "../lower_helpers.h"
#include "../../src/codec/svg/lower/lower_internal.h"

#include <ghoti.io/shape-scene/image.h>
#include <ghoti.io/shape-scene/image_paint.h>

#include <cmath>
#include <filesystem>
#include <string>

using namespace gvec_test;

namespace {

const char * const kImages = "tests/data/images";

std::vector<unsigned char> ImageBytes(const std::string & name) {
  std::string path = std::string(kImages) + "/" + name;
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

std::string Base64(const std::vector<unsigned char> & in, bool wrap = false) {
  static const char * const digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  size_t column = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned v = (unsigned) in[i] << 16 | (unsigned) in[i + 1] << 8 | in[i + 2];
    out += digits[v >> 18];
    out += digits[(v >> 12) & 63];
    out += digits[(v >> 6) & 63];
    out += digits[v & 63];
    column += 4;
    if (wrap && column >= 16) {
      out += "\n  ";
      column = 0;
    }
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

/** The `href` value of an image of tests/data/images as a base64 `data:` URI. */
std::string Data(const std::string & name, const char * mime = "image/png", bool wrap = false) {
  return std::string("data:") + mime + ";base64," + Base64(ImageBytes(name), wrap);
}

/** The same bytes percent-encoded: every byte, so that nothing depends on which bytes are safe. */
std::string Percent(const std::string & name) {
  static const char * const hex = "0123456789ABCDEF";
  std::string out = "data:image/png,";
  for (unsigned char c : ImageBytes(name)) {
    out += '%';
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out;
}

std::string Doc(const std::string & body, const std::string & attributes = "width=\"40\" height=\"40\"") {
  return Svg(attributes, body);
}

std::string Image(const std::string & attributes, const std::string & href) {
  return "<image " + attributes + " href=\"" + href + "\"/>";
}

/** The first fill of the list that has an image paint. */
const GSHSCENE_Item * ImageFill(const Lowered & l, size_t which = 0) {
  size_t seen = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    const GSHSCENE_Item & it = l.Item(i);
    if (it.kind == GSHSCENE_ITEM_FILL && it.paint.kind == GSHSCENE_PAINT_IMAGE && seen++ == which) {
      return &it;
    }
  }
  return nullptr;
}

size_t ImageFills(const Lowered & l) {
  size_t n = 0;
  while (ImageFill(l, n)) {
    n++;
  }
  return n;
}

/** The image fills of the list and of the pictures its instances draw (a `use` is an instance of a picture). */
size_t ImageFillsIn(const GSHSCENE_List * list) {
  size_t n = 0;
  for (size_t i = 0; i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * it = nullptr;
    EXPECT_EQ(gshscene_list_item(list, i, &it), GSHSCENE_OK);
    if (it->kind == GSHSCENE_ITEM_FILL && it->paint.kind == GSHSCENE_PAINT_IMAGE) {
      n++;
    } else if (it->kind == GSHSCENE_ITEM_INSTANCE && gshscene_resource_picture(it->resource)) {
      n += ImageFillsIn(gshscene_resource_picture(it->resource));
    }
  }
  return n;
}

struct Painted {
  GSHSCENE_Image_Paint paint{};
  GSHSCENE_Image_Info image{};
  bool ok = false;
};

Painted Read(const GSHSCENE_Item * item) {
  Painted p;
  if (item && gshscene_resource_image_paint(item->paint.resource, &p.paint) == GSHSCENE_OK &&
      gshscene_resource_image(p.paint.image, &p.image) == GSHSCENE_OK) {
    p.ok = true;
  }
  return p;
}

/** The point a texel corner of the image takes in the element's user space. */
std::pair<double, double> Apply(const GSHGEOM_Map & map, double x, double y) {
  return {map.m[0][0] * x + map.m[0][1] * y + map.t[0], map.m[1][0] * x + map.m[1][1] * y + map.t[1]};
}

const GVEC_Diagnostic * Unusable(const Lowered & l, GVEC_Image_Problem problem) {
  for (size_t i = 0; i < l.by_list.count; i++) {
    if (l.by_list.items[i].code == GVEC_DIAG_IMAGE_UNUSABLE && l.by_list.items[i].detail == (uint32_t) problem) {
      return &l.by_list.items[i];
    }
  }
  return nullptr;
}

size_t UnusableCount(const Lowered & l) {
  return gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_IMAGE_UNUSABLE);
}

/** Lower `svg` with the image root set to the images of the tests. */
void LowerIn(const std::string & svg, Lowered & l, const char * base = kImages, const char * root = kImages) {
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.image_root = root;
  options.image_base = base;
  LowerSvg(svg, l, 0, 0, &options);
}

void ExpectImage(const std::string & name, const char * mime, uint32_t width, uint32_t height, uint32_t layout, uint32_t bits) {
  Lowered l;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", Data(name, mime))), l);
  ASSERT_EQ(l.Refusals(), (size_t) 0) << name;
  ASSERT_EQ(UnusableCount(l), (size_t) 0) << name;
  Painted p = Read(ImageFill(l));
  ASSERT_TRUE(p.ok) << name;
  EXPECT_EQ(p.image.width, width) << name;
  EXPECT_EQ(p.image.height, height) << name;
  EXPECT_EQ(p.image.layout, layout) << name;
  EXPECT_EQ(p.image.bits, bits) << name;
}

}  // namespace

//-----------------------------------------------------------------------------
// The formats and the encodings of a data: URI
//-----------------------------------------------------------------------------

TEST(LowerImage, EachRasterFormatTheDecodersReadIsDrawn) {
  // The decoders give a PNG, a JPEG and a 16-bit PNG of colors as RGBA (opaque), and a gray one as gray.
  ExpectImage("rgb-3x2.png", "image/png", 3, 2, GSHSCENE_IMAGE_RGBA, 8);
  ExpectImage("rgba-3x2.png", "image/png", 3, 2, GSHSCENE_IMAGE_RGBA, 8);
  ExpectImage("gray-4x2.png", "image/png", 4, 2, GSHSCENE_IMAGE_GRAY, 8);
  ExpectImage("rgb16-2x1.png", "image/png", 2, 1, GSHSCENE_IMAGE_RGBA, 16);
  ExpectImage("photo-8x8.jpg", "image/jpeg", 8, 8, GSHSCENE_IMAGE_RGBA, 8);
  ExpectImage("lossless-2x2.webp", "image/webp", 2, 2, GSHSCENE_IMAGE_RGBA, 8);
}

TEST(LowerImage, AnIndexedImageAndAGifAreExpandedToTheColorsTheyName) {
  Lowered indexed;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", Data("indexed-2x2.png"))), indexed);
  Painted p = Read(ImageFill(indexed));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.image.width, 2u);
  EXPECT_EQ(p.image.height, 2u);
  EXPECT_NE(p.image.layout, (uint32_t) GSHSCENE_IMAGE_GRAY) << "the palette is expanded: the image has colors, not indexes";
  Lowered gif;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", Data("animated-2x2.gif", "image/gif"))), gif);
  p = Read(ImageFill(gif));
  ASSERT_TRUE(p.ok) << "the first frame of an animation is the image";
  EXPECT_EQ(p.image.width, 2u);
  EXPECT_EQ(p.image.height, 2u);
}

TEST(LowerImage, TheBytesDecideTheFormatNotTheMediaTypeOrItsAbsence) {
  for (const char * mime : {"image/jpeg", "image/gif", "text/plain", "application/octet-stream", ""}) {
    Lowered l;
    LowerSvg(Doc(Image("width=\"8\" height=\"8\"", Data("rgb-3x2.png", mime))), l);
    EXPECT_EQ(UnusableCount(l), (size_t) 0) << mime;
    EXPECT_TRUE(Read(ImageFill(l)).ok) << mime;
  }
  Lowered bare;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", "data:;base64," + Base64(ImageBytes("rgb-3x2.png")))), bare);
  EXPECT_TRUE(Read(ImageFill(bare)).ok) << "no media type at all";
}

TEST(LowerImage, Base64WithWhiteSpaceAndPercentEncodingAreBothRead) {
  Lowered wrapped;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", Data("rgb-3x2.png", "image/png", true))), wrapped);
  Painted a = Read(ImageFill(wrapped));
  ASSERT_TRUE(a.ok) << "base64 broken over lines, as the corpus writes it";
  Lowered percent;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", Percent("rgb-3x2.png"))), percent);
  Painted b = Read(ImageFill(percent));
  ASSERT_TRUE(b.ok);
  EXPECT_EQ(a.image.width, b.image.width);
  EXPECT_EQ(a.image.height, b.image.height);
  double wa[4], wb[4];
  ASSERT_EQ(gshscene_image_decode(a.paint.image, 2, 1, wa), GSHSCENE_OK);
  ASSERT_EQ(gshscene_image_decode(b.paint.image, 2, 1, wb), GSHSCENE_OK);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(wa[i], wb[i]) << "the same bytes are the same picture, however they were written";
  }
}

TEST(LowerImage, XlinkHrefIsReadAndHrefWins) {
  Lowered xlink;
  LowerSvg(Doc("<image width=\"8\" height=\"8\" xlink:href=\"" + Data("rgb-3x2.png") + "\"/>"), xlink);
  Painted x = Read(ImageFill(xlink));
  ASSERT_TRUE(x.ok);
  EXPECT_EQ(x.image.layout, (uint32_t) GSHSCENE_IMAGE_RGBA);
  Lowered both;
  LowerSvg(Doc("<image width=\"8\" height=\"8\" xlink:href=\"" + Data("rgb-3x2.png") + "\" href=\"" + Data("gray-4x2.png") + "\"/>"), both);
  Painted b = Read(ImageFill(both));
  ASSERT_TRUE(b.ok);
  EXPECT_EQ(b.image.layout, (uint32_t) GSHSCENE_IMAGE_GRAY) << "SVG 2: href takes precedence over xlink:href";
}

//-----------------------------------------------------------------------------
// Where the image goes
//-----------------------------------------------------------------------------

TEST(LowerImage, TheFillIsTheRectangleTheImageFillsUnderTheElementsMap) {
  Lowered l;
  LowerSvg(Doc(Image("x=\"4\" y=\"6\" width=\"6\" height=\"3\" transform=\"translate(1 2)\"", Data("red-blue-2x1.png"))), l);
  const GSHSCENE_Item * it = ImageFill(l);
  ASSERT_NE(it, nullptr);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Items(), (size_t) 1);
  EXPECT_EQ(it->map.t[0], 1.0);
  EXPECT_EQ(it->map.t[1], 2.0);
  Painted p = Read(it);
  ASSERT_TRUE(p.ok);
  // A 2 by 1 image into 6 by 3: scaled by 3, at (4, 6): meet with the same aspect ratio fills the viewport.
  EXPECT_EQ(p.paint.map.m[0][0], 3.0);
  EXPECT_EQ(p.paint.map.m[1][1], 3.0);
  EXPECT_EQ(p.paint.map.m[0][1], 0.0);
  EXPECT_EQ(p.paint.map.t[0], 4.0);
  EXPECT_EQ(p.paint.map.t[1], 6.0);
  EXPECT_EQ(p.paint.extend_x, (uint32_t) GSHSCENE_IMAGE_EXTEND_PAD);
  EXPECT_EQ(p.paint.extend_y, (uint32_t) GSHSCENE_IMAGE_EXTEND_PAD);
  // The fill is the drawn-image shape the writers recognize (IP14), not just any rectangle.
  double quad[8];
  EXPECT_NE(gshscene_image_fill_is_draw(it, quad), 0);
}

TEST(LowerImage, AMissingOrAutoSizeIsTheImagesOwnOrFollowsTheOtherSide) {
  struct Row {
    const char * attributes;
    double sx, sy;  // the scale of the image into the user space: the viewport over the 3 by 2 image
  };
  const Row rows[] = {
      {"", 1, 1},                                       // no size: 3 by 2 user units
      {"width=\"auto\" height=\"auto\"", 1, 1},
      {"width=\"6\"", 2, 2},                            // height from the aspect ratio: 4
      {"height=\"4\"", 2, 2},                           // width: 6
      {"width=\"6\" height=\"auto\"", 2, 2},
      {"width=\"auto\" height=\"4\"", 2, 2},
      {"width=\"9\" height=\"4\" preserveAspectRatio=\"none\"", 3, 2},
  };
  for (const Row & r : rows) {
    Lowered l;
    LowerSvg(Doc(Image(r.attributes, Data("rgb-3x2.png"))), l);
    Painted p = Read(ImageFill(l));
    ASSERT_TRUE(p.ok) << r.attributes;
    EXPECT_EQ(p.paint.map.m[0][0], r.sx) << r.attributes;
    EXPECT_EQ(p.paint.map.m[1][1], r.sy) << r.attributes;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << r.attributes;
  }
}

TEST(LowerImage, PercentagesAreOfTheViewport) {
  Lowered l;
  LowerSvg(Doc(Image("x=\"10%\" y=\"25%\" width=\"50%\" height=\"25%\" preserveAspectRatio=\"none\"", Data("red-blue-2x1.png")), "width=\"40\" height=\"20\""), l);
  Painted p = Read(ImageFill(l));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.paint.map.t[0], 4.0);
  EXPECT_EQ(p.paint.map.t[1], 5.0);
  EXPECT_EQ(p.paint.map.m[0][0], 10.0);  // 20 wide over 2 texels
  EXPECT_EQ(p.paint.map.m[1][1], 5.0);   // 5 high over 1
}

TEST(LowerImage, EveryAlignmentOfMeetAndSliceFitsTheImageAsTheSpecificationSays) {
  // A 2 by 1 image into 20 by 6 at (3, 4): meet scales by 6 (12 by 6), slice by 10 (20 by 10).
  const char * const aligns[] = {"xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"};
  for (int slice = 0; slice < 2; slice++) {
    for (int a = 0; a < 9; a++) {
      std::string par = std::string(aligns[a]) + (slice ? " slice" : " meet");
      Lowered l;
      LowerSvg(Doc(Image("x=\"3\" y=\"4\" width=\"20\" height=\"6\" preserveAspectRatio=\"" + par + "\"", Data("red-blue-2x1.png")), "width=\"40\" height=\"40\""), l);
      Painted p = Read(ImageFill(l));
      ASSERT_TRUE(p.ok) << par;
      const double s = slice ? 10 : 6;
      const double fit_w = 2 * s, fit_h = s;
      const int column = a % 3, row = a / 3;  // 0 min, 1 mid, 2 max
      const double tx = 3 + (20 - fit_w) * column / 2.0;
      const double ty = 4 + (6 - fit_h) * row / 2.0;
      EXPECT_EQ(p.paint.map.m[0][0], s) << par;
      EXPECT_EQ(p.paint.map.m[1][1], s) << par;
      EXPECT_EQ(p.paint.map.t[0], tx) << par;
      EXPECT_EQ(p.paint.map.t[1], ty) << par;
      // Only a slice reaches past the viewport, and only then is the viewport a clip.
      size_t clips = l.Count(GSHSCENE_ITEM_CLIP_PUSH);
      EXPECT_EQ(clips, slice ? (size_t) 1 : (size_t) 0) << par;
      EXPECT_EQ(l.Count(GSHSCENE_ITEM_CLIP_POP), clips) << par;
    }
  }
  Lowered none;
  LowerSvg(Doc(Image("x=\"3\" y=\"4\" width=\"20\" height=\"6\" preserveAspectRatio=\"none\"", Data("red-blue-2x1.png"))), none);
  Painted p = Read(ImageFill(none));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.paint.map.m[0][0], 10.0);
  EXPECT_EQ(p.paint.map.m[1][1], 6.0);
  EXPECT_EQ(none.Count(GSHSCENE_ITEM_CLIP_PUSH), (size_t) 0) << "none fits the viewport exactly";
}

TEST(LowerImage, TheSliceClipIsTheViewportInTheElementsOwnSpaceAndOverflowVisibleRemovesIt) {
  Lowered clipped;
  LowerSvg(Doc(Image("x=\"3\" y=\"4\" width=\"20\" height=\"6\" preserveAspectRatio=\"xMidYMid slice\" transform=\"translate(1 2)\"", Data("red-blue-2x1.png"))), clipped);
  ASSERT_EQ(clipped.Items(), (size_t) 3);
  ASSERT_EQ(clipped.Item(0).kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(Describe(clipped.Item(0).resource), "M3,4 L23,4 L23,10 L3,10 Z");
  EXPECT_EQ(clipped.Item(0).map.t[0], 1.0);
  EXPECT_EQ(clipped.Item(0).map.t[1], 2.0);
  EXPECT_EQ(clipped.Item(1).kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(clipped.Item(2).kind, GSHSCENE_ITEM_CLIP_POP);
  Lowered visible;
  LowerSvg(Doc(Image("x=\"3\" y=\"4\" width=\"20\" height=\"6\" preserveAspectRatio=\"xMidYMid slice\" overflow=\"visible\"", Data("red-blue-2x1.png"))), visible);
  EXPECT_EQ(visible.Items(), (size_t) 1) << "overflow: visible does not clip";
}

TEST(LowerImage, OpacityIsAGroupAndHiddenOrNoneDrawsNothingAndDecodesNothing) {
  Lowered faded;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\" opacity=\"0.5\"", Data("rgb-3x2.png"))), faded);
  ASSERT_EQ(faded.Items(), (size_t) 3);
  EXPECT_EQ(faded.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(faded.Item(0).group.opacity, 0.5);
  Lowered invisible;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\" opacity=\"0\"", Data("rgb-3x2.png"))), invisible);
  EXPECT_EQ(invisible.Items(), (size_t) 0);
  // What is not seen is not read: a broken image under these says nothing.
  for (const char * attributes : {"visibility=\"hidden\"", "display=\"none\"", "style=\"display: none\"", "opacity=\"0\""}) {
    Lowered l;
    LowerSvg(Doc(Image(std::string("width=\"8\" height=\"8\" ") + attributes, "data:image/png;base64,AAAA")), l);
    EXPECT_EQ(l.Items(), (size_t) 0) << attributes;
    EXPECT_EQ(UnusableCount(l), (size_t) 0) << attributes;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << attributes;
  }
  Lowered in_defs;
  LowerSvg(Doc("<defs>" + Image("width=\"8\" height=\"8\"", "data:image/png;base64,AAAA") + "</defs>"), in_defs);
  EXPECT_EQ(UnusableCount(in_defs), (size_t) 0) << "an image in defs is not drawn";
}

TEST(LowerImage, AZeroSizeDisablesTheRenderingWithANoticeAndTheImageIsNeverRead) {
  for (const char * attributes : {"width=\"0\" height=\"8\"", "width=\"8\" height=\"0\"", "width=\"0\" height=\"0\"", "width=\"0%\" height=\"8\""}) {
    Lowered l;
    LowerSvg(Doc(Image(attributes, "data:image/png;base64,AAAA")), l);
    EXPECT_EQ(l.Items(), (size_t) 0) << attributes;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << attributes;
    EXPECT_EQ(UnusableCount(l), (size_t) 0) << attributes << ": a size of zero is the document's meaning, not a broken image";
    EXPECT_EQ(gvec_diagnostics_count_code(&l.by_list, GVEC_DIAG_RENDERING_DISABLED), (size_t) 1) << attributes;
  }
}

TEST(LowerImage, ANegativeSizeIsInvalidAndTheStyleLayerTakesTheInitialValueAuto) {
  Lowered l;
  LowerSvg(Doc(Image("width=\"-3\" height=\"4\"", Data("rgb-3x2.png"))), l);
  Painted p = Read(ImageFill(l));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.paint.map.m[0][0], 2.0) << "the width follows the height at the image's aspect ratio";
}

TEST(LowerImage, TheElementsTransformOpacityAndUseAllApply) {
  Lowered l;
  LowerSvg(Doc("<defs>" + Image("id=\"i\" width=\"8\" height=\"8\"", Data("rgb-3x2.png")) + "</defs><use href=\"#i\" x=\"2\"/><use href=\"#i\" x=\"20\" y=\"5\"/>"), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_INSTANCE), (size_t) 2) << "each use is an instance of a picture of the image";
  EXPECT_EQ(ImageFillsIn(l.list), (size_t) 2);
  EXPECT_EQ(UnusableCount(l), (size_t) 0);
}

//-----------------------------------------------------------------------------
// Orientation
//-----------------------------------------------------------------------------

TEST(LowerImage, EachExifOrientationIsAppliedAndTheIntrinsicSizeIsTheOrientedOne) {
  // The stored picture is 3 by 2: red, green, blue over cyan, magenta, yellow. The picture as displayed (EXIF 1 to 8), row by row.
  struct Row {
    int orientation;
    uint32_t width, height;
    const char * rows;  // one letter a pixel (R G B C M Y), rows separated by `/`
  };
  const Row rows[] = {
      {1, 3, 2, "RGB/CMY"}, {2, 3, 2, "BGR/YMC"}, {3, 3, 2, "YMC/BGR"}, {4, 3, 2, "CMY/RGB"},
      {5, 2, 3, "RC/GM/BY"}, {6, 2, 3, "CR/MG/YB"}, {7, 2, 3, "YB/MG/CR"}, {8, 2, 3, "BY/GM/RC"},
  };
  for (const Row & r : rows) {
    Lowered l;
    LowerSvg(Doc(Image("", Data("exif-" + std::to_string(r.orientation) + ".png"))), l);
    Painted p = Read(ImageFill(l));
    ASSERT_TRUE(p.ok) << r.orientation;
    EXPECT_EQ(p.image.width, r.width) << r.orientation;
    EXPECT_EQ(p.image.height, r.height) << r.orientation;
    // With no size, the viewport is the oriented picture: the fill covers width by height, from the origin.
    GSHGEOM_Bounds bounds;
    ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(l.Item(0).resource), &bounds), GSHGEOM_OK);
    EXPECT_EQ(bounds.max.x - bounds.min.x, (double) r.width) << r.orientation;
    EXPECT_EQ(bounds.max.y - bounds.min.y, (double) r.height) << r.orientation;
    uint32_t x = 0, y = 0;
    for (const char * c = r.rows; *c; c++) {
      if (*c == '/') {
        y++;
        x = 0;
        continue;
      }
      static const double want[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 0}};
      const char * names = "RGBCMY";
      const double * w = want[std::strchr(names, *c) - names];
      double px[4];
      ASSERT_EQ(gshscene_image_decode(p.paint.image, x, y, px), GSHSCENE_OK);
      for (int k = 0; k < 3; k++) {
        EXPECT_NEAR(px[k], w[k], 1e-9) << "orientation " << r.orientation << " pixel " << x << "," << y << " channel " << k;
      }
      x++;
    }
  }
}

//-----------------------------------------------------------------------------
// image-rendering
//-----------------------------------------------------------------------------

TEST(LowerImage, ImageRenderingChoosesTheMagnifyAndMinifyFiltersOfThePaint) {
  struct Row {
    const char * value;
    uint32_t magnify, minify;
  };
  const Row rows[] = {
      {"", GSHSCENE_IMAGE_FILTER_CUBIC, GSHSCENE_IMAGE_FILTER_AREA},
      {"auto", GSHSCENE_IMAGE_FILTER_CUBIC, GSHSCENE_IMAGE_FILTER_AREA},
      {"optimizeQuality", GSHSCENE_IMAGE_FILTER_CUBIC, GSHSCENE_IMAGE_FILTER_AREA},
      {"high-quality", GSHSCENE_IMAGE_FILTER_CUBIC, GSHSCENE_IMAGE_FILTER_AREA},
      {"smooth", GSHSCENE_IMAGE_FILTER_LINEAR, GSHSCENE_IMAGE_FILTER_AREA},
      {"optimizeSpeed", GSHSCENE_IMAGE_FILTER_NEAREST, GSHSCENE_IMAGE_FILTER_NEAREST},
      {"crisp-edges", GSHSCENE_IMAGE_FILTER_NEAREST, GSHSCENE_IMAGE_FILTER_NEAREST},
      {"pixelated", GSHSCENE_IMAGE_FILTER_NEAREST, GSHSCENE_IMAGE_FILTER_NEAREST},
  };
  for (const Row & r : rows) {
    for (int style = 0; style < 2; style++) {
      std::string attributes = std::string("width=\"8\" height=\"8\"");
      if (r.value[0]) {
        attributes += style ? std::string(" style=\"image-rendering: ") + r.value + "\"" : std::string(" image-rendering=\"") + r.value + "\"";
      }
      Lowered l;
      LowerSvg(Doc(Image(attributes, Data("rgb-3x2.png"))), l);
      Painted p = Read(ImageFill(l));
      ASSERT_TRUE(p.ok) << attributes;
      EXPECT_EQ(p.paint.magnify, r.magnify) << attributes;
      EXPECT_EQ(p.paint.minify, r.minify) << attributes;
    }
  }
  Lowered inherited;
  LowerSvg(Doc("<g image-rendering=\"optimizeSpeed\">" + Image("width=\"8\" height=\"8\"", Data("rgb-3x2.png")) + "</g>"), inherited);
  Painted p = Read(ImageFill(inherited));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.paint.magnify, (uint32_t) GSHSCENE_IMAGE_FILTER_NEAREST) << "the property inherits";
}

//-----------------------------------------------------------------------------
// What cannot be drawn
//-----------------------------------------------------------------------------

TEST(LowerImage, AnImageWithNoDataOrBrokenDataDrawsNothingAndSaysWhy) {
  struct Row {
    const char * name;
    std::string element;
    GVEC_Image_Problem problem;
  };
  const Row rows[] = {
      {"no href", "<image width=\"8\" height=\"8\"/>", GVEC_IMAGE_NO_DATA},
      {"empty href", "<image width=\"8\" height=\"8\" href=\"\"/>", GVEC_IMAGE_NO_DATA},
      {"blank href", "<image width=\"8\" height=\"8\" href=\"   \"/>", GVEC_IMAGE_NO_DATA},
      {"empty payload", "<image width=\"8\" height=\"8\" href=\"data:image/png;base64,\"/>", GVEC_IMAGE_NO_DATA},
      {"bad base64", "<image width=\"8\" height=\"8\" href=\"data:image/png;base64,!!!!\"/>", GVEC_IMAGE_NO_DATA},
      {"bad escape", "<image width=\"8\" height=\"8\" href=\"data:image/png,%zz\"/>", GVEC_IMAGE_NO_DATA},
      {"an element", "<rect id=\"r\" width=\"0\" height=\"1\"/><image width=\"8\" height=\"8\" href=\"#r\"/>", GVEC_IMAGE_NOT_AN_IMAGE},
      {"a fragment that names nothing", "<image width=\"8\" height=\"8\" href=\"#nothing\"/>", GVEC_IMAGE_NOT_AN_IMAGE},
      {"text", "<image width=\"8\" height=\"8\" href=\"data:text/plain,hello\"/>", GVEC_IMAGE_UNDECODABLE},
      {"a truncated image", "<image width=\"8\" height=\"8\" href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAAC\"/>", GVEC_IMAGE_UNDECODABLE},
  };
  for (const Row & r : rows) {
    Lowered l;
    LowerSvg(Doc(r.element + "<rect width=\"4\" height=\"4\"/>"), l);
    EXPECT_EQ(ImageFills(l), (size_t) 0) << r.name;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << r.name << ": a broken image is not a refusal";
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1) << r.name << ": the rest of the document is drawn";
    ASSERT_EQ(UnusableCount(l), (size_t) 1) << r.name;
    const GVEC_Diagnostic * d = Unusable(l, r.problem);
    ASSERT_NE(d, nullptr) << r.name;
    EXPECT_EQ(d->severity, GVEC_DIAG_WARNING) << r.name;
    EXPECT_STREQ(d->element, "image") << r.name;
    EXPECT_EQ(d->line, (size_t) 1) << r.name;
  }
}

/** The fills of the list and of the pictures its instances draw. */
size_t FillsIn(const GSHSCENE_List * list) {
  size_t n = 0;
  for (size_t i = 0; i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * it = nullptr;
    EXPECT_EQ(gshscene_list_item(list, i, &it), GSHSCENE_OK);
    if (it->kind == GSHSCENE_ITEM_FILL) {
      n++;
    } else if (it->kind == GSHSCENE_ITEM_INSTANCE && gshscene_resource_picture(it->resource)) {
      n += FillsIn(gshscene_resource_picture(it->resource));
    }
  }
  return n;
}

size_t Instances(const Lowered & l) {
  size_t n = 0;
  for (size_t i = 0; i < l.Items(); i++) {
    n += l.Item(i).kind == GSHSCENE_ITEM_INSTANCE;
  }
  return n;
}

TEST(LowerImage, AnImageThatIsAnSvgDocumentIsDrawnAsAPictureOfItsOwnWhateverCarriesIt) {
  // The markup of a one-rectangle document, written so that it can sit in an attribute (the data: URI's text is what the parser gives back).
  const std::string svg =
      "&lt;svg xmlns=&quot;http://www.w3.org/2000/svg&quot; width=&quot;4&quot; height=&quot;4&quot;&gt;&lt;rect width=&quot;4&quot; height=&quot;4&quot;/&gt;&lt;/svg&gt;";
  struct Row {
    const char * name;
    std::string href;
  };
  const Row rows[] = {
      {"media type", "data:image/svg+xml;utf8," + svg},
      {"an upper-case media type", "DATA:IMAGE/SVG+XML;utf8," + svg},
      {"markup under another media type", "data:text/plain," + svg},
      {"markup without a media type", "data:," + svg},
      {"markup with a byte order mark and white space", "data:,%EF%BB%BF%20%0A" + svg},
  };
  for (const Row & r : rows) {
    Lowered l;
    LowerSvg(Doc(Image("width=\"8\" height=\"8\"", r.href) + "<rect width=\"4\" height=\"4\"/>"), l);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << r.name;
    EXPECT_EQ(Instances(l), (size_t) 1) << r.name << ": the document is a picture, drawn through an instance";
    EXPECT_EQ(FillsIn(l.list), (size_t) 2) << r.name << ": the document's rectangle and the document's own";
    EXPECT_EQ(ImageFills(l), (size_t) 0) << r.name << ": no raster";
  }
  Lowered file;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "document.svg")), file);
  EXPECT_EQ(file.Refusals(), (size_t) 0) << "a file that is an SVG document";
  EXPECT_EQ(FillsIn(file.list), (size_t) 1);
  Lowered broken;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", "data:image/svg+xml;utf8,%3Csvg%20") + "<rect width=\"4\" height=\"4\"/>"), broken);
  EXPECT_EQ(broken.Refusals(), (size_t) 0) << "a document that does not parse is a broken image (R10)";
  EXPECT_EQ(broken.Count(GSHSCENE_ITEM_FILL), (size_t) 1);
  EXPECT_EQ(Instances(broken), (size_t) 0);
}

TEST(LowerImage, AnSvgImageIsPlacedByItsViewBoxWhenItGivesNoSizeAndClippedToItsSize) {
  const std::string svg =
      "data:image/svg+xml;utf8,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20viewBox%3D%220%200%20100%2050%22%3E"
      "%3Crect%20width%3D%22100%22%20height%3D%2250%22%2F%3E%3C%2Fsvg%3E";
  Lowered l;
  LowerSvg(Doc(Image("x=\"10\" y=\"20\" width=\"50\" height=\"25\"", svg)), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  const GSHSCENE_Item * instance = nullptr;
  for (size_t i = 0; i < l.Items(); i++) {
    if (l.Item(i).kind == GSHSCENE_ITEM_INSTANCE) {
      instance = &l.Item(i);
    }
  }
  ASSERT_NE(instance, nullptr);
  EXPECT_NEAR(instance->map.m[0][0], 0.5, 1e-12) << "a 100 by 50 document in a 50 by 25 viewport";
  EXPECT_NEAR(instance->map.m[1][1], 0.5, 1e-12);
  EXPECT_NEAR(instance->map.t[0], 10, 1e-12);
  EXPECT_NEAR(instance->map.t[1], 20, 1e-12);
  const GSHSCENE_List * picture = gshscene_resource_picture(instance->resource);
  ASSERT_NE(picture, nullptr);
  const GSHSCENE_Item * first = nullptr;
  ASSERT_EQ(gshscene_list_item(picture, 0, &first), GSHSCENE_OK);
  EXPECT_EQ(first->kind, GSHSCENE_ITEM_CLIP_PUSH) << "the document is clipped to its own size";
  EXPECT_NEAR(first->map.m[0][0], 1.0, 1e-12) << "in the document's own units, which the instance's map scales";
  EXPECT_NEAR(first->map.m[1][1], 1.0, 1e-12);
  GSHGEOM_Bounds bounds;
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(first->resource), &bounds), GSHGEOM_OK);
  EXPECT_NEAR(bounds.max.x, 100, 1e-12) << "the box is the document's: 100 by 50";
  EXPECT_NEAR(bounds.max.y, 50, 1e-12);
}

TEST(LowerImage, SvgImagesNestedDeeperThanTheLoweringAllowsAreRefused) {
  // each level names the next as a data: URI inside an attribute; escaped once more at every level
  std::string inner = "&lt;svg xmlns=&quot;http://www.w3.org/2000/svg&quot; width=&quot;4&quot; height=&quot;4&quot;&gt;&lt;rect width=&quot;4&quot; height=&quot;4&quot;/&gt;&lt;/svg&gt;";
  std::string doc = "data:image/svg+xml;utf8," + inner;
  for (int level = 0; level < 8; level++) {
    std::string markup = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"><image width=\"4\" height=\"4\" href=\"" + doc + "\"/></svg>";
    std::string percent;
    static const char * const hex = "0123456789ABCDEF";
    for (unsigned char c : markup) {
      percent += '%';
      percent += hex[c >> 4];
      percent += hex[c & 15];
    }
    doc = "data:image/svg+xml;utf8," + percent;
  }
  Lowered l;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", doc)), l);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr) << "eight levels are more than the lowering nests";
}

namespace {

size_t PictureDepth(const GSHSCENE_List * list) {
  size_t deepest = 0;
  for (size_t i = 0; list && i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * it = nullptr;
    if (gshscene_list_item(list, i, &it) == GSHSCENE_OK && it->kind == GSHSCENE_ITEM_INSTANCE) {
      const size_t d = 1 + PictureDepth(gshscene_resource_picture(it->resource));
      deepest = d > deepest ? d : deepest;
    }
  }
  return deepest;
}

}  // namespace

TEST(LowerImage, ADocumentThatIsTheImageOfItselfIsDrawnOnceAndItsOwnImageDrawsNothing) {
  // the main document names recursive.svg, which names itself: the first is drawn (one picture), the image inside that draws nothing
  char dir[] = "/tmp/gvec-recursive-XXXXXX";
  ASSERT_NE(mkdtemp(dir), nullptr);
  const std::string file = std::string(dir) + "/recursive.svg";
  FILE * f = std::fopen(file.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const char * markup = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" viewBox=\"0 0 80 80\">"
                        "<image x=\"8\" y=\"8\" width=\"64\" height=\"64\" xlink:href=\"recursive.svg\"/>"
                        "<rect x=\"1\" y=\"1\" width=\"78\" height=\"78\" fill=\"none\" stroke=\"black\"/></svg>";
  std::fwrite(markup, 1, std::strlen(markup), f);
  std::fclose(f);
  Lowered l;
  LowerIn(markup, l, dir, dir);
  EXPECT_EQ(l.Refusals(), (size_t) 0) << "not the nesting limit: the recursion is found";
  EXPECT_EQ(PictureDepth(l.list), (size_t) 1) << "the document once, as a picture; the image inside it draws nothing";
  EXPECT_GE(UnusableCount(l), (size_t) 1);
  std::remove(file.c_str());
  rmdir(dir);
}

//-----------------------------------------------------------------------------
// The resource policy: files
//-----------------------------------------------------------------------------

TEST(LowerImage, AFileUnderTheImageRootIsReadAndAnythingElseIsRefusedByName) {
  Lowered ok;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "red-blue-2x1.png")), ok);
  Painted p = Read(ImageFill(ok));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.image.width, 2u);
  EXPECT_EQ(ok.Refusals(), (size_t) 0);

  struct Row {
    const char * name;
    std::string href;
  };
  const Row refused[] = {
      {"http", "http://example.com/a.png"},
      {"https", "https://example.com/a.png"},
      {"file", "file:///etc/hostname"},
      {"a scheme in capitals", "HTTP://example.com/a.png"},
      {"a network path", "//example.com/a.png"},
      {"an absolute path", "/etc/hostname"},
      {"a drive", "C:/windows/a.png"},
      {"above the root", "../red-blue-2x1.png"},
      {"above the root through a directory", "sub/../../red-blue-2x1.png"},
      {"above the root through an escape", "%2e%2e/red-blue-2x1.png"},
      {"above the root through a doubly encoded escape that decodes once", "%2E%2E%2fred-blue-2x1.png"},
      {"a backslash", "sub\\a.png"},
      {"a backslash escape", "sub%5ca.png"},
      {"a NUL escape", "red-blue-2x1.png%00.png"},
      {"a control character", "red-blue-2x1.png%0a"},
      {"a data URI's cousin", "javascript:alert(1)"},
  };
  for (const Row & r : refused) {
    Lowered l;
    LowerIn(Doc(Image("width=\"8\" height=\"8\"", r.href) + "<rect width=\"4\" height=\"4\"/>"), l);
    ASSERT_EQ(l.Refusals(), (size_t) 1) << r.name;
    EXPECT_NE(l.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr) << r.name;
    EXPECT_EQ(ImageFills(l), (size_t) 0) << r.name;
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1) << r.name;
  }
}

TEST(LowerImage, WithNoImageRootEveryFileIsRefused) {
  for (const char * href : {"red-blue-2x1.png", "./red-blue-2x1.png", "http://example.com/a.png"}) {
    Lowered l;
    LowerSvg(Doc(Image("width=\"8\" height=\"8\"", href) + "<rect width=\"4\" height=\"4\"/>"), l);
    ASSERT_EQ(l.Refusals(), (size_t) 1) << href;
    const GVEC_Diagnostic * d = l.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE);
    ASSERT_NE(d, nullptr) << href;
  }
  Lowered data;
  LowerSvg(Doc(Image("width=\"8\" height=\"8\"", Data("red-blue-2x1.png"))), data);
  EXPECT_EQ(data.Refusals(), (size_t) 0) << "a data: URI needs no root";
}

TEST(LowerImage, ADocumentRelativePathIsResolvedAgainstTheBaseAndHeldToTheRoot) {
  Lowered up;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "../images/red-blue-2x1.png")), up, "tests/data/images", "tests/data");
  EXPECT_TRUE(Read(ImageFill(up)).ok) << "the root is the parent, and the path goes up and down inside it";
  Lowered dot;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "./././red-blue-2x1.png")), dot);
  EXPECT_TRUE(Read(ImageFill(dot)).ok);
  Lowered escaped;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "%72ed-blue-2x1.png")), escaped);
  EXPECT_TRUE(Read(ImageFill(escaped)).ok) << "an escape decodes to an ordinary letter";
  Lowered space;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "red-blue-2x1%20spaced.png")), space);
  EXPECT_TRUE(Read(ImageFill(space)).ok) << "an escaped space";
  Lowered query;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "red-blue-2x1.png?cache=1#frag")), query);
  EXPECT_TRUE(Read(ImageFill(query)).ok) << "the path of a URL ends at the query or the fragment";
  Lowered outside_base;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "red-blue-2x1.png") + "<rect width=\"4\" height=\"4\"/>"), outside_base, "tests/data/svg", "tests/data/images");
  EXPECT_EQ(outside_base.Refusals(), (size_t) 1) << "a base that is not inside the root reaches nothing";
  EXPECT_NE(outside_base.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr);
  Lowered base_only;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "red-blue-2x1.png")), base_only, nullptr, kImages);
  EXPECT_TRUE(Read(ImageFill(base_only)).ok) << "no base: the root";
}

TEST(LowerImage, AFileThatIsMissingEmptyOrNotAnImageDrawsNothing) {
  struct Row {
    const char * href;
    GVEC_Image_Problem problem;
  };
  const Row rows[] = {
      {"does-not-exist.png", GVEC_IMAGE_UNREADABLE},
      {"empty.bin", GVEC_IMAGE_NO_DATA},
      {"not-an-image.bin", GVEC_IMAGE_UNDECODABLE},
      {"?query-only", GVEC_IMAGE_NO_DATA},
      {"#fragment-only", GVEC_IMAGE_NOT_AN_IMAGE},
  };
  for (const Row & r : rows) {
    Lowered l;
    LowerIn(Doc(Image("width=\"8\" height=\"8\"", r.href) + "<rect width=\"4\" height=\"4\"/>"), l);
    EXPECT_EQ(l.Refusals(), (size_t) 0) << r.href;
    EXPECT_EQ(ImageFills(l), (size_t) 0) << r.href;
    EXPECT_NE(Unusable(l, r.problem), nullptr) << r.href;
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1) << r.href;
  }
}

TEST(LowerImage, TheLexicalPolicyIsCheckedOnItsOwn) {
  struct Row {
    const char * root;
    const char * base;
    const char * reference;
    gvec_lw_image_path_status status;
    const char * path;
  };
  const Row rows[] = {
      {"/r", "/r", "a.png", GVEC_LW_IMAGE_PATH_OK, "/r/a.png"},
      {"/r", "/r/d", "a.png", GVEC_LW_IMAGE_PATH_OK, "/r/d/a.png"},
      {"/r", "/r/d", "../a.png", GVEC_LW_IMAGE_PATH_OK, "/r/a.png"},
      {"/r", "/r/d", "../../a.png", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},
      {"/r", "/r/d", "../../r/a.png", GVEC_LW_IMAGE_PATH_OK, "/r/a.png"},
      {"/r", "/r-other", "a.png", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},
      {"/r", "/r/d", "../../r-other/a.png", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},
      {"/r", "/r", "a/./b//c.png", GVEC_LW_IMAGE_PATH_OK, "/r/a/b/c.png"},
      {"/r", "/r", "a/b/../../c.png", GVEC_LW_IMAGE_PATH_OK, "/r/c.png"},
      {"/r", "/r", ".", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},
      {"/r", "/r", "a/..", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},
      {"/r", "/r", "/etc/x", GVEC_LW_IMAGE_PATH_ABSOLUTE, nullptr},
      {"/r", "/r", "%2fetc/x", GVEC_LW_IMAGE_PATH_ABSOLUTE, nullptr},
      {"/r", "/r", "//host/x", GVEC_LW_IMAGE_PATH_NETWORK, nullptr},
      {"/r", "/r", "ftp://host/x", GVEC_LW_IMAGE_PATH_NETWORK, nullptr},
      {"/r", "/r", "a+b.c-d:e", GVEC_LW_IMAGE_PATH_NETWORK, nullptr},  // a scheme, by the grammar of a scheme
      {"/r", "/r", "1a:b", GVEC_LW_IMAGE_PATH_OK, "/r/1a:b"},        // a scheme begins with a letter
      {"/r", "/r", "a\\b", GVEC_LW_IMAGE_PATH_INVALID, nullptr},
      {"/r", "/r", "a%5Cb", GVEC_LW_IMAGE_PATH_INVALID, nullptr},
      {"/r", "/r", "a%", GVEC_LW_IMAGE_PATH_INVALID, nullptr},
      {"/r", "/r", "a%4", GVEC_LW_IMAGE_PATH_INVALID, nullptr},
      {"/r", "/r", "a%4g", GVEC_LW_IMAGE_PATH_INVALID, nullptr},
      {"/r", "/r", "a%00b", GVEC_LW_IMAGE_PATH_INVALID, nullptr},
      {"/r", "/r", "a%7fb", GVEC_LW_IMAGE_PATH_INVALID, nullptr},
      {"/r", "/r", "?x", GVEC_LW_IMAGE_PATH_EMPTY, nullptr},
      {"/r", "/r", "", GVEC_LW_IMAGE_PATH_EMPTY, nullptr},
      {"/r", "/r", "a%2Fb.png", GVEC_LW_IMAGE_PATH_OK, "/r/a/b.png"},
      {"/r", "/r", "a.png#%zz", GVEC_LW_IMAGE_PATH_OK, "/r/a.png"},
      {"rel/root", "rel/root", "a.png", GVEC_LW_IMAGE_PATH_OK, "rel/root/a.png"},
      {"rel/root", "rel/root/d", "../../a.png", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},
      {"rel/root", "/rel/root", "a.png", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},  // an absolute base is not inside a relative root
      {"../up", "../up/d", "a.png", GVEC_LW_IMAGE_PATH_OK, "../up/d/a.png"},
      {"../up", "../up", "../../x.png", GVEC_LW_IMAGE_PATH_OUTSIDE, nullptr},
      {"/", "/", "a.png", GVEC_LW_IMAGE_PATH_OK, "/a.png"},
      {"/r/", "/r/", "a.png", GVEC_LW_IMAGE_PATH_OK, "/r/a.png"},
      {"", "", "a.png", GVEC_LW_IMAGE_PATH_NO_ROOT, nullptr},
      {nullptr, nullptr, "a.png", GVEC_LW_IMAGE_PATH_NO_ROOT, nullptr},
      {"/r", nullptr, "a.png", GVEC_LW_IMAGE_PATH_OK, "/r/a.png"},
  };
  for (const Row & r : rows) {
    gvec_lw_image_path_status status;
    char * path = nullptr;
    ASSERT_EQ(gvec_lw_image_resolve(nullptr, r.root, r.base, r.reference, &status, &path), GVEC_OK) << r.reference;
    EXPECT_EQ(status, r.status) << (r.root ? r.root : "(null)") << " " << (r.base ? r.base : "(null)") << " " << r.reference;
    if (r.path) {
      ASSERT_NE(path, nullptr) << r.reference;
      EXPECT_STREQ(path, r.path) << r.reference;
    } else {
      EXPECT_EQ(path, nullptr) << r.reference;
    }
    gvec_free(nullptr, path);
  }
}

//-----------------------------------------------------------------------------
// The resource policy: what the file system adds to the lexical check
//-----------------------------------------------------------------------------

#if !defined(_WIN32)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/** A scratch directory of its own, removed with what is in it. */
struct Scratch {
  std::string root;
  Scratch() {
    char pattern[] = "/tmp/gvec-image-XXXXXX";
    char * made = mkdtemp(pattern);
    if (made) {
      root = made;
    }
  }
  ~Scratch() {
    if (!root.empty()) {
      std::error_code error;
      std::filesystem::remove_all(root, error);
      EXPECT_FALSE(error) << "could not remove " << root;
    }
  }
  std::string Path(const std::string & name) const { return root + "/" + name; }
  void Copy(const std::string & from, const std::string & to) const {
    std::vector<unsigned char> bytes = ImageBytes(from);
    FILE * f = std::fopen(Path(to).c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
  }
};

}  // namespace

TEST(LowerImage, ALinkThatLeavesTheRootIsRefusedAndALinkThatStaysInsideIsFollowed) {
  Scratch s;
  ASSERT_FALSE(s.root.empty());
  ASSERT_EQ(mkdir(s.Path("root").c_str(), 0700), 0);
  ASSERT_EQ(mkdir(s.Path("root/inner").c_str(), 0700), 0);
  ASSERT_EQ(mkdir(s.Path("outside").c_str(), 0700), 0);
  ASSERT_EQ(mkdir(s.Path("root-evil").c_str(), 0700), 0);  // a sibling whose name begins with the root's
  s.Copy("red-blue-2x1.png", "root-evil/secret.png");
  s.Copy("red-blue-2x1.png", "root/inner/real.png");
  s.Copy("red-blue-2x1.png", "outside/secret.png");
  ASSERT_EQ(symlink("inner/real.png", s.Path("root/inside-link.png").c_str()), 0);
  ASSERT_EQ(symlink("../outside/secret.png", s.Path("root/escape-link.png").c_str()), 0);
  ASSERT_EQ(symlink("../outside", s.Path("root/escape-dir").c_str()), 0);
  ASSERT_EQ(symlink("../root-evil/secret.png", s.Path("root/sibling-link.png").c_str()), 0);
  ASSERT_EQ(symlink(s.Path("outside/secret.png").c_str(), s.Path("root/absolute-link.png").c_str()), 0);
  ASSERT_EQ(symlink("loop-b.png", s.Path("root/loop-a.png").c_str()), 0);
  ASSERT_EQ(symlink("loop-a.png", s.Path("root/loop-b.png").c_str()), 0);
  const std::string root = s.Path("root");
  for (const char * href : {"inner/real.png", "inside-link.png"}) {
    Lowered l;
    LowerIn(Doc(Image("width=\"8\" height=\"8\"", href)), l, root.c_str(), root.c_str());
    EXPECT_TRUE(Read(ImageFill(l)).ok) << href;
    EXPECT_EQ(l.Refusals(), (size_t) 0) << href;
  }
  for (const char * href : {"escape-link.png", "escape-dir/secret.png", "absolute-link.png", "sibling-link.png"}) {
    Lowered l;
    LowerIn(Doc(Image("width=\"8\" height=\"8\"", href) + "<rect width=\"4\" height=\"4\"/>"), l, root.c_str(), root.c_str());
    ASSERT_EQ(l.Refusals(), (size_t) 1) << href;
    EXPECT_NE(l.Refusal(GVEC_LOWER_EXTERNAL_REFERENCE), nullptr) << href << ": a file whose real path is outside the root";
    EXPECT_EQ(ImageFills(l), (size_t) 0) << href;
  }
  Lowered loop;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "loop-a.png")), loop, root.c_str(), root.c_str());
  EXPECT_EQ(loop.Refusals(), (size_t) 0);
  EXPECT_NE(Unusable(loop, GVEC_IMAGE_UNREADABLE), nullptr) << "a loop of links is a file that cannot be read";
  // A root that is itself reached through a link names the same directory.
  ASSERT_EQ(symlink(root.c_str(), s.Path("root-link").c_str()), 0);
  const std::string through = s.Path("root-link");
  Lowered via;
  LowerIn(Doc(Image("width=\"8\" height=\"8\"", "inner/real.png")), via, through.c_str(), through.c_str());
  EXPECT_TRUE(Read(ImageFill(via)).ok);
}

TEST(LowerImage, ADirectoryAPipeAndAFileThatIsTooLargeAreNotRead) {
  Scratch s;
  ASSERT_FALSE(s.root.empty());
  ASSERT_EQ(mkdir(s.Path("dir.png").c_str(), 0700), 0);
  ASSERT_EQ(mkfifo(s.Path("pipe.png").c_str(), 0600), 0);
  s.Copy("photo-8x8.jpg", "photo.jpg");
  s.Copy("red-blue-2x1.png", "ok.png");
  for (const char * href : {"dir.png", "pipe.png"}) {
    Lowered l;
    LowerIn(Doc(Image("width=\"8\" height=\"8\"", href)), l, s.root.c_str(), s.root.c_str());  // a pipe would block if it were opened
    EXPECT_EQ(l.Refusals(), (size_t) 0) << href;
    EXPECT_NE(Unusable(l, GVEC_IMAGE_UNREADABLE), nullptr) << href << ": only a regular file is read";
  }
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_image_bytes = 100;  // the photo has 650
  Lowered big;
  LoadSvg(Doc(Image("width=\"8\" height=\"8\"", "photo.jpg") + Image("width=\"8\" height=\"8\"", "ok.png")), big.loaded, 0, 0, &limits);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.image_root = s.root.c_str();
  LowerBoth(big.loaded, big, &options);
  EXPECT_EQ(big.Refusals(), (size_t) 1);
  const GVEC_Diagnostic * d = big.Refusal(GVEC_LOWER_LIMIT);
  ASSERT_NE(d, nullptr) << "a file longer than max_image_bytes";
  EXPECT_EQ(d->line, (size_t) 1);
  EXPECT_EQ(ImageFills(big), (size_t) 1) << "the small one is drawn";
}

#endif  // !_WIN32

//-----------------------------------------------------------------------------
// Limits
//-----------------------------------------------------------------------------

namespace {

/** Lower `svg` under `limits`. */
void LowerLimited(const std::string & svg, const GVEC_Limits & limits, Lowered & l) {
  LoadSvg(svg, l.loaded, 0, 0, &limits);
  GVEC_Lower_Options options;
  std::memset(&options, 0, sizeof options);
  options.image_root = kImages;
  options.image_base = kImages;
  LowerBoth(l.loaded, l, &options);
}

}  // namespace

TEST(LowerImage, TheEncodedSizeOfAnImageIsHeldToMaxImageBytesForADataUriAndAFile) {
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_image_bytes = 100;
  for (const std::string & href : {Data("photo-8x8.jpg", "image/jpeg"), Percent("photo-8x8.jpg"), std::string("photo-8x8.jpg")}) {
    Lowered l;
    LowerLimited(Doc(Image("width=\"8\" height=\"8\"", href) + "<rect width=\"4\" height=\"4\"/>"), limits, l);
    ASSERT_EQ(l.Refusals(), (size_t) 1) << href.substr(0, 30);
    EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr) << href.substr(0, 30);
    EXPECT_EQ(ImageFills(l), (size_t) 0);
    EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1) << "the rest is drawn";
  }
  // An image of exactly the cap is read: the bound is on what is larger.
  limits.max_image_bytes = ImageBytes("red-blue-2x1.png").size();
  Lowered exact;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", Data("red-blue-2x1.png"))), limits, exact);
  EXPECT_TRUE(Read(ImageFill(exact)).ok);
  limits.max_image_bytes = ImageBytes("red-blue-2x1.png").size() - 1;
  Lowered under;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", Data("red-blue-2x1.png"))), limits, under);
  EXPECT_NE(under.Refusal(GVEC_LOWER_LIMIT), nullptr);
  // The same bound on a file: exactly the cap is read, one byte less is not.
  limits.max_image_bytes = ImageBytes("red-blue-2x1.png").size();
  Lowered file_exact;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", "red-blue-2x1.png")), limits, file_exact);
  EXPECT_TRUE(Read(ImageFill(file_exact)).ok);
  limits.max_image_bytes = ImageBytes("red-blue-2x1.png").size() - 1;
  Lowered file_under;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", "red-blue-2x1.png")), limits, file_under);
  EXPECT_NE(file_under.Refusal(GVEC_LOWER_LIMIT), nullptr);
}

TEST(LowerImage, ThePixelsOfAnImageAreHeldToMaxImagePixelsBeforeTheyAreAllocated) {
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_image_pixels = 5;  // the picture has 6
  Lowered over;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", Data("rgb-3x2.png")) + "<rect width=\"4\" height=\"4\"/>"), limits, over);
  EXPECT_EQ(over.Refusals(), (size_t) 1);
  EXPECT_NE(over.Refusal(GVEC_LOWER_LIMIT), nullptr);
  EXPECT_EQ(ImageFills(over), (size_t) 0);
  limits.max_image_pixels = 6;
  Lowered exact;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", Data("rgb-3x2.png"))), limits, exact);
  EXPECT_TRUE(Read(ImageFill(exact)).ok) << "an image of exactly the cap is drawn";
}

TEST(LowerImage, AHostileSizeInAFewKilobytesIsRefusedUnderTheDefaultLimitsWithoutTheMemoryItNames) {
  // 20,000 by 20,000 pixels of one bit: 400 million pixels, past the default 2^25, in 48 KB of file.
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  Lowered l;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", "sparse-20000x20000.png") + "<rect width=\"4\" height=\"4\"/>"), limits, l);
  EXPECT_EQ(l.Refusals(), (size_t) 1);
  EXPECT_NE(l.Refusal(GVEC_LOWER_LIMIT), nullptr);
  EXPECT_EQ(ImageFills(l), (size_t) 0);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1);
}

TEST(LowerImage, TheDecodedBytesOfAllTheImagesOfALoweringAreHeldToTheirBudgetAnImageCountedOnce) {
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  limits.max_image_decoded_bytes = 60;  // a 3 by 2 image decodes to 24 bytes of RGBA
  const std::string href = Data("rgb-3x2.png");
  Lowered three;
  LowerLimited(Doc(Image("id=\"a\" width=\"8\" height=\"8\"", href) + Image("id=\"b\" x=\"9\" width=\"8\" height=\"8\"", href) +
                      Image("id=\"c\" x=\"18\" width=\"8\" height=\"8\"", href)),
      limits, three);
  EXPECT_EQ(ImageFills(three), (size_t) 2) << "24 and 48 bytes are within 60, 72 is not";
  ASSERT_EQ(three.Refusals(), (size_t) 1);
  const GVEC_Diagnostic * d = three.Refusal(GVEC_LOWER_LIMIT);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->line, (size_t) 1);
  // One image drawn many times is decoded once.
  Lowered many;
  LowerLimited(Doc("<defs>" + Image("id=\"i\" width=\"8\" height=\"8\"", href) + "</defs><use href=\"#i\"/><use href=\"#i\" x=\"9\"/><use href=\"#i\" x=\"18\"/><use href=\"#i\" x=\"27\"/>"), limits, many);
  EXPECT_EQ(ImageFillsIn(many.list), (size_t) 4);
  EXPECT_EQ(many.Refusals(), (size_t) 0);
  limits.max_image_decoded_bytes = 23;
  Lowered tiny;
  LowerLimited(Doc(Image("width=\"8\" height=\"8\"", href)), limits, tiny);
  EXPECT_NE(tiny.Refusal(GVEC_LOWER_LIMIT), nullptr) << "an image larger than the whole budget";
}

TEST(LowerImage, TheImageLimitsHaveCeilingsAndDefaults) {
  GVEC_Limits limits;
  gvec_limits_default(&limits);
  EXPECT_EQ(limits.max_image_bytes, (size_t) 32 * 1024 * 1024);
  EXPECT_EQ(limits.max_image_pixels, (size_t) 1 << 25);
  EXPECT_EQ(limits.max_image_decoded_bytes, (size_t) 512 * 1024 * 1024);
  GVEC_Limits above = limits;
  above.max_image_pixels = ((size_t) 1 << 28) + 1;
  Loaded l;
  Load(Bytes(Doc("")), &above, l);
  EXPECT_EQ(l.result, GVEC_ERR_INVALID);
  ASSERT_NE(l.First(GVEC_DIAG_LIMIT), nullptr);
  EXPECT_EQ(l.First(GVEC_DIAG_LIMIT)->detail, (uint32_t) GVEC_LIMIT_IMAGE_PIXELS);
}

//-----------------------------------------------------------------------------
// clip-path, mask and filter on the element
//-----------------------------------------------------------------------------

TEST(LowerImage, AClipPathAMaskAndAFilterOnAnImageAreTheGroupsOfAnyOtherElement) {
  Lowered clip;
  LowerSvg(Doc("<clipPath id=\"c\"><rect width=\"4\" height=\"4\"/></clipPath>" + Image("width=\"8\" height=\"8\" clip-path=\"url(#c)\"", Data("rgb-3x2.png"))), clip);
  EXPECT_EQ(clip.Refusals(), (size_t) 0);
  EXPECT_EQ(clip.Count(GSHSCENE_ITEM_CLIP_PUSH), (size_t) 1);
  EXPECT_TRUE(Read(ImageFill(clip)).ok);
  Lowered mask;
  LowerSvg(Doc("<mask id=\"m\"><rect width=\"8\" height=\"8\" fill=\"white\"/></mask>" + Image("width=\"8\" height=\"8\" mask=\"url(#m)\"", Data("rgb-3x2.png"))), mask);
  EXPECT_EQ(mask.Refusals(), (size_t) 0);
  EXPECT_EQ(mask.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_NE(mask.Item(0).group.soft_mask, nullptr);
  Lowered filter;
  LowerSvg(Doc("<filter id=\"f\"><feOffset dx=\"1\"/></filter>" + Image("x=\"4\" y=\"6\" width=\"8\" height=\"8\" filter=\"url(#f)\"", Data("rgb-3x2.png"))), filter);
  EXPECT_EQ(filter.Refusals(), (size_t) 0);
  GSHSCENE_Filter f;
  ASSERT_EQ(filter.Item(0).kind, GSHSCENE_ITEM_GROUP_PUSH);
  ASSERT_NE(filter.Item(0).group.filter, nullptr);
  ASSERT_EQ(gshscene_resource_filter(filter.Item(0).group.filter, &f), GSHSCENE_OK);
  // The default region is -10% to 120% of the box, which is the image's viewport (the box of an image is its x, y, width and height).
  EXPECT_DOUBLE_EQ(f.region[0], 3.2);
  EXPECT_DOUBLE_EQ(f.region[1], 5.2);
  EXPECT_DOUBLE_EQ(f.region[2], 9.6);
  EXPECT_DOUBLE_EQ(f.region[3], 9.6);
}

TEST(LowerImage, TheBoxOfAnImageIsItsViewportForClipsMasksAndFiltersInTheUnitsOfTheBox) {
  Lowered clip;
  LowerSvg(Doc("<clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"0.5\" height=\"1\"/></clipPath>" +
                  Image("x=\"4\" y=\"6\" width=\"8\" height=\"8\" clip-path=\"url(#c)\"", Data("rgb-3x2.png"))), clip);
  EXPECT_EQ(clip.Refusals(), (size_t) 0);
  ASSERT_EQ(clip.Item(0).kind, GSHSCENE_ITEM_CLIP_PUSH);
  GSHGEOM_Bounds b;
  GSHGEOM_Map m = clip.Item(0).map;
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(clip.Item(0).resource), &b), GSHGEOM_OK);
  auto lo = Apply(m, b.min.x, b.min.y), hi = Apply(m, b.max.x, b.max.y);
  EXPECT_EQ(lo.first, 4.0);
  EXPECT_EQ(lo.second, 6.0);
  EXPECT_EQ(hi.first, 8.0);
  EXPECT_EQ(hi.second, 14.0);
  // A group holding an image has a box too.
  Lowered group;
  LowerSvg(Doc("<clipPath id=\"c\" clipPathUnits=\"objectBoundingBox\"><rect width=\"0.5\" height=\"1\"/></clipPath><g clip-path=\"url(#c)\">" +
                  Image("x=\"4\" y=\"6\" width=\"8\" height=\"8\"", Data("rgb-3x2.png")) + "<rect x=\"20\" y=\"6\" width=\"4\" height=\"8\"/></g>"), group);
  EXPECT_EQ(group.Refusals(), (size_t) 0);
}

TEST(LowerImage, ABasicShapeClipOnAnImageTakesTheViewportAsItsReferenceBox) {
  Lowered l;
  LowerSvg(Doc(Image("x=\"4\" y=\"6\" width=\"8\" height=\"8\" style=\"clip-path: inset(25%)\"", Data("rgb-3x2.png"))), l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  ASSERT_EQ(l.Item(0).kind, GSHSCENE_ITEM_CLIP_PUSH);
  GSHGEOM_Bounds b;
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(l.Item(0).resource), &b), GSHGEOM_OK);
  auto lo = Apply(l.Item(0).map, b.min.x, b.min.y), hi = Apply(l.Item(0).map, b.max.x, b.max.y);
  // inset(25%) of the box 4, 6, 8, 8 (a square, so that the basis of the percentage is not in question): 2 from each side.
  EXPECT_EQ(lo.first, 6.0);
  EXPECT_EQ(lo.second, 8.0);
  EXPECT_EQ(hi.first, 10.0);
  EXPECT_EQ(hi.second, 12.0);
}

TEST(LowerImage, AnImageInAPatternAMaskAndAMarkerIsDrawn) {
  Lowered l;
  LowerSvg(Doc("<pattern id=\"p\" width=\"4\" height=\"4\" patternUnits=\"userSpaceOnUse\">" + Image("width=\"4\" height=\"4\"", Data("rgb-3x2.png")) +
                  "</pattern><rect width=\"20\" height=\"20\" fill=\"url(#p)\"/>"),
      l);
  EXPECT_EQ(l.Refusals(), (size_t) 0);
  EXPECT_EQ(l.Count(GSHSCENE_ITEM_FILL), (size_t) 1);
  const GSHSCENE_Item * fill = nullptr;
  for (size_t i = 0; i < l.Items(); i++) {
    if (l.Item(i).kind == GSHSCENE_ITEM_FILL) {
      fill = &l.Item(i);
    }
  }
  ASSERT_NE(fill, nullptr);
  ASSERT_EQ(fill->paint.kind, GSHSCENE_PAINT_PATTERN);
  GSHSCENE_Pattern pattern;
  ASSERT_EQ(gshscene_resource_pattern(fill->paint.resource, &pattern), GSHSCENE_OK);
  const GSHSCENE_List * tile = gshscene_resource_picture(pattern.tile);
  ASSERT_NE(tile, nullptr);
  bool image = false;
  for (size_t i = 0; i < gshscene_list_count(tile); i++) {
    const GSHSCENE_Item * item = nullptr;
    ASSERT_EQ(gshscene_list_item(tile, i, &item), GSHSCENE_OK);
    image = image || (item->kind == GSHSCENE_ITEM_FILL && item->paint.kind == GSHSCENE_PAINT_IMAGE);
  }
  EXPECT_TRUE(image) << "the tile holds the image";
  Lowered marker;
  LowerSvg(Doc("<marker id=\"k\" markerWidth=\"4\" markerHeight=\"4\">" + Image("width=\"4\" height=\"4\"", Data("rgb-3x2.png")) +
                  "</marker><path d=\"M2 2L10 10\" stroke=\"black\" marker-end=\"url(#k)\"/>"),
      marker);
  EXPECT_EQ(marker.Refusals(), (size_t) 0);
  Lowered masked;
  LowerSvg(Doc("<mask id=\"m\">" + Image("width=\"8\" height=\"8\"", Data("rgb-3x2.png")) + "</mask><rect width=\"8\" height=\"8\" mask=\"url(#m)\"/>"), masked);
  EXPECT_EQ(masked.Refusals(), (size_t) 0);
}

//-----------------------------------------------------------------------------
// The sweep of failing allocations
//-----------------------------------------------------------------------------

TEST(LowerImage, EveryAllocationOfAnImageLoweringMayFailAndNothingLeaks) {
  const std::string text = Doc("<defs>" + Image("id=\"i\" width=\"8\" height=\"8\"", Data("rgb-3x2.png")) + "</defs>" +
                               Image("x=\"9\" y=\"1\" preserveAspectRatio=\"xMinYMin slice\" width=\"8\" height=\"4\"", Data("red-blue-2x1.png")) +
                               "<use href=\"#i\" x=\"20\"/>" + Image("x=\"1\" y=\"20\" width=\"8\" height=\"8\"", "red-blue-2x1.png") +
                               Image("x=\"20\" y=\"20\" width=\"8\" height=\"8\"", "no-such-file.png") +
                               Image("width=\"8\" height=\"8\"", "data:image/svg+xml;utf8,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20viewBox%3D%220%200%204%204%22%3E%3Crect%20width%3D%224%22%20height%3D%224%22%2F%3E%3C%2Fsvg%3E"));
  size_t total = 0;
  for (long fail_at = 1; fail_at < 4000; fail_at++) {
    Failing alloc;
    alloc.fail_at = fail_at;
    std::vector<unsigned char> bytes = Bytes(text);
    GVEC_Stream * stream = nullptr;
    ASSERT_EQ(gvec_stream_create_memory(bytes.data(), bytes.size(), &stream), GVEC_OK);
    GVEC_Load_Options load;
    std::memset(&load, 0, sizeof load);
    load.allocator = &alloc.a;
    GVEC_Diagnostics diagnostics;
    gvec_diagnostics_init(&diagnostics, &alloc.a);
    GVEC_Doc * doc = nullptr;
    GVEC_Result r = gvec_doc_load(stream, &load, &diagnostics, &doc);
    gvec_stream_destroy(stream);
    GSHSCENE_Scene * scene = nullptr;
    GSHSCENE_List * list = nullptr;
    GVEC_Result lowered = GVEC_OK;
    if (r == GVEC_OK) {
      ASSERT_EQ(gshscene_scene_create(nullptr, nullptr, nullptr, &scene), GSHSCENE_OK);
      ASSERT_EQ(gshscene_list_create(nullptr, nullptr, gshscene_scene_store(scene), &list), GSHSCENE_OK);
      GVEC_Lower_Options options;
      std::memset(&options, 0, sizeof options);
      options.store = gshscene_scene_store(scene);
      options.image_root = kImages;
      options.image_base = kImages;
      lowered = gvec_doc_lower_list(doc, &options, list, &diagnostics);
      EXPECT_TRUE(lowered == GVEC_OK || lowered == GVEC_ERR_OOM) << fail_at << " " << gvec_result_string(lowered);
    }
    else {
      EXPECT_EQ(r, GVEC_ERR_OOM) << fail_at;
    }
    gshscene_list_destroy(list);
    gshscene_scene_destroy(scene);
    gvec_doc_free(doc);
    gvec_diagnostics_destroy(&diagnostics);
    EXPECT_EQ(alloc.outstanding, 0) << "a leak when allocation " << fail_at << " fails";
    total = (size_t) alloc.attempts;
    if (alloc.attempts < fail_at) {
      break;  // the sequence ended before the failure was reached: every allocation has been failed once
    }
  }
  EXPECT_GT(total, (size_t) 50);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
