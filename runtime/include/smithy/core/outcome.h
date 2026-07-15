#ifndef SMITHY_CORE_OUTCOME_H_
#define SMITHY_CORE_OUTCOME_H_

#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "smithy/core/error.h"
#include "smithy/core/fatal.h"

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
//
// Accessing the side that isn't held is a contract violation and terminates
// the process with the error's code and message (never a context-free
// std::bad_variant_access). Prefer value_or_die("context") over a bare
// deref when there is no ok() check in sight — the context string lands in
// the crash report.
template <typename T, typename E = Error>
class Outcome {
 public:
  Outcome(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Outcome(E error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  bool ok() const { return storage_.index() == 0; }
  explicit operator bool() const { return ok(); }

  // Precondition for value()/operator*/operator->: ok() is true. A violation
  // dies with the held error's context.
  T& value() & {
    RequireValue("Outcome::value() on error");
    return std::get<0>(storage_);
  }
  const T& value() const& {
    RequireValue("Outcome::value() on error");
    return std::get<0>(storage_);
  }
  T&& value() && {
    RequireValue("Outcome::value() on error");
    return std::get<0>(std::move(storage_));
  }

  // value() with caller-supplied context: dies as
  // "<context>: <code>: <message>" when the outcome holds an error.
  //
  //   auto client = CafeClient::Create(cfg).value_or_die("creating cafe client");
  T& value_or_die(std::string_view context) & {
    RequireValue(context);
    return std::get<0>(storage_);
  }
  const T& value_or_die(std::string_view context) const& {
    RequireValue(context);
    return std::get<0>(storage_);
  }
  T&& value_or_die(std::string_view context) && {
    RequireValue(context);
    return std::get<0>(std::move(storage_));
  }

  // Precondition: ok() is false.
  E& error() & {
    RequireError();
    return std::get<1>(storage_);
  }
  const E& error() const& {
    RequireError();
    return std::get<1>(storage_);
  }
  E&& error() && {
    RequireError();
    return std::get<1>(std::move(storage_));
  }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

  template <typename U>
  T value_or(U&& fallback) const& {
    return ok() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

  friend bool operator==(const Outcome& a, const Outcome& b) { return a.storage_ == b.storage_; }

 private:
  void RequireValue(std::string_view context) const {
    if (ok()) return;
    std::string message(context);
    if constexpr (requires(const E& e) {
                    { e.code() } -> std::convertible_to<std::string_view>;
                    { e.message() } -> std::convertible_to<std::string_view>;
                  }) {
      const E& e = std::get<1>(storage_);
      message += ": ";
      message += e.code();
      message += ": ";
      message += e.message();
    }
    internal::Fatal(message);
  }

  void RequireError() const {
    if (!ok()) return;
    internal::Fatal("Outcome::error() called on a value");
  }

  std::variant<T, E> storage_;
};

}  // namespace smithy

#endif  // SMITHY_CORE_OUTCOME_H_
