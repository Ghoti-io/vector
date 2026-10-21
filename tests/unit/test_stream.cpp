/**
 * @file
 *
 * Streams (L2): memory, file, output, and the test-only no-seek and chunked
 * sources.
 */

#include "test_helpers.h"

#include <string>

#include "../../src/stream/stream_internal.h"

using namespace gvec_test;

static const char kText[] = "0123456789abcdef";

TEST(Stream, AMemoryStreamReadsPeeksSeeksAndReportsItsEnd) {
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_memory(kText, 16, &s), GVEC_OK);
  char buf[32];
  size_t n = 0;
  EXPECT_EQ(gvec_stream_size(s), (size_t) 16);
  ASSERT_EQ(gvec_stream_peek(s, buf, 4, &n), GVEC_OK);
  EXPECT_EQ(n, (size_t) 4);
  EXPECT_EQ(std::string(buf, 4), "0123");
  EXPECT_EQ(gvec_stream_tell(s), (size_t) 0);
  ASSERT_EQ(gvec_stream_read(s, buf, 6, &n), GVEC_OK);
  // A read after a peek returns the peeked bytes first; a short read is allowed.
  EXPECT_GE(n, (size_t) 1);
  EXPECT_EQ(std::string(buf, n), std::string(kText, n));
  ASSERT_EQ(gvec_stream_seek(s, 10), GVEC_OK);
  EXPECT_EQ(gvec_stream_tell(s), (size_t) 10);
  ASSERT_EQ(gvec_stream_read_exact(s, buf, 6), GVEC_OK);
  EXPECT_EQ(std::string(buf, 6), "abcdef");
  EXPECT_FALSE(gvec_stream_eof(s));
  ASSERT_EQ(gvec_stream_read(s, buf, 1, &n), GVEC_OK);
  EXPECT_EQ(n, (size_t) 0);
  EXPECT_TRUE(gvec_stream_eof(s));
  EXPECT_EQ(gvec_stream_read_exact(s, buf, 1), GVEC_ERR_IO);
  EXPECT_EQ(gvec_stream_seek(s, 17), GVEC_ERR_IO);
  ASSERT_EQ(gvec_stream_seek(s, 0), GVEC_OK);
  EXPECT_FALSE(gvec_stream_eof(s));
  EXPECT_EQ(gvec_stream_error(s), GVEC_OK);
  EXPECT_EQ(gvec_stream_write(s, "x", 1), GVEC_ERR_IO);
  gvec_stream_destroy(s);
}

TEST(Stream, AShortReadThatReachesTheEndSetsEof) {
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_memory(kText, 16, &s), GVEC_OK);
  char buf[32];
  size_t n = 0;
  ASSERT_EQ(gvec_stream_read(s, buf, sizeof(buf), &n), GVEC_OK);
  EXPECT_EQ(n, (size_t) 16);
  EXPECT_TRUE(gvec_stream_eof(s));
  gvec_stream_destroy(s);
}

TEST(Stream, ANoSeekStreamPeeksButRefusesToSeekAndHasNoSize) {
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_memory_no_seek_with_allocator(nullptr, kText, 16, &s), GVEC_OK);
  char buf[16];
  size_t n = 0;
  EXPECT_EQ(gvec_stream_size(s), GVEC_STREAM_SIZE_UNKNOWN);
  ASSERT_EQ(gvec_stream_peek(s, buf, 8, &n), GVEC_OK);
  EXPECT_EQ(std::string(buf, n), "01234567");
  EXPECT_EQ(gvec_stream_seek(s, 0), GVEC_ERR_UNSUPPORTED);
  ASSERT_EQ(gvec_stream_read_exact(s, buf, 16), GVEC_OK);
  EXPECT_EQ(std::string(buf, 16), kText);
  gvec_stream_destroy(s);
}

TEST(Stream, AChunkedStreamGivesTheSameBytesInShortReads) {
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_memory_chunked_with_allocator(nullptr, kText, 16, 3, &s), GVEC_OK);
  char buf[16];
  size_t n = 0;
  ASSERT_EQ(gvec_stream_read(s, buf, 16, &n), GVEC_OK);
  EXPECT_EQ(n, (size_t) 3);
  ASSERT_EQ(gvec_stream_peek(s, buf, 13, &n), GVEC_OK);
  EXPECT_EQ(n, (size_t) 13); // a peek fills across short reads
  EXPECT_EQ(std::string(buf, 13), std::string(kText + 3, 13));
  EXPECT_EQ(gvec_stream_create_memory_chunked_with_allocator(nullptr, kText, 16, 0, &s), GVEC_ERR_INVALID);
  gvec_stream_destroy(s);
}

TEST(Stream, AFileStreamReadsTheFileAndAnUnreadablePathIsIo) {
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_file("tests/data/svg/minimal.svg", &s), GVEC_OK);
  std::vector<unsigned char> expected = Fixture("minimal.svg");
  EXPECT_EQ(gvec_stream_size(s), expected.size());
  std::vector<unsigned char> got(expected.size());
  ASSERT_EQ(gvec_stream_read_exact(s, got.data(), got.size()), GVEC_OK);
  EXPECT_EQ(got, expected);
  ASSERT_EQ(gvec_stream_seek(s, 1), GVEC_OK);
  unsigned char c = 0;
  ASSERT_EQ(gvec_stream_read_exact(s, &c, 1), GVEC_OK);
  EXPECT_EQ(c, expected[1]);
  gvec_stream_destroy(s);
  s = nullptr;
  EXPECT_EQ(gvec_stream_create_file("tests/data/svg/no-such-file.svg", &s), GVEC_ERR_IO);
  EXPECT_EQ(s, nullptr);
  EXPECT_EQ(gvec_stream_create_file(nullptr, &s), GVEC_ERR_INVALID);
}

TEST(Stream, AnOutputStreamCollectsWhatIsWritten) {
  GVEC_Stream * s = nullptr;
  ASSERT_EQ(gvec_stream_create_memory_output(&s), GVEC_OK);
  const void * data = (const void *) 1;
  size_t size = 1;
  gvec_stream_output_buffer(s, &data, &size);
  EXPECT_EQ(data, nullptr);
  EXPECT_EQ(size, (size_t) 0);
  std::string all;
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(gvec_stream_write(s, kText, 16), GVEC_OK);
    all += kText;
  }
  gvec_stream_output_buffer(s, &data, &size);
  EXPECT_EQ(std::string((const char *) data, size), all);
  EXPECT_EQ(gvec_stream_tell(s), all.size());
  char buf[4];
  EXPECT_EQ(gvec_stream_read(s, buf, 4, nullptr), GVEC_ERR_IO);
  EXPECT_EQ(gvec_stream_seek(s, 0), GVEC_ERR_UNSUPPORTED);
  gvec_stream_destroy(s);
}

TEST(Stream, NullArgumentsAreInvalid) {
  GVEC_Stream * s = nullptr;
  EXPECT_EQ(gvec_stream_create_memory(nullptr, 1, &s), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_stream_create_memory(kText, 1, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_stream_create_memory_output(nullptr), GVEC_ERR_INVALID);
  ASSERT_EQ(gvec_stream_create_memory(nullptr, 0, &s), GVEC_OK); // an empty stream
  EXPECT_EQ(gvec_stream_read(nullptr, nullptr, 0, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_stream_peek(s, nullptr, 1, nullptr), GVEC_ERR_INVALID);
  EXPECT_EQ(gvec_stream_tell(nullptr), (size_t) 0);
  EXPECT_EQ(gvec_stream_size(nullptr), GVEC_STREAM_SIZE_UNKNOWN);
  EXPECT_FALSE(gvec_stream_eof(nullptr));
  EXPECT_EQ(gvec_stream_error(nullptr), GVEC_ERR_INVALID);
  gvec_stream_destroy(s);
  gvec_stream_destroy(nullptr);
}

TEST(Stream, EveryAllocationFailureOfAStreamIsOomAndLeaksNothing) {
  for (long fail = 1; fail < 50; fail++) {
    Failing f;
    f.fail_at = fail;
    GVEC_Stream * s = nullptr;
    GVEC_Result r = gvec_stream_create_memory_output_with_allocator(&f.a, &s);
    if (r == GVEC_OK) {
      for (int i = 0; i < 100 && r == GVEC_OK; i++) {
        r = gvec_stream_write(s, kText, 16);
      }
      if (r != GVEC_OK) {
        EXPECT_EQ(r, GVEC_ERR_OOM);
        EXPECT_EQ(gvec_stream_write(s, kText, 1), GVEC_ERR_IO); // the failure sticks
      }
      gvec_stream_destroy(s);
    }
    else {
      EXPECT_EQ(r, GVEC_ERR_OOM);
    }
    EXPECT_EQ(f.outstanding, 0) << fail;
    if (f.attempts < fail) {
      break;
    }
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
