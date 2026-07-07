#ifndef SMITHY_HTTP_HEADERS_H_
#define SMITHY_HTTP_HEADERS_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smithy::http {

// HTTP header collection: case-insensitive names, repeated names preserved in
// insertion order (as list-valued headers require).
class Headers {
 public:
  // First value for the name, if present.
  std::optional<std::string> Get(std::string_view name) const;
  std::vector<std::string> GetAll(std::string_view name) const;
  bool Has(std::string_view name) const;

  // Replaces every existing value for the name.
  void Set(std::string_view name, std::string_view value);
  // Appends without touching existing values.
  void Add(std::string_view name, std::string_view value);
  void Remove(std::string_view name);

  const std::vector<std::pair<std::string, std::string>>& entries() const { return entries_; }
  std::size_t size() const { return entries_.size(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

// ASCII case-insensitive equality, as HTTP header names require.
bool HeaderNameEquals(std::string_view a, std::string_view b);

// Splits a comma-separated list-valued header into entries with surrounding
// whitespace trimmed ("a, b,c" -> {"a", "b", "c"}). Quoted-string entries are
// returned verbatim, quotes included — unescaping is the caller's concern.
std::vector<std::string> SplitHeaderListValues(std::string_view value);

// The media type of a Content-Type value: parameters and surrounding
// whitespace stripped, lowercased ("Application/JSON; charset=utf-8" ->
// "application/json").
std::string MediaTypeOf(std::string_view content_type);

// Splits a list-valued header of HTTP-dates, which themselves contain one
// comma ("Mon, 16 Dec 2019 23:48:18 GMT, Tue, 17 Dec ..."): consecutive
// comma-separated tokens are re-joined two at a time.
std::vector<std::string> SplitHttpDateHeaderValues(std::string_view value);

}  // namespace smithy::http

#endif  // SMITHY_HTTP_HEADERS_H_
