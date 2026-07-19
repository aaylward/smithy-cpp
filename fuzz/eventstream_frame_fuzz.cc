// Fuzz target: the event-stream framing codec (ADR-0014). Decode must never
// crash on arbitrary bytes, an accepted frame must re-encode byte-exactly
// (the CRCs make the mapping bijective), and every strict prefix of a valid
// frame must ask for more bytes rather than error — the incremental
// contract every transport will rely on.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "smithy/eventstream/frame.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto decoded = smithy::eventstream::DecodeMessage(text);
  if (!decoded.ok() || !decoded->has_value()) {
    return 0;
  }
  const auto& frame = **decoded;
  const auto reencoded = smithy::eventstream::EncodeMessage(frame.message);
  if (!reencoded.ok()) std::abort();  // decoded but not re-encodable: bounds asymmetry
  if (*reencoded != text.substr(0, frame.bytes_consumed)) std::abort();  // not byte-identical
  for (std::size_t length = 0; length < frame.bytes_consumed; ++length) {
    const auto prefix = smithy::eventstream::DecodeMessage(text.substr(0, length));
    if (!prefix.ok() || prefix->has_value()) std::abort();  // prefix must ask for more
  }
  return 0;
}
