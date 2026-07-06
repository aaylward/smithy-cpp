#ifndef SMITHY_HTTP_BEAST_TRANSPORT_H_
#define SMITHY_HTTP_BEAST_TRANSPORT_H_

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "smithy/http/transport.h"

namespace smithy::http {

// Production HTTP/1.1 server transport on Boost.Beast/asio (ADR-0006):
// concurrent connections on an asio thread pool, keep-alive, per-connection
// timeouts, and graceful shutdown. This is what generated services should
// run on; TLS and WebSocket upgrades (Phase 8) extend this transport.
//
//   smithy::http::BeastServerTransport server({.port = 8080});
//   server.Start(service.Handler());
//   ...
//   server.Stop();
class BeastServerTransport : public HttpServerTransport {
 public:
  struct Options {
    // "127.0.0.1" keeps test servers private; use "0.0.0.0" to serve externally.
    std::string address = "127.0.0.1";
    int port = 0;  // 0 binds an ephemeral port; read port() after Start.
    int threads = 4;
    int request_timeout_seconds = 30;
    std::size_t max_body_bytes = std::size_t{64} * 1024 * 1024;
  };

  BeastServerTransport() : BeastServerTransport(Options{}) {}
  explicit BeastServerTransport(Options options);
  ~BeastServerTransport() override;

  Outcome<Unit> Start(RequestHandler handler) override;
  void Stop() override;

  int port() const;

 private:
  struct State;  // Hides boost headers from this public header.

  void Shutdown() noexcept;

  Options options_;
  std::shared_ptr<State> state_;
  std::vector<std::thread> threads_;
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_BEAST_TRANSPORT_H_
