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
 * The load fuzzer's custom mutator (libFuzzer builds only).
 *
 * A mutation of raw bytes rarely survives the XML parser, so most of what libFuzzer makes of an SVG never reaches
 * the attribute grammars. This mutator keeps the document well-formed by changing one attribute *value* at a time:
 * it finds the values (`="..."`), and replaces one with an entry of a dictionary of valid, near-valid and hostile
 * values of every grammar (numbers, lengths, lists, path data, viewBox, aspect ratios, references, data: URIs,
 * language lists), or with a small edit of the value it had (a digit changed, a separator doubled, a piece
 * dropped or repeated). A quarter of the time it changes the options byte instead, which names the limit to
 * shrink; the rest of the time it leaves the choice to libFuzzer's own mutator.
 */

#include <stdint.h>
#include <string.h>

size_t LLVMFuzzerMutate(uint8_t * data, size_t size, size_t max_size);
size_t LLVMFuzzerCustomMutator(uint8_t * data, size_t size, size_t max_size, unsigned int seed);

static const char * const k_values[] = {
    "0", "-0", "1", "-1", "+1", ".5", "5.", "1e3", "1E-3", "1e999", "-1e999", "1e-999", "0x10", "nan", "inf",
    "10px", "10PX", "1em", "1ex", "1ch", "1rem", "1vw", "1vmin", "50%", "-50%", "1in", "1cm", "1mm", "1Q", "1pt", "1pc",
    "90deg", "100grad", "1rad", "0.25turn", "1 2 3", "1,2,3", "1 , 2 ,3", "1,,2", "1,", ",1", "1 2,", "1-2", "10-5",
    "0 0 10 10", "0,0,10,10", "0 0 -1 5", "0 0 0 0", "0 0 10", "0 0 1e999 1", "xMidYMid meet", "xMinYMax slice",
    "none", "defer xMinYMin", "xMidYMid meet slice", "auto", "auto-start-reverse", "45", "left", "center", "bottom",
    "userSpaceOnUse", "objectBoundingBox", "pad", "reflect", "repeat", "strokeWidth", "preserve", "default",
    "M 0 0 L 10 10 Z", "m 1 1 2 2 3 3", "M0 0C1 1 2 2 3 3S5 5 6 6Q1 1 2 2T4 4", "M0 0A1 1 0 11 5 5a2 3 45 0 1 10 -4",
    "M10-5.5L.5.5 1e2-1e-1", "L 1 1", "M 1", "M 1 2 L 3", "M 1e999 2", "M 0 0 A 1 1 0 2 0 5 5", "M 0 0 Z 5",
    "M 0 0 L 1 1 L 2 2 L 3 3 L 4 4 L 5 5 L 6 6 L 7 7 L 8 8", "M 1e308 0 l 1e308 0", "M 0 0 Q 1 1 2 2 T 3 3 T 4 4 T 5 5",
    "0,0 10,0 10,10", "0,0 10", "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20",
    "#a", "#b", "#missing", "#a%20b", "#", "other.svg#x", "http://example.org/x",
    "data:image/png;base64,iVBORw0KGgo=", "data:,hello%20world", "data:;base64,AAAA", "data:image/png;base64", "data:,%zz",
    "en", "en-US", "fr, de", "", " ", "en-US,fr", "english", "http://example.org/ext", "a b c",
    "a", "b", "c", "dup", "a b", "-3", "3", "100000000000000000000000", "red", "url(#a)", "rotate(45)",
};

static uint64_t next(uint64_t * state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

/* Positions of the attribute values: each is the index just after `="` and the index of the closing quote. */
static size_t find_values(const uint8_t * data, size_t size, size_t * begin, size_t * end, size_t max) {
  size_t count = 0;
  for (size_t i = 2; i + 2 < size && count < max; i++) {
    if (data[i] == '=' && data[i + 1] == '"') {
      size_t j = i + 2;
      while (j < size && data[j] != '"') {
        j++;
      }
      if (j < size) {
        begin[count] = i + 2;
        end[count] = j;
        count++;
        i = j;
      }
    }
  }
  return count;
}

size_t LLVMFuzzerCustomMutator(uint8_t * data, size_t size, size_t max_size, unsigned int seed) {
  uint64_t state = ((uint64_t) seed << 32 | 0x9e3779b9u) | 1u;
  if (size < 3) {
    return LLVMFuzzerMutate(data, size, max_size);
  }
  uint64_t pick = next(&state) % 8;
  if (pick < 2) {
    data[next(&state) & 1] = (uint8_t) next(&state);  /* the options byte, or the read-size byte */
    return size;
  }
  if (pick < 7) {
    size_t begin[256], end[256];
    size_t n = find_values(data, size, begin, end, 256);
    if (n == 0) {
      return LLVMFuzzerMutate(data, size, max_size);
    }
    size_t which = (size_t) (next(&state) % n);
    uint8_t replacement[256];
    size_t length = 0;
    if (pick < 5) {
      const char * v = k_values[next(&state) % (sizeof k_values / sizeof k_values[0])];
      length = strlen(v);
      memcpy(replacement, v, length);
    } else {
      length = end[which] - begin[which];
      if (length > sizeof replacement - 8) {
        length = sizeof replacement - 8;
      }
      memcpy(replacement, data + begin[which], length);
      switch (next(&state) % 5) {
      case 0:
        if (length) {
          replacement[next(&state) % length] = (uint8_t) "0123456789.-+, eE%#"[next(&state) % 18];
        }
        break;
      case 1:
        if (length) {
          size_t at = (size_t) (next(&state) % length);
          memmove(replacement + at + 1, replacement + at, length - at);
          replacement[at] = (uint8_t) " ,.-+"[next(&state) % 5];
          length++;
        }
        break;
      case 2:
        if (length) {
          size_t at = (size_t) (next(&state) % length);
          memmove(replacement + at, replacement + at + 1, length - at - 1);
          length--;
        }
        break;
      case 3:
        if (length && length * 2 <= sizeof replacement) {
          memcpy(replacement + length, replacement, length);
          length *= 2;
        }
        break;
      default:
        length = length / 2;
        break;
      }
    }
    for (size_t i = 0; i < length; i++) {
      if (replacement[i] == '"' || replacement[i] == '<' || replacement[i] == '&') {
        replacement[i] = ' ';  /* keep the markup well-formed */
      }
    }
    size_t old = end[which] - begin[which];
    size_t fresh = size - old + length;
    if (fresh > max_size) {
      return size;
    }
    memmove(data + begin[which] + length, data + end[which], size - end[which]);
    memcpy(data + begin[which], replacement, length);
    return fresh;
  }
  return LLVMFuzzerMutate(data, size, max_size);
}
