// Fuzz target: CBOR bytes -> Document -> CBOR bytes. Decode must never crash;
// anything it accepts must re-encode without throwing.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "smithy/cbor/cbor.h"
#include "smithy/core/blob.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto blob =
      smithy::Blob::FromString(std::string(reinterpret_cast<const char*>(data), size));
  auto doc = smithy::cbor::Decode(blob);
  if (doc.ok()) {
    (void)smithy::cbor::Encode(*doc);
  }
  return 0;
}
