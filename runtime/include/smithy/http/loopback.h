#ifndef SMITHY_HTTP_LOOPBACK_H_
#define SMITHY_HTTP_LOOPBACK_H_

#include <utility>

#include "smithy/http/server_dispatch.h"
#include "smithy/http/transport.h"

namespace smithy::http {

// In-memory transport: an HttpClient wired directly to a request handler with
// no sockets, serialization of the connection, or threads. The backbone of
// fast client<->server integration tests (PLAN Phase 5) — the same test body
// runs against Loopback and a real socket transport. There is no connection,
// so HttpRequest::peer_address stays whatever the caller set (empty by
// default) — a test can stamp one to exercise peer-dependent handlers.
class Loopback : public HttpClient, public HttpServerTransport {
 public:
  // HttpServerTransport:
  Outcome<Unit> Start(RequestHandler handler) override {
    handler_ = std::move(handler);
    return Unit{};
  }
  void Stop() override { handler_ = nullptr; }

  // HttpClient:
  Outcome<HttpResponse> Send(const HttpRequest& request) override {
    if (!handler_) return Error::Transport("loopback: no handler installed", /*retryable=*/false);
    return InvokeHandlerGuarded(handler_, request);
  }

 private:
  RequestHandler handler_;
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_LOOPBACK_H_
