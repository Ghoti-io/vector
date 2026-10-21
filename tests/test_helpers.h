/**
 * @file
 *
 * Shared helpers for the Ghoti.io Vector unit tests: fixtures, a load that
 * keeps its diagnostics, and an allocator that fails on a chosen call.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GVEC_TEST_HELPERS_H
#define GHOTI_IO_GVEC_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/vector/vector.h>

namespace gvec_test {

/** The bytes of tests/data/svg/NAME (written by tools/gen-test-data.py). */
inline std::vector<unsigned char> Fixture(const std::string & name) {
  std::string path = "tests/data/svg/" + name;
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

inline std::vector<unsigned char> Bytes(const std::string & text) {
  return std::vector<unsigned char>(text.begin(), text.end());
}

/** What one load did. */
struct Loaded {
  GVEC_Result result = GVEC_ERR_INTERNAL;
  GVEC_Doc * doc = nullptr;
  GVEC_Diagnostics diagnostics{};

  Loaded() { gvec_diagnostics_init(&diagnostics, nullptr); }
  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;
  ~Loaded() {
    gvec_doc_free(doc);
    gvec_diagnostics_destroy(&diagnostics);
  }

  /** The first entry of a code, or nullptr. */
  const GVEC_Diagnostic * First(GVEC_Diag_Code code) const {
    for (size_t i = 0; i < diagnostics.count; i++) {
      if (diagnostics.items[i].code == code) {
        return &diagnostics.items[i];
      }
    }
    return nullptr;
  }

  size_t Count(GVEC_Diag_Code code) const { return gvec_diagnostics_count_code(&diagnostics, code); }
};

/** Load `bytes` with `limits` (nullptr for the defaults) into `out`. */
inline void Load(const std::vector<unsigned char> & bytes, const GVEC_Limits * limits, Loaded & out,
    const GVEC_Allocator * allocator = nullptr) {
  GVEC_Stream * stream = nullptr;
  ASSERT_EQ(gvec_stream_create_memory(bytes.empty() ? "" : (const void *) bytes.data(), bytes.size(), &stream),
      GVEC_OK);
  GVEC_Load_Options options;
  std::memset(&options, 0, sizeof(options));
  options.limits = limits;
  options.allocator = allocator;
  out.result = gvec_doc_load(stream, &options, &out.diagnostics, &out.doc);
  gvec_stream_destroy(stream);
}

inline void LoadFixture(const std::string & name, const GVEC_Limits * limits, Loaded & out) {
  Load(Fixture(name), limits, out);
}

/** An allocator that fails its Nth call (1-based) and counts blocks outstanding. */
struct Failing {
  GVEC_Allocator a{};
  long attempts = 0;
  long fail_at = -1;
  long outstanding = 0;

  Failing() {
    a.ctx = this;
    a.malloc_fn = [](void * ctx, size_t size) -> void * {
      Failing * f = (Failing *) ctx;
      if (f->Fail()) {
        return nullptr;
      }
      void * p = std::malloc(size ? size : 1u);
      if (p) {
        f->outstanding++;
      }
      return p;
    };
    a.calloc_fn = [](void * ctx, size_t n, size_t size) -> void * {
      Failing * f = (Failing *) ctx;
      if (f->Fail() || (n != 0 && size > (size_t) -1 / n)) {
        return nullptr;
      }
      void * p = std::calloc(n ? n : 1u, size ? size : 1u);
      if (p) {
        f->outstanding++;
      }
      return p;
    };
    a.realloc_fn = [](void * ctx, void * ptr, size_t size) -> void * {
      Failing * f = (Failing *) ctx;
      if (f->Fail()) {
        return nullptr;
      }
      void * p = std::realloc(ptr, size ? size : 1u);
      if (p && !ptr) {
        f->outstanding++;
      }
      return p;
    };
    a.free_fn = [](void * ctx, void * ptr) {
      Failing * f = (Failing *) ctx;
      if (ptr) {
        f->outstanding--;
        std::free(ptr);
      }
    };
  }

  bool Fail() {
    attempts++;
    return fail_at >= 0 && attempts == fail_at;
  }
};

} // namespace gvec_test

#endif /* GHOTI_IO_GVEC_TEST_HELPERS_H */
