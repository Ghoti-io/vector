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
 * The lowering's equality gate, end to end, and its goldens.
 *
 *   lower_gate [--update] [--ppm DIR] --golden DIR FIXTURE.svg...
 *
 * For each fixture (an SVG document that draws on a 64 by 64 canvas) the document is lowered into a list, and into an edit
 * that is published; the gate requires that
 *
 *   - the list the published version emits equals the recorded list (gshscene_list_equal, and the same hash);
 *   - both rendered through shape-cpu give the same 64 by 64 straight RGBA8 raster, byte for byte;
 *   - the sink reports nothing it refused or approximated;
 *   - the list's hash, the raster's hash, the diagnostics and the list's dump are those of the golden file
 *     DIR/NAME.txt. With --update the golden files are written instead of compared.
 *
 * The golden is a review aid and a regression guard: the independent checks of the lowering's values are in the unit tests
 * (the reference vectors for the viewBox map among them). It exits 1 on the first fixture that fails, naming it.
 *
 * It is test code: it links shape-cpu and image, which the library never does.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/discover.h>
#include <ghoti.io/image/image.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-scene/shape-scene.h>
#include <ghoti.io/vector/vector.h>

#define CANVAS 64

static int g_failed;
static const char * g_ppm_dir;
static GVEC_Fonts * g_fonts; /* The test font of tests/data/fonts, beside the goldens' directory; text in a fixture is drawn with it. */

static void fail(const char * name, const char * what) {
  fprintf(stderr, "lower_gate: %s: %s\n", name, what);
  g_failed = 1;
}

static uint64_t fnv(const void * data, size_t size, uint64_t h) {
  const unsigned char * p = data;
  for (size_t i = 0; i < size; i++) {
    h = (h ^ p[i]) * UINT64_C(0x100000001b3);
  }
  return h;
}

/** Render a closed list on a CANVAS square straight RGBA8 raster (cleared to transparent black); the raster's hash, or 0. */
static int render(const GSHSCENE_List * list, const char * name, unsigned char * pixels, uint64_t * hash) {
  GIMG_Raster * raster = NULL;
  GSHSCENE_Sink * sink = NULL;
  GSHSCENE_Report * report = NULL;
  GIMG_Pixel_Format format = GIMG_PIXEL_RGBA8;
  int ok = 0;
  if (gimg_raster_create(CANVAS, CANVAS, &format, GIMG_RASTER_OWNED, NULL, 0, &raster) != GIMG_OK ||
      gshcpu_sink_create(NULL, NULL, NULL, &sink) != GSHCPU_OK || gshscene_report_create(NULL, NULL, &report) != GSHSCENE_OK) {
    fail(name, "render setup");
    goto done;
  }
  for (uint32_t y = 0; y < CANVAS; y++) {
    memset((unsigned char *) gimg_raster_pixels(raster) + y * gimg_raster_stride_bytes(raster), 0, CANVAS * 4);
  }
  GSHSCENE_Target target;
  memset(&target, 0, sizeof target);
  target.kind = GSHSCENE_TARGET_RASTER;
  target.surface = raster;
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = list;
  request.target = &target;
  request.report = report;
  if (gshscene_sink_render(sink, &request) != GSHSCENE_OK) {
    fail(name, "the sink did not render");
    goto done;
  }
  GSHSCENE_Report_Summary summary;
  if (gshscene_report_summary(report, &summary) != GSHSCENE_OK ||
      summary.by_class[GSHSCENE_ENTRY_REFUSED] + summary.by_class[GSHSCENE_ENTRY_APPROXIMATED] != 0) {
    fail(name, "the sink refused or approximated an item");
    goto done;
  }
  uint64_t h = UINT64_C(0xcbf29ce484222325);
  for (uint32_t y = 0; y < CANVAS; y++) {
    const unsigned char * row = (const unsigned char *) gimg_raster_pixels_const(raster) + y * gimg_raster_stride_bytes(raster);
    memcpy(pixels + (size_t) y * CANVAS * 4, row, CANVAS * 4);
    h = fnv(row, CANVAS * 4, h);
  }
  *hash = h;
  ok = 1;
done:
  gshscene_report_destroy(report);
  gshscene_sink_destroy(sink);
  gimg_raster_destroy(raster);
  return ok;
}

static char * slurp(const char * path, size_t * size) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 1 << 14, used = 0;
  char * data = malloc(cap + 1);
  while (data) {
    size_t got = fread(data + used, 1, cap - used, f);
    used += got;
    if (used < cap) {
      break;
    }
    cap *= 2;
    char * grown = realloc(data, cap + 1);
    if (!grown) {
      free(data);
      data = NULL;
      break;
    }
    data = grown;
  }
  fclose(f);
  if (data) {
    data[used] = 0;
    *size = used;
  }
  return data;
}

/** The golden text of a fixture: hashes, the diagnostics, and the list's dump. NULL on failure. */
static char * golden_text(const char * name, uint64_t list_hash, uint64_t raster_hash, const GVEC_Diagnostics * diagnostics,
    const GSHSCENE_List * list, size_t * size) {
  FILE * f = tmpfile(); /* not open_memstream: that is POSIX, and the gate runs on Windows too */
  if (!f) {
    return NULL;
  }
  fprintf(f, "fixture %s\nlist-hash %016llx\nraster-hash %016llx\ndiagnostics %llu\n", name, (unsigned long long) list_hash,
      (unsigned long long) raster_hash, (unsigned long long) diagnostics->count);
  for (size_t i = 0; i < diagnostics->count; i++) {
    const GVEC_Diagnostic * d = &diagnostics->items[i];
    fprintf(f, "diagnostic code=%d detail=%u line=%llu column=%llu element=%s\n", (int) d->code, (unsigned) d->detail,
        (unsigned long long) d->line, (unsigned long long) d->column, d->element ? d->element : "-");
  }
  fprintf(f, "list\n");
  if (gshscene_list_dump(list, f) != GSHSCENE_OK || fflush(f) != 0 || fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long length = ftell(f);
  if (length < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char * buffer = malloc((size_t) length + 1);
  if (buffer && fread(buffer, 1, (size_t) length, f) != (size_t) length) {
    free(buffer);
    buffer = NULL;
  }
  fclose(f);
  if (buffer) {
    buffer[length] = 0;
    *size = (size_t) length;
  }
  return buffer;
}

static const char * base_name(const char * path, char * out, size_t cap) {
  const char * slash = strrchr(path, '/');
  const char * base = slash ? slash + 1 : path;
  size_t n = strlen(base);
  if (n > 4 && strcmp(base + n - 4, ".svg") == 0) {
    n -= 4;
  }
  if (n >= cap) {
    n = cap - 1;
  }
  memcpy(out, base, n);
  out[n] = 0;
  return out;
}

static void check_fixture(const char * path, const char * golden_dir, int update) {
  char name[256];
  base_name(path, name, sizeof name);
  GVEC_Stream * stream = NULL;
  GVEC_Doc * doc = NULL;
  GVEC_Diagnostics by_list, by_edit;
  GSHSCENE_Scene * scene = NULL;
  GSHSCENE_List * list = NULL;
  GSHSCENE_Edit * edit = NULL;
  const GSHSCENE_Version * version = NULL;
  const GSHSCENE_List * emitted = NULL;
  unsigned char * pixels_a = malloc((size_t) CANVAS * CANVAS * 4);
  unsigned char * pixels_b = malloc((size_t) CANVAS * CANVAS * 4);
  char * golden = NULL;
  gvec_diagnostics_init(&by_list, NULL);
  gvec_diagnostics_init(&by_edit, NULL);
  if (!pixels_a || !pixels_b) {
    fail(name, "out of memory");
    goto done;
  }
  if (gvec_stream_create_file(path, &stream) != GVEC_OK) {
    fail(name, "unreadable");
    goto done;
  }
  GVEC_Diagnostics load_diagnostics;
  gvec_diagnostics_init(&load_diagnostics, NULL);
  GVEC_Result r = gvec_doc_load(stream, NULL, &load_diagnostics, &doc);
  gvec_diagnostics_destroy(&load_diagnostics);
  if (r != GVEC_OK) {
    fail(name, "does not load");
    goto done;
  }
  if (gshscene_scene_create(NULL, NULL, NULL, &scene) != GSHSCENE_OK ||
      gshscene_list_create(NULL, NULL, gshscene_scene_store(scene), &list) != GSHSCENE_OK) {
    fail(name, "scene allocation");
    goto done;
  }
  GVEC_Lower_Options options;
  memset(&options, 0, sizeof options);
  options.store = gshscene_scene_store(scene);
  options.fonts = g_fonts;
  if (gvec_doc_lower_list(doc, &options, list, &by_list) != GVEC_OK || gshscene_list_close(list) != GSHSCENE_OK) {
    fail(name, "lowering into a list");
    goto done;
  }
  if (gshscene_scene_open_edit(scene, NULL, &edit) != GSHSCENE_OK || gvec_doc_lower_edit(doc, &options, edit, &by_edit) != GVEC_OK ||
      gshscene_scene_publish(scene, edit, NULL, &version) != GSHSCENE_OK || gshscene_version_list(version, &emitted) != GSHSCENE_OK) {
    fail(name, "lowering into an edit and publishing it");
    goto done;
  }
  if (!gshscene_list_equal(list, emitted) || gshscene_list_hash(list) != gshscene_list_hash(emitted)) {
    fail(name, "the list the edit emits is not the list that was recorded");
    goto done;
  }
  if (by_list.count != by_edit.count) {
    fail(name, "the two lowerings report a different number of diagnostics");
    goto done;
  }
  uint64_t raster_a = 0, raster_b = 0;
  if (!render(list, name, pixels_a, &raster_a) || !render(emitted, name, pixels_b, &raster_b)) {
    goto done;
  }
  if (memcmp(pixels_a, pixels_b, (size_t) CANVAS * CANVAS * 4) != 0) {
    fail(name, "the two lists render to different rasters");
    goto done;
  }
  if (g_ppm_dir) {
    /* For review: the raster over white, as a binary PPM. */
    char ppm_path[4096];
    snprintf(ppm_path, sizeof ppm_path, "%s/%s.ppm", g_ppm_dir, name);
    FILE * ppm = fopen(ppm_path, "wb");
    if (ppm) {
      fprintf(ppm, "P6\n%d %d\n255\n", CANVAS, CANVAS);
      for (size_t i = 0; i < (size_t) CANVAS * CANVAS; i++) {
        const unsigned char * px = pixels_a + i * 4;
        for (int c = 0; c < 3; c++) {
          fputc((px[c] * px[3] + 255 * (255 - px[3]) + 127) / 255, ppm);
        }
      }
      fclose(ppm);
    }
  }
  size_t golden_size = 0;
  golden = golden_text(name, gshscene_list_hash(list), raster_a, &by_list, list, &golden_size);
  if (!golden) {
    fail(name, "golden text");
    goto done;
  }
  char golden_path[4096];
  snprintf(golden_path, sizeof golden_path, "%s/%s.txt", golden_dir, name);
  if (update) {
    FILE * f = fopen(golden_path, "wb");
    if (!f || fwrite(golden, 1, golden_size, f) != golden_size || fclose(f) != 0) {
      fail(name, "cannot write the golden file");
    }
  }
  else {
    size_t have_size = 0;
    char * have = slurp(golden_path, &have_size);
    if (!have) {
      fail(name, "no golden file (run with --update and review it)");
    }
    else {
      if (have_size != golden_size || memcmp(have, golden, golden_size) != 0) {
        fail(name, "differs from its golden file");
      }
      free(have);
    }
  }
done:
  free(golden);
  free(pixels_a);
  free(pixels_b);
  gshscene_version_release(version);
  gshscene_edit_destroy(edit);
  gshscene_list_destroy(list);
  gshscene_scene_destroy(scene);
  gvec_doc_free(doc);
  gvec_stream_destroy(stream);
  gvec_diagnostics_destroy(&by_list);
  gvec_diagnostics_destroy(&by_edit);
}

int main(int argc, char ** argv) {
  const char * golden_dir = NULL;
  int update = 0;
  int first = 1;
  for (; first < argc; first++) {
    if (strcmp(argv[first], "--update") == 0) {
      update = 1;
    }
    else if (strcmp(argv[first], "--golden") == 0 && first + 1 < argc) {
      golden_dir = argv[++first];
    }
    else if (strcmp(argv[first], "--ppm") == 0 && first + 1 < argc) {
      g_ppm_dir = argv[++first];
    }
    else {
      break;
    }
  }
  if (!golden_dir || first >= argc) {
    fprintf(stderr, "usage: lower_gate [--update] [--ppm DIR] --golden DIR FIXTURE.svg...\n");
    return 2;
  }
  /* The test font is beside the goldens' directory (tests/golden/lower, tests/data/fonts); a document with text has none without it. */
  GFNT_FontSet * font_set = NULL;
  {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/../../data/fonts", golden_dir);
    const char * dirs[1] = {dir};
    static const GFNT_FontAlias aliases[] = {{"sans-serif", "Vector Test"}, {"serif", "Vector Test"}};
    if (gfnt_fontset_scan(dirs, 1, aliases, 2, NULL, &font_set, NULL) == GFNT_OK && gvec_fonts_new(font_set, NULL, &g_fonts) != GVEC_OK) {
      g_fonts = NULL;
    }
  }
  for (int i = first; i < argc; i++) {
    check_fixture(argv[i], golden_dir, update);
  }
  gvec_fonts_free(g_fonts);
  gfnt_fontset_free(font_set);
  if (!g_failed) {
    printf("lower_gate: %d fixtures, lists equal, rasters equal%s\n", argc - first, update ? ", goldens written" : ", goldens match");
  }
  return g_failed ? 1 : 0;
}
