#include "smithy/core/version.h"

namespace smithy {

// Pre-release: the 0.1.0 milestone is developed on main but not yet tagged, so
// the runtime reports a "-dev" suffix (see docs/versioning.md). This is the one
// source of truth for the product version; the client User-Agent mirrors it.
std::string_view Version() { return "0.1.0-dev"; }

}  // namespace smithy
