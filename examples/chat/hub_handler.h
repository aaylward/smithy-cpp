#ifndef SMITHY_EXAMPLES_CHAT_HUB_HANDLER_H_
#define SMITHY_EXAMPLES_CHAT_HUB_HANDLER_H_

// The multi-client hub (issue #112): the consumer pattern the Go-style
// WebSocket hub hand-rolls, built on the two runtime primitives that make
// it safe and short — owning session handles (EventStream::Share) and the
// queued fan-out registry (smithy::server::SessionRegistry). Every session
// registers its handle under "<room>/<name>"; room traffic fans out through
// the registry's bounded per-session queues, so one slow client never
// stalls a room (the default policy disconnects it instead); and shutdown
// is Drain(): close every session, wait for the handlers to unwind.
//
// Both e2e suites drive this one handler — hub_e2e_test.cc through the
// in-memory pair, hub_cli_test.sh as real processes over real WebSockets
// (hub_server_main.cc + hub_client_main.cc driven by shell commands).
//
// Room membership stays application-level on purpose (the issue's
// non-goals): it is exactly the id scheme, nothing more. A production
// service would validate names (a nickname containing '/' or a leading '#'
// would confuse this toy scheme) the way it validates any input.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "example/chat/server.h"
#include "smithy/core/outcome.h"
#include "smithy/server/session_registry.h"

namespace example::chat {

class HubHandler final : public ChatHandler {
 public:
  using Registry = smithy::server::SessionRegistry<RoomEvents>;

  HubHandler() = default;
  explicit HubHandler(Registry::Options options) : registry_(std::move(options)) {}

  // The unary neighbor reports live occupancy straight from the registry:
  // one converse member per "<room>/<name>" id (watchers, "#watch-<n>",
  // observe without counting).
  smithy::Outcome<ListRoomsOutput> ListRooms(const ListRoomsInput&,
                                             const smithy::server::RequestContext&) override {
    std::map<std::string, int> occupancy;
    for (const std::string& id : registry_.Ids()) {
      const auto [room, name] = SplitId(id);
      if (!name.starts_with('#')) ++occupancy[room];
    }
    ListRoomsOutput output;
    for (const auto& [room, members] : occupancy) {
      output.rooms.push_back(RoomSummary{.name = room, .members = members});
    }
    return output;
  }

  smithy::Outcome<smithy::Unit> Converse(const ConverseInput& input, ConverseServerStream& stream,
                                         const smithy::server::RequestContext&) override {
    const std::string name = input.nickname.value_or("anonymous");
    const std::string id = input.room + "/" + name;

    // The owning handle replaces the borrowed `stream&` in the registry —
    // Add's atomicity doubles as the nickname reservation.
    if (!registry_.Add(id, stream.Share())) {
      smithy::Error taken =
          smithy::Error::Modeled("Kicked", "nickname '" + name + "' is already in " + input.room);
      taken.set_detail(
          Kicked{.message = "nickname '" + name + "' is already in " + input.room, .by = "hub"});
      return taken;  // one typed exception message, then the close
    }

    registry_.Broadcast(RoomIds(input.room), RoomEvents::FromJoined(MemberJoined{.member = name}));

    bool left_cleanly = false;
    while (true) {
      auto event = stream.Receive();
      if (!event.ok() || !event->has_value()) break;  // wire failed / client vanished
      if ((*event)->is_leave()) {
        left_cleanly = true;
        break;
      }
      if (!(*event)->is_message()) continue;
      // Per-recipient construction — the redaction seam: the author reads
      // "you", everyone else reads the author's name.
      const std::string text = (*event)->as_message().text;
      registry_.Broadcast(RoomIds(input.room), [&](const std::string& member) {
        ChatMessage view;
        view.text = text;
        view.sender = member == id ? "you" : name;
        return RoomEvents::FromMessage(view);
      });
    }

    // Deregister before announcing, so the departure fans out to the others
    // only (Remove would discard this session's own copy anyway); the clean
    // leaver still gets its goodbye directly, on the stream it is draining.
    registry_.Remove(id);
    registry_.Broadcast(RoomIds(input.room), RoomEvents::FromLeft(MemberLeft{.member = name}));
    if (left_cleanly) (void)stream.Send(RoomEvents::FromLeft(MemberLeft{.member = name}));
    return smithy::Unit{};  // the generated caller closes the session
  }

  smithy::Outcome<smithy::Unit> Watch(const WatchInput& input, WatchServerStream& stream,
                                      const smithy::server::RequestContext&) override {
    // Watchers hold the same handle type as conversers (both directions
    // transmit RoomEvents), so one registry fans out to both.
    const std::string id = input.room + "/#watch-" + std::to_string(next_watcher_.fetch_add(1) + 1);
    (void)registry_.Add(id, stream.Share());
    // Watchers only listen: this returns at the client's close (nullopt), a
    // wire failure, or the hub closing the session (drain / slow-consumer).
    (void)stream.Receive();
    registry_.Remove(id);
    return smithy::Unit{};
  }

  // The graceful-shutdown step (issue #112 proposal 3), for main() to run
  // before the transport's abort-flavored Stop().
  bool Drain(std::chrono::milliseconds grace) { return registry_.Drain(grace); }

  std::size_t sessions() const { return registry_.size(); }

 private:
  static std::pair<std::string, std::string> SplitId(const std::string& id) {
    const std::size_t slash = id.find('/');
    return {id.substr(0, slash), slash == std::string::npos ? "" : id.substr(slash + 1)};
  }

  std::vector<std::string> RoomIds(const std::string& room) const {
    const std::string prefix = room + "/";
    std::vector<std::string> ids;
    for (std::string& id : registry_.Ids()) {
      if (id.starts_with(prefix)) ids.push_back(std::move(id));
    }
    return ids;
  }

  Registry registry_;
  std::atomic<int> next_watcher_{0};
};

}  // namespace example::chat

#endif  // SMITHY_EXAMPLES_CHAT_HUB_HANDLER_H_
