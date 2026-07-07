// Fuzz target: URI machinery. PercentDecode must never crash, and anything it
// accepts must survive an encode -> decode round trip; the encoders must
// accept arbitrary bytes.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "smithy/http/uri.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  auto decoded = smithy::http::PercentDecode(text);
  if (decoded.ok()) {
    auto reencoded = smithy::http::EncodeQueryComponent(*decoded);
    auto redecoded = smithy::http::PercentDecode(reencoded);
    if (!redecoded.ok() || *redecoded != *decoded) __builtin_trap();
  }
  (void)smithy::http::EncodePathSegment(text);
  (void)smithy::http::EncodeGreedyPathSegment(text);
  return 0;
}
