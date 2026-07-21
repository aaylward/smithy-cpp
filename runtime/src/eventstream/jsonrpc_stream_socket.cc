#include "smithy/eventstream/jsonrpc_stream_socket.h"

#include <string_view>
#include <utility>

#include "smithy/core/blob.h"
#include "smithy/core/error.h"
#include "smithy/eventstream/jsonrpc_frame.h"

namespace smithy::eventstream {
namespace {

// One outbound event as the headerless raw-text Message the wire carries.
Outcome<Message> TranslateOutbound(const Message& message, const Document& id) {
  auto text = EncodeJsonRpcNotification(message, id);
  if (!text.ok()) return std::move(text).error();
  Message raw;
  raw.payload = Blob::FromString(*std::move(text));
  return raw;
}

// One inbound completion, translated. The result envelope maps to the
// stream's clean end (nullopt) — terminal by wire contract; the server
// closes right behind it, so nothing meaningful can follow.
Outcome<std::optional<Message>> TranslateInbound(Outcome<std::optional<Message>> raw,
                                                 const Document& id) {
  if (!raw.ok() || !raw->has_value()) return raw;
  const Message& message = **raw;
  if (!message.headers.empty()) {
    return Error::Serialization(
        "jsonrpc stream: peer sent an event-stream framed message on the JSON-RPC text wire");
  }
  auto frame = DecodeJsonRpcStreamFrame(
      std::string_view(reinterpret_cast<const char*>(message.payload.data()),
                       message.payload.size()),
      id);
  if (!frame.ok()) return std::move(frame).error();
  if (frame->kind == JsonRpcStreamFrame::Kind::kResult) return std::optional<Message>();
  return std::optional<Message>(std::move(frame->message));
}

}  // namespace

JsonRpcStreamSocket::JsonRpcStreamSocket(std::shared_ptr<http::WebSocket> inner, Document id)
    : inner_(std::move(inner)), id_(std::move(id)) {}

Outcome<std::optional<Message>> JsonRpcStreamSocket::Receive() {
  return TranslateInbound(inner_->Receive(), id_);
}

Outcome<Unit> JsonRpcStreamSocket::Send(const Message& message) {
  auto raw = TranslateOutbound(message, id_);
  if (!raw.ok()) return std::move(raw).error();
  return inner_->Send(*raw);
}

void JsonRpcStreamSocket::Close() { inner_->Close(); }

void JsonRpcStreamSocket::ReceiveAsync(ReceiveCallback callback) {
  // The id travels by value into the completion: the wrapper may be gone
  // by the time the transport completes (only the inner socket's lifetime
  // is pinned by the callback), and a Document is cheap to copy.
  inner_->ReceiveAsync(
      [id = id_, callback = std::move(callback)](Outcome<std::optional<Message>> raw) {
        callback(TranslateInbound(std::move(raw), id));
      });
}

void JsonRpcStreamSocket::SendAsync(const Message& message, SendCallback callback) {
  auto raw = TranslateOutbound(message, id_);
  if (!raw.ok()) {
    // Inline refusal, the facade's immediate-refusal shape: the session is
    // untouched and stays usable.
    callback(std::move(raw).error());
    return;
  }
  // The raw message must outlive the call: the facade never promises the
  // transport is done with the reference before the completion fires.
  auto keep_alive = std::make_shared<Message>(*std::move(raw));
  inner_->SendAsync(*keep_alive,
                    [keep_alive, callback = std::move(callback)](Outcome<Unit> sent) {
                      callback(std::move(sent));
                    });
}

bool JsonRpcStreamSocket::SupportsAsync() const { return inner_->SupportsAsync(); }

}  // namespace smithy::eventstream
