#ifndef SMITHY_EXAMPLES_CHAT_ROOM_HANDLER_H_
#define SMITHY_EXAMPLES_CHAT_ROOM_HANDLER_H_

// The reference ChatHandler both e2e suites drive — chat_e2e_test.cc through
// the in-memory pair and chat_e2e_beast_test.cc over real WebSockets, one
// handler so the two halves cannot drift: greets each joiner, echoes
// messages back to the room, kicks anyone who asks (the modeled mid-stream
// error), and announces leavers before closing its side.

#include <string>

#include "example/chat/server.h"
#include "smithy/core/outcome.h"

namespace example::chat {

inline constexpr char kModerator[] = "moderator";

class RoomHandler final : public ChatHandler {
 public:
  smithy::Outcome<ListRoomsOutput> ListRooms(const ListRoomsInput&,
                                             const smithy::server::RequestContext&) override {
    ListRoomsOutput output;
    output.rooms.push_back(RoomSummary{.name = "lobby", .members = 2});
    return output;
  }

  smithy::Outcome<smithy::Unit> Converse(const ConverseInput& input, ConverseServerStream& stream,
                                         const smithy::server::RequestContext&) override {
    // The nickname rode the upgrade request's headers; announce the joiner.
    MemberJoined joined;
    joined.member = input.nickname.value_or("anonymous");
    if (!stream.Send(RoomEvents::FromJoined(joined)).ok()) return smithy::Unit{};
    while (true) {
      auto event = stream.Receive();
      if (!event.ok()) return smithy::Unit{};          // wire failed: nothing to add
      if (!event->has_value()) return smithy::Unit{};  // client closed cleanly
      const ChatEvents& received = **event;
      if (received.is_message()) {
        const ChatMessage& message = received.as_message();
        if (message.text == "kick me") {
          // The modeled mid-stream error: one exception message, then close.
          smithy::Error kicked = smithy::Error::Modeled("Kicked", "kicked from " + input.room);
          kicked.set_detail(Kicked{.message = "kicked from " + input.room, .by = kModerator});
          return kicked;
        }
        ChatMessage broadcast;
        broadcast.text = message.text;
        broadcast.sender = message.sender.value_or(joined.member);
        if (!stream.Send(RoomEvents::FromMessage(broadcast)).ok()) return smithy::Unit{};
      } else if (received.is_leave()) {
        MemberLeft left;
        left.member = joined.member;
        (void)stream.Send(RoomEvents::FromLeft(left));
        return smithy::Unit{};  // the server's side of a clean close
      }
    }
  }

  smithy::Outcome<smithy::Unit> Watch(const WatchInput& input, WatchServerStream& stream,
                                      const smithy::server::RequestContext&) override {
    for (int i = 0; i < 3; ++i) {
      ChatMessage message;
      message.text = input.room + "-update-" + std::to_string(i);
      message.sender = "room";
      if (!stream.Send(RoomEvents::FromMessage(message)).ok()) return smithy::Unit{};
    }
    return smithy::Unit{};  // push, then close
  }
};

}  // namespace example::chat

#endif  // SMITHY_EXAMPLES_CHAT_ROOM_HANDLER_H_
