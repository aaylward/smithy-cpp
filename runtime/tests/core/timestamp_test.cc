#include "smithy/core/timestamp.h"

#include <gtest/gtest.h>

namespace smithy {
namespace {

TEST(TimestampParseTest, EpochSecondsIsStrict) {
  using smithy::Timestamp;
  using smithy::TimestampFormat;
  EXPECT_TRUE(Timestamp::Parse("1515531081", TimestampFormat::kEpochSeconds).ok());
  EXPECT_TRUE(Timestamp::Parse("1515531081.123", TimestampFormat::kEpochSeconds).ok());
  EXPECT_TRUE(Timestamp::Parse("-5", TimestampFormat::kEpochSeconds).ok());
  for (const char* bad :
       {"0x42", "1e3", "Infinity", "NaN", "true", "1515531081ABC", "1.", ".5", "-", "1.2.3", ""}) {
    EXPECT_FALSE(Timestamp::Parse(bad, TimestampFormat::kEpochSeconds).ok()) << bad;
  }
}

TEST(TimestampTest, FormatsDateTime) {
  // 1985-04-12T23:20:50.520Z, the Smithy spec's canonical example.
  const auto ts = Timestamp::FromEpochMilliseconds(482196050520);
  EXPECT_EQ(ts.Format(TimestampFormat::kDateTime), "1985-04-12T23:20:50.52Z");
}

TEST(TimestampTest, FormatsDateTimeWithoutFraction) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238000);
  EXPECT_EQ(ts.Format(TimestampFormat::kDateTime), "2014-04-29T18:30:38Z");
}

TEST(TimestampTest, ParsesDateTime) {
  const auto ts = Timestamp::Parse("1985-04-12T23:20:50.52Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok()) << ts.error().message();
  EXPECT_EQ(ts->epoch_milliseconds(), 482196050520);
}

TEST(TimestampTest, ParsesDateTimeWithOffset) {
  const auto with_offset =
      Timestamp::Parse("2014-04-29T20:30:38+02:00", TimestampFormat::kDateTime);
  ASSERT_TRUE(with_offset.ok());
  EXPECT_EQ(with_offset->epoch_milliseconds(), 1398796238000);
}

TEST(TimestampTest, ParsesLowercaseSeparators) {
  const auto ts = Timestamp::Parse("2014-04-29t18:30:38z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts->epoch_milliseconds(), 1398796238000);
}

TEST(TimestampTest, TruncatesSubMillisecondDigits) {
  const auto ts = Timestamp::Parse("1985-04-12T23:20:50.52099Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts->epoch_milliseconds(), 482196050520);
}

TEST(TimestampTest, RejectsInvalidDateTime) {
  const char* bad[] = {
      "1985-04-12",                        // date only
      "1985-04-12T23:20:50",               // missing offset
      "1985-04-31T23:20:50Z",              // April has 30 days
      "1985-02-29T00:00:00Z",              // not a leap year
      "1985-13-12T23:20:50Z",              // month 13
      "1985-04-12T24:00:00Z",              // hour 24
      "1985-04-12T23:20:60Z",              // leap second unsupported
      "1985-04-12T23:20:50.Z",             // empty fraction
      "1985-04-12T23:20:50Zjunk",          // trailing junk
      "1985-04-12T23:20:50+2:00",          // malformed offset
      "trailer 1985-04-12T23:20:50.52Z 1"  // garbage
  };
  for (const char* text : bad) {
    EXPECT_FALSE(Timestamp::Parse(text, TimestampFormat::kDateTime).ok()) << text;
  }
}

TEST(TimestampTest, LeapDayRoundTrips) {
  const auto ts = Timestamp::Parse("2024-02-29T12:00:00Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts->Format(TimestampFormat::kDateTime), "2024-02-29T12:00:00Z");
}

TEST(TimestampTest, PreEpochRoundTrips) {
  const auto ts = Timestamp::Parse("1969-07-20T20:17:40Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_LT(ts->epoch_milliseconds(), 0);
  EXPECT_EQ(ts->Format(TimestampFormat::kDateTime), "1969-07-20T20:17:40Z");
  EXPECT_EQ(ts->Format(TimestampFormat::kHttpDate), "Sun, 20 Jul 1969 20:17:40 GMT");
}

TEST(TimestampTest, FormatsAndParsesHttpDate) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238000);
  const std::string text = ts.Format(TimestampFormat::kHttpDate);
  EXPECT_EQ(text, "Tue, 29 Apr 2014 18:30:38 GMT");
  const auto parsed = Timestamp::Parse(text, TimestampFormat::kHttpDate);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(*parsed, ts);
}

TEST(TimestampTest, RejectsInvalidHttpDate) {
  const char* bad[] = {"Wed, 29 Apr 2014 18:30:38 GMT",   // 2014-04-29 was a Tuesday
                       "Tue, 29 Apr 2014 18:30:38 UTC",   // zone must be GMT
                       "Tue, 29 Abc 2014 18:30:38 GMT",   // bad month
                       "Tuesday, 29 Apr 2014 18:30 GMT",  // not IMF-fixdate
                       ""};
  for (const char* text : bad) {
    EXPECT_FALSE(Timestamp::Parse(text, TimestampFormat::kHttpDate).ok()) << text;
  }
}

TEST(TimestampTest, EpochSecondsRoundTrips) {
  const auto ts = Timestamp::FromEpochMilliseconds(1515531081123);
  EXPECT_EQ(ts.Format(TimestampFormat::kEpochSeconds), "1515531081.123");
  const auto parsed = Timestamp::Parse("1515531081.123", TimestampFormat::kEpochSeconds);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(*parsed, ts);
}

TEST(TimestampTest, EpochSecondsIntegral) {
  const auto ts = Timestamp::FromEpochMilliseconds(1515531081000);
  EXPECT_EQ(ts.Format(TimestampFormat::kEpochSeconds), "1515531081");
}

TEST(TimestampTest, EpochSecondsPreEpochFormat) {
  // -0.5s => floor(seconds) = -1 with fraction .5 => "-0.5".
  const auto ts = Timestamp::FromEpochMilliseconds(-500);
  const auto parsed = Timestamp::Parse("-0.5", TimestampFormat::kEpochSeconds);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(*parsed, ts);
}

TEST(TimestampTest, RejectsInvalidEpochSeconds) {
  const char* bad[] = {"", "abc", "1.2.3", "1e999", "nan", "inf"};
  for (const char* text : bad) {
    EXPECT_FALSE(Timestamp::Parse(text, TimestampFormat::kEpochSeconds).ok()) << text;
  }
}

}  // namespace
}  // namespace smithy
