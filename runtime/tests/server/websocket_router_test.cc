// Pins ADR-0016's streaming router against the unary Router's behavior:
// the same pattern grammar, precedence, and context construction (drift
// between the two is the failure mode the shared matcher exists to
// prevent), Gate() refusals shaped like Router's dispatch failures, and
// Serve() dispatching the winning route's callback.

#include "smithy/server/websocket_router.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "smithy/http/websocket_pair.h"

namespace smithy::server {
namespace {

http::HttpRequest Request(std::string method, std::string target) {
  http::HttpRequest request;
  request.method = std::move(method);
  request.target = std::move(target);
  return request;
}

// What one dispatched serve call saw.
struct ServeRecord {
  std::string tag;
  RequestContext context;  // context.request compared by identity only
};

// A serve callback that records its dispatch into `record` (which may be
// null for routes a test never expects to fire).
StreamServe Recorder(std::shared_ptr<ServeRecord> record, const std::string& tag) {
  return [record = std::move(record), tag](const http::HttpRequest&, const RequestContext& context,
                                           http::WebSocket&) {
    if (record == nullptr) return;
    record->tag = tag;
    record->context = context;
  };
}

TEST(WebSocketRouterTest, ServeExtractsLabelsQueryAndRequestLikeRouter) {
  auto record = std::make_shared<ServeRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}/stream", Recorder(record, "chat"), "Chat").ok());

  const http::HttpRequest request = Request("GET", "/rooms/a%20b/stream?since=10&verbose");
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  router.Serve()(request, *socket);

  EXPECT_EQ(record->tag, "chat");
  ASSERT_EQ(record->context.labels.size(), 1U);
  EXPECT_EQ(record->context.labels.at("roomId"), "a b");  // labels arrive decoded
  const std::vector<std::pair<std::string, std::string>> expected_query = {{"since", "10"},
                                                                           {"verbose", ""}};
  EXPECT_EQ(record->context.query_params, expected_query);
  // The context's request IS the served request, Router::Route's wiring.
  EXPECT_EQ(record->context.request, &request);
}

TEST(WebSocketRouterTest, ServeDispatchesTheMostSpecificRoute) {
  auto literal = std::make_shared<ServeRecord>();
  auto label = std::make_shared<ServeRecord>();
  auto greedy = std::make_shared<ServeRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/streams/{name+}", Recorder(greedy, "greedy")).ok());
  ASSERT_TRUE(router.Add("GET", "/streams/{name}", Recorder(label, "label")).ok());
  ASSERT_TRUE(router.Add("GET", "/streams/live", Recorder(literal, "literal")).ok());
  const auto serve = router.Serve();

  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/streams/live"), *socket);
  EXPECT_EQ(literal->tag, "literal");  // literal outranks label outranks greedy
  serve(Request("GET", "/streams/weather"), *socket);
  EXPECT_EQ(label->tag, "label");
  EXPECT_EQ(label->context.labels.at("name"), "weather");
  serve(Request("GET", "/streams/us/west/alerts"), *socket);
  EXPECT_EQ(greedy->tag, "greedy");
  EXPECT_EQ(greedy->context.labels.at("name"), "us/west/alerts");  // slashes kept
}

TEST(WebSocketRouterTest, GateAdmitsMatchesAndRefusesLikeRouter) {
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}/stream", Recorder(nullptr, ""), "Chat").ok());
  ASSERT_TRUE(router.Add("PUT", "/rooms/{roomId}/stream", Recorder(nullptr, ""), "Push").ok());
  const auto gate = router.Gate();

  // A route will serve it: admitted.
  EXPECT_EQ(gate(Request("GET", "/rooms/lobby/stream")), std::nullopt);

  // Router's refusal shapes: 404 for a target no pattern serves...
  const auto not_found = gate(Request("GET", "/nowhere"));
  ASSERT_TRUE(not_found.has_value());
  EXPECT_EQ(not_found->status, 404);
  EXPECT_NE(not_found->body.find("NotFound"), std::string::npos);

  // ...405 with the deterministic Allow list for a method mismatch...
  const auto wrong_method = gate(Request("DELETE", "/rooms/lobby/stream"));
  ASSERT_TRUE(wrong_method.has_value());
  EXPECT_EQ(wrong_method->status, 405);
  EXPECT_EQ(wrong_method->headers.Get("allow").value_or(""), "GET, PUT");

  // ...and 400 for a malformed target.
  const auto malformed = gate(Request("GET", "/bad%2"));
  ASSERT_TRUE(malformed.has_value());
  EXPECT_EQ(malformed->status, 400);
}

TEST(WebSocketRouterTest, GateAndServeAgreeWithRouterOnTrailingSlashes) {
  auto record = std::make_shared<ServeRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms", Recorder(record, "rooms")).ok());
  EXPECT_EQ(router.Gate()(Request("GET", "/rooms/")), std::nullopt);  // "/a/" matches "/a"
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  router.Serve()(Request("GET", "/rooms/"), *socket);
  EXPECT_EQ(record->tag, "rooms");
}

TEST(WebSocketRouterTest, ServeClosesTheSessionWhenNothingMatches) {
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}/stream", Recorder(nullptr, "")).ok());
  const auto serve = router.Serve();

  // A gate-bypassing request must not leave the session dangling: the
  // socket closes, so the peer sees the stream's clean end.
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/nowhere"), *socket);
  auto at_peer = peer->Receive();
  ASSERT_TRUE(at_peer.ok());
  EXPECT_FALSE(at_peer->has_value());

  // The malformed-target path closes the same way.
  auto [socket2, peer2] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/bad%2"), *socket2);
  auto at_peer2 = peer2->Receive();
  ASSERT_TRUE(at_peer2.ok());
  EXPECT_FALSE(at_peer2->has_value());
}

TEST(WebSocketRouterTest, AddRejectsBadPatternsAndConflictsLikeRouter) {
  WebSocketRouter router;
  EXPECT_FALSE(router.Add("GET", "no-slash", Recorder(nullptr, "")).ok());
  EXPECT_FALSE(router.Add("GET", "/rooms/{}", Recorder(nullptr, "")).ok());
  EXPECT_FALSE(router.Add("GET", "/files/{path+}/tail", Recorder(nullptr, "")).ok());

  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}", Recorder(nullptr, "")).ok());
  // Same shape (a label matches whatever the other label matches): conflict.
  const auto conflict = router.Add("GET", "/rooms/{other}", Recorder(nullptr, ""));
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().kind(), ErrorKind::kValidation);
  // A different method is a different route, like Router.
  EXPECT_TRUE(router.Add("PUT", "/rooms/{roomId}", Recorder(nullptr, "")).ok());
}

}  // namespace
}  // namespace smithy::server
