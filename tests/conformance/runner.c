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
 *     runner --corpus DIR --list FILE --max-diff N --max-bad-ppm N [--stub NAME] [--timeout SECONDS]
 *     runner --compare A.png B.png
 *
 * A case is DIR/tests/<case>, an SVG file, with its reference at the same path
 * with .png for its extension. Each case runs in a child process under a time
 * and an address-space limit, so a hang or a crash is that case's ERROR and the
 * run goes on. The line is
 *
 *     <case> TAB <OUTCOME> TAB <detail>
 *
 * where OUTCOME is PASS, FAIL, REFUSED, NOT_LOADED or ERROR. EXCLUDED is the
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
} Diff;

/** The comparison: both rasters are straight RGBA8 of the same size. */
static Diff compare_rasters(const GIMG_Raster * a, const GIMG_Raster * b) {
  Diff d = {0, 0, 0};
  uint32_t w = gimg_raster_width(a), h = gimg_raster_height(a);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * pa = (const uint8_t *) gimg_raster_pixels_const(a) + y * gimg_raster_stride_bytes(a);
    const uint8_t * pb = (const uint8_t *) gimg_raster_pixels_const(b) + y * gimg_raster_stride_bytes(b);
    for (uint32_t x = 0; x < w; x++, pa += 4, pb += 4) {
      d.total++;
      if (pa[3] == 0 && pb[3] == 0) {
        continue;
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

typedef struct Options {
  const char * corpus;
  const char * list;
  unsigned max_diff;
  uint64_t max_bad_ppm;
  Conf_Lower_Fn lower;
  int timeout_s;
} Options;

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
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = list;
  request.target = &target;
  request.report = report;
  GSHSCENE_Result sr = gshscene_sink_render(sink, &request);
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
    for (size_t i = 0; i < gshscene_report_count(report); i++) {
      GSHSCENE_Report_Entry entry;
      if (gshscene_report_entry(report, i, &entry) != GSHSCENE_OK ||
          (entry.entry_class != GSHSCENE_ENTRY_REFUSED && entry.entry_class != GSHSCENE_ENTRY_APPROXIMATED)) {
        continue;
      }
      GVEC_Lower_Site site;
      if (gvec_lower_trace_element(trace, NULL, report, i, &site) == GVEC_OK && site.placed) {
        snprintf(where, sizeof where, "%s:%zu", site.element, site.line);
      }
      break;
    }
    say(v, "REFUSED", "sink-report=%zu first=%s", unsound, where);
    goto done;
  }
  Diff d = compare_rasters(raster, reference);
  int pass = d.max_diff <= o->max_diff && d.bad * 1000000u <= o->max_bad_ppm * d.total;
  say(v, pass ? "PASS" : "FAIL", "maxdiff=%u bad=%llu/%llu", d.max_diff, (unsigned long long) d.bad,
      (unsigned long long) d.total);

done:
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
    Diff d = compare_rasters(ra, rb);
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
  Options o = {NULL, NULL, 0, 0, gvec_doc_lower_list, DEFAULT_TIMEOUT_S};
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
    } else if (strcmp(arg, "--timeout") == 0) {
      o.timeout_s = atoi(value);
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
  return 0;
}
