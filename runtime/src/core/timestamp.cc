#include "smithy/core/timestamp.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Calendar math uses the C++20 <chrono> civil calendar types (year_month_day,
// sys_days, weekday), which are constexpr, timezone-free, and supported at our
// compiler floor. String formatting/parsing stays hand-rolled: std::format and
// std::chrono::parse for chrono types are not available across gcc 11 / clang 14
// / MSVC 19.30.

namespace smithy {
namespace {

constexpr std::int64_t kMsPerSecond = 1000;
constexpr std::int64_t kMsPerDay = 86400 * kMsPerSecond;

constexpr std::array<std::string_view, 7> kWeekdays = {"Sun", "Mon", "Tue", "Wed",
                                                       "Thu", "Fri", "Sat"};
constexpr std::array<std::string_view, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct CivilTime {
  int year = 1970;
  unsigned month = 1;  // 1-12
  unsigned day = 1;    // 1-31
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millisecond = 0;

  // False for out-of-range fields, including day-of-month vs. leap years.
  bool ok() const {
    return ymd().ok() && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 &&
           second <= 59;
  }

  std::chrono::year_month_day ymd() const {
    return std::chrono::year{year} / static_cast<int>(month) / static_cast<int>(day);
  }
};

std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
  return a / b - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0);
}

std::int64_t EpochDays(const CivilTime& c) {
  return std::chrono::sys_days(c.ymd()).time_since_epoch().count();
}

CivilTime Decompose(std::int64_t ms) {
  const std::int64_t days = FloorDiv(ms, kMsPerDay);
  std::int64_t ms_of_day = ms - days * kMsPerDay;

  const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
  CivilTime civil;
  civil.year = static_cast<int>(ymd.year());
  civil.month = static_cast<unsigned>(ymd.month());
  civil.day = static_cast<unsigned>(ymd.day());
  civil.hour = static_cast<int>(ms_of_day / (3600 * kMsPerSecond));
  ms_of_day %= 3600 * kMsPerSecond;
  civil.minute = static_cast<int>(ms_of_day / (60 * kMsPerSecond));
  ms_of_day %= 60 * kMsPerSecond;
  civil.second = static_cast<int>(ms_of_day / kMsPerSecond);
  civil.millisecond = static_cast<int>(ms_of_day % kMsPerSecond);
  return civil;
}

std::int64_t Compose(const CivilTime& c) {
  return EpochDays(c) * kMsPerDay +
         (static_cast<std::int64_t>(c.hour) * 3600 + static_cast<std::int64_t>(c.minute) * 60 +
          c.second) *
             kMsPerSecond +
         c.millisecond;
}

std::size_t WeekdayIndex(std::int64_t epoch_days) {
  const std::chrono::weekday weekday{std::chrono::sys_days{std::chrono::days{epoch_days}}};
  return weekday.c_encoding();  // 0 = Sunday, matching kWeekdays.
}

// Appends ".<fraction>" with trailing zeros trimmed, or nothing when ms == 0.
void AppendFraction(std::string* out, int ms) {
  if (ms == 0) return;
  std::array<char, 8> buffer{};
  std::snprintf(buffer.data(), buffer.size(), ".%03d", ms);
  std::string_view fraction(buffer.data());
  while (fraction.ends_with('0')) fraction.remove_suffix(1);
  out->append(fraction);
}

bool ParseDigits(std::string_view text, std::size_t pos, std::size_t count, int* out) {
  if (pos + count > text.size()) return false;
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[pos + i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

bool ParseDigits(std::string_view text, std::size_t pos, std::size_t count, unsigned* out) {
  int value = 0;
  if (!ParseDigits(text, pos, count, &value)) return false;
  *out = static_cast<unsigned>(value);
  return true;
}

Outcome<Timestamp> ParseDateTime(std::string_view text) {
  const auto invalid = [&] {
    return Error::Serialization("timestamp: invalid date-time: " + std::string(text));
  };
  CivilTime c;
  if (!ParseDigits(text, 0, 4, &c.year) || text.size() < 20 || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':') {
    return invalid();
  }
  if (!ParseDigits(text, 5, 2, &c.month) || !ParseDigits(text, 8, 2, &c.day) ||
      !ParseDigits(text, 11, 2, &c.hour) || !ParseDigits(text, 14, 2, &c.minute) ||
      !ParseDigits(text, 17, 2, &c.second)) {
    return invalid();
  }
  std::size_t pos = 19;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    const std::size_t fraction_start = pos;
    int scale = 100;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (pos - fraction_start < 3) {
        c.millisecond += (text[pos] - '0') * scale;
        scale /= 10;
      }
      ++pos;
    }
    if (pos == fraction_start) return invalid();
  }
  // Offset: 'Z' or ±hh:mm.
  std::int64_t offset_ms = 0;
  if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
    ++pos;
  } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    const bool negative = text[pos] == '-';
    int oh = 0;
    int om = 0;
    if (!ParseDigits(text, pos + 1, 2, &oh) || pos + 3 >= text.size() || text[pos + 3] != ':' ||
        !ParseDigits(text, pos + 4, 2, &om) || oh > 23 || om > 59) {
      return invalid();
    }
    offset_ms =
        (static_cast<std::int64_t>(oh) * 3600 + static_cast<std::int64_t>(om) * 60) * kMsPerSecond;
    if (negative) offset_ms = -offset_ms;
    pos += 6;
  } else {
    return invalid();
  }
  if (pos != text.size() || !c.ok()) return invalid();
  return Timestamp::FromEpochMilliseconds(Compose(c) - offset_ms);
}

Outcome<Timestamp> ParseHttpDate(std::string_view text) {
  const auto invalid = [&] {
    return Error::Serialization("timestamp: invalid http-date: " + std::string(text));
  };
  // IMF-fixdate is fixed-width: "Sun, 06 Nov 1994 08:49:37 GMT" (29 chars).
  if (text.size() != 29 || text.substr(3, 2) != ", " || text[7] != ' ' || text[11] != ' ' ||
      text[16] != ' ' || text[19] != ':' || text[22] != ':' || text.substr(25) != " GMT") {
    return invalid();
  }
  CivilTime c;
  if (!ParseDigits(text, 5, 2, &c.day) || !ParseDigits(text, 12, 4, &c.year) ||
      !ParseDigits(text, 17, 2, &c.hour) || !ParseDigits(text, 20, 2, &c.minute) ||
      !ParseDigits(text, 23, 2, &c.second)) {
    return invalid();
  }
  c.month = 0;
  const std::string_view month_text = text.substr(8, 3);
  for (std::size_t i = 0; i < kMonths.size(); ++i) {
    if (kMonths[i] == month_text) c.month = static_cast<unsigned>(i) + 1;
  }
  if (c.month == 0 || !c.ok()) return invalid();
  if (kWeekdays[WeekdayIndex(EpochDays(c))] != text.substr(0, 3)) return invalid();
  return Timestamp::FromEpochMilliseconds(Compose(c));
}

Outcome<Timestamp> ParseEpochSeconds(std::string_view text) {
  const std::string buffer(text);
  const char* begin = buffer.c_str();
  char* end = nullptr;
  errno = 0;
  const double seconds = std::strtod(begin, &end);
  if (end != begin + buffer.size() || buffer.empty() || errno == ERANGE ||
      !std::isfinite(seconds)) {
    return Error::Serialization("timestamp: invalid epoch-seconds: " + buffer);
  }
  return Timestamp::FromEpochSeconds(seconds);
}

}  // namespace

Timestamp Timestamp::FromEpochSeconds(double seconds) {
  return Timestamp(static_cast<std::int64_t>(std::llround(seconds * 1000.0)));
}

std::string Timestamp::Format(TimestampFormat format) const {
  const CivilTime c = Decompose(ms_);
  std::array<char, 40> buffer{};
  std::string out;
  switch (format) {
    case TimestampFormat::kEpochSeconds: {
      const std::int64_t whole_seconds = FloorDiv(ms_, kMsPerSecond);
      out = std::to_string(whole_seconds);
      AppendFraction(&out, static_cast<int>(ms_ - whole_seconds * kMsPerSecond));
      return out;
    }
    case TimestampFormat::kDateTime: {
      std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02uT%02d:%02d:%02d", c.year, c.month,
                    c.day, c.hour, c.minute, c.second);
      out = buffer.data();
      AppendFraction(&out, c.millisecond);
      out.push_back('Z');
      return out;
    }
    case TimestampFormat::kHttpDate: {
      const std::size_t weekday = WeekdayIndex(FloorDiv(ms_, kMsPerDay));
      std::snprintf(buffer.data(), buffer.size(), "%s, %02u %s %04d %02d:%02d:%02d GMT",
                    kWeekdays[weekday].data(), c.day, kMonths[c.month - 1].data(), c.year, c.hour,
                    c.minute, c.second);
      return buffer.data();
    }
  }
  return out;  // Unreachable with a valid format.
}

Outcome<Timestamp> Timestamp::Parse(std::string_view text, TimestampFormat format) {
  switch (format) {
    case TimestampFormat::kEpochSeconds:
      return ParseEpochSeconds(text);
    case TimestampFormat::kDateTime:
      return ParseDateTime(text);
    case TimestampFormat::kHttpDate:
      return ParseHttpDate(text);
  }
  return Error::Serialization("timestamp: unknown format");
}

}  // namespace smithy
