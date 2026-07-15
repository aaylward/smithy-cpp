#ifndef SMITHY_CORE_FATAL_H_
#define SMITHY_CORE_FATAL_H_

#include <cstdio>
#include <cstdlib>
#include <string>

namespace smithy::internal {

// Terminates the process after printing `message` to stderr. Used for
// precondition violations (an Outcome dereferenced on the wrong side, a union
// accessed as a member it doesn't hold): ADR-0003 keeps exceptions out of the
// public API, so contract violations fail fast with context instead of
// throwing std::bad_variant_access with none.
[[noreturn]] inline void Fatal(const std::string& message) {
  std::fprintf(stderr, "smithy: %s\n", message.c_str());
  std::abort();
}

// Contract violation for generated unions: as_<requested>() was called while
// `engaged` is the active member. Centralized so every generated accessor is
// one branch + one call.
[[noreturn]] inline void FatalWrongUnionAccess(const char* union_name, const char* requested,
                                               const char* engaged) {
  std::fprintf(stderr, "smithy: %s::as_%s(): engaged member is %s\n", union_name, requested,
               engaged);
  std::abort();
}

}  // namespace smithy::internal

#endif  // SMITHY_CORE_FATAL_H_
