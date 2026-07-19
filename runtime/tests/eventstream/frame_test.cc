// Pins ADR-0014's framing codec: byte-exact vectors against an independent
// CRC32 implementation (python zlib produced the expected bytes), a
// round-trip across every header wire type, the incremental feed-me-more
// contract, and the hostile bank — every corrupted byte must surface as an
// error, never as a wrong message.

#include "smithy/eventstream/frame.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace smithy::eventstream {
namespace {

std::string EncodeOrDie(const Message& message) {
  auto frame = EncodeMessage(message);
  EXPECT_TRUE(frame.ok()) << frame.error().message();
  return frame.ok() ? *frame : std::string();
}

DecodedFrame DecodeOrDie(std::string_view buffer) {
  auto decoded = DecodeMessage(buffer);
  EXPECT_TRUE(decoded.ok()) << decoded.error().message();
  EXPECT_TRUE(decoded.ok() && decoded->has_value());
  return decoded.ok() && decoded->has_value() ? std::move(**decoded) : DecodedFrame{};
}

// Test-local CRC32 (zlib polynomial) for crafting hostile frames whose
// preludes must still pass the CRC gate; deliberately independent of the
// codec's internal implementation.
std::uint32_t TestCrc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : bytes) {
    crc ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void AppendU32(std::string& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

// A frame with the given declared lengths, both CRCs valid, sized to the
// declared total — for reaching the bounds checks behind the CRC gate.
std::string CraftFrame(std::uint32_t total, std::uint32_t headers_length) {
  std::string frame;
  AppendU32(frame, total);
  AppendU32(frame, headers_length);
  AppendU32(frame, TestCrc32(frame));
  if (total >= 16) {
    frame.resize(total - 4, '\0');
    AppendU32(frame, TestCrc32(frame));
  }
  return frame;
}

TEST(EventStreamFrameTest, TheEmptyMessageIsByteExact) {
  // Independent vector: python zlib computed both CRCs.
  const std::vector<std::uint8_t> expected = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
                                              0x05, 0xC2, 0x48, 0xEB, 0x7D, 0x98, 0xC8, 0xFF};
  const std::string frame = EncodeOrDie(Message{});
  ASSERT_EQ(frame.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(static_cast<std::uint8_t>(frame[i]), expected[i]) << "byte " << i;
  }
  const auto decoded = DecodeOrDie(frame);
  EXPECT_TRUE(decoded.message.headers.empty());
  EXPECT_EQ(decoded.message.payload.size(), 0u);
  EXPECT_EQ(decoded.bytes_consumed, 16u);
}

TEST(EventStreamFrameTest, AStringHeaderAndPayloadAreByteExact) {
  const std::vector<std::uint8_t> expected = {
      0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x13, 0xE8, 0xBD, 0x3E, 0xC3, 0x0B,
      0x3A, 0x65, 0x76, 0x65, 0x6E, 0x74, 0x2D, 0x74, 0x79, 0x70, 0x65, 0x07, 0x00,
      0x04, 0x63, 0x68, 0x61, 0x74, 0x68, 0x69, 0xDD, 0xF4, 0xE3, 0xAC};
  const std::string frame =
      EncodeOrDie(Message{.headers = {{":event-type", HeaderValue{std::string("chat")}}},
                          .payload = Blob::FromString("hi")});
  ASSERT_EQ(frame.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(static_cast<std::uint8_t>(frame[i]), expected[i]) << "byte " << i;
  }
}

TEST(EventStreamFrameTest, EveryHeaderTypeRoundTrips) {
  Uuid uuid;
  for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
    uuid.bytes[i] = static_cast<std::uint8_t>(i * 17);
  }
  const Message message{
      .headers =
          {
              {"t", HeaderValue{true}},
              {"f", HeaderValue{false}},
              {"byte", HeaderValue{std::int8_t{-7}}},
              {"short", HeaderValue{std::int16_t{-30000}}},
              {"int", HeaderValue{std::int32_t{-2000000000}}},
              {"long", HeaderValue{std::int64_t{-9000000000000000000LL}}},
              {"bytes", HeaderValue{Blob::FromString(std::string("\x00\xFF\x7F", 3))}},
              {"string", HeaderValue{std::string("héllo")}},
              {"when", HeaderValue{Timestamp{1721400000000}}},
              {"id", HeaderValue{uuid}},
          },
      .payload = Blob::FromString("payload bytes")};
  const std::string frame = EncodeOrDie(message);
  const auto decoded = DecodeOrDie(frame);
  EXPECT_EQ(decoded.message, message);
  EXPECT_EQ(decoded.bytes_consumed, frame.size());
}

TEST(EventStreamFrameTest, EveryStrictPrefixAsksForMoreBytes) {
  // The incremental contract: an incomplete buffer is never an error.
  const std::string frame =
      EncodeOrDie(Message{.headers = {{":event-type", HeaderValue{std::string("chat")}}},
                          .payload = Blob::FromString("hi")});
  for (std::size_t length = 0; length < frame.size(); ++length) {
    const auto decoded = DecodeMessage(std::string_view(frame).substr(0, length));
    ASSERT_TRUE(decoded.ok()) << "prefix " << length << ": " << decoded.error().message();
    EXPECT_FALSE(decoded->has_value()) << "prefix " << length;
  }
  EXPECT_TRUE(DecodeMessage(frame).ok());
}

TEST(EventStreamFrameTest, EveryFlippedByteIsAnErrorNeverAWrongMessage) {
  // The CRCs' whole job: corruption surfaces as an error, not as content.
  const Message message{.headers = {{":event-type", HeaderValue{std::string("chat")}},
                                    {"n", HeaderValue{std::int32_t{42}}}},
                        .payload = Blob::FromString("payload")};
  const std::string frame = EncodeOrDie(message);
  for (std::size_t i = 0; i < frame.size(); ++i) {
    std::string corrupted = frame;
    corrupted[i] = static_cast<char>(corrupted[i] ^ 0x01);
    const auto decoded = DecodeMessage(corrupted);
    if (decoded.ok() && decoded->has_value()) {
      // A flip in the total-length field may legitimately turn the buffer
      // into "incomplete" (larger total) — but never into a DIFFERENT
      // successfully-decoded message.
      ADD_FAILURE() << "flipped byte " << i << " decoded as a message";
    }
  }
}

TEST(EventStreamFrameTest, BackToBackFramesDecodeInOrder) {
  const std::string first = EncodeOrDie(Message{.payload = Blob::FromString("one")});
  const std::string second = EncodeOrDie(
      Message{.headers = {{"n", HeaderValue{std::int8_t{2}}}}, .payload = Blob::FromString("two")});
  std::string buffer = first + second;

  const auto a = DecodeOrDie(buffer);
  EXPECT_EQ(a.bytes_consumed, first.size());
  EXPECT_EQ(a.message.payload.ToString(), "one");
  const auto b = DecodeOrDie(std::string_view(buffer).substr(a.bytes_consumed));
  EXPECT_EQ(b.bytes_consumed, second.size());
  EXPECT_EQ(b.message.payload.ToString(), "two");
}

TEST(EventStreamFrameTest, DeclaredLengthsOutOfBoundsAreRejectedBehindTheCrcGate) {
  // CraftFrame produces valid CRCs, so these reach the bounds checks —
  // which must reject before any buffering decision trusts the lengths.
  const auto too_small = DecodeMessage(CraftFrame(15, 0));
  EXPECT_FALSE(too_small.ok());
  const auto headers_over_block_limit = DecodeMessage(CraftFrame(1024, 512 * 1024));
  EXPECT_FALSE(headers_over_block_limit.ok());
  const auto headers_over_total = DecodeMessage(CraftFrame(32, 20));
  EXPECT_FALSE(headers_over_total.ok());

  // An over-limit declared total must reject immediately from the 12-byte
  // prelude — a decoder that waited for 2^32-ish bytes would buffer forever.
  std::string huge;
  AppendU32(huge, 0x7FFFFFFFu);
  AppendU32(huge, 0);
  AppendU32(huge, TestCrc32(huge));
  const auto over_limit = DecodeMessage(huge);
  EXPECT_FALSE(over_limit.ok());
}

TEST(EventStreamFrameTest, MalformedHeaderBlocksAreHardErrors) {
  // Craft frames whose CRCs are valid but whose header blocks are hostile;
  // the message CRC is recomputed after each mutation so only the header
  // parser can reject.
  const auto craft_with_block = [](const std::string& block) {
    std::string frame;
    AppendU32(frame, static_cast<std::uint32_t>(16 + block.size()));
    AppendU32(frame, static_cast<std::uint32_t>(block.size()));
    AppendU32(frame, TestCrc32(frame));
    frame += block;
    AppendU32(frame, TestCrc32(frame));
    return frame;
  };

  // A zero-length header name.
  EXPECT_FALSE(DecodeMessage(craft_with_block(std::string(1, '\0'))).ok());
  // A name that runs past the block.
  EXPECT_FALSE(DecodeMessage(craft_with_block(std::string(1, '\x05') + "ab")).ok());
  // An unknown wire type (10).
  EXPECT_FALSE(DecodeMessage(craft_with_block(std::string(1, '\x01') + "n" + '\x0A')).ok());
  // A string value whose declared length runs past the block.
  EXPECT_FALSE(DecodeMessage(
                   craft_with_block(std::string(1, '\x01') + "n" + '\x07' + '\x00' + '\x09' + "ab"))
                   .ok());
  // A truncated fixed-width value at the end of the block.
  EXPECT_FALSE(DecodeMessage(craft_with_block(std::string(1, '\x01') + "n" + '\x04' + "ab")).ok());
}

TEST(EventStreamFrameTest, EncodeRefusesWhatDecodeWouldReject) {
  // The symmetric-bounds contract (ADR-0014).
  const auto empty_name = EncodeMessage(Message{.headers = {{"", HeaderValue{true}}}});
  EXPECT_FALSE(empty_name.ok());
  const auto long_name =
      EncodeMessage(Message{.headers = {{std::string(256, 'n'), HeaderValue{true}}}});
  EXPECT_FALSE(long_name.ok());
  const auto oversized_value =
      EncodeMessage(Message{.headers = {{"v", HeaderValue{std::string(0x10000, 'x')}}}});
  EXPECT_FALSE(oversized_value.ok());

  // 500 headers of 255-byte values blow the 128 KiB block limit.
  Message block_bomb;
  for (int i = 0; i < 500; ++i) {
    block_bomb.headers.push_back({"h" + std::to_string(i), HeaderValue{std::string(255, 'x')}});
  }
  EXPECT_FALSE(EncodeMessage(block_bomb).ok());

  const auto payload_bomb =
      EncodeMessage(Message{.payload = Blob(std::vector<std::uint8_t>(kMaxMessageBytes, 0))});
  EXPECT_FALSE(payload_bomb.ok());

  // And the boundary itself fits: a payload sized exactly to the limit.
  const auto at_limit =
      EncodeMessage(Message{.payload = Blob(std::vector<std::uint8_t>(kMaxMessageBytes - 16, 0))});
  EXPECT_TRUE(at_limit.ok());
  EXPECT_EQ(at_limit->size(), kMaxMessageBytes);
}

}  // namespace
}  // namespace smithy::eventstream
