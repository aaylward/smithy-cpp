// The thread-free hub (ADR-0019): the chat Converse wire served through the
// shared-session seam — one Detached coroutine per connection over
// AsyncEventStream, fan-out through a SessionRegistry in async delivery
// mode — so N rooms of players run on the transport's fixed io threads
// plus a handler pool that only launches. The same generated ChatClient
// (and hub_client) speaks to it unchanged; async_hub_cli_test.sh drives it
// as real processes.
//
// Serde calls are hand-written here on the public envelope helpers and
// the generated serde functions (the generated streaming serve path stays
// blocking by design in this slice, ADR-0019's non-goals); route matching
// rides WebSocketRouter's shared seam (issue #118) — this main is what a
// hand-written async mount looks like until the generated one lands.
//
//   bazel run //examples/chat:async_hub_server
//   bazel run //examples/chat:hub_client -- 8080 lobby ada    # per terminal
//   kill -TERM <pid>   # drains the hub, then exits 0

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "example/chat/serde.h"
#include "example/chat/types.h"
#include "smithy/core/blob.h"
#include "smithy/core/outcome.h"
#include "smithy/eventstream/async_event_stream.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/json/json.h"
#include "smithy/server/session_registry.h"
#include "smithy/server/websocket_router.h"

namespace {

using example::chat::ChatEvents;
using example::chat::ChatMessage;
using example::chat::Kicked;
using example::chat::LeaveNotice;
using example::chat::MemberJoined;
using example::chat::MemberLeft;
using example::chat::RoomEvents;

// The Converse wire, hand-mounted: the same envelope convention the
// generated codecs speak (ADR-0016), over the exported serde functions.
smithy::eventstream::Message MakeJsonEvent(const char* type, const smithy::Document& doc) {
  return smithy::eventstream::MakeEventMessage(type, "application/json",
                                               smithy::Blob::FromString(smithy::json::Encode(doc)));
}

smithy::Outcome<smithy::eventstream::Message> EncodeRoomEvent(const RoomEvents& event) {
  if (event.is_message()) {
    return MakeJsonEvent("message", example::chat::SerializeChatMessage(event.as_message()));
  }
  if (event.is_joined()) {
    return MakeJsonEvent("joined", example::chat::SerializeMemberJoined(event.as_joined()));
  }
  if (event.is_left()) {
    return MakeJsonEvent("left", example::chat::SerializeMemberLeft(event.as_left()));
  }
  return smithy::Error::Validation("RoomEvents: no event member engaged");
}

smithy::Outcome<ChatEvents> DecodeChatEvent(const smithy::eventstream::Message& message) {
  auto envelope = smithy::eventstream::ParseEnvelope(message);
  if (!envelope) return std::move(envelope).error();
  if (envelope->kind == smithy::eventstream::EventEnvelope::Kind::kException) {
    return smithy::Error::Modeled(envelope->type, "peer sent an exception message");
  }
  auto doc = smithy::json::Decode(envelope->payload.ToString());
  if (!doc) return std::move(doc).error();
  if (envelope->type == "message") {
    auto event = example::chat::DeserializeChatMessage(*doc);
    if (!event) return std::move(event).error();
    return ChatEvents::FromMessage(*std::move(event));
  }
  if (envelope->type == "leave") {
    auto event = example::chat::DeserializeLeaveNotice(*doc);
    if (!event) return std::move(event).error();
    return ChatEvents::FromLeave(*std::move(event));
  }
  return smithy::Error::Serialization("Converse: unknown event type: " + envelope->type);
}

using AsyncStream = smithy::eventstream::AsyncEventStream<RoomEvents, ChatEvents>;

// The fan-out state: ids are "<room>/<name>", exactly the hub_handler
// scheme, over the async-delivery registry — no writer threads, no parked
// handler threads, all delivery on the transport's completions.
class AsyncHub {
 public:
  explicit AsyncHub(std::chrono::seconds grace) : registry_(RegistryOptions(this, grace)) {}

  smithy::server::SessionRegistry<RoomEvents>& registry() { return registry_; }

  // The roster a resumed session replays as its snapshot (ADR-0020's
  // recovery model: authoritative current state, not missed messages).
  std::vector<std::string> RoomMembers(const std::string& room) const {
    const std::string prefix = room + "/";
    std::vector<std::string> names;
    for (const std::string& id : registry_.Ids()) {
      if (id.starts_with(prefix)) names.push_back(id.substr(prefix.size()));
    }
    return names;
  }

  void BroadcastToRoom(const std::string& room, const RoomEvents& event) {
    registry_.Broadcast(RoomIds(room), event);
  }

  void BroadcastMessage(const std::string& room, const std::string& author_id,
                        const std::string& author, const std::string& text) {
    registry_.Broadcast(RoomIds(room), [&](const std::string& member) {
      ChatMessage view;
      view.text = text;
      view.sender = member == author_id ? "you" : author;
      return RoomEvents::FromMessage(view);
    });
  }

 private:
  static smithy::server::SessionRegistry<RoomEvents>::Options RegistryOptions(
      AsyncHub* hub, std::chrono::seconds grace) {
    smithy::server::SessionRegistry<RoomEvents>::Options options;
    options.async_delivery = true;  // ADR-0019: chains, not writer threads
    options.grace_period = grace;   // ADR-0020: abrupt losses detach, not vanish
    options.on_expired = [hub](const std::string& id) {
      // Grace ran out: the departure the disconnect deferred. Runs on the
      // registry's expiry thread; Broadcast is safe from any thread.
      const auto slash = id.find('/');
      if (slash == std::string::npos) return;
      hub->BroadcastToRoom(id.substr(0, slash),
                           RoomEvents::FromLeft(MemberLeft{.member = id.substr(slash + 1)}));
    };
    return options;
  }

  std::vector<std::string> RoomIds(const std::string& room) const {
    const std::string prefix = room + "/";
    std::vector<std::string> ids;
    for (std::string& id : registry_.Ids()) {
      if (id.starts_with(prefix)) ids.push_back(std::move(id));
    }
    return ids;
  }

  smithy::server::SessionRegistry<RoomEvents> registry_;
};

// One session's whole life, thread-free: launched from the handler pool,
// then living entirely on completion contexts.
smithy::eventstream::Detached Serve(AsyncHub& hub, std::string room, std::string name,
                                    std::shared_ptr<smithy::http::WebSocket> socket) {
  const std::string id = room + "/" + name;
  // The socket is copied, not moved: the refusal branch below sends the
  // typed exception on the raw socket.
  AsyncStream stream(socket, EncodeRoomEvent, DecodeChatEvent);
  // Resume first (the reconnect story, ADR-0020), then a fresh join. A
  // reconnect can beat the old wire's failure notice — the entry looks
  // live for a beat — so retry briefly before refusing the nickname.
  // Pre-co_await, on the launching handler thread: blocking briefly is fine.
  bool resumed = false;
  bool added = false;
  for (int attempt = 0; attempt < 20 && !resumed && !added; ++attempt) {
    resumed = hub.registry().Resume(id, stream.Share());
    if (!resumed) added = hub.registry().Add(id, stream.Share());
    if (!resumed && !added) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!resumed && !added) {
    // The nickname reservation, refused the way the generated path would:
    // one typed exception message, then the close.
    Kicked kicked;
    kicked.message = "nickname '" + name + "' is already in " + room;
    kicked.by = "hub";
    smithy::DocumentMap body = example::chat::SerializeKicked(kicked).as_map();
    (void)socket->Send(smithy::eventstream::MakeExceptionMessage(
        "Kicked", "application/json",
        smithy::Blob::FromString(smithy::json::Encode(smithy::Document(std::move(body))))));
    stream.Close();
    co_return;
  }

  if (resumed) {
    // Snapshot replay, the blessed recovery: the roster as it stands now,
    // as this session's first events. Direct sends are the handler's
    // request/reply moment; a collision with a broadcast chain just
    // converts this session to writer delivery.
    for (const std::string& member : hub.RoomMembers(room)) {
      (void)co_await stream.Send(RoomEvents::FromJoined(MemberJoined{.member = member}));
    }
  } else {
    hub.BroadcastToRoom(room, RoomEvents::FromJoined(MemberJoined{.member = name}));
  }

  bool left_cleanly = false;
  while (true) {
    auto received = co_await stream.Receive();
    if (!received.ok()) break;  // wire failed
    const std::optional<ChatEvents>& event = *received;
    if (!event.has_value()) break;  // client closed without a leave
    if (event->is_leave()) {
      left_cleanly = true;
      break;
    }
    if (!event->is_message()) continue;
    hub.BroadcastMessage(room, id, name, event->as_message().text);
  }

  if (left_cleanly) {
    hub.registry().Remove(id);
    hub.BroadcastToRoom(room, RoomEvents::FromLeft(MemberLeft{.member = name}));
    // Best-effort: a registry chain still draining this session's tail
    // holds the send slot and refuses this — the discard is deliberate.
    (void)co_await stream.Send(RoomEvents::FromLeft(MemberLeft{.member = name}));
  } else if (!hub.registry().Detach(id)) {
    // Grace disabled (or the entry already gone): the pre-ADR-0020 path.
    hub.registry().Remove(id);
    hub.BroadcastToRoom(room, RoomEvents::FromLeft(MemberLeft{.member = name}));
  }
  // An abrupt loss inside grace stays silent: the resumed session replays
  // the roster, and only expiry announces the departure (on_expired).
  stream.Close();
}

}  // namespace

int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  // argv: [port [grace-seconds]] — 0 binds an ephemeral port; the short
  // grace is what lets the CLI test watch an expiry inside its timeout.
  const std::chrono::seconds grace{argc > 2 ? std::atoi(argv[2]) : 300};
  AsyncHub hub(grace);
  // Route matching through the streaming router (issue #118): the same
  // pattern grammar and refusal shapes as every other route in the repo,
  // mounted on the shared seam in the same two lines as the borrowed one.
  // Declared before the transport; the returned callables refer into it.
  smithy::server::WebSocketRouter router;
  if (auto added = router.AddSession(
          "GET", "/rooms/{room}/converse",
          [&hub](const smithy::http::HttpRequest& request,
                 const smithy::server::RequestContext& context,
                 std::shared_ptr<smithy::http::WebSocket> socket) {
            const std::string name = request.headers.Get("x-chat-nickname").value_or("anonymous");
            Serve(hub, context.labels.at("room"), name, std::move(socket));  // returns immediately
          },
          "Converse");
      !added.ok()) {
    std::fprintf(stderr, "async-hub: route: %s\n", added.error().message().c_str());
    return 1;
  }

  smithy::http::BeastServerTransport::Options options;
  options.address = "0.0.0.0";
  options.port = argc > 1 ? std::atoi(argv[1]) : 8080;  // 0 binds an ephemeral port
  options.handler_threads = 2;                          // launches only — sessions hold no thread
  options.websocket_gate = router.Gate();
  options.on_websocket_session = router.ServeSession();
  smithy::http::BeastServerTransport transport(options);
  smithy::Outcome<smithy::Unit> started = transport.Start([](const smithy::http::HttpRequest&) {
    smithy::http::HttpResponse response;
    response.status = 404;
    response.body = "streams only";
    return response;
  });
  if (!started.ok()) {
    std::fprintf(stderr, "async-hub: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "async-hub: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);
  std::fprintf(stderr, "async-hub: signal %d, draining %zu session(s)\n", signal_number,
               hub.registry().size());
  const bool drained = hub.registry().Drain(std::chrono::seconds(5));
  std::fprintf(stderr, drained ? "async-hub: drained\n"
                               : "async-hub: drain timed out; aborting remaining sessions\n");
  transport.Stop();
  return drained ? 0 : 1;
}
