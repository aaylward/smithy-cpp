#ifndef SMITHY_CLIENT_PAGINATION_H_
#define SMITHY_CLIENT_PAGINATION_H_

#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>

#include "smithy/core/outcome.h"

namespace smithy {

// Single-pass input iterator over a generated paginator, surfaced through
// the paginator's begin()/end() so range-for works (issue #49):
//
//   for (auto& page : client.PaginateListCities({.pageSize = 100})) {
//     if (!page.ok()) return page.error();  // the range ends by itself next
//     for (const auto& city : page->items) Process(city);
//   }
//
// Iteration yields Outcome<Page>&. A failed call is yielded exactly once and
// then the range ends without calling Next() again, mirroring the
// paginator's stop-on-first-error contract. Single-pass: the iterator drives
// the paginator's state, so call begin() once per paginator.
template <typename Paginator>
class PageIterator {
 public:
  using Page = typename Paginator::Page;
  using iterator_category = std::input_iterator_tag;
  using value_type = Outcome<Page>;
  using difference_type = std::ptrdiff_t;
  using pointer = Outcome<Page>*;
  using reference = Outcome<Page>&;

  // The past-the-end iterator.
  PageIterator() = default;
  // Fetches the first page immediately.
  explicit PageIterator(Paginator* paginator) : paginator_(paginator) { Advance(); }

  Outcome<Page>& operator*() { return *current_; }
  Outcome<Page>* operator->() { return &*current_; }

  PageIterator& operator++() {
    if (current_.has_value() && !current_->ok()) {
      // The error was yielded; end the range without touching Next() again.
      paginator_ = nullptr;
      current_.reset();
    } else {
      Advance();
    }
    return *this;
  }

  friend bool operator==(const PageIterator& a, const PageIterator& b) {
    return a.paginator_ == b.paginator_ && a.current_.has_value() == b.current_.has_value();
  }

 private:
  void Advance() {
    auto next = paginator_->Next();
    if (!next.ok()) {
      current_.emplace(std::move(next).error());
      return;
    }
    if (!next->has_value()) {
      paginator_ = nullptr;
      current_.reset();
      return;
    }
    current_.emplace(*std::move(*next));
  }

  Paginator* paginator_ = nullptr;
  std::optional<Outcome<Page>> current_;
};

}  // namespace smithy

#endif  // SMITHY_CLIENT_PAGINATION_H_
