#ifndef SMITHY_CORE_BOXED_H_
#define SMITHY_CORE_BOXED_H_

#include <memory>
#include <utility>

namespace smithy {

// Value-semantic heap indirection for recursive generated members (a
// structure member whose target refers back to its container). Copy is a
// deep copy and equality compares the pointed-to values, so generated
// structs keep their defaulted copy/equality semantics. Never null except
// moved-from; using a moved-from Boxed (other than assigning to it or
// destroying it) is undefined behavior.
//
// The template compiles with an incomplete T: member bodies only
// instantiate at their call sites, where the generated types header has
// completed every type.
template <typename T>
class Boxed {
 public:
  Boxed() : value_(std::make_unique<T>()) {}
  Boxed(T value) : value_(std::make_unique<T>(std::move(value))) {}  // NOLINT
  Boxed(const Boxed& other) : value_(std::make_unique<T>(*other.value_)) {}
  Boxed(Boxed&&) noexcept = default;
  Boxed& operator=(const Boxed& other) {
    if (this != &other) value_ = std::make_unique<T>(*other.value_);
    return *this;
  }
  Boxed& operator=(Boxed&&) noexcept = default;
  Boxed& operator=(T value) {
    value_ = std::make_unique<T>(std::move(value));
    return *this;
  }
  ~Boxed() = default;

  T& operator*() { return *value_; }
  const T& operator*() const { return *value_; }
  T* operator->() { return value_.get(); }
  const T* operator->() const { return value_.get(); }

  friend bool operator==(const Boxed& a, const Boxed& b) { return *a.value_ == *b.value_; }
  friend bool operator!=(const Boxed& a, const Boxed& b) { return !(a == b); }

 private:
  std::unique_ptr<T> value_;
};

}  // namespace smithy

#endif  // SMITHY_CORE_BOXED_H_
