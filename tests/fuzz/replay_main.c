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
 * The replay gate: every file given is fed once through the fuzz entry point in
 * an ordinary build, so a crash a fuzzer once found is a failing test and not a
 * campaign to repeat.  It fails on no input at all, and on a corpus that never
 * reaches an outcome (a limit, a malformed file, a damaged transport): a replay over
 * nothing reports success over a population of zero.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);
int gvec_fuzz_unreached(void);

int main(int argc, char * argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: replay FILE...\n");
    return 2;
  }
  size_t replayed = 0;
  for (int i = 1; i < argc; i++) {
    FILE * file = fopen(argv[i], "rb");
    if (!file) {
      fprintf(stderr, "replay: cannot open %s\n", argv[i]);
      return 2;
    }
    size_t capacity = 4096;
    size_t length = 0;
    uint8_t * buffer = malloc(capacity);
    while (buffer) {
      if (length == capacity) {
        capacity *= 2;
        uint8_t * grown = realloc(buffer, capacity);
        if (!grown) {
          free(buffer);
          buffer = NULL;
          break;
        }
        buffer = grown;
      }
      size_t got = fread(buffer + length, 1, capacity - length, file);
      if (got == 0) {
        break;
      }
      length += got;
    }
    fclose(file);
    if (!buffer) {
      fprintf(stderr, "replay: out of memory reading %s\n", argv[i]);
      return 2;
    }
    LLVMFuzzerTestOneInput(buffer, length);
    free(buffer);
    replayed++;
  }
  if (replayed == 0) {
    fprintf(stderr, "replay: no inputs\n");
    return 2;
  }
  int missing = gvec_fuzz_unreached();
  if (missing) {
    fprintf(stderr, "replay: %d outcome(s) never reached by %llu inputs\n", missing, (unsigned long long) replayed);
    return 1;
  }
  printf("replay: %llu inputs, no crash, every outcome reached\n", (unsigned long long) replayed);
  return 0;
}
