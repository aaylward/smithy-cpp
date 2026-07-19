// Out-of-tree acceptance for the WebSocket transports (ADR-0015, the
// definition-of-done e2e ADR-0014 pinned): a consumer wires a streaming
// server — gate and serve callback — through the module boundary, dials it
// with the runtime's own client, and drains real event-stream frames both
// ways. This is the wiring applications use ahead of slice 3's generated
// EventStream API.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "smithy/eventstream/frame.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/websocket.h"

namespace {

using smithy::eventstream::Message;
using smithy::http::BeastServerTransport;
using smithy::http::BeastWebSocketClient;
using smithy::http::HttpRequest;
using smithy::http::HttpResponse;
using smithy::http::WebSocket;

Message Event(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = smithy::Blob::FromString(body)};
}

TEST(WebSocketAcceptanceTest, AConsumerServesAndDrainsAStreamThroughTheModuleBoundary) {
  BeastServerTransport::Options options;
  options.websocket_gate = [](const HttpRequest& request) -> std::optional<HttpResponse> {
    if (request.headers.Get("authorization").value_or("") != "Bearer consumer-token") {
      HttpResponse refusal;
      refusal.status = 401;
      return refusal;
    }
    return std::nullopt;
  };
  options.on_websocket = [](const HttpRequest& request, WebSocket& socket) {
    // Greet, then echo until the client closes — dispatching on the
    // FindString lookup the codec ships.
    (void)socket.Send(Event("greeting", "hello " + request.target));
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        return;
      }
      const std::string* kind = (*message)->FindString(":event-type");
      Message reply =
          Event(kind != nullptr ? *kind : "unknown", "echo:" + (*message)->payload.ToString());
      if (!socket.Send(reply).ok()) {
        return;
      }
    }
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 404;
                    return response;
                  })
                  .ok());

  // An unauthenticated dial is refused before any upgrade exists.
  EXPECT_FALSE(BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()}).ok());

  smithy::http::Headers credentials;
  credentials.Add("authorization", "Bearer consumer-token");
  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .target = "/events", .headers = credentials});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const auto& socket = *dialed;

  auto greeting = socket->Receive();
  ASSERT_TRUE(greeting.ok() && greeting->has_value());
  EXPECT_EQ((**greeting).payload.ToString(), "hello /events");

  for (int i = 0; i < 5; ++i) {
    const std::string body = "consumer-" + std::to_string(i);
    ASSERT_TRUE(socket->Send(Event("chat", body)).ok());
    auto echo = socket->Receive();
    ASSERT_TRUE(echo.ok() && echo->has_value());
    ASSERT_NE((**echo).FindString(":event-type"), nullptr);
    EXPECT_EQ(*(**echo).FindString(":event-type"), "chat");
    EXPECT_EQ((**echo).payload.ToString(), "echo:" + body);
  }

  socket->Close();
  auto end = socket->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
  server.Stop();
}

}  // namespace
