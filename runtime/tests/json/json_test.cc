#include "smithy/json/json.h"

#include <gtest/gtest.h>

namespace smithy::json {
namespace {

Document CityDocument() {
  DocumentMap coords;
  coords.emplace("latitude", Document(47.6));
  coords.emplace("longitude", Document(-122.3));
  DocumentMap map;
  map.emplace("name", Document("Seattle"));
  map.emplace("coordinates", Document(std::move(coords)));
  map.emplace("population", Document(750000));
  map.emplace("rainy", Document(true));
  map.emplace("nickname", Document(nullptr));
  return Document(std::move(map));
}

TEST(JsonTest, EncodesDeterministically) {
  EXPECT_EQ(Encode(CityDocument()),
            R"({"coordinates":{"latitude":47.6,"longitude":-122.3},)"
            R"("name":"Seattle","nickname":null,"population":750000,"rainy":true})");
}

TEST(JsonTest, RoundTripsJsonNativeNodes) {
  const Document original = CityDocument();
  const auto decoded = Decode(Encode(original));
  ASSERT_TRUE(decoded.ok()) << decoded.error().message();
  EXPECT_EQ(*decoded, original);
}

TEST(JsonTest, EncodesBlobAsBase64) {
  DocumentMap map;
  map.emplace("payload", Document(Blob::FromString("foobar")));
  EXPECT_EQ(Encode(Document(std::move(map))), R"({"payload":"Zm9vYmFy"})");
}

TEST(JsonTest, EncodesTimestampsPerFormat) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238000);
  EXPECT_EQ(Encode(Document::FromTimestamp(ts, TimestampFormat::kEpochSeconds)), "1398796238");
  EXPECT_EQ(Encode(Document::FromTimestamp(ts, TimestampFormat::kDateTime)),
            R"("2014-04-29T18:30:38Z")");
  EXPECT_EQ(Encode(Document::FromTimestamp(ts, TimestampFormat::kHttpDate)),
            R"("Tue, 29 Apr 2014 18:30:38 GMT")");
  const auto fractional = Timestamp::FromEpochMilliseconds(1398796238500);
  EXPECT_EQ(Encode(Document::FromTimestamp(fractional, TimestampFormat::kEpochSeconds)),
            "1398796238.5");
}

TEST(JsonTest, DecodesNumbers) {
  const auto doc = Decode(R"({"int":-3,"big":9223372036854775807,"float":0.5})");
  ASSERT_TRUE(doc.ok());
  EXPECT_EQ(doc->Find("int")->as_int(), -3);
  EXPECT_EQ(doc->Find("big")->as_int(), 9223372036854775807LL);
  EXPECT_DOUBLE_EQ(doc->Find("float")->as_double(), 0.5);
}

TEST(JsonTest, RejectsUint64Overflow) { EXPECT_FALSE(Decode("18446744073709551615").ok()); }

TEST(JsonTest, RejectsMalformedText) {
  const char* bad[] = {"", "{", "[1,", R"({"a":})", "tru"};
  for (const char* text : bad) {
    EXPECT_FALSE(Decode(text).ok()) << text;
  }
}

TEST(JsonTest, DecodesNestedLists) {
  const auto doc = Decode(R"([1,[2,"three"],null])");
  ASSERT_TRUE(doc.ok());
  ASSERT_TRUE(doc->is_list());
  EXPECT_EQ(doc->as_list()[1].as_list()[1].as_string(), "three");
  EXPECT_TRUE(doc->as_list()[2].is_null());
}

}  // namespace
}  // namespace smithy::json
