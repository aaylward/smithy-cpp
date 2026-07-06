#include "smithy/core/document_serde.h"

#include <gtest/gtest.h>

#include "smithy/core/uuid.h"

namespace smithy {
namespace {

TEST(DocumentSerdeTest, TimestampFromAllWireShapes) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238500);

  // CBOR path: typed node, format irrelevant.
  const Document node = Document::FromTimestamp(ts, TimestampFormat::kEpochSeconds);
  ASSERT_TRUE(TimestampFromDocument(node, TimestampFormat::kDateTime).ok());
  EXPECT_EQ(*TimestampFromDocument(node, TimestampFormat::kEpochSeconds), ts);

  // JSON epoch-seconds: number.
  EXPECT_EQ(*TimestampFromDocument(Document(1398796238.5), TimestampFormat::kEpochSeconds), ts);
  EXPECT_EQ(
      *TimestampFromDocument(Document(std::int64_t{1398796238}), TimestampFormat::kEpochSeconds),
      Timestamp::FromEpochMilliseconds(1398796238000));

  // String formats.
  EXPECT_EQ(*TimestampFromDocument(Document("2014-04-29T18:30:38.5Z"), TimestampFormat::kDateTime),
            ts);
  EXPECT_EQ(
      *TimestampFromDocument(Document("Tue, 29 Apr 2014 18:30:38 GMT"), TimestampFormat::kHttpDate),
      Timestamp::FromEpochMilliseconds(1398796238000));
}

TEST(DocumentSerdeTest, TimestampRejectsMismatchedShapes) {
  EXPECT_FALSE(TimestampFromDocument(Document("123"), TimestampFormat::kEpochSeconds).ok());
  EXPECT_FALSE(TimestampFromDocument(Document(5), TimestampFormat::kDateTime).ok());
  EXPECT_FALSE(TimestampFromDocument(Document(true), TimestampFormat::kEpochSeconds).ok());
  EXPECT_FALSE(TimestampFromDocument(Document("garbage"), TimestampFormat::kDateTime).ok());
}

TEST(DocumentSerdeTest, BlobFromNodeAndBase64) {
  const Blob blob = Blob::FromString("foobar");
  EXPECT_EQ(*BlobFromDocument(Document(blob)), blob);
  EXPECT_EQ(*BlobFromDocument(Document("Zm9vYmFy")), blob);
  EXPECT_FALSE(BlobFromDocument(Document("not base64!")).ok());
  EXPECT_FALSE(BlobFromDocument(Document(7)).ok());
}

TEST(UuidTest, GeneratesCanonicalV4) {
  const std::string uuid = GenerateUuidV4();
  ASSERT_EQ(uuid.size(), 36u);
  EXPECT_EQ(uuid[8], '-');
  EXPECT_EQ(uuid[13], '-');
  EXPECT_EQ(uuid[18], '-');
  EXPECT_EQ(uuid[23], '-');
  EXPECT_EQ(uuid[14], '4');  // version nibble
  const char variant = uuid[19];
  EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b') << uuid;
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    const char c = uuid[i];
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << uuid;
  }
}

TEST(UuidTest, GeneratesDistinctValues) {
  const std::string a = GenerateUuidV4();
  const std::string b = GenerateUuidV4();
  EXPECT_NE(a, b);
}

}  // namespace
}  // namespace smithy
