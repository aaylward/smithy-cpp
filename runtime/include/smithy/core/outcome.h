#ifndef SMITHY_CORE_OUTCOME_H_
#define SMITHY_CORE_OUTCOME_H_

#include <utility>
#include <variant>

#include "smithy/core/error.h"

namespace smithy {

// Marker value for operations that succeed without producing anything.
struct Unit {
  friend bool operator==(Unit /*lhs*/, Unit /*rhs*/) { return true; }
};

// Result of an operation that can fail: holds either a value T or an error E.
// std::expected-like; the representation switches to std::expected once the
// compiler floor reaches C++23 (ADR-0003).
//
//   Outcome<int> Parse(std::string_view s);
//   if (auto n = Parse(s)) use(*n); else log(n.error().message());
template <typename T, typename E = Error>
class Outcome {
 public:
  Outcome(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Outcome(E error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  bool ok() const { return storage_.index() == 0; }
  explicit operator bool() const { return ok(); }

  // Precondition for value()/operator*/operator->: ok() is true.
  T& value() & { return std::get<0>(storage_); }
  const T& value() const& { return std::get<0>(storage_); }
  T&& value() && { return std::get<0>(std::move(storage_)); }

  // Precondition: ok() is false.
  E& error() & { return std::get<1>(storage_); }
  const E& error() const& { return std::get<1>(storage_); }
  E&& error() && { return std::get<1>(std::move(storage_)); }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

  template <typename U>
  T value_or(U&& fallback) const& {
    return ok() ? value() : static_cast<T>(std::forward<U>(fallback));
  }

  friend bool operator==(const Outcome& a, const Outcome& b) { return a.storage_ == b.storage_; }

 private:
  std::variant<T, E> storage_;
};

}  // namespace smithy

#endif  // SMITHY_CORE_OUTCOME_H_
