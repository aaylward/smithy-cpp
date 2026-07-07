#ifndef SMITHY_HTTP_BEAST_TRANSPORT_H_
#define SMITHY_HTTP_BEAST_TRANSPORT_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "smithy/http/transport.h"

namespace smithy::http {

// Production HTTP/1.1 server transport on Boost.Beast/asio (ADR-0006):
// concurrent connections on an asio thread pool, keep-alive, per-connection
// timeouts, graceful shutdown, and optional TLS termination (ADR-0007).
// This is what generated services should run on; WebSocket upgrades
// (Phase 8) extend this transport.
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
    std::size_t max_header_bytes = std::size_t{8} * 1024;
    // Stop() drains: no new connections or keep-alive reads, and in-flight
    // requests get this long to finish before the pool is torn down.
    int drain_timeout_seconds = 10;
    // TLS termination: set both (PEM text, not file paths) to serve https.
    std::string tls_certificate_chain_pem;
    std::string tls_private_key_pem;
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

// Production HTTP/1.1 client transport on Boost.Beast/asio (ADR-0007):
// keep-alive connection reuse behind a small idle pool, per-request
// timeouts, and TLS via asio SSL (BoringSSL) with certificate and hostname
// verification on by default. Thread-safe: concurrent Send() calls use
// distinct connections.
//
//   auto transport = smithy::http::BeastHttpClient::FromEndpoint(
//       "https://api.example.com");
//   config.endpoint = "https://api.example.com";   // path prefix + identity
//   config.http_client = *transport;               // the wire
//   auto client = MyServiceClient::Create(std::move(config));
class BeastHttpClient : public HttpClient {
 public:
  struct Options {
    std::string host;
    int port = 80;
    bool tls = false;
    // Certificate + hostname verification is on by default. `ca_pem`
    // replaces the system trust roots (private CAs, tests); setting
    // `verify_peer = false` disables verification entirely — never do that
    // in production.
    bool verify_peer = true;
    std::string ca_pem;
    int request_timeout_ms = 30000;
    // Idle keep-alive connections retained for reuse.
    std::size_t max_idle_connections = 4;
  };

  explicit BeastHttpClient(Options options);
  ~BeastHttpClient() override;

  // Convenience: host/port/tls from an "http(s)://host[:port]" URL (any path
  // prefix belongs to ClientConfig::endpoint, not the transport).
  static Outcome<std::shared_ptr<BeastHttpClient>> FromEndpoint(std::string_view url);

  Outcome<HttpResponse> Send(const HttpRequest& request) override;

 private:
  struct State;  // Hides boost headers from this public header.

  std::shared_ptr<State> state_;
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_BEAST_TRANSPORT_H_
