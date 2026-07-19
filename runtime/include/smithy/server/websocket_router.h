#ifndef SMITHY_SERVER_WEBSOCKET_ROUTER_H_
#define SMITHY_SERVER_WEBSOCKET_ROUTER_H_

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "smithy/core/outcome.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/server/router.h"

namespace smithy::server {

// A streaming operation's serve callback: the upgrade request, the routing
// context built from it (labels + query + the request pointer — exactly
// what Router::Route hands unary handlers), and the accepted session. Runs
// on the transport's handler thread and blocks for the session's lifetime;
// the WebSocket& is on_websocket's borrow, valid until the callback
// returns, whose return ends the session with a close handshake
// (beast_transport.h).
using StreamServe =
    std::function<void(const http::HttpRequest&, const RequestContext&, http::WebSocket&)>;

// Method + URI-pattern dispatch for WebSocket upgrades (ADR-0016): the
// unary Router's streaming parallel, sharing its pattern grammar and
// matching precedence (internal:: in router.h) so routing behavior cannot
// drift between the two. The upgrade path deliberately bypasses the HTTP
// middleware chain (ADR-0015), so a generated server's streaming routes
// mount in two lines:
//
//   options.websocket_gate = server.StreamRouter()->Gate();
//   options.on_websocket = server.StreamRouter()->Serve();
//
// Application admission policy (auth, rate limits) composes by wrapping
// Gate(): run the application's refusal first, then defer to the router's.
//
//   options.websocket_gate =
//       [gate = router.Gate()](const http::HttpRequest& request)
//           -> std::optional<http::HttpResponse> {
//         if (!Authorized(request)) return MakeErrorResponse(401, "Unauthorized", "no token");
//         return gate(request);
//       };
class WebSocketRouter {
 public:
  // Fails on invalid patterns and on route conflicts (same method + pattern
  // shape), like Router::Add. The operation name is recorded for the
  // observability wiring streaming dispatch grows later; today it only
  // documents the route.
  Outcome<Unit> Add(std::string_view method, std::string_view pattern, StreamServe serve,
                    std::string_view operation = "");

  // The websocket_gate decision: nullopt admits an upgrade some route will
  // serve; refusals are shaped exactly like Router's dispatch failures —
  // 404 (no pattern match), 405 with an Allow header (pattern match, wrong
  // method), 400 (malformed target). The returned callable refers to this
  // router: keep the router alive, and complete Add calls, before the
  // transport starts.
  std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)> Gate() const;

  // The on_websocket dispatcher: re-matches the upgrade request, builds the
  // RequestContext the way Router::Route does, and blocks in the winning
  // route's serve callback. A request no route matches — unreachable when
  // Gate() screened the upgrade — just closes the session. Same lifetime
  // rule as Gate().
  std::function<void(const http::HttpRequest&, http::WebSocket&)> Serve() const;

 private:
  struct StreamRoute {
    std::vector<internal::Segment> segments;
    StreamServe serve;
    std::string operation;
  };

  // The best (most specific) matching route for the method, or nullptr.
  const StreamRoute* FindBest(const std::string& method,
                              const std::vector<std::string>& segments) const;

  // Keyed by HTTP method, like Router's index: a request scans only its own
  // method's routes, and map order makes the 405 Allow list deterministic.
  std::map<std::string, std::vector<StreamRoute>> routes_;
};

}  // namespace smithy::server

#endif  // SMITHY_SERVER_WEBSOCKET_ROUTER_H_
