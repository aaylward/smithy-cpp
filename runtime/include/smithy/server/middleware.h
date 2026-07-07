#ifndef SMITHY_SERVER_MIDDLEWARE_H_
#define SMITHY_SERVER_MIDDLEWARE_H_

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "smithy/http/transport.h"

namespace smithy::server {

// User-supplied middleware wraps the transport-facing handler a generated
// server exposes: inspect or reject requests before dispatch, observe or
// amend responses after. Middleware composes outside the generated router,
// so it works with any HttpServerTransport:
//
//   WeatherServer server(handler);
//   transport.Start(smithy::server::Chain({AuthCheck(), Observe(log)},
//                                         server.Handler()));
using Middleware = std::function<http::RequestHandler(http::RequestHandler)>;

// Composes middleware around a handler. The first element is outermost: it
// sees the request first and the response last.
http::RequestHandler Chain(std::vector<Middleware> middleware, http::RequestHandler handler);

// One served request, as seen from outside the router.
struct RequestObservation {
  std::string method;
  std::string target;
  int status = 0;
  std::chrono::milliseconds duration{0};
};

// Middleware reporting every request to a callback — the structured-logging
// and metrics hook (count = callbacks, latency = duration). The callback runs
// on the transport's request thread after the response is built; keep it
// cheap or hand off. now is injectable for deterministic tests (null means
// steady_clock).
Middleware Observe(std::function<void(const RequestObservation&)> callback,
                   std::function<std::chrono::steady_clock::time_point()> now = nullptr);

}  // namespace smithy::server

#endif  // SMITHY_SERVER_MIDDLEWARE_H_
