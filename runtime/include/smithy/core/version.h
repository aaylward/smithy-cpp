#ifndef SMITHY_CORE_VERSION_H_
#define SMITHY_CORE_VERSION_H_

#include <string_view>

namespace smithy {

// Returns the smithy-cpp runtime version as a semantic version string.
std::string_view Version();

}  // namespace smithy

#endif  // SMITHY_CORE_VERSION_H_
