#ifndef SMITHY_HTTP_SOCKET_TRANSPORT_H_
#define SMITHY_HTTP_SOCKET_TRANSPORT_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "smithy/http/transport.h"

namespace smithy::http {

// Built-in dependency-free HTTP/1.1 client over TCP (ADR-0005). One
// connection per request, Connection: close semantics. Suitable for tests and
// simple deployments; pooled/TLS transports plug in behind HttpClient later.
class SocketHttpClient : public HttpClient {
 public:
  SocketHttpClient(std::string host, int port, int timeout_ms = 30000)
      : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

  Outcome<HttpResponse> Send(const HttpRequest& request) override;

 private:
  std::string host_;
  int port_;
  int timeout_ms_;
};

// Built-in dependency-free HTTP/1.1 server over TCP, bound to 127.0.0.1.
// Accept loop on a background thread, one connection at a time — enough for
// integration testing (PLAN Phase 1/5); production-grade concurrency is a
// Phase 7 concern.
class SocketHttpServer : public HttpServerTransport {
 public:
  // port 0 binds an ephemeral port; read the real one from port() after Start.
  explicit SocketHttpServer(int port = 0) : requested_port_(port) {}
  ~SocketHttpServer() override;

  Outcome<Unit> Start(RequestHandler handler) override;
  void Stop() override;

  int port() const { return bound_port_; }

 private:
  void AcceptLoop();
  // Non-virtual teardown shared by Stop() and the destructor.
  void Shutdown() noexcept;

  int requested_port_ = 0;
  int bound_port_ = 0;
  std::intptr_t listener_ = -1;  // platform socket handle
  RequestHandler handler_;
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_SOCKET_TRANSPORT_H_
