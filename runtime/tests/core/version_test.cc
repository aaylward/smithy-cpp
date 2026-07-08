#include "smithy/core/version.h"

#include <gtest/gtest.h>

namespace smithy {
namespace {

TEST(VersionTest, ReturnsSemanticVersion) {
  EXPECT_FALSE(Version().empty());
  // Pre-release until the first signed tag (docs/versioning.md).
  EXPECT_EQ(Version(), "0.1.0-dev");
}

}  // namespace
}  // namespace smithy
