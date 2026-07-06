#include "smithy/core/version.h"

#include <gtest/gtest.h>

namespace smithy {
namespace {

TEST(VersionTest, ReturnsSemanticVersion) {
  EXPECT_FALSE(Version().empty());
  EXPECT_EQ(Version(), "0.0.0-dev");
}

}  // namespace
}  // namespace smithy
