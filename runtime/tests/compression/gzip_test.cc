#include "smithy/compression/gzip.h"

#include <gtest/gtest.h>

#include <string>

namespace smithy {
namespace {

TEST(GzipTest, RoundTrips) {
  const std::string text(100000, 'a');
  const auto compressed = GzipCompress(text);
  ASSERT_TRUE(compressed.ok());
  EXPECT_LT(compressed->size(), text.size() / 10);  // trivially compressible
  const auto restored = GzipDecompress(*compressed);
  ASSERT_TRUE(restored.ok());
  EXPECT_EQ(*restored, text);
}

TEST(GzipTest, RoundTripsEmpty) {
  const auto compressed = GzipCompress("");
  ASSERT_TRUE(compressed.ok());
  const auto restored = GzipDecompress(*compressed);
  ASSERT_TRUE(restored.ok());
  EXPECT_EQ(*restored, "");
}

TEST(GzipTest, RejectsGarbage) {
  EXPECT_FALSE(GzipDecompress("definitely not gzip").ok());
  EXPECT_FALSE(GzipDecompress("").ok());
}

TEST(GzipTest, EnforcesOutputLimit) {
  const std::string text(100000, 'a');
  const auto compressed = GzipCompress(text);
  ASSERT_TRUE(compressed.ok());
  EXPECT_FALSE(GzipDecompress(*compressed, 1024).ok());
}

TEST(GzipTest, RejectsTrailingGarbage) {
  const auto compressed = GzipCompress("payload");
  ASSERT_TRUE(compressed.ok());
  EXPECT_FALSE(GzipDecompress(*compressed + "extra").ok());
}

}  // namespace
}  // namespace smithy
