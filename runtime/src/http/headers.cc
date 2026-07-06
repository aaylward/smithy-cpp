#include "smithy/http/headers.h"

#include <algorithm>

namespace smithy::http {
namespace {

char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

bool HeaderNameEquals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return AsciiLower(x) == AsciiLower(y);
         });
}

std::optional<std::string> Headers::Get(std::string_view name) const {
  for (const auto& [key, value] : entries_) {
    if (HeaderNameEquals(key, name)) return value;
  }
  return std::nullopt;
}

std::vector<std::string> Headers::GetAll(std::string_view name) const {
  std::vector<std::string> values;
  for (const auto& [key, value] : entries_) {
    if (HeaderNameEquals(key, name)) values.push_back(value);
  }
  return values;
}

bool Headers::Has(std::string_view name) const { return Get(name).has_value(); }

void Headers::Set(std::string_view name, std::string_view value) {
  Remove(name);
  Add(name, value);
}

void Headers::Add(std::string_view name, std::string_view value) {
  entries_.emplace_back(std::string(name), std::string(value));
}

void Headers::Remove(std::string_view name) {
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [&](const auto& entry) { return HeaderNameEquals(entry.first, name); }),
      entries_.end());
}

}  // namespace smithy::http
