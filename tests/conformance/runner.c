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
 * The conformance runner: scores each case of a list through the final path,
 * and prints one line per case.
 *
 *     runner --corpus DIR --list FILE --max-diff N --max-bad-ppm N [--stub NAME] [--timeout SECONDS] [--dump DIR]
 *     runner --compare A.png B.png
 *
 * A case is DIR/tests/<case>, an SVG file, with its reference at the same path
 * with .png for its extension. Each case runs in a child process under a time
 * and an address-space limit, so a hang or a crash is that case's ERROR and the
 * run goes on. The line is
 *
 *     <case> TAB <OUTCOME> TAB <detail>
 *
 * where OUTCOME is PASS, FAIL, REFUSED, NOT_LOADED or ERROR. For a rendered case the
 * detail is "maxdiff=M bad=B/T white-over=W": the strict figures (the largest per-channel
 * difference over straight RGBA, the pixels off by more than one level, of T), and W, the
 * pixels that differ by more than --max-diff once both rasters are composited on white. EXCLUDED is the
 * scorer's: an excluded case is never given to the runner.
 *
 * The final path is only the public API: gvec_doc_load on a file stream,
 * gvec_doc_lower_list into a list, shape-cpu to render it, image to decode the
 * reference. The document is loaded with the reference's size as its viewport (the
 * size a browser gives it). A lowering that reports any refusal is REFUSED and is not
 * rendered, so a drawing that left something out can never pass; a sink that
 * reports an item it refused or approximated makes the case REFUSED too, with the
 * report entry mapped back to the element it came from. The comparison
 * is on straight 8-bit RGBA; two pixels that are both fully transparent are
 * equal whatever their colour channels hold. The runner reads no environment
 * variable and no clock, so its output is a function of the case's bytes and
 * the arguments.
 *
 * The picture is the document at the size the reference was rendered at: the document's own size (gvec_doc_size) is mapped to the
 * reference's. --dump DIR also writes each rendered raster, as a PAM file, to look at a case that failed.
 *
 * --compare prints the two numbers of the comparison for two PNG files:
 * the largest per-channel difference and the count of pixels differing by more
 * than 1/255 in any channel.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ghoti.io/image/image.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-scene/shape-scene.h>
#include <ghoti.io/shape-scene/sink.h>
#include <ghoti.io/vector/vector.h>

#include "stubs.h"

enum {
  MAX_DIM = 4096,                         ///< A reference larger than this in either side is ERROR (size).
  MAX_REFERENCE_BYTES = 64 * 1024 * 1024, ///< A reference file larger than this is ERROR (size).
  DEFAULT_TIMEOUT_S = 60
};

/** What one case came to; the detail is a short line without tabs or newlines. */
typedef struct Verdict {
  const char * outcome;
  char detail[400];
} Verdict;

static void say(Verdict * v, const char * outcome, const char * format, ...) __attribute__((format(printf, 3, 4)));

#include <stdarg.h>

static void say(Verdict * v, const char * outcome, const char * format, ...) {
  v->outcome = outcome;
  va_list args;
  va_start(args, format);
  vsnprintf(v->detail, sizeof v->detail, format, args);
  va_end(args);
}

static uint8_t * read_file(const char * path, size_t * size) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 1 << 16, used = 0;
  uint8_t * data = malloc(cap);
  while (data) {
    size_t got = fread(data + used, 1, cap - used, f);
    used += got;
    if (used < cap) {
      break;
    }
    if (cap >= MAX_REFERENCE_BYTES) {
      free(data);
      data = NULL;
      break;
    }
    cap *= 2;
    uint8_t * grown = realloc(data, cap);
    if (!grown) {
      free(data);
      data = NULL;
      break;
    }
    data = grown;
  }
  int bad = ferror(f);
  fclose(f);
  if (bad && data) {
    free(data);
    data = NULL;
  }
  *size = used;
  return data;
}

static int is_straight_rgba8(const GIMG_Pixel_Format * f) {
  return f->channel_model == GIMG_CHANNEL_RGBA && f->channel_count == 4 && f->bits_per_channel[0] == 8 &&
      f->bits_per_channel[1] == 8 && f->bits_per_channel[2] == 8 && f->bits_per_channel[3] == 8 &&
      f->alpha_form == GIMG_ALPHA_STRAIGHT;
}

/** Decode a PNG file to a straight RGBA8 raster; NULL with a reason in `why` on failure. */
static GIMG_Raster * load_reference(const char * path, const char ** why) {
  size_t size = 0;
  uint8_t * bytes = read_file(path, &size);
  if (!bytes) {
    *why = "reference unreadable or over the size cap";
    return NULL;
  }
  GIMG_Stream * stream = NULL;
  if (gimg_stream_create_memory(bytes, size, &stream) != GIMG_OK) {
    free(bytes);
    *why = "reference stream";
    return NULL;
  }
  GIMG_Diagnostics diag;
  memset(&diag, 0, sizeof diag);
  gimg_diagnostics_init(&diag, NULL);
  GIMG_Doc * doc = NULL;
  GIMG_Raster * out = NULL;
  if (gimg_doc_load(stream, NULL, &diag, &doc) != GIMG_OK || gimg_doc_item_count(doc) < 1) {
    *why = "reference does not decode";
  } else {
    GIMG_Raster * raster = NULL;
    if (gimg_item_decode(gimg_doc_item(doc, 0), NULL, &raster) != GIMG_OK || !raster) {
      *why = "reference does not decode";
    } else if (gimg_raster_width(raster) > MAX_DIM || gimg_raster_height(raster) > MAX_DIM) {
      *why = "reference larger than the size cap";
    } else if (is_straight_rgba8(gimg_raster_format(raster))) {
      out = raster;
      raster = NULL;
    } else if (gimg_ops_convert_pixel_format(raster, &GIMG_PIXEL_RGBA8, &out) != GIMG_OK) {
      *why = "reference pixel format has no conversion to straight RGBA8";
      out = NULL;
    }
    gimg_raster_destroy(raster);
  }
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_diagnostics_destroy(&diag);
  gimg_stream_destroy(stream);
  free(bytes);
  return out;
}

typedef struct Diff {
  unsigned max_diff;
  uint64_t bad;
  uint64_t total;
  /** Pixels that differ by more than the tolerance's level once both rasters are composited on white. */
  uint64_t white_over;
} Diff;

/** A straight channel value composited on white, in 1/255 levels (rounded). */
static unsigned on_white(unsigned c, unsigned a) {
  return (c * a + 255u * (255u - a) + 127u) / 255u;
}

/** The comparison: both rasters are straight RGBA8 of the same size. */
static Diff compare_rasters(const GIMG_Raster * a, const GIMG_Raster * b, unsigned tolerance) {
  Diff d = {0, 0, 0, 0};
  uint32_t w = gimg_raster_width(a), h = gimg_raster_height(a);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * pa = (const uint8_t *) gimg_raster_pixels_const(a) + y * gimg_raster_stride_bytes(a);
    const uint8_t * pb = (const uint8_t *) gimg_raster_pixels_const(b) + y * gimg_raster_stride_bytes(b);
    for (uint32_t x = 0; x < w; x++, pa += 4, pb += 4) {
      d.total++;
      if (pa[3] == 0 && pb[3] == 0) {
        continue;
      }
      unsigned white = 0;
      for (int c = 0; c < 3; c++) {
        unsigned wa = on_white(pa[c], pa[3]), wb = on_white(pb[c], pb[3]);
        unsigned diff = wa > wb ? wa - wb : wb - wa;
        if (diff > white) {
          white = diff;
        }
      }
      if (white > tolerance) {
        d.white_over++;
      }
      unsigned worst = 0;
      for (int c = 0; c < 4; c++) {
        unsigned diff = pa[c] > pb[c] ? (unsigned) (pa[c] - pb[c]) : (unsigned) (pb[c] - pa[c]);
        if (diff > worst) {
          worst = diff;
        }
      }
      if (worst > d.max_diff) {
        d.max_diff = worst;
      }
      if (worst > 1) {
        d.bad++;
      }
    }
  }
  return d;
}

/** Write the rendered raster as DIR/<case with / as _>.pam, for looking at a drawing that failed. */
static void dump_raster(const char * dir, const char * name, const GIMG_Raster * raster) {
  char path[4096];
  int n = snprintf(path, sizeof path, "%s/", dir);
  for (const char * c = name; *c && n < (int) sizeof path - 5; c++) {
    path[n++] = *c == '/' ? '_' : *c;
  }
  snprintf(path + n, sizeof path - (size_t) n, ".pam");
  FILE * f = fopen(path, "wb");
  if (!f) {
    return;
  }
  uint32_t w = gimg_raster_width(raster), h = gimg_raster_height(raster);
  fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
  for (uint32_t y = 0; y < h; y++) {
    fwrite((const uint8_t *) gimg_raster_pixels_const(raster) + y * gimg_raster_stride_bytes(raster), 4, w, f);
  }
  fclose(f);
}

typedef struct Options {
  const char * corpus;
  const char * list;
  unsigned max_diff;
  uint64_t max_bad_ppm;
  Conf_Lower_Fn lower;
  int timeout_s;
  const char * dump;
  GVEC_Fonts * fonts; ///< The corpus's fonts, under the generic families its references use.
  const char * remote_dir;   ///< Where the pinned copies of the remote files are (each named by its hash), or NULL.
  char ** remote_url;        ///< The remote table: the URLs a case names and the file that serves each, read from --remote-table.
  char ** remote_file;
  size_t remote_count;
} Options;

/**
 * The resolver the runner gives the library (the library opens nothing itself): a reference that is a URL of the remote table is served from its pinned
 * copy (score.py checked its hash before the run), a relative one from the corpus (the case's `resources` and its neighbours), and anything else is refused.
 * A relative reference that climbs out of the corpus, names an absolute path, or holds an escape, a backslash or a control character is refused.
 */
static GVEC_Resource_Status serve_file(const char * path, const GVEC_Resource_Request * request, GVEC_Resource_Reply * reply, unsigned char ** hold) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    return GVEC_RESOURCE_NOT_FOUND;
  }
  size_t capacity = 4096, used = 0;
  unsigned char * buffer = malloc(capacity);
  if (!buffer) {
    fclose(f);
    return GVEC_RESOURCE_OOM;
  }
  for (;;) {
    if (used == capacity) {
      unsigned char * fresh = realloc(buffer, capacity * 2);
      if (!fresh) {
        free(buffer);
        fclose(f);
        return GVEC_RESOURCE_OOM;
      }
      buffer = fresh;
      capacity *= 2;
    }
    size_t got = fread(buffer + used, 1, capacity - used, f);
    used += got;
    if (got == 0) {
      break;
    }
    if (used > request->max_bytes) {
      break;
    }
  }
  fclose(f);
  free(*hold);
  *hold = buffer;
  reply->bytes = buffer;
  reply->size = used;
  size_t n = strlen(path);
  reply->media_type = n > 4 && strcmp(path + n - 4, ".svg") == 0 ? "image/svg+xml" : NULL;
  return GVEC_RESOURCE_OK;
}

typedef struct Serve {
  const Options * o;
  unsigned char * hold; ///< The last file served: the library copies it before the next request.
} Serve;

static GVEC_Resource_Status resolver(void * user, const GVEC_Resource_Request * request, GVEC_Resource_Reply * reply) {
  Serve * serve = user;
  const Options * o = serve->o;
  const char * url = request->url;
  char path[4096];
  for (size_t i = 0; i < o->remote_count; i++) {
    if (strcmp(url, o->remote_url[i]) == 0) {
      if (!o->remote_dir) {
        return GVEC_RESOURCE_NOT_FOUND;
      }
      snprintf(path, sizeof path, "%s/%s", o->remote_dir, o->remote_file[i]);
      return serve_file(path, request, reply, &serve->hold);
    }
  }
  size_t n = strlen(url);
  if (n == 0 || url[0] == '/' || strstr(url, "..") || strchr(url, '\\') || strchr(url, '%') || strchr(url, ':') || strchr(url, '?')) {
    return GVEC_RESOURCE_REFUSED;
  }
  for (size_t i = 0; i < n; i++) {
    if ((unsigned char) url[i] < 0x20) {
      return GVEC_RESOURCE_REFUSED;
    }
  }
  snprintf(path, sizeof path, "%s/%s", o->corpus, url);
  return serve_file(path, request, reply, &serve->hold);
}

static const char * diag_first_refusal(const GVEC_Diagnostics * d, char * buf, size_t n) {
  for (size_t i = 0; i < d->count; i++) {
    if (d->items[i].code == GVEC_DIAG_LOWER_REFUSED) {
      snprintf(buf, n, "%s:%zu", d->items[i].element ? d->items[i].element : "?", d->items[i].line);
      return buf;
    }
  }
  return "";
}

/** Run one case in this process. */
static void run_case(const Options * o, const char * name, Verdict * v) {
  char svg[4096], png[4096];
  int n = snprintf(svg, sizeof svg, "%s/tests/%s", o->corpus, name);
  size_t len = strlen(name);
  if (n < 0 || (size_t) n >= sizeof svg || len < 5 || strcmp(name + len - 4, ".svg") != 0) {
    say(v, "ERROR", "bad case name");
    return;
  }
  snprintf(png, sizeof png, "%s/tests/%.*s.png", o->corpus, (int) (len - 4), name);

  const char * why = "";
  GIMG_Raster * reference = load_reference(png, &why);
  if (!reference) {
    say(v, "ERROR", "%s", why);
    return;
  }

  GVEC_Stream * stream = NULL;
  GVEC_Diagnostics diag;
  gvec_diagnostics_init(&diag, NULL);
  GVEC_Doc * doc = NULL;
  GSHSCENE_Store * store = NULL;
  GSHSCENE_List * list = NULL;
  GSHSCENE_Sink * sink = NULL;
  GSHSCENE_Report * report = NULL;
  GIMG_Raster * raster = NULL;
  GVEC_Lower_Trace * trace = NULL;
  Serve serve = {o, NULL};

  if (gvec_stream_create_file(svg, &stream) != GVEC_OK) {
    say(v, "ERROR", "case file unreadable");
    goto done;
  }
  GVEC_Load_Options load;
  memset(&load, 0, sizeof load);
  load.viewport_width = (double) gimg_raster_width(reference);
  load.viewport_height = (double) gimg_raster_height(reference);
  GVEC_Result r = gvec_doc_load(stream, &load, &diag, &doc);
  if (r != GVEC_OK) {
    say(v, "NOT_LOADED", "%s code=%d line=%zu", gvec_result_string(r), diag.count ? (int) diag.items[0].code : 0,
        diag.count ? diag.items[0].line : (size_t) 0);
    goto done;
  }
  if (gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK ||
      gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
    say(v, "ERROR", "scene allocation");
    goto done;
  }
  conf_stub_set_store(store);
  if (gvec_lower_trace_create(NULL, &trace) != GVEC_OK) {
    say(v, "ERROR", "scene allocation");
    goto done;
  }
  GVEC_Lower_Options lower;
  memset(&lower, 0, sizeof lower);
  lower.store = store;
  lower.trace = trace;
  lower.fonts = o->fonts;
  /* The images a case names are files of the corpus, beside it or in its resources directory: the root is the corpus, the base the case's directory. */
  char base[4096];
  snprintf(base, sizeof base, "%s", svg);
  char * slash = strrchr(base, '/');
  if (slash) {
    *slash = 0;
  }
  lower.image_root = o->corpus;
  lower.image_base = base;
  /* The same references through the resolver (it takes over what `image_root` served): a URL of the remote table, a path of the corpus. */
  lower.resolver = resolver;
  lower.resolver_user = &serve;
  char document_uri[4096];
  snprintf(document_uri, sizeof document_uri, "tests/%s", name);
  lower.base_uri = document_uri;
  lower.flags = GVEC_LOWER_MARKER_CLOSED_ENDS_OPEN | GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK; /* the reference images are resvg's */
  GVEC_Result lr = o->lower(doc, &lower, list, &diag);
  if (lr != GVEC_OK) {
    say(v, "ERROR", "lowering returned %s", gvec_result_string(lr));
    goto done;
  }
  size_t refusals = gvec_diagnostics_count_code(&diag, GVEC_DIAG_LOWER_REFUSED);
  if (refusals > 0 || diag.dropped > 0) {
    char first[96];
    say(v, "REFUSED", "refusals=%zu first=%s", refusals, diag_first_refusal(&diag, first, sizeof first));
    goto done;
  }
  GIMG_Pixel_Format format = GIMG_PIXEL_RGBA8;
  uint32_t w = gimg_raster_width(reference), h = gimg_raster_height(reference);
  if (gshscene_list_close(list) != GSHSCENE_OK ||
      gimg_raster_create(w, h, &format, GIMG_RASTER_OWNED, NULL, 0, &raster) != GIMG_OK ||
      gshcpu_sink_create(NULL, NULL, NULL, &sink) != GSHCPU_OK ||
      gshscene_report_create(NULL, NULL, &report) != GSHSCENE_OK) {
    say(v, "ERROR", "render setup");
    goto done;
  }
  GSHSCENE_Target target;
  memset(&target, 0, sizeof target);
  target.kind = GSHSCENE_TARGET_RASTER;
  target.surface = raster;
  /* The reference is the document at the size its renderer was asked for: the document's own size scaled to the raster. */
  double doc_w = 0, doc_h = 0;
  if (gvec_doc_size(doc, &doc_w, &doc_h) == GVEC_OK && doc_w > 0 && doc_h > 0 &&
      gshgeom_map_scale(2, (double) w / doc_w, (double) h / doc_h, 1, &target.map) != GSHGEOM_OK) {
    say(v, "ERROR", "render setup");
    goto done;
  }
  /* A filter sees the source graphic inside the target's extent only; the references draw a filter over its whole region, so the render is made over
   * the extent joined with the regions of the filters (at most the extent's size beyond it on a side) and the extent is cut from it. */
  GIMG_Raster * wide_raster = NULL;
  GSHSCENE_Target wide_target = target;
  int64_t wide_x = 0, wide_y = 0;
  {
    GSHSCENE_Rect reach;
    int found = 0;
    if (gshscene_filter_reach(list, &target.map, NULL, &reach, &found) == GSHSCENE_OK && found &&
        (reach.x0 < 0 || reach.y0 < 0 || reach.x1 > (int64_t) w || reach.y1 > (int64_t) h)) {
      const int64_t margin_x = (int64_t) w, margin_y = (int64_t) h;
      const int64_t x0 = reach.x0 < -margin_x ? -margin_x : reach.x0 < 0 ? reach.x0 : 0, y0 = reach.y0 < -margin_y ? -margin_y : reach.y0 < 0 ? reach.y0 : 0;
      const int64_t x1 = reach.x1 > (int64_t) w + margin_x ? (int64_t) w + margin_x : reach.x1 > (int64_t) w ? reach.x1 : (int64_t) w;
      const int64_t y1 = reach.y1 > (int64_t) h + margin_y ? (int64_t) h + margin_y : reach.y1 > (int64_t) h ? reach.y1 : (int64_t) h;
      GSHGEOM_Map shift;
      if (gimg_raster_create((uint32_t) (x1 - x0), (uint32_t) (y1 - y0), &format, GIMG_RASTER_OWNED, NULL, 0, &wide_raster) != GIMG_OK ||
          gshgeom_map_translate(2, (double) -x0, (double) -y0, 0, &shift) != GSHGEOM_OK ||
          gshgeom_map_compose(&shift, &target.map, &wide_target.map) != GSHGEOM_OK) {
        say(v, "ERROR", "render setup (the wide target)");
        goto done;
      }
      wide_target.surface = wide_raster;
      wide_x = -x0;
      wide_y = -y0;
    }
  }
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = list;
  request.target = wide_raster ? &wide_target : &target;
  request.report = report;
  GSHSCENE_Result sr = gshscene_sink_render(sink, &request);
  if (sr == GSHSCENE_OK && wide_raster) {
    /* The extent of the target, cut from the wide render. */
    const size_t pixel = gimg_raster_bytes_per_pixel(&format);
    unsigned char * to = gimg_raster_pixels(raster);
    const unsigned char * from = gimg_raster_pixels(wide_raster);
    const size_t to_stride = gimg_raster_stride_bytes(raster), from_stride = gimg_raster_stride_bytes(wide_raster);
    for (uint32_t y = 0; y < h; y++) {
      memcpy(to + (size_t) y * to_stride, from + (size_t) (y + wide_y) * from_stride + (size_t) wide_x * pixel, (size_t) w * pixel);
    }
  }
  gimg_raster_destroy(wide_raster);
  wide_raster = NULL;
  if (sr != GSHSCENE_OK) {
    say(v, "ERROR", "render %s", gshscene_result_string(sr));
    goto done;
  }
  GSHSCENE_Report_Summary summary;
  if (gshscene_report_summary(report, &summary) != GSHSCENE_OK) {
    say(v, "ERROR", "render report");
    goto done;
  }
  size_t unsound = summary.by_class[GSHSCENE_ENTRY_REFUSED] + summary.by_class[GSHSCENE_ENTRY_APPROXIMATED];
  if (unsound > 0) {
    /* The sink left something out or drew it otherwise than defined: the picture cannot be scored as the document's. */
    char where[160] = "unplaced";
    const char * why = "";
    for (size_t i = 0; i < gshscene_report_count(report); i++) {
      GSHSCENE_Report_Entry entry;
      if (gshscene_report_entry(report, i, &entry) != GSHSCENE_OK ||
          (entry.entry_class != GSHSCENE_ENTRY_REFUSED && entry.entry_class != GSHSCENE_ENTRY_APPROXIMATED)) {
        continue;
      }
      why = gshscene_reason_string(entry.reason);
      GVEC_Lower_Site site;
      if (gvec_lower_trace_element(trace, NULL, report, i, &site) == GVEC_OK && site.placed) {
        snprintf(where, sizeof where, "%s:%zu", site.element, site.line);
      }
      break;
    }
    say(v, "REFUSED", "sink-report=%zu first=%s reason=%s", unsound, where, why);
    goto done;
  }
  if (o->dump) {
    dump_raster(o->dump, name, raster);
  }
  Diff d = compare_rasters(raster, reference, o->max_diff);
  int pass = d.max_diff <= o->max_diff && d.bad * 1000000u <= o->max_bad_ppm * d.total;
  say(v, pass ? "PASS" : "FAIL", "maxdiff=%u bad=%llu/%llu white-over=%llu", d.max_diff,
      (unsigned long long) d.bad, (unsigned long long) d.total, (unsigned long long) d.white_over);

done:
  free(serve.hold);
  gvec_lower_trace_free(trace);
  gshscene_report_destroy(report);
  gshscene_sink_destroy(sink);
  gimg_raster_destroy(raster);
  gshscene_list_destroy(list);
  gshscene_store_destroy(store);
  if (doc) {
    gvec_doc_free(doc);
  }
  gvec_diagnostics_destroy(&diag);
  gvec_stream_destroy(stream);
  gimg_raster_destroy(reference);
}

/** Run a case in a child, with a time and a memory limit; fill `v` from what the child wrote. */
static void run_isolated(const Options * o, const char * name, Verdict * v) {
  int fds[2];
  if (pipe(fds) != 0) {
    say(v, "ERROR", "pipe");
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    say(v, "ERROR", "fork");
    return;
  }
  if (pid == 0) {
    close(fds[0]);
    struct rlimit lim = {(rlim_t) 4 << 30, (rlim_t) 4 << 30};
    setrlimit(RLIMIT_AS, &lim);
    Verdict mine;
    run_case(o, name, &mine);
    char line[sizeof mine.detail + 32];
    int len = snprintf(line, sizeof line, "%s\t%s", mine.outcome, mine.detail);
    ssize_t ignored = write(fds[1], line, (size_t) len);
    (void) ignored;
    _exit(0);
  }
  close(fds[1]);
  char buf[sizeof v->detail + 32];
  size_t used = 0;
  int timed_out = 0;
  for (;;) {
    struct pollfd p = {fds[0], POLLIN, 0};
    int pr = poll(&p, 1, o->timeout_s * 1000);
    if (pr == 0) {
      timed_out = 1;
      kill(pid, SIGKILL);
      break;
    }
    if (pr < 0 && errno == EINTR) {
      continue;
    }
    ssize_t got = pr < 0 ? -1 : read(fds[0], buf + used, sizeof buf - 1 - used);
    if (got <= 0) {
      break;
    }
    used += (size_t) got;
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  buf[used] = 0;
  if (timed_out) {
    say(v, "ERROR", "timeout after %d s", o->timeout_s);
  } else if (WIFSIGNALED(status)) {
    say(v, "ERROR", "crash signal %d", WTERMSIG(status));
  } else {
    char * tab = strchr(buf, '\t');
    static const char * const ok[] = {"PASS", "FAIL", "REFUSED", "NOT_LOADED", "ERROR"};
    int known = 0;
    if (tab) {
      *tab = 0;
      for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        if (strcmp(buf, ok[i]) == 0) {
          v->outcome = ok[i];
          known = 1;
        }
      }
    }
    if (known) {
      snprintf(v->detail, sizeof v->detail, "%s", tab + 1);
    } else {
      say(v, "ERROR", "child gave no verdict");
    }
  }
  for (char * c = v->detail; *c; c++) {
    if (*c == '\t' || *c == '\n') {
      *c = ' ';
    }
  }
}

static int compare_files(const char * a, const char * b) {
  const char * why = "";
  GIMG_Raster * ra = load_reference(a, &why);
  if (!ra) {
    fprintf(stderr, "%s: %s\n", a, why);
    return 2;
  }
  GIMG_Raster * rb = load_reference(b, &why);
  if (!rb) {
    fprintf(stderr, "%s: %s\n", b, why);
    gimg_raster_destroy(ra);
    return 2;
  }
  int status = 0;
  if (gimg_raster_width(ra) != gimg_raster_width(rb) || gimg_raster_height(ra) != gimg_raster_height(rb)) {
    printf("size %ux%u %ux%u\n", gimg_raster_width(ra), gimg_raster_height(ra), gimg_raster_width(rb),
        gimg_raster_height(rb));
    status = 3;
  } else {
    Diff d = compare_rasters(ra, rb, 32);
    printf("maxdiff=%u bad=%llu total=%llu\n", d.max_diff, (unsigned long long) d.bad, (unsigned long long) d.total);
  }
  gimg_raster_destroy(ra);
  gimg_raster_destroy(rb);
  return status;
}

static int usage(void) {
  fprintf(stderr,
      "usage: runner --corpus DIR --list FILE --max-diff N --max-bad-ppm N [--stub NAME] [--timeout S]\n"
      "       runner --compare A.png B.png\n");
  return 2;
}

int main(int argc, char ** argv) {
  Options o = {NULL, NULL, 0, 0, gvec_doc_lower_list, DEFAULT_TIMEOUT_S, NULL, NULL, NULL, NULL, NULL, 0};
  const char * remote_table = NULL;
  int have_diff = 0, have_ppm = 0;
  for (int i = 1; i < argc; i++) {
    const char * arg = argv[i];
    if (strcmp(arg, "--compare") == 0 && argc == 4) {
      return compare_files(argv[i + 1], argv[i + 2]);
    }
    if (i + 1 >= argc) {
      return usage();
    }
    const char * value = argv[++i];
    if (strcmp(arg, "--corpus") == 0) {
      o.corpus = value;
    } else if (strcmp(arg, "--list") == 0) {
      o.list = value;
    } else if (strcmp(arg, "--max-diff") == 0) {
      o.max_diff = (unsigned) strtoul(value, NULL, 10);
      have_diff = 1;
    } else if (strcmp(arg, "--max-bad-ppm") == 0) {
      o.max_bad_ppm = strtoull(value, NULL, 10);
      have_ppm = 1;
    } else if (strcmp(arg, "--dump") == 0) {
      o.dump = value;
    } else if (strcmp(arg, "--timeout") == 0) {
      o.timeout_s = atoi(value);
    } else if (strcmp(arg, "--remote-dir") == 0) {
      o.remote_dir = value;
    } else if (strcmp(arg, "--remote-table") == 0) {
      remote_table = value;
    } else if (strcmp(arg, "--stub") == 0) {
      o.lower = conf_stub_find(value);
      if (!o.lower) {
        fprintf(stderr, "runner: no stub named %s\n", value);
        return 2;
      }
    } else {
      return usage();
    }
  }
  if (!o.corpus || !o.list || !have_diff || !have_ppm || o.timeout_s <= 0) {
    return usage();
  }
  /* The remote table: <url> TAB <sha256> TAB <bytes> TAB <file> per line; the file is what serves the url. */
  if (remote_table) {
    FILE * table = fopen(remote_table, "r");
    if (!table) {
      fprintf(stderr, "runner: cannot read %s\n", remote_table);
      return 2;
    }
    char line[4096];
    while (fgets(line, sizeof line, table)) {
      if (line[0] == '#' || line[0] == '\n') {
        continue;
      }
      char * field[4];
      char * at = line;
      size_t count = 0;
      while (count < 4 && at) {
        field[count++] = at;
        at = strchr(at, '\t');
        if (at) {
          *at++ = 0;
        }
      }
      if (count < 4) {
        fprintf(stderr, "runner: malformed line in %s\n", remote_table);
        return 2;
      }
      field[3][strcspn(field[3], "\r\n")] = 0;
      char ** urls = realloc(o.remote_url, (o.remote_count + 1) * sizeof *urls);
      char ** files = realloc(o.remote_file, (o.remote_count + 1) * sizeof *files);
      if (!urls || !files) {
        return 2;
      }
      o.remote_url = urls;
      o.remote_file = files;
      o.remote_url[o.remote_count] = strdup(field[0]);
      o.remote_file[o.remote_count] = strdup(field[3]);
      o.remote_count++;
    }
    fclose(table);
  }
  /* The fonts of the corpus, with the generic families of the references' renderer (resvg's test configuration). */
  GFNT_FontSet * fontset = NULL;
  {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/fonts", o.corpus);
    const char * dirs[1] = {dir};
    static const GFNT_FontAlias aliases[] = {{"serif", "Noto Serif"}, {"sans-serif", "Noto Sans"}, {"cursive", "Yellowtail"},
        {"fantasy", "Sedgwick Ave Display"}, {"monospace", "Noto Mono"}};
    /* A corpus with no fonts (the self-test's) draws no text: every element with text is refused. */
    if (gfnt_fontset_scan(dirs, 1, aliases, 5, NULL, &fontset, NULL) != GFNT_OK || gvec_fonts_new(fontset, NULL, &o.fonts) != GVEC_OK) {
      gfnt_fontset_free(fontset);
      fontset = NULL;
      o.fonts = NULL;
    }
  }
  FILE * list = fopen(o.list, "r");
  if (!list) {
    fprintf(stderr, "runner: cannot read %s\n", o.list);
    return 2;
  }
  char name[4096];
  while (fgets(name, sizeof name, list)) {
    size_t n = strlen(name);
    while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r')) {
      name[--n] = 0;
    }
    if (n == 0) {
      continue;
    }
    Verdict v;
    run_isolated(&o, name, &v);
    printf("%s\t%s\t%s\n", name, v.outcome, v.detail);
    fflush(stdout);
  }
  fclose(list);
  gvec_fonts_free(o.fonts);
  gfnt_fontset_free(fontset);
  return 0;
}
