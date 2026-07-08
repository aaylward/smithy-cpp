#include "smithy/core/timestamp.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

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

TEST(TimestampCheckedTest, AcceptsInRangeInstants) {
  const auto a = Timestamp::FromEpochSecondsChecked(1515531081.123);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->epoch_milliseconds(), 1515531081123);
  EXPECT_TRUE(Timestamp::FromEpochSecondsChecked(0.0).ok());
  EXPECT_TRUE(Timestamp::FromEpochSecondsChecked(-5.0).ok());  // pre-epoch
  EXPECT_TRUE(Timestamp::FromEpochMillisecondsChecked(0).ok());
}

TEST(TimestampCheckedTest, AcceptsTheRepresentableBoundaries) {
  // The exact edges of the RFC 3339 window round-trip.
  const auto max_dt = Timestamp::Parse("9999-12-31T23:59:59.999Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(max_dt.ok());
  EXPECT_TRUE(Timestamp::FromEpochMillisecondsChecked(max_dt->epoch_milliseconds()).ok());
  const auto min_dt = Timestamp::Parse("0000-01-01T00:00:00Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(min_dt.ok());
  EXPECT_TRUE(Timestamp::FromEpochMillisecondsChecked(min_dt->epoch_milliseconds()).ok());
}

TEST(TimestampCheckedTest, RejectsOutOfRangeAndNonFinite) {
  // The values the CBOR/JSON UB findings are about: a huge integer scaled to
  // milliseconds, an out-of-range float, and non-finite doubles.
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(1e300).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(-1e300).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(static_cast<double>(9223372036854775LL)).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(std::numeric_limits<double>::infinity()).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(std::nan("")).ok());
  // Just past year 9999 / before year 0000.
  const auto past_max = Timestamp::Parse("9999-12-31T23:59:59.999Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(past_max.ok());
  EXPECT_FALSE(Timestamp::FromEpochMillisecondsChecked(past_max->epoch_milliseconds() + 1).ok());
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
