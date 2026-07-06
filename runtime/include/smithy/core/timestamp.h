#ifndef SMITHY_CORE_TIMESTAMP_H_
#define SMITHY_CORE_TIMESTAMP_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "smithy/core/outcome.h"

namespace smithy {

// The three timestamp serializations defined by the Smithy spec. Which one a
// member uses is decided by the protocol and the @timestampFormat trait.
enum class TimestampFormat {
  kEpochSeconds,  // 1515531081.123  (number, fractional seconds allowed)
  kDateTime,      // 1985-04-12T23:20:50.520Z  (RFC 3339)
  kHttpDate,      // Tue, 29 Apr 2014 18:30:38 GMT  (RFC 7231 IMF-fixdate)
};

// A point in time with millisecond precision, independent of time zone.
// Portable: no locale, environment, or platform time APIs are involved.
//
//   auto ts = Timestamp::Parse("2014-04-29T18:30:38Z", TimestampFormat::kDateTime);
//   std::string http_date = ts->Format(TimestampFormat::kHttpDate);
class Timestamp {
 public:
  Timestamp() = default;

  static Timestamp FromEpochMilliseconds(std::int64_t ms) { return Timestamp(ms); }
  // Rounds to the nearest millisecond.
  static Timestamp FromEpochSeconds(double seconds);

  std::int64_t epoch_milliseconds() const { return ms_; }
  double epoch_seconds() const { return static_cast<double>(ms_) / 1000.0; }

  std::string Format(TimestampFormat format) const;
  static Outcome<Timestamp> Parse(std::string_view text, TimestampFormat format);

  friend bool operator==(Timestamp a, Timestamp b) { return a.ms_ == b.ms_; }
  friend bool operator!=(Timestamp a, Timestamp b) { return a.ms_ != b.ms_; }
  friend bool operator<(Timestamp a, Timestamp b) { return a.ms_ < b.ms_; }

 private:
  explicit Timestamp(std::int64_t ms) : ms_(ms) {}

  std::int64_t ms_ = 0;  // Milliseconds since 1970-01-01T00:00:00Z.
};

}  // namespace smithy

#endif  // SMITHY_CORE_TIMESTAMP_H_
