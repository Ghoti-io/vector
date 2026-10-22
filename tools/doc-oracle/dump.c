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
 * Load a file and write its dump to standard output: the program the independent document oracle
 * (tools/doc-oracle/oracle.py) reads.
 *
 *     dump FILE [language...]
 */

#include <stdio.h>
#include <string.h>

#include <ghoti.io/vector/vector.h>

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: dump FILE [language...]\n");
    return 2;
  }
  GVEC_Stream * stream = NULL;
  GVEC_Stream * out = NULL;
  GVEC_Doc * doc = NULL;
  GVEC_Diagnostics diagnostics;
  GVEC_Load_Options options;
  memset(&options, 0, sizeof options);
  const char * languages[16];
  int n = 0;
  for (int i = 2; i < argc && n < 16; i++) {
    languages[n++] = argv[i];
  }
  options.languages = n ? languages : NULL;
  options.language_count = (size_t) n;
  gvec_diagnostics_init(&diagnostics, NULL);
  if (gvec_stream_create_file(argv[1], &stream) != GVEC_OK) {
    fprintf(stderr, "dump: cannot read %s\n", argv[1]);
    return 2;
  }
  GVEC_Result r = gvec_doc_load(stream, &options, &diagnostics, &doc);
  gvec_stream_destroy(stream);
  if (r != GVEC_OK) {
    fprintf(stderr, "dump: %s\n", gvec_result_string(r));
    gvec_diagnostics_destroy(&diagnostics);
    return 1;
  }
  gvec_stream_create_memory_output(&out);
  gvec_doc_dump(doc, out);
  const void * data = NULL;
  size_t size = 0;
  gvec_stream_output_buffer(out, &data, &size);
  fwrite(data, 1, size, stdout);
  gvec_stream_destroy(out);
  gvec_doc_free(doc);
  gvec_diagnostics_destroy(&diagnostics);
  return 0;
}
