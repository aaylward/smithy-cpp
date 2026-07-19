#ifndef SMITHY_EVENTSTREAM_FRAME_H_
#define SMITHY_EVENTSTREAM_FRAME_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "smithy/core/blob.h"
#include "smithy/core/outcome.h"

namespace smithy::eventstream {

// The event-stream message framing the protocol specs define event streams
// against (ADR-0014): a CRC-guarded prelude, a typed header block, and an
// opaque payload. This layer carries protocol bytes; it does not interpret
// them.
//
//   [ total length (u32 BE) | headers length (u32 BE) | prelude CRC32 ]
//   [ headers block ][ payload ][ message CRC32 ]
//
// Timestamps and UUIDs are distinct value types rather than aliases of
// long/byte-array, so a decoded header can never be confused across wire
// types that share a C++ representation.
struct Timestamp {
  std::int64_t epoch_millis = 0;
  friend bool operator==(const Timestamp&, const Timestamp&) = default;
};
struct Uuid {
  std::array<std::uint8_t, 16> bytes{};
  friend bool operator==(const Uuid&, const Uuid&) = default;
};

// One alternative per wire type; the two boolean wire tags collapse into
// the one bool alternative (the tag is recovered from the value on encode).
using HeaderValue = std::variant<bool, std::int8_t, std::int16_t, std::int32_t, std::int64_t, Blob,
                                 std::string, Timestamp, Uuid>;

struct Header {
  std::string name;
  HeaderValue value;
  friend bool operator==(const Header&, const Header&) = default;
};

struct Message {
  std::vector<Header> headers;
  Blob payload;
  friend bool operator==(const Message&, const Message&) = default;
};

// The format's own documented limits, enforced symmetrically: Encode
// refuses what Decode would reject, so a message that cannot round-trip
// cannot be produced. Header names are 1..255 bytes (u8 length prefix);
// string and byte-array header values are bounded by their u16 length.
inline constexpr std::size_t kMaxHeaderBlockBytes = std::size_t{128} * 1024;
inline constexpr std::size_t kMaxMessageBytes = std::size_t{16} * 1024 * 1024;

// Encodes one message as a complete frame. Validation errors (an empty or
// oversized header name, an oversized string/byte-array value, a header
// block or total frame over the limits) fail the encode — nothing partial
// is produced. Frames travel as std::string (unlike the cbor codec's Blob)
// because they splice into transport buffers, which are string-shaped;
// the payload INSIDE stays a Blob.
Outcome<std::string> EncodeMessage(const Message& message);

// One message decoded from the FRONT of `buffer`, and how many bytes its
// frame consumed (the caller drops them and calls again — the decoder is
// incremental because transports feed it from sockets).
struct DecodedFrame {
  Message message;
  std::size_t bytes_consumed = 0;
};

// nullopt means the buffer does not yet hold a complete frame: feed more
// bytes, it is never an error. Errors are permanent for the stream —
// a prelude or message CRC mismatch, a declared length outside the
// format's bounds, a header block that overruns or underruns its declared
// length, or an unknown header wire type. The prelude CRC is verified
// before any declared length is trusted; the message CRC before any
// content is surfaced.
Outcome<std::optional<DecodedFrame>> DecodeMessage(std::string_view buffer);

}  // namespace smithy::eventstream

#endif  // SMITHY_EVENTSTREAM_FRAME_H_
