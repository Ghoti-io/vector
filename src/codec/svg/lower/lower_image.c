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
 * @file lower_image.c
 *
 * The images of an `image` element and of an `feImage` (R1 to R11 of the Images section of design.md): the bytes an `href` names, their decoding by the
 * image library under the document's limits, the waist's image resource, and where the image sits in the element's user space.
 *
 * - The bytes come from a `data:` URI (base64 or percent-encoded, any media type: the bytes decide the format) or, under the
 *   caller's image root (lower_image_file.c), from a file. Nothing is fetched. A reference that the policy refuses is refused whole
 *   (`external-reference`); a missing, empty, damaged or unreadable image draws nothing and says so (`GVEC_DIAG_IMAGE_UNUSABLE`),
 *   as a browser draws nothing for a broken image.
 * - An image that is an SVG document is refused by name (`image`): lowering it would nest a second document, with its own
 *   style context and its own references, inside this one.
 * - The decoder is held to the document's limits before it allocates: pixels, the one allocation, and the bytes of every decoded
 *   image of the lowering together.
 * - EXIF orientation is applied, as browsers do (CSS Images 3, `image-orientation: from-image`, the initial value): by the image
 *   library's decode, which hands back the picture as it is displayed, so the raster interned is the oriented one and the intrinsic
 *   size is its size.
 * - The image is placed by the same arithmetic as a `viewBox` (`preserveAspectRatio`, lower_viewport.c), and drawn as the waist
 *   specifies a drawn image (IP14): a fill of the rectangle the paint's window maps to, with the image paint and `PAD`.
 */

#include <ghoti.io/vector/macros.h>

#include <stdint.h>
#include <string.h>

#include <ghoti.io/cutil/safemath.h>

#include <ghoti.io/image/image.h>
#include <ghoti.io/shape-scene/image.h>
#include <ghoti.io/shape-scene/image_paint.h>

#include "lower_internal.h"

//-----------------------------------------------------------------------------
// The notices
//-----------------------------------------------------------------------------

static GVEC_Result unusable(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, GVEC_Image_Problem problem, const char * message) {
  return gvec_diag_add(ctx->diagnostics, GVEC_DIAG_WARNING, GVEC_DIAG_IMAGE_UNUSABLE, (uint32_t) problem, e->node.line, e->node.column,
      e->node.offset, e->name, message);
}

//-----------------------------------------------------------------------------
// The cache: one answer for each element that names an image
//-----------------------------------------------------------------------------

typedef struct gvec_lw_image_entry {
  const GVEC_Svg_Element * element;
  gvec_lw_image_state state;
  gvec_lw_image image;
} gvec_lw_image_entry;

static const gvec_lw_image_entry * cache_find(const gvec_lw_ctx * ctx, const GVEC_Svg_Element * e) {
  const gvec_lw_image_entry * entries = ctx->images;
  for (size_t i = 0; i < ctx->image_count; i++) {
    if (entries[i].element == e) {
      return &entries[i];
    }
  }
  return NULL;
}

static GVEC_Result cache_add(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, gvec_lw_image_state state, const gvec_lw_image * image) {
  if (ctx->image_count == ctx->image_capacity) {
    size_t capacity = ctx->image_capacity ? ctx->image_capacity * 2 : 8, bytes;
    if (!gcu_safe_mul_size(capacity, sizeof(gvec_lw_image_entry), &bytes)) {
      return GVEC_ERR_OOM;
    }
    void * fresh = gvec_realloc(ctx->allocator, ctx->images, bytes);
    if (!fresh) {
      return GVEC_ERR_OOM;
    }
    ctx->images = fresh;
    ctx->image_capacity = capacity;
  }
  gvec_lw_image_entry * entries = ctx->images;
  gvec_lw_image_entry * entry = &entries[ctx->image_count++];
  memset(entry, 0, sizeof *entry);
  entry->element = e;
  entry->state = state;
  if (image) {
    entry->image = *image;
  }
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// The bytes
//-----------------------------------------------------------------------------

/* Whether the bytes (or the media type of a `data:` URI) are an SVG document: the media type names it, the content is markup or a
 * gzip stream (`.svgz`), or a UTF-16 byte order mark begins a markup document. No raster format begins that way. */
static bool is_svg_document(const unsigned char * b, size_t n, const char * media_type) {
  if (media_type && strcmp(media_type, "image/svg+xml") == 0) {
    return true;
  }
  if (n >= 2 && b[0] == 0x1f && b[1] == 0x8b) {
    return true;
  }
  if (n >= 2 && ((b[0] == 0xff && b[1] == 0xfe) || (b[0] == 0xfe && b[1] == 0xff))) {
    return true;
  }
  size_t i = 0;
  if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
    i = 3;
  }
  while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) {
    i++;
  }
  return i < n && b[i] == '<';
}

/* The attribute that names the image: `href`, else `xlink:href`. */
static const GVEC_Svg_Attribute * href_attribute(const GVEC_Svg_Element * e) {
  const GVEC_Svg_Attribute * a = gvec_svg_element_find_attribute(e, "href");
  return a ? a : gvec_svg_element_find_attribute(e, "xlink:href");
}

/* What the bytes of an image came to. */
typedef enum bytes_state {
  BYTES_READY,    /* `*bytes` is the image's bytes */
  BYTES_NOTHING,  /* nothing is drawn (reported) */
  BYTES_REFUSED   /* the element is refused (reported) */
} bytes_state;

static GVEC_Result image_bytes(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, unsigned char ** bytes, size_t * size, bool * svg,
    bytes_state * state) {
  const GVEC_Limits * limits = &ctx->document->limits;
  *bytes = NULL;
  *size = 0;
  *svg = false;
  *state = BYTES_NOTHING;
  const GVEC_Svg_Attribute * href = href_attribute(e);
  GVEC_Svg_Reference ref;
  if (!href || gvec_svg_attribute_class(href) != GVEC_SVG_ATTR_TYPED || !gvec_svg_attribute_reference(href, &ref) ||
      ref.kind == GVEC_SVG_REF_EMPTY) {
    return unusable(ctx, e, GVEC_IMAGE_NO_DATA, "the image names no data");
  }
  if (ref.kind == GVEC_SVG_REF_INTERNAL) {
    return unusable(ctx, e, GVEC_IMAGE_NOT_AN_IMAGE, "the image names an element of the document, which is not an image");
  }
  if (ref.kind == GVEC_SVG_REF_DATA) {
    size_t length = ref.data.payload_length;
    if (length == 0) {
      return unusable(ctx, e, GVEC_IMAGE_NO_DATA, "the data: URI of the image has no data");
    }
    /* The decoded size is at most this, whichever way the payload is encoded. */
    size_t bound = ref.data.base64 ? (length / 4 + 1) * 3 : length;
    size_t capacity = bound < limits->max_image_bytes ? bound : limits->max_image_bytes;
    unsigned char * buffer = gvec_realloc(ctx->allocator, NULL, capacity ? capacity : 1);
    if (!buffer) {
      return GVEC_ERR_OOM;
    }
    size_t written = 0;
    GVEC_Result r = gvec_svg_data_uri_decode(&ref.data, buffer, capacity, &written);
    if (r == GVEC_ERR_LIMIT) {
      gvec_free(ctx->allocator, buffer);
      *state = BYTES_REFUSED;
      return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image's data is larger than max_image_bytes allows");
    }
    if (r != GVEC_OK || written == 0) {
      gvec_free(ctx->allocator, buffer);
      return unusable(ctx, e, GVEC_IMAGE_NO_DATA, "the data: URI of the image is not valid base64 or percent-encoding");
    }
    *bytes = buffer;
    *size = written;
    *svg = is_svg_document(buffer, written, ref.data.media_type);
    *state = BYTES_READY;
    return GVEC_OK;
  }
  /* A reference to a file: the resource policy decides. */
  gvec_lw_image_path_status path_status;
  char * path = NULL;
  const GVEC_Lower_Options * options = ctx->options;
  GVEC_Result r = gvec_lw_image_resolve(ctx->allocator, options ? options->image_root : NULL, options ? options->image_base : NULL, ref.url,
      &path_status, &path);
  if (r != GVEC_OK) {
    return r;
  }
  const char * why = NULL;
  switch (path_status) {
  case GVEC_LW_IMAGE_PATH_OK:
    break;
  case GVEC_LW_IMAGE_PATH_NO_ROOT:
    why = "the image names a file, and the options give no image_root to read one from";
    break;
  case GVEC_LW_IMAGE_PATH_NETWORK:
    why = "the image names a URL with a scheme or a network path, which is never fetched";
    break;
  case GVEC_LW_IMAGE_PATH_ABSOLUTE:
    why = "the image names an absolute path, which is outside the image root";
    break;
  case GVEC_LW_IMAGE_PATH_OUTSIDE:
    why = "the image names a file outside the image root";
    break;
  case GVEC_LW_IMAGE_PATH_INVALID:
    why = "the image names a path with a character the resource policy does not follow";
    break;
  case GVEC_LW_IMAGE_PATH_EMPTY:
    return unusable(ctx, e, GVEC_IMAGE_NO_DATA, "the image names no file");
  }
  if (why) {
    *state = BYTES_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_EXTERNAL_REFERENCE, why);
  }
  gvec_lw_image_file_status file_status;
  void * contents = NULL;
  size_t contents_size = 0;
  r = gvec_lw_image_read_file(ctx->allocator, options->image_root, path, limits->max_image_bytes, &file_status, &contents, &contents_size);
  gvec_free(ctx->allocator, path);
  if (r != GVEC_OK) {
    return r;
  }
  switch (file_status) {
  case GVEC_LW_IMAGE_FILE_OK:
    break;
  case GVEC_LW_IMAGE_FILE_OUTSIDE:
    *state = BYTES_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_EXTERNAL_REFERENCE, "the image names a file that a link takes outside the image root");
  case GVEC_LW_IMAGE_FILE_TOO_BIG:
    *state = BYTES_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image's file is larger than max_image_bytes allows");
  case GVEC_LW_IMAGE_FILE_UNREADABLE:
    return unusable(ctx, e, GVEC_IMAGE_UNREADABLE, "the image's file does not exist, is not a regular file or cannot be read");
  }
  if (contents_size == 0) {
    gvec_free(ctx->allocator, contents);
    return unusable(ctx, e, GVEC_IMAGE_NO_DATA, "the image's file is empty");
  }
  *bytes = contents;
  *size = contents_size;
  *svg = is_svg_document(contents, contents_size, NULL);
  *state = BYTES_READY;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// Decoding
//-----------------------------------------------------------------------------

/* Whether the scene's image resource carries the raster's format as it is (IM2). */
static bool scene_format(const GIMG_Pixel_Format * f) {
  if (f->layout != GIMG_LAYOUT_INTERLEAVED) {
    return false;
  }
  size_t channels;
  switch (f->channel_model) {
  case GIMG_CHANNEL_GRAY:
    channels = 1;
    break;
  case GIMG_CHANNEL_RGB:
    channels = 3;
    break;
  case GIMG_CHANNEL_RGBA:
    channels = 4;
    break;
  default:
    return false;
  }
  if (f->channel_count != channels) {
    return false;
  }
  for (size_t i = 0; i < channels; i++) {
    if (f->bits_per_channel[i] != f->bits_per_channel[0]) {
      return false;
    }
  }
  if (f->channel_type == GIMG_CHANNEL_UNORM) {
    return f->bits_per_channel[0] == 8 || f->bits_per_channel[0] == 12 || f->bits_per_channel[0] == 16;
  }
  return f->channel_type == GIMG_CHANNEL_FLOAT && f->bits_per_channel[0] == 32;
}

/* Decode `bytes` and intern the image. `*state` says what came of it. */
static GVEC_Result decode_image(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const unsigned char * bytes, size_t size, gvec_lw_image * out,
    gvec_lw_image_state * state) {
  const GVEC_Limits * limits = &ctx->document->limits;
  *state = GVEC_LW_IMAGE_NOTHING;
  const uint64_t budget = (uint64_t) limits->max_image_decoded_bytes;
  if (ctx->image_decoded >= budget) {
    *state = GVEC_LW_IMAGE_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the images decoded so far use all of max_image_decoded_bytes");
  }
  const uint64_t remaining = budget - ctx->image_decoded;
  GIMG_Limits image_limits;
  memset(&image_limits, 0, sizeof image_limits);
  image_limits.max_decoded_pixels = limits->max_image_pixels;
  /* The decoder's cap is on its largest single allocation, which is the raster; its working buffers are small beside it, so the cap is
   * never below a floor that holds them, and the raster is held to the budget exactly after it is decoded (never allocating more than the
   * budget or the floor, whichever is larger). */
  const uint64_t floor_bytes = UINT64_C(1) << 20;
  const uint64_t cap = remaining > floor_bytes ? remaining : floor_bytes;
  image_limits.max_memory = cap > (uint64_t) SIZE_MAX ? SIZE_MAX : (size_t) cap;
  GIMG_Load_Options load;
  memset(&load, 0, sizeof load);
  load.limits = &image_limits;
  load.strictness = GIMG_NORMAL;
  GIMG_Decode_Options decode;
  memset(&decode, 0, sizeof decode);
  decode.limits = &image_limits;

  GIMG_Stream * stream = NULL;
  GIMG_Diagnostics diag;
  memset(&diag, 0, sizeof diag);
  gimg_diagnostics_init(&diag, NULL);
  GIMG_Doc * doc = NULL;
  GIMG_Raster * raster = NULL;
  GIMG_Raster * converted = NULL;
  GVEC_Result result = GVEC_OK;
  GIMG_Result g = gimg_stream_create_memory(bytes, size, &stream);
  if (g != GIMG_OK) {
    result = g == GIMG_ERR_OOM ? GVEC_ERR_OOM : GVEC_ERR_INTERNAL;
    goto done;
  }
  g = gimg_doc_load(stream, &load, &diag, &doc);
  if (g == GIMG_OK && gimg_doc_item_count(doc) < 1) {
    g = GIMG_ERR_FORMAT;
  }
  if (g == GIMG_OK) {
    g = gimg_item_decode(gimg_doc_item(doc, 0), &decode, &raster);
  }
  if (g == GIMG_ERR_OOM) {
    result = GVEC_ERR_OOM;
    goto done;
  }
  if (g == GIMG_ERR_LIMIT) {
    *state = GVEC_LW_IMAGE_REFUSED;
    result = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image is larger than max_image_pixels or max_image_decoded_bytes allows");
    goto done;
  }
  if (g != GIMG_OK || !raster) {
    result = unusable(ctx, e, GVEC_IMAGE_UNDECODABLE, "the image's bytes are not an image the decoders read");
    goto done;
  }
  const GIMG_Pixel_Format * format = gimg_raster_format(raster);
  if (!scene_format(format)) {
    /* An indexed or four-ink image, or a layout the waist does not carry: as RGBA at the depth of the source. */
    const GIMG_Pixel_Format * target = format->bits_per_channel[0] > 8 ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_RGBA8;
    g = gimg_ops_convert_pixel_format(raster, target, &converted);
    if (g == GIMG_ERR_OOM) {
      result = GVEC_ERR_OOM;
      goto done;
    }
    if (g == GIMG_ERR_LIMIT) {
      *state = GVEC_LW_IMAGE_REFUSED;
      result = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image is larger than max_image_decoded_bytes allows");
      goto done;
    }
    if (g != GIMG_OK || !converted) {
      result = unusable(ctx, e, GVEC_IMAGE_UNDECODABLE, "the image's pixel format is not one the lowering converts");
      goto done;
    }
    gimg_raster_destroy(raster);
    raster = converted;
    converted = NULL;
    format = gimg_raster_format(raster);
  }
  const uint32_t width = gimg_raster_width(raster), height = gimg_raster_height(raster);
  const uint64_t decoded = (uint64_t) width * (uint64_t) height * (uint64_t) gimg_raster_bytes_per_pixel(format);
  if (width == 0 || height == 0 || (uint64_t) width * (uint64_t) height > (uint64_t) limits->max_image_pixels || decoded > remaining) {
    *state = GVEC_LW_IMAGE_REFUSED;
    result = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image is larger than max_image_pixels or max_image_decoded_bytes allows");
    goto done;
  }
  GSHSCENE_Image_Params params;
  memset(&params, 0, sizeof params);
  GSHSCENE_Color_Space space;
  if (gshscene_image_space_from_raster(raster, &space) == GSHSCENE_OK) {
    params.space = space; /* a profile, or no statement, is the target's space: the zero value */
  }
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result sr = gshscene_store_intern_image_take(ctx->store, raster, &params, &resource);
  if (sr == GSHSCENE_OK) {
    raster = NULL; /* the store has it */
  }
  else if (sr == GSHSCENE_ERR_OOM) {
    result = GVEC_ERR_OOM;
    goto done;
  }
  else if (sr == GSHSCENE_ERR_LIMIT) {
    *state = GVEC_LW_IMAGE_REFUSED;
    result = gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image is past what the waist's image resource carries");
    goto done;
  }
  else {
    result = unusable(ctx, e, GVEC_IMAGE_UNDECODABLE, "the image's samples are not ones the waist's image resource carries");
    goto done;
  }
  result = gvec_lw_hold(ctx, resource);
  if (result != GVEC_OK) {
    goto done;
  }
  ctx->image_decoded += decoded;
  memset(out, 0, sizeof *out);
  out->resource = resource;
  /* The decoder has applied the Exif orientation: the raster is the picture as it is displayed. A side is at most the pixel limit's 2^28. */
  out->width = (double) (int32_t) width;
  out->height = (double) (int32_t) height;
  *state = GVEC_LW_IMAGE_READY;
done:
  gimg_raster_destroy(converted);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_diagnostics_destroy(&diag);
  gimg_stream_destroy(stream);
  return result;
}

GVEC_Result gvec_lw_image_load(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, gvec_lw_image_state * state, gvec_lw_image * out) {
  const gvec_lw_image_entry * known = cache_find(ctx, e);
  if (known) {
    *state = known->state;
    *out = known->image;
    return GVEC_OK;
  }
  unsigned char * bytes = NULL;
  size_t size = 0;
  bool svg = false;
  bytes_state bs;
  gvec_lw_image result;
  memset(&result, 0, sizeof result);
  *state = GVEC_LW_IMAGE_NOTHING;
  GVEC_Result r = image_bytes(ctx, e, &bytes, &size, &svg, &bs);
  if (r == GVEC_OK && bs == BYTES_READY) {
    if (svg) {
      /* A second document inside this one would need its own style context, ids and references; it is refused by name. */
      *state = GVEC_LW_IMAGE_REFUSED;
      r = gvec_lw_refuse(ctx, e, GVEC_LOWER_IMAGE, "an image that is an SVG document is not lowered: a document is not nested inside another");
    }
    else {
      r = decode_image(ctx, e, bytes, size, &result, state);
    }
  }
  else if (bs == BYTES_REFUSED) {
    *state = GVEC_LW_IMAGE_REFUSED;
  }
  gvec_free(ctx->allocator, bytes);
  if (r != GVEC_OK) {
    return r;
  }
  r = cache_add(ctx, e, *state, &result);
  *out = result;
  return r;
}

void gvec_lw_image_cache_free(gvec_lw_ctx * ctx) {
  gvec_free(ctx->allocator, ctx->images);
  ctx->images = NULL;
  ctx->image_count = ctx->image_capacity = 0;
}

//-----------------------------------------------------------------------------
// Placement and the paint
//-----------------------------------------------------------------------------

void gvec_lw_image_auto_size(const gvec_lw_image * image, bool has_width, double * width, bool has_height, double * height) {
  const double iw = image->width, ih = image->height;
  if (!has_width && !has_height) {
    *width = iw;
    *height = ih;
  }
  else if (!has_width) {
    *width = *height * iw / ih;
  }
  else if (!has_height) {
    *height = *width * ih / iw;
  }
}

bool gvec_lw_image_place(const gvec_lw_image * image, const GVEC_Svg_Preserve_Aspect_Ratio * par, double x, double y, double width, double height,
    GSHGEOM_Map * out) {
  GVEC_Svg_View_Box box = {0, 0, image->width, image->height};
  GSHGEOM_Map view, shift;
  if (!gvec_lw_viewbox_map(&box, par, width, height, &view) || gshgeom_map_translate(2, x, y, 0, &shift) != GSHGEOM_OK) {
    return false;
  }
  return gvec_lw_compose(&shift, &view, out);
}

GVEC_Result gvec_lw_image_filters(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, uint32_t * magnify, uint32_t * minify, bool * usable) {
  GVEC_Svg_Style_Value v;
  *magnify = GSHSCENE_IMAGE_FILTER_CUBIC;
  *minify = GSHSCENE_IMAGE_FILTER_AREA;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, GVEC_SVG_PROP_IMAGE_RENDERING, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  if (gvec_lw_keyword_is(&v, "optimizespeed") || gvec_lw_keyword_is(&v, "crisp-edges") || gvec_lw_keyword_is(&v, "pixelated")) {
    *magnify = *minify = GSHSCENE_IMAGE_FILTER_NEAREST;
  }
  else if (gvec_lw_keyword_is(&v, "smooth")) {
    *magnify = GSHSCENE_IMAGE_FILTER_LINEAR;
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_image_paint(gvec_lw_ctx * ctx, const GVEC_Svg_Element * e, const gvec_lw_image * image, const GSHGEOM_Map * map,
    uint32_t magnify, uint32_t minify, GSHSCENE_Paint * paint, const GSHSCENE_Resource ** path, bool * ok) {
  *ok = false;
  GSHSCENE_Image_Paint ip;
  gshscene_image_paint_init(&ip);
  ip.image = image->resource;
  ip.map = *map;
  ip.extend_x = GSHSCENE_IMAGE_EXTEND_PAD;
  ip.extend_y = GSHSCENE_IMAGE_EXTEND_PAD;
  ip.magnify = magnify;
  ip.minify = minify;
  const GSHSCENE_Resource * resource = NULL;
  GSHSCENE_Result sr = gshscene_store_intern_image_paint(ctx->store, &ip, &resource);
  if (sr == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (sr != GSHSCENE_OK) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image is placed past the range the waist can carry");
  }
  GVEC_Result r = gvec_lw_hold(ctx, resource);
  if (r != GVEC_OK) {
    return r;
  }
  const GSHSCENE_Resource * rect = NULL;
  sr = gshscene_image_paint_rect_path(ctx->store, resource, &rect);
  if (sr == GSHSCENE_ERR_OOM) {
    return GVEC_ERR_OOM;
  }
  if (sr != GSHSCENE_OK) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image is placed past the range the waist can carry");
  }
  r = gvec_lw_hold(ctx, rect);
  if (r != GVEC_OK) {
    return r;
  }
  memset(paint, 0, sizeof *paint);
  paint->kind = GSHSCENE_PAINT_IMAGE;
  paint->rgba[3] = 1.0;
  paint->resource = resource;
  *path = rect;
  *ok = true;
  return GVEC_OK;
}

//-----------------------------------------------------------------------------
// The element
//-----------------------------------------------------------------------------

/* A geometry property of the element as a length in user units; `given` is false for `auto` (and for what is not a length). */
static GVEC_Result geometry_property(gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, GVEC_Svg_Property property, double * out, bool * given,
    bool * usable) {
  GVEC_Svg_Style_Value v;
  *out = 0;
  *given = false;
  GVEC_Result r = gvec_lw_read_property(ctx, inst, property, GVEC_LOWER_STYLE, &v, usable);
  if (r != GVEC_OK || !*usable) {
    return r;
  }
  if (v.kind == GVEC_SVG_STYLE_LENGTH && !v.length_auto) {
    *out = v.number;
    *given = true;
  }
  return GVEC_OK;
}

static bool finite_double(double v) {
  return v - v == 0;
}

GVEC_Result gvec_lw_image_geometry_of(
    gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, gvec_lw_image_state * state, gvec_lw_image_geometry * out) {
  const GVEC_Svg_Element * e = gvec_svg_instance_element(inst);
  double x, y, w, h;
  bool gx, gy, gw, gh, usable;
  *state = GVEC_LW_IMAGE_REFUSED;
  memset(out, 0, sizeof *out);
  GVEC_Result r = geometry_property(ctx, inst, GVEC_SVG_PROP_X, &x, &gx, &usable);
  if (r == GVEC_OK && usable) {
    r = geometry_property(ctx, inst, GVEC_SVG_PROP_Y, &y, &gy, &usable);
  }
  if (r == GVEC_OK && usable) {
    r = geometry_property(ctx, inst, GVEC_SVG_PROP_WIDTH, &w, &gw, &usable);
  }
  if (r == GVEC_OK && usable) {
    r = geometry_property(ctx, inst, GVEC_SVG_PROP_HEIGHT, &h, &gh, &usable);
  }
  if (r != GVEC_OK || !usable) {
    return r;
  }
  if (!finite_double(x) || !finite_double(y) || !finite_double(w) || !finite_double(h)) {
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image's geometry is past the range the waist can carry");
  }
  *state = GVEC_LW_IMAGE_NOTHING;
  if ((gw && !(w > 0)) || (gh && !(h > 0))) {
    /* A width or a height of zero, or a negative one (an error), disables the rendering of the element (SVG 2, 5.3). */
    out->disabled = true;
    return GVEC_OK;
  }
  r = gvec_lw_image_load(ctx, e, state, &out->image);
  if (r != GVEC_OK || *state != GVEC_LW_IMAGE_READY) {
    return r;
  }
  gvec_lw_image_auto_size(&out->image, gw, &w, gh, &h);
  if (!finite_double(w) || !finite_double(h) || !(w > 0) || !(h > 0)) {
    *state = GVEC_LW_IMAGE_REFUSED;
    return gvec_lw_refuse(ctx, e, GVEC_LOWER_LIMIT, "the image's size is past the range the waist can carry");
  }
  out->x = x;
  out->y = y;
  out->width = w;
  out->height = h;
  out->par.align = GVEC_SVG_ALIGN_X_MID_Y_MID;
  const GVEC_Svg_Attribute * p = gvec_svg_element_find_attribute(e, "preserveAspectRatio");
  if (p && gvec_svg_attribute_class(p) == GVEC_SVG_ATTR_TYPED) {
    gvec_svg_attribute_preserve_aspect_ratio(p, &out->par);
  }
  return GVEC_OK;
}

GVEC_Result gvec_lw_image_leaf_box(
    gvec_lw_ctx * ctx, const GVEC_Svg_Instance * inst, const GSHGEOM_Map * map, GSHGEOM_Bounds * box, bool * ok) {
  gvec_lw_image_state state;
  gvec_lw_image_geometry g;
  *ok = false;
  GVEC_Result r = gvec_lw_image_geometry_of(ctx, inst, &state, &g);
  if (r != GVEC_OK) {
    return r;
  }
  if (state == GVEC_LW_IMAGE_REFUSED) {
    return GVEC_OK;
  }
  *ok = true;
  if (state == GVEC_LW_IMAGE_NOTHING) {
    return GVEC_OK; /* an image that draws nothing has no box */
  }
  GSHGEOM_Bounds mapped;
  memset(&mapped, 0, sizeof mapped);
  mapped.empty = 1;
  for (int c = 0; c < 4; c++) {
    GSHGEOM_Point in, at;
    memset(&in, 0, sizeof in);
    in.x = (c & 1) ? g.x + g.width : g.x;
    in.y = (c & 2) ? g.y + g.height : g.y;
    if (gshgeom_map_apply_point(map, in, &at) != GSHGEOM_OK) {
      *ok = false;
      return GVEC_OK;
    }
    if (mapped.empty) {
      mapped.empty = 0;
      mapped.min = mapped.max = at;
    }
    else {
      mapped.min.x = at.x < mapped.min.x ? at.x : mapped.min.x;
      mapped.min.y = at.y < mapped.min.y ? at.y : mapped.min.y;
      mapped.max.x = at.x > mapped.max.x ? at.x : mapped.max.x;
      mapped.max.y = at.y > mapped.max.y ? at.y : mapped.max.y;
    }
  }
  if (box->empty) {
    *box = mapped;
  }
  else {
    box->min.x = mapped.min.x < box->min.x ? mapped.min.x : box->min.x;
    box->min.y = mapped.min.y < box->min.y ? mapped.min.y : box->min.y;
    box->max.x = mapped.max.x > box->max.x ? mapped.max.x : box->max.x;
    box->max.y = mapped.max.y > box->max.y ? mapped.max.y : box->max.y;
  }
  return GVEC_OK;
}
