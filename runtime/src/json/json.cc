#include "smithy/json/json.h"

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

#include "smithy/core/base64.h"

namespace smithy::json {
namespace {

nlohmann::json ToBackend(const Document& doc) {
  if (doc.is_null()) return nullptr;
  if (doc.is_bool()) return doc.as_bool();
  if (doc.is_int()) return doc.as_int();
  if (doc.is_double()) return doc.as_double();
  if (doc.is_string()) return doc.as_string();
  if (doc.is_blob()) return Base64Encode(doc.as_blob());
  if (doc.is_timestamp()) {
    const TimestampValue& ts = doc.as_timestamp();
    if (ts.format == TimestampFormat::kEpochSeconds) {
      const double seconds = ts.value.epoch_seconds();
      const auto whole = static_cast<std::int64_t>(seconds);
      if (static_cast<double>(whole) == seconds) return whole;
      return seconds;
    }
    return ts.value.Format(ts.format);
  }
  if (doc.is_list()) {
    nlohmann::json out = nlohmann::json::array();
    for (const Document& item : doc.as_list()) out.push_back(ToBackend(item));
    return out;
  }
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [key, value] : doc.as_map()) out[key] = ToBackend(value);
  return out;
}

Outcome<Document> FromBackend(const nlohmann::json& value) {
  switch (value.type()) {
    case nlohmann::json::value_t::null:
      return Document(nullptr);
    case nlohmann::json::value_t::boolean:
      return Document(value.get<bool>());
    case nlohmann::json::value_t::number_integer:
      return Document(value.get<std::int64_t>());
    case nlohmann::json::value_t::number_unsigned: {
      const auto raw = value.get<std::uint64_t>();
      if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Error::Serialization("json: integer exceeds int64 range");
      }
      return Document(static_cast<std::int64_t>(raw));
    }
    case nlohmann::json::value_t::number_float:
      return Document(value.get<double>());
    case nlohmann::json::value_t::string:
      return Document(value.get<std::string>());
    case nlohmann::json::value_t::array: {
      DocumentList list;
      list.reserve(value.size());
      for (const auto& item : value) {
        auto converted = FromBackend(item);
        if (!converted) return std::move(converted).error();
        list.push_back(std::move(*converted));
      }
      return Document(std::move(list));
    }
    case nlohmann::json::value_t::object: {
      DocumentMap map;
      for (const auto& [key, item] : value.items()) {
        auto converted = FromBackend(item);
        if (!converted) return std::move(converted).error();
        map.insert_or_assign(key, std::move(*converted));
      }
      return Document(std::move(map));
    }
    default:
      return Error::Serialization("json: unsupported value type");
  }
}

}  // namespace

std::string Encode(const Document& doc) { return ToBackend(doc).dump(); }

Outcome<Document> Decode(std::string_view text) {
  const nlohmann::json parsed = nlohmann::json::parse(text, /*cb=*/nullptr,
                                                      /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return Error::Serialization("json: malformed document");
  }
  return FromBackend(parsed);
}

}  // namespace smithy::json
