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
 * @file lower.h
 *
 * Lowering a loaded document into the shape waist.
 *
 * A document is lowered into one of `shape-scene`'s two recording forms: an
 * edit of the retained scene (keyed, so that lowering an unchanged document
 * again changes nothing), or an immediate-mode list. Both entry points run the
 * same walk and emit the same drawing: the list a published edit emits equals
 * (gshscene_list_equal) the list the other entry point records. Neither
 * publishes, renders or touches a sink: the caller owns the edit or list and
 * decides what happens to it.
 *
 * What cannot be lowered is reported, never dropped: each refused element
 * gets one GVEC_DIAG_LOWER_REFUSED entry with its source location and a
 * GVEC_Lower_Reason in `detail`. An element is refused whole: when any part of
 * it cannot be drawn, none of it is, so the report and the drawing say the same
 * thing. documentation/refusals.md lists the reasons.
 *
 * ### What is drawn
 *
 * Structure (viewports, `transform`, `g`, `a`, `switch`, `use`, `symbol`,
 * `display`, `visibility`, `opacity`) and solid fills of `path`, `polygon`,
 * `polyline` (and `line`, which has no area). A transform composes into the
 * map each item carries; a group exists only for `opacity` below 1 over
 * painted content and for a clip. A `use` is an instance of a picture of its
 * referent drawn in the clone's context.
 *
 * ### Keys
 *
 * Every node the edit entry point makes is keyed in a domain of this library
 * from the element's key and the chain of `use` elements above it, so lowering
 * the same document again finds its nodes: it changes nothing that did not
 * change, and a node the document no longer has is removed. All the nodes are
 * under one group of the library's, which is placed under `parent`.
 *
 * ### The trace
 *
 * A GVEC_Lower_Trace records which element each item came from, so that a
 * report a sink gives about an item can be printed as `line 40, path: ...`.
 */

#ifndef GHOTI_IO_GVEC_LOWER_H
#define GHOTI_IO_GVEC_LOWER_H

#include <ghoti.io/vector/fonts.h>
#include <ghoti.io/vector/macros.h>

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/vector/allocator.h>
#include <ghoti.io/vector/codec.h>
#include <ghoti.io/vector/core.h>

#include <ghoti.io/shape-scene/allocator.h>
#include <ghoti.io/shape-scene/core.h>
#include <ghoti.io/shape-scene/paint.h>
#include <ghoti.io/shape-scene/report.h>
#include <ghoti.io/shape-scene/types.h>
#include <ghoti.io/shape-scene/version.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Why an element was not lowered (the `detail` of GVEC_DIAG_LOWER_REFUSED). The numbers are stable; a reason retires
 * from the list when the feature it names is drawn. Each is spelled by gvec_lower_reason_string() and described in
 * documentation/refusals.md.
 */
typedef enum GVEC_Lower_Reason {
  GVEC_LOWER_GRADIENT = 1,                 ///< A gradient paint.
  GVEC_LOWER_CLIP_PATH,                ///< `clip-path` (G4).
  GVEC_LOWER_MASK,                     ///< `mask` (G5).
  GVEC_LOWER_FILTER,                   ///< `filter` (G10).
  GVEC_LOWER_TEXT,                     ///< `text` (G11 and the text phase).
  GVEC_LOWER_CONTEXT_PAINT,            ///< `context-fill` or `context-stroke`.
  GVEC_LOWER_UNSUPPORTED_COLOR_SPACE,  ///< A colour the waist cannot carry (G9).
  GVEC_LOWER_FOREIGN_OBJECT,           ///< `foreignObject`.
  GVEC_LOWER_EXTERNAL_REFERENCE,       ///< A reference to something outside the document, which is never fetched.
  GVEC_LOWER_EXPANSION_LIMIT,          ///< A `use` that instantiates more elements than the limit allows.
  GVEC_LOWER_LIMIT,                    ///< A limit of the document, of the lowering or of `shape-scene` was reached.
  GVEC_LOWER_STYLE,                    ///< A property of the element has no usable value (reported where it was read).
  GVEC_LOWER_TRANSFORM_BOX,            ///< A `transform` whose percentages are of a bounding box.
  GVEC_LOWER_EMBEDDED_CONTENT,         ///< `audio`, `video`, `canvas` or `iframe`.
  GVEC_LOWER_VECTOR_EFFECT,            ///< A `vector-effect` the static rendering model does not have.
  GVEC_LOWER_STYLESHEET,               ///< A `style` element with rules in it: the library applies no style sheet yet.
  GVEC_LOWER_REASON_COUNT              ///< One past the last reason; not a reason.
} GVEC_Lower_Reason;

/** What was wrong with the data of an `image` or `feImage` that drew nothing (the `detail` of GVEC_DIAG_IMAGE_UNUSABLE). The numbers are stable. */
typedef enum GVEC_Image_Problem {
  GVEC_IMAGE_NO_DATA = 1,    ///< The `href` is missing or empty, or the `data:` URI's payload is empty or is not valid base64 or percent-encoding.
  GVEC_IMAGE_UNREADABLE,     ///< A file inside the image root that does not exist, is not a regular file, or could not be read.
  GVEC_IMAGE_UNDECODABLE,    ///< Bytes that no image decoder reads, or a damaged image.
  GVEC_IMAGE_NOT_AN_IMAGE    ///< The `href` names an element of the document (`#id`), which is not an image.
} GVEC_Image_Problem;

/**
 * What a resolver is asked for (GVEC_Resource_Request::kind). The numbers are stable.
 */
typedef enum GVEC_Resource_Kind {
  GVEC_RESOURCE_IMAGE = 1,  ///< The data of an `image` or an `feImage`: a raster, or an SVG document (the bytes decide).
  GVEC_RESOURCE_STYLE,      ///< A style sheet named by an `@import` of a `style` element or of a sheet already imported.
  GVEC_RESOURCE_DOCUMENT,   ///< An SVG document that an `href` of a `use`, or a `url()` of a `clip-path`, `mask`, `filter`, `fill`, `stroke` or `marker-*`, names an element of.
  GVEC_RESOURCE_COLOR_PROFILE, ///< An ICC profile named by the `src` of a `@color-profile` (the space of a `color(--name ...)`, or of `device-cmyk()`); bounded by `max_image_bytes`.
  GVEC_RESOURCE_FONT          ///< A font (TrueType, OpenType, WOFF or WOFF2) named by a `url()` of the `src` of an `@font-face` that is not a `data:` URI; bounded by `max_image_bytes`.
} GVEC_Resource_Kind;

/**
 * What a resolver is asked. Every string is valid for the call only; the resolver keeps nothing of it.
 *
 * A resolver is asked only for a reference the lowering could not answer from the document itself: never for a `data:`
 * URI, never for a reference to an element of the same document (`#id`), and never for a resource past a limit.
 */
typedef struct GVEC_Resource_Request {
  GVEC_Resource_Kind kind;
  /** The reference as the document wrote it (a `url()` or a string of a sheet, an `href`), with no `#fragment`. */
  const char * reference;
  /**
   * The reference resolved against `base` by the rules of RFC 3986 (section 5), still with no fragment: an absolute reference
   * is itself, a relative one is merged with the base and its `.` and `..` segments are removed. With no base (or a base that
   * is itself relative) a relative reference stays relative, and a `..` that has nothing above it is kept. It is only a
   * string: the library neither opens it nor checks that it names anything, and a resolver that maps it to a file, a
   * table, a cache or a network is the caller's own. A resolver that maps it to a path must check the path itself (a `..`
   * in it, a symbolic link, a scheme it does not serve): the library gives it no more than the string.
   */
  const char * url;
  /**
   * What `reference` was resolved against: the `base_uri` of the options for the document being lowered, the `url` of
   * the resource that named it for one that was loaded through a resolver. NULL when there is none.
   */
  const char * base;
  /** The most bytes the library will take for this resource: the limit of its kind and `max_resource_bytes`, whichever is smaller. */
  size_t max_bytes;
  /** How far down the resource is: 1 for a reference of the document being lowered, 2 for one of a resource that reference loaded, and so on. */
  size_t depth;
} GVEC_Resource_Request;

/** What a resolver answers with. */
typedef enum GVEC_Resource_Status {
  GVEC_RESOURCE_OK = 0,     ///< `reply` holds the resource.
  GVEC_RESOURCE_REFUSED,    ///< The caller's policy does not allow this reference. The element that named it is refused (GVEC_LOWER_EXTERNAL_REFERENCE).
  GVEC_RESOURCE_NOT_FOUND,  ///< There is nothing at the reference. An image draws nothing, a `use` draws nothing, a style sheet that cannot be had is not applied (and says so).
  GVEC_RESOURCE_FAILED,     ///< The resource could not be had (an I/O error, a timeout). Treated as NOT_FOUND, with a message that says the resolver failed.
  GVEC_RESOURCE_OOM         ///< The resolver ran out of memory: the lowering fails with GVEC_ERR_OOM.
} GVEC_Resource_Status;

/**
 * The resource of a GVEC_RESOURCE_OK answer. The library copies the bytes (and the media type) before the resolver returns and
 * keeps no pointer into the resolver's memory, so a static buffer, a memory map or a buffer reused by the next call is fine: the resolver owns what it points
 * to and the library never frees it. The library reads `size` bytes at `bytes` and nothing else.
 */
typedef struct GVEC_Resource_Reply {
  const void * bytes;       ///< The data; may be NULL only when `size` is zero.
  size_t size;              ///< Its length, at most `max_bytes` of the request; an answer longer than that is refused as past the limit without being read.
  const char * media_type;  ///< The media type, for example "image/svg+xml", NUL-terminated; NULL when the resolver does not know. A hint: the bytes decide (an SVG document begins as markup).
} GVEC_Resource_Reply;

/**
 * The resolver: the one door by which an `image`, a `style` and a `use` of this library may reach bytes that the document does not hold.
 * It is called synchronously from the lowering call, on the thread that made it, and never concurrently with another call of the same
 * lowering. It may not call back into this library with the same lowering's objects. A reference is asked of it at most once for each
 * (kind, url) in a lowering: the answer, a refusal and a failure included, is cached for the rest of the call.
 */
typedef GVEC_Resource_Status (*GVEC_Resource_Resolver)(void * user, const GVEC_Resource_Request * request, GVEC_Resource_Reply * reply);

/** The name of a reason, for example "gradient"; "unknown" for a value outside the enum. Static. */
GVEC_API const char * gvec_lower_reason_string(GVEC_Lower_Reason reason);

/** Which element, drawn how, each item of a lowering came from (see the file's description). Free with gvec_lower_trace_free(). */
typedef struct GVEC_Lower_Trace GVEC_Lower_Trace;

/** Make an empty trace; NULL `allocator` selects the default. */
GVEC_API GVEC_Result gvec_lower_trace_create(const GVEC_Allocator * allocator, GVEC_Lower_Trace ** out_trace);

/** Free a trace. Ignored when NULL. */
GVEC_API void gvec_lower_trace_free(GVEC_Lower_Trace * trace);

/** The element an item came from. */
typedef struct GVEC_Lower_Site {
  int placed;              ///< Nonzero when the item was placed; zero for one the trace cannot place (the rest is then zero).
  const char * element;    ///< The element's name as written; valid while the document lives.
  size_t line, column;     ///< Where the element starts in the source.
  uint64_t element_key;    ///< The element's key (AD-9).
  size_t use_depth;        ///< How many `use` elements the drawing was reached through.
} GVEC_Lower_Site;

/**
 * Place one entry of a sink's report: the element it is about.
 *
 * The trace answers for the last lowering that filled it. For a list lowering pass a NULL `version`: the entry's chain
 * is read as indexes into the list and the pictures of its instance items. For an edit lowering pass the version the
 * edit was published as (the one whose list was rendered): the entry is resolved to its node with
 * gshscene_report_resolve() and the node's key is looked up.
 *
 * @param report A report of a render of the lowered drawing.
 * @param index The entry.
 * @param out_site Receives the site; `placed` is zero when the trace cannot place the entry (a render-level entry,
 *   a chain that is not an item of the lowering). That is not an error.
 * @return GVEC_OK; GVEC_ERR_INVALID for a NULL argument or an entry out of range.
 */
GVEC_API GVEC_Result gvec_lower_trace_element(const GVEC_Lower_Trace * trace, const GSHSCENE_Version * version,
    const GSHSCENE_Report * report, size_t index, GVEC_Lower_Site * out_site);

/** The fields of GVEC_Lower_Options::flags. */
enum {
  GVEC_LOWER_AT_POSITION = 1u << 0,    ///< Edit: place the drawing at `position` rather than after the last child of `parent`.
  GVEC_LOWER_SET_FOREGROUND = 1u << 1, ///< Set the target's foreground to `foreground`.
  /**
   * The first and last vertex of a path that closes are oriented like those of an open path: an `orient="auto"` start marker
   * points along the first segment and an end marker along the last, where by default each is half way between the two
   * (SVG 2, 13.7.4, which does not say whether the closing segment is the segment before the start, and says nothing of a
   * reversal or a segment of no length). The renderers disagree: the browsers bisect, `resvg`, Batik and Inkscape do this.
   */
  GVEC_LOWER_MARKER_CLOSED_ENDS_OPEN = 1u << 2,
  /**
   * A gradient with no stops, named by a paint that has a fallback (`fill="url(#g) green"`), is drawn with the fallback. By default
   * it is `none` (SVG 2, 13.2.3, which says a gradient with no stops is as `none`, and uses the fallback only when the reference
   * cannot be resolved). The renderers disagree: the browsers paint nothing, `resvg` and Batik use the fallback.
   */
  GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK = 1u << 3,
  /**
   * Draw the document as it is at `animation_time_ms` of document time, its SMIL animations and CSS Animations applied (SVG 2's animated
   * modes, with no events and no script: design.md A1 to A12). Without it a lowering is static, as SVG 2's static modes require: no
   * animation is applied, not even one that begins at 0. An SVG document an `image` draws is at the same time; one a `use` or a paint
   * server of another document names is drawn static.
   */
  GVEC_LOWER_ANIMATED = 1u << 4
};

/** Options for a lowering. Zero-fill it, then set what you need. */
typedef struct GVEC_Lower_Options {
  uint32_t flags;                        ///< GVEC_LOWER_AT_POSITION, GVEC_LOWER_SET_FOREGROUND, GVEC_LOWER_MARKER_CLOSED_ENDS_OPEN, GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK.
  /**
   * The store the paths and pictures are interned in. A list lowering requires it and it must be the store the list
   * was made over (shape-scene lists do not say); an edit lowering ignores it and uses the edit's own.
   */
  GSHSCENE_Store * store;
  /** Edit: the group the drawing's group is placed under; 0 for the root. Only that group is touched outside the drawing. */
  uint64_t parent;
  size_t position;                       ///< Edit with GVEC_LOWER_AT_POSITION: the index in `parent`'s children, as gshscene_edit_add() takes it.
  /** With GVEC_LOWER_SET_FOREGROUND: what `currentColor` resolves to when the document leaves `color` at its initial value. */
  GSHSCENE_Color foreground;
  /**
   * The allocator the pictures a list lowering records in are made with: NULL for the default. Give the list's own.
   */
  const GSHSCENE_Allocator * scene_allocator;
  /**
   * The structural limits of the scene the drawing is for: NULL for the defaults. Give the limits the scene and the list were made
   * with. The lowering holds the drawing to them (the levels of nodes, the clips and groups open in one list, the pictures nested
   * in pictures, the items of a list, the expansion and the nodes of a version) in both forms, and refuses an element that does not
   * fit with GVEC_LOWER_LIMIT, so that publishing what it makes never fails for a structural limit of the scene.
   */
  const GSHSCENE_Limits * scene_limits;
  GVEC_Lower_Trace * trace;              ///< Receives the sites; NULL for none. Reset by the call.
  /**
   * The most marker instances one lowering draws, counted over the whole document, nested markers included: 0 for the default,
   * which is also the ceiling, half the nodes a version of the scene may hold (an instance is two nodes). An element whose markers
   * would pass it is refused with GVEC_LOWER_LIMIT, whole, and reported.
   */
  size_t max_marker_instances;
  /**
   * The fonts text is drawn with (fonts.h): NULL for none, and then an element with text is refused (GVEC_LOWER_TEXT). They must outlive the
   * scene the lowering makes.
   */
  const GVEC_Fonts * fonts;
  /**
   * Where an `image` or `feImage` may read an image file from (the resource policy): the directory every file must lie in, as a UTF-8
   * path. NULL for none, and then a reference to a file is refused with GVEC_LOWER_EXTERNAL_REFERENCE; a `data:` URI needs no root.
   * Only a relative reference is resolved, against `image_base`, and only to a regular file that stays inside this directory once `.`
   * and `..` are resolved and, where the platform can say (POSIX), once symbolic links are: a reference with a scheme (`http:`,
   * `https:`, `file:`), a network-path reference (`//host/path`), an absolute path and one that leaves the root are refused with
   * GVEC_LOWER_EXTERNAL_REFERENCE. Nothing is ever fetched from a network.
   * Both paths are relative to the working directory or both absolute; `\` is not a separator in a reference (it is refused).
   */
  const char * image_root;
  /**
   * The directory a relative image reference is resolved against, usually the directory of the document: NULL for `image_root`.
   * It must lie in `image_root`; one that does not refuses every reference to a file.
   */
  const char * image_base;
  /**
   * The resolver (see GVEC_Resource_Resolver): NULL for none, which is the default, and then a reference to something outside the document is
   * refused with GVEC_LOWER_EXTERNAL_REFERENCE, a `style` with an `@import` is not applied and a `use` of an element of another document is refused,
   * exactly as when `image_root` is NULL. The library never opens a socket or a file for a reference but through `image_root` (see below):
   * a URL is a string, and what it names is the resolver's to say.
   *
   * With a resolver, an `image` or `feImage` that names a file or a URL is answered by the resolver, not by `image_root`; `image_root` and
   * `image_base` remain only as a convenience that serves images from a directory when there is no resolver (a resolver that maps its
   * `url` into a directory is the same thing with the policy in the caller's hands). A `data:` URI is never asked of a resolver.
   * The three kinds are described at GVEC_Resource_Kind: an `image` that is an SVG document is lowered with its own references resolved against its own
   * url, a `style`'s `@import` (with `layer`; one with `media`, `supports` or `scope` is refused as a sheet that cannot be applied whole) enters the
   * cascade where it stands, and a `use` of `other.svg#id` is drawn from that document with that document's styles and ids and the inheritance of the `use`.
   */
  GVEC_Resource_Resolver resolver;
  void * resolver_user;                  ///< Passed to `resolver`.
  /**
   * The URI of the document being lowered, which relative references are resolved against: a path or an URL as a string, NULL for none.
   * It is only a string handed to the resolver as the `base` of a request; it is not opened.
   */
  const char * base_uri;
  /** The most bytes of one resource: 0 for the default (32 MiB, the ceiling is 1 GiB). The limit of its kind (`max_image_bytes`, `max_style_bytes`, `max_input_bytes`) applies as well. */
  size_t max_resource_bytes;
  /** The most resources a resolver is asked for in one lowering (distinct (kind, url) pairs, the refused and the failed ones counted): 0 for the default, 64; the ceiling is 4096. The one past it is refused with GVEC_LOWER_LIMIT without being asked. */
  size_t max_resources;
  /** The deepest nesting of documents and sheets that name one another (see GVEC_Resource_Request::depth): 0 for the default, 8; the ceiling is 64. A reference past it is refused with GVEC_LOWER_LIMIT. */
  size_t max_resource_depth;
  /**
   * The most `use` references into other documents one lowering instantiates (each `use` of `other.svg#id` is one, however many elements the referent has, and
   * however many times the document was asked of the resolver: it is asked once): 0 for the default, 1024; the ceiling is 1,048,576. The one past it is refused with
   * GVEC_LOWER_LIMIT. Each instantiation styles its elements afresh, as an instance of a `use` does within a document, so this bounds the work a set of documents that
   * name one another can make: with the depth and the cycle check, a hostile set costs at most this many instantiations, not their product.
   */
  size_t max_resource_instances;
  /** With GVEC_LOWER_ANIMATED: the document time, in milliseconds from the document's begin; finite and not negative (else GVEC_ERR_INVALID). */
  double animation_time_ms;
  uint8_t _reserved[8];                  ///< Zero.
} GVEC_Lower_Options;

/**
 * Lower a document into an edit of the retained scene.
 *
 * @param options NULL for the defaults.
 * @param edit The edit to fill; it is not published.
 * @param diagnostics Receives each refusal; may be NULL.
 * @return GVEC_OK when the document was walked, whatever was refused;
 *   GVEC_ERR_INVALID for a NULL document or edit, a reserved byte that is not zero or a parent that is not a group;
 *   GVEC_ERR_UNSUPPORTED when the document's codec cannot lower; GVEC_ERR_OOM. After GVEC_ERR_OOM the edit holds part of
 *   the drawing: destroy it.
 */
GVEC_API GVEC_Result gvec_doc_lower_edit(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_Edit * edit, GVEC_Diagnostics * diagnostics);

/**
 * As gvec_doc_lower_edit(), into an immediate-mode list, which is not closed. Needs `options->store`.
 * After GVEC_ERR_OOM the list holds part of the drawing, possibly with a group open: destroy it.
 */
GVEC_API GVEC_Result gvec_doc_lower_list(const GVEC_Doc * doc, const GVEC_Lower_Options * options,
    GSHSCENE_List * list, GVEC_Diagnostics * diagnostics);

/**
 * The size the document draws at, in the user units its drawing is lowered in: for an SVG, the used width and height of the
 * outermost `svg` (a missing or `auto` size is the load's viewport, a percentage is of it; a document with no viewBox that gives no
 * width, or no height, is as wide, or as high, as its drawing reaches right, or down, strokes included, when the drawing is only shapes and
 * groups of them). A renderer that wants the picture at
 * another size maps this one to it.
 *
 * @return GVEC_OK; GVEC_ERR_INVALID for a NULL argument; GVEC_ERR_UNSUPPORTED when the codec has no size;
 *   GVEC_ERR_OOM. A document whose size is zero or negative returns GVEC_OK with that size: it draws nothing.
 */
GVEC_API GVEC_Result gvec_doc_size(const GVEC_Doc * doc, double * width, double * height);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GVEC_LOWER_H
