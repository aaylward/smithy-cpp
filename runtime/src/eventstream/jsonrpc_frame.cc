#include "smithy/eventstream/jsonrpc_frame.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

#include "smithy/core/error.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/json/json.h"

namespace smithy::eventstream {
namespace {

constexpr std::string_view kVersionMember = "jsonrpc";
constexpr std::string_view kVersionValue = "2.0";
constexpr std::string_view kMethodMember = "method";
constexpr std::string_view kParamsMember = "params";
constexpr std::string_view kIdMember = "id";
constexpr std::string_view kPayloadMember = "payload";
constexpr std::string_view kResultMember = "result";
constexpr std::string_view kErrorMember = "error";
constexpr std::string_view kCodeMember = "code";
constexpr std::string_view kMessageMember = "message";
constexpr std::string_view kDataMember = "data";
constexpr std::string_view kTypeMember = "__type";
constexpr std::string_view kFallbackExceptionType = "JsonRpcError";
constexpr std::string_view kJsonContentType = "application/json";

// Encode-side refusals are Validation (the session is untouched and stays
// usable — the WebSocket Send contract); decode-side are Serialization
// (the wire produced something malformed: the session is dead). The same
// split json_frame.cc draws.
Error CannotEncode(std::string what) {
  return Error::Validation("jsonrpc stream: " + std::move(what));
}

Error Malformed(std::string what) {
  return Error::Serialization("jsonrpc stream: " + std::move(what));
}

// The media type of a :content-type value: parameters stripped, trimmed,
// lowercased. Local on purpose — http's MediaTypeOf lives above this
// library in the dependency order (json_frame.cc's twin).
std::string MediaType(std::string_view content_type) {
  const std::size_t semicolon = content_type.find(';');
  if (semicolon != std::string_view::npos) {
    content_type = content_type.substr(0, semicolon);
  }
  while (!content_type.empty() && (content_type.front() == ' ' || content_type.front() == '\t')) {
    content_type.remove_prefix(1);
  }
  while (!content_type.empty() && (content_type.back() == ' ' || content_type.back() == '\t')) {
    content_type.remove_suffix(1);
  }
  std::string lowered(content_type);
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lowered;
}

Outcome<JsonRpcStreamFrame> DecodeNotification(const Document& envelope, const Document& id) {
  const Document& method = *envelope.Find(kMethodMember);
  if (!method.is_string() || method.as_string().empty()) {
    return Malformed("\"method\" is not a non-empty string");
  }
  const Document* params = envelope.Find(kParamsMember);
  if (params == nullptr || !params->is_map()) {
    return Malformed("notification \"params\" is missing or not a JSON object");
  }
  // Exactly {"id", "payload"}, fail-closed: an extra member means a dialect
  // this stream does not speak, refused instead of half-understood.
  for (const auto& [name, value] : params->as_map()) {
    if (name != kIdMember && name != kPayloadMember) {
      return Malformed("unknown notification params member: " + name);
    }
  }
  const Document* echoed = params->Find(kIdMember);
  if (echoed == nullptr) {
    return Malformed("notification params carry no \"id\" echo");
  }
  if (*echoed != id) {
    return Malformed("notification for a different call's id (one stream per socket)");
  }
  const Document* payload = params->Find(kPayloadMember);
  if (payload == nullptr || !payload->is_map()) {
    return Malformed("notification \"payload\" is missing or not a JSON object");
  }
  JsonRpcStreamFrame frame;
  frame.kind = JsonRpcStreamFrame::Kind::kEvent;
  // Re-encoded through the runtime's JSON codec: generated serde sees one
  // dialect regardless of what the peer typed.
  frame.message = MakeEventMessage(method.as_string(), kJsonContentType,
                                   Blob::FromString(json::Encode(*payload)));
  return frame;
}

Outcome<JsonRpcStreamFrame> DecodeResponse(const Document& envelope, const Document& id) {
  const Document* echoed = envelope.Find(kIdMember);
  if (echoed == nullptr) {
    return Malformed("response envelope carries no \"id\"");
  }
  if (*echoed != id) {
    return Malformed("response for a different call's id (one stream per socket)");
  }
  const Document* result = envelope.Find(kResultMember);
  const Document* error = envelope.Find(kErrorMember);
  if ((result != nullptr) == (error != nullptr)) {
    return Malformed(result != nullptr ? "response carries both \"result\" and \"error\""
                                       : "envelope is neither a notification nor a response");
  }
  JsonRpcStreamFrame frame;
  if (result != nullptr) {
    // The terminal result — the stream's clean end. Its value is preserved
    // but not policed: today's servers answer {}, and whatever rides here
    // when initial-response support lands is terminal either way.
    frame.kind = JsonRpcStreamFrame::Kind::kResult;
    frame.result = *result;
    return frame;
  }
  if (!error->is_map()) {
    return Malformed("\"error\" is not a JSON object");
  }
  for (const auto& [name, value] : error->as_map()) {
    if (name != kCodeMember && name != kMessageMember && name != kDataMember) {
      return Malformed("unknown error member: " + name);
    }
  }
  const Document* code = error->Find(kCodeMember);
  if (code == nullptr || !code->is_int()) {
    return Malformed("error \"code\" is missing or not an integer");
  }
  const Document* message = error->Find(kMessageMember);
  if (message != nullptr && !message->is_string()) {
    return Malformed("error \"message\" is not a string");
  }
  const Document* data = error->Find(kDataMember);
  if (data != nullptr && !data->is_map()) {
    return Malformed("error \"data\" is not a JSON object");
  }
  DocumentMap payload = data != nullptr ? data->as_map() : DocumentMap();
  // The unary client's fallback, mirrored: an error message member fills a
  // data object that carries none.
  if (message != nullptr && !message->as_string().empty() &&
      payload.find(kMessageMember) == payload.end()) {
    payload.emplace(kMessageMember, *message);
  }
  const Document* type = data != nullptr ? data->Find(kTypeMember) : nullptr;
  const bool typed = type != nullptr && type->is_string() && !type->as_string().empty();
  frame.kind = JsonRpcStreamFrame::Kind::kException;
  frame.message =
      MakeExceptionMessage(typed ? type->as_string() : kFallbackExceptionType, kJsonContentType,
                           Blob::FromString(json::Encode(Document(std::move(payload)))));
  return frame;
}

}  // namespace

Outcome<std::string> EncodeJsonRpcNotification(const Message& message, const Document& id) {
  auto envelope = ParseEnvelope(message);
  if (!envelope.ok()) {
    // Only envelope-bearing messages can ride this wire; surface the parse
    // failure at the encode kind the Send contract promises.
    return CannotEncode(envelope.error().message());
  }
  if (envelope->kind != EventEnvelope::Kind::kEvent) {
    return CannotEncode(
        "exceptions do not ride notifications: the terminal error envelope is the generated "
        "serve path's business (ADR-0023)");
  }
  // No header channel beyond the envelope's own — json_frame.cc's counting
  // argument, verbatim: ParseEnvelope proved the envelope headers are
  // present, so any extra count is a header this wire cannot represent.
  const std::size_t envelope_headers = envelope->content_type.empty() ? 2 : 3;
  if (message.headers.size() != envelope_headers) {
    return CannotEncode("message carries headers beyond the envelope's own");
  }
  if (!envelope->content_type.empty() && MediaType(envelope->content_type) != kJsonContentType) {
    return CannotEncode("content type is not application/json: " + envelope->content_type);
  }
  auto payload = json::Decode(std::string_view(
      reinterpret_cast<const char*>(message.payload.data()), message.payload.size()));
  if (!payload.ok() || !payload->is_map()) {
    return CannotEncode("payload is not a JSON object");
  }
  DocumentMap params;
  params.emplace(kIdMember, id);
  params.emplace(kPayloadMember, std::move(*payload));
  DocumentMap fields;
  fields.emplace(kVersionMember, Document(std::string(kVersionValue)));
  fields.emplace(kMethodMember, Document(std::move(envelope->type)));
  fields.emplace(kParamsMember, Document(std::move(params)));
  std::string text = json::Encode(Document(std::move(fields)));
  if (text.size() > kMaxMessageBytes) {
    return CannotEncode("message over the 16 MiB limit");
  }
  return text;
}

Outcome<JsonRpcStreamFrame> DecodeJsonRpcStreamFrame(std::string_view text, const Document& id) {
  if (text.size() > kMaxMessageBytes) {
    return Malformed("message over the 16 MiB limit");
  }
  const auto envelope = json::Decode(text);
  if (!envelope.ok()) {
    return Malformed("text frame is not JSON: " + envelope.error().message());
  }
  if (!envelope->is_map()) {
    return Malformed("envelope is not a JSON object");
  }
  // Only the members the two envelope shapes own, fail-closed — a peer
  // speaking some richer dialect is refused instead of half-understood.
  for (const auto& [name, value] : envelope->as_map()) {
    if (name != kVersionMember && name != kMethodMember && name != kParamsMember &&
        name != kIdMember && name != kResultMember && name != kErrorMember) {
      return Malformed("unknown envelope member: " + name);
    }
  }
  const Document* version = envelope->Find(kVersionMember);
  if (version == nullptr || !version->is_string() || version->as_string() != kVersionValue) {
    return Malformed("expected jsonrpc: \"2.0\"");
  }
  if (envelope->Find(kMethodMember) != nullptr) {
    if (envelope->Find(kIdMember) != nullptr) {
      // A request envelope: this stream's call already opened, and one
      // stream per socket means no second call can.
      return Malformed("a request envelope after the opening call");
    }
    if (envelope->Find(kResultMember) != nullptr || envelope->Find(kErrorMember) != nullptr) {
      return Malformed("envelope mixes a notification with a response");
    }
    return DecodeNotification(*envelope, id);
  }
  return DecodeResponse(*envelope, id);
}

}  // namespace smithy::eventstream
