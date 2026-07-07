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
  // The Smithy operation that handled the request (from the generated
  // router's HttpResponse::operation annotation); empty for 404/405/400
  // dispatch failures.
  std::string operation;
  // The request's W3C traceparent header, verbatim, for log correlation;
  // empty when absent. See smithy/http/trace_context.h to parse it.
  std::string trace_parent;
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

// 401 unless the request carries "authorization: Bearer <token>" (scheme
// matched case-insensitively per RFC 6750) and validator(token) returns
// true — the server-side counterpart of @httpBearerAuth.
Middleware RequireBearerAuth(std::function<bool(const std::string&)> validator);

// 401 unless header_name carries the key — prefixed "<scheme> " when scheme
// is non-empty — and validator(key) returns true; the server-side
// counterpart of @httpApiKeyAuth(in: "header").
Middleware RequireApiKeyHeader(std::string header_name, std::string scheme,
                               std::function<bool(const std::string&)> validator);

}  // namespace smithy::server

#endif  // SMITHY_SERVER_MIDDLEWARE_H_
